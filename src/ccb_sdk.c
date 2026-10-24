// ccb_sdk implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_proc.h"
#include "ccb_json.h"
#include "ccb_sdk.h"
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

char *ccb_sdk_find_on_path(const char *name) {
  if (strchr(name, '/') != NULL) {
    if (access(name, X_OK) == 0) {
      return ccb_xstrdup(name);
    }
    return NULL;
  }
  const char *path = getenv("PATH");
  if (path == NULL) {
    return NULL;
  }
  const char *p = path;
  while (1) {
    const char *colon = strchr(p, ':');
    size_t dir_len = colon == NULL ? strlen(p) : (size_t)(colon - p);
    ccb_str s;
    ccb_str_init(&s);
    if (dir_len == 0) {
      ccb_str_append(&s, ".");
    } else {
      ccb_str_append_n(&s, p, dir_len);
    }
    ccb_str_append_c(&s, '/');
    ccb_str_append(&s, name);
    char *candidate = ccb_str_steal(&s);
    if (access(candidate, X_OK) == 0) {
      return candidate;
    }
    free(candidate);
    if (colon == NULL) {
      break;
    }
    p = colon + 1;
  }
  return NULL;
}

static const char *ccb_host_tag(void) {
#if defined(__APPLE__)
#if defined(__aarch64__)
  return "darwin-arm64";
#else
  return "darwin-x86_64";
#endif
#elif defined(__aarch64__)
  return "linux-aarch64";
#else
  return "linux-x86_64";
#endif
}

static void ccb_sdk_diag(ccb_diag_list *diags, const char *code, const char *msg) {
  ccb_diag d = ccb_diag_make(code, "toolchain", msg);
  ccb_diag_list_push(diags, &d);
}

static void ccb_sdk_diagf(ccb_diag_list *diags, const char *code, const char *what, const char *path) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, what);
  ccb_str_append(&s, ": ");
  ccb_str_append(&s, path);
  ccb_diag d = ccb_diag_make(code, "toolchain", s.buf);
  d.suggestion = ccb_xstrdup("run 'ccb setup' to install the missing component, or point [sdk] home at an existing SDK");
  ccb_diag_list_push(diags, &d);
  ccb_str_free(&s);
}

// An empty env value counts as unset, so `CCB_AAPT2=` behaves like no
// override at all instead of a broken empty path.
static const char *ccb_nonempty_env(const char *name) {
  const char *v = getenv(name);
  if (v != NULL && v[0] == '\0') {
    return NULL;
  }
  return v;
}

// A broken explicit tool override: CCB-E2009, naming both the source
// (e.g. "[sdk] aapt2" or "CCB_AAPT2") and the path, so the user knows
// exactly which setting to fix. Never falls back to the default.
static void ccb_sdk_diag_override(ccb_diag_list *diags, const char *label, const char *path) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "explicit tool override ");
  ccb_str_append(&s, label);
  ccb_str_append(&s, " is missing or not executable: ");
  ccb_str_append(&s, path);
  ccb_diag d = ccb_diag_make("CCB-E2009", "toolchain", s.buf);
  d.suggestion = ccb_xstrdup("fix the override path, or unset it to use the default SDK tool");
  ccb_diag_list_push(diags, &d);
  ccb_str_free(&s);
}

void ccb_sdk_free(ccb_sdk *sdk) {
  if (sdk == NULL) {
    return;
  }
  free(sdk->sdk_home);
  free(sdk->build_tools_dir);
  free(sdk->build_tools_version);
  free(sdk->aapt2);
  free(sdk->aapt2_source);
  free(sdk->d8);
  free(sdk->apksigner);
  free(sdk->zipalign);
  free(sdk->zipalign_source);
  free(sdk->r8_jar);
  free(sdk->r8_source);
  free(sdk->android_jar);
  free(sdk->java);
  free(sdk->javac);
  free(sdk->kotlin_home);
  free(sdk->kotlinc);
  free(sdk->kotlin_version);
  free(sdk->ndk_home);
  free(sdk->ndk_clang);
  free(sdk);
}

// Parse the major version out of `java -version` stderr, which looks
// like: openjdk version "17.0.20.1" ... or: java version "21.0.1" ...
static int ccb_parse_java_major(const char *text) {
  const char *v = strstr(text, "version \"");
  if (v == NULL) {
    return -1;
  }
  v += 9;
  char *end = NULL;
  long major = strtol(v, &end, 10);
  if (end == v || major <= 0) {
    return -1;
  }
  return (int)major;
}

ccb_status ccb_sdk_find_java(char **out, int *major_out) {
  *out = NULL;
  if (major_out != NULL) {
    *major_out = 0;
  }
  const char *java_home = getenv("JAVA_HOME");
  char *java = NULL;
  if (java_home != NULL) {
    char *p = ccb_path_join(java_home, "bin/java");
    if (access(p, X_OK) == 0) {
      java = p;
    } else {
      free(p);
    }
  }
  if (java == NULL) {
    java = ccb_sdk_find_on_path("java");
  }
  if (java == NULL) {
    return CCB_ERR_TOOLCHAIN;
  }
  char *argv[] = { java, "-version", NULL };
  ccb_proc_result r;
  if (ccb_proc_run(argv, NULL, NULL, &r) != CCB_OK) {
    ccb_proc_result_free(&r);
    free(java);
    return CCB_ERR_TOOLCHAIN;
  }
  int major = ccb_parse_java_major(r.err);
  ccb_proc_result_free(&r);
  if (major < 17) {
    free(java);
    return CCB_ERR_TOOLCHAIN;
  }
  *out = java;
  if (major_out != NULL) {
    *major_out = major;
  }
  return CCB_OK;
}

// Parse kotlinc -version stderr: "info: kotlinc-jvm 2.4.21 (JRE ...)".
char *ccb_sdk_parse_kotlin_version(const char *text) {
  const char *v = strstr(text, "kotlinc-jvm ");
  if (v == NULL) {
    return NULL;
  }
  v += 12;
  size_t n = 0;
  while (v[n] != '\0' && (isdigit((unsigned char)v[n]) || v[n] == '.')) {
    n++;
  }
  if (n == 0) {
    return NULL;
  }
  return ccb_xstrndup(v, n);
}

// 1 when the tool at path spawns: any exit code except 127 (the
// child's exec-failure exit) and no fatal signal. arg may be NULL
// for no arguments. The argument must be harmless (a version query
// or no arguments at all): it only proves the binary runs.
int ccb_sdk_tool_runnable(const char *path, const char *arg) {
  char *argv[] = { (char *)path, (char *)arg, NULL };
  ccb_proc_result r;
  int ok = 0;
  if (ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK) {
    ok = r.exit_code != 127 && !r.signaled;
  }
  ccb_proc_result_free(&r);
  return ok;
}

// --- Tool probe cache -------------------------------------------------
// File: <build_dir>/intermediates/tool-probes.json, version 1:
// {"version":1,"probes":[{"tool":"kotlinc","path":"...",
//   "mtime_sec":123,"mtime_nsec":456,"size":789,"version":"2.4.21"}]}
// A lookup stats the tool and compares path + mtime + size; anything
// else is a miss. Stores never create directories and never fail the
// caller: when intermediates/ does not exist yet (resolve runs before
// the pipeline creates it) the store silently skips, and the pipeline
// records the probes after mkdir instead.

static char *probe_cache_path(const char *build_dir) {
  if (build_dir == NULL) {
    return NULL;
  }
  char *inter = ccb_path_join(build_dir, "intermediates");
  char *p = ccb_path_join(inter, "tool-probes.json");
  free(inter);
  return p;
}

static int probe_stat(const char *path, long long *sec, long *nsec, long long *size) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return 0;
  }
  *sec = (long long)st.st_mtime;
#if defined(__APPLE__)
  *nsec = (long)st.st_mtimespec.tv_nsec;
#else
  *nsec = (long)st.st_mtim.tv_nsec;
#endif
  *size = (long long)st.st_size;
  return 1;
}

char *ccb_sdk_probe_cache_lookup(const char *build_dir, const char *tool, const char *path) {
  if (build_dir == NULL || tool == NULL || path == NULL) {
    return NULL;
  }
  long long sec = 0;
  long nsec = 0;
  long long size = 0;
  if (!probe_stat(path, &sec, &nsec, &size)) {
    return NULL;
  }
  char *cpath = probe_cache_path(build_dir);
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(cpath, &text, &len) != CCB_OK) {
    free(cpath);
    return NULL;
  }
  free(cpath);
  char *err = NULL;
  ccb_json_value *doc = ccb_json_parse(ccb_slice_from(text), &err);
  free(err);
  free(text);
  if (doc == NULL || doc->type != CCB_JSON_OBJ) {
    ccb_json_free(doc);
    return NULL;
  }
  char *found = NULL;
  const ccb_json_value *ver = ccb_json_obj_get(doc, "version");
  const ccb_json_value *probes = ccb_json_obj_get(doc, "probes");
  if (ver != NULL && ver->type == CCB_JSON_NUM && ver->num == 1 &&
      probes != NULL && probes->type == CCB_JSON_ARR) {
    for (size_t i = 0; i < probes->count; i++) {
      const ccb_json_value *e = probes->items[i];
      if (e == NULL || e->type != CCB_JSON_OBJ) {
        continue;
      }
      const ccb_json_value *t = ccb_json_obj_get(e, "tool");
      const ccb_json_value *p = ccb_json_obj_get(e, "path");
      const ccb_json_value *ms = ccb_json_obj_get(e, "mtime_sec");
      const ccb_json_value *mn = ccb_json_obj_get(e, "mtime_nsec");
      const ccb_json_value *sz = ccb_json_obj_get(e, "size");
      const ccb_json_value *v = ccb_json_obj_get(e, "version");
      if (t == NULL || t->type != CCB_JSON_STR || strcmp(t->str, tool) != 0) {
        continue;
      }
      if (p == NULL || p->type != CCB_JSON_STR || strcmp(p->str, path) != 0) {
        continue;
      }
      if (ms == NULL || ms->type != CCB_JSON_NUM || (long long)ms->num != sec) {
        continue;
      }
      if (mn == NULL || mn->type != CCB_JSON_NUM || (long long)mn->num != nsec) {
        continue;
      }
      if (sz == NULL || sz->type != CCB_JSON_NUM || (long long)sz->num != size) {
        continue;
      }
      if (v == NULL || v->type != CCB_JSON_STR || v->str[0] == '\0') {
        continue;
      }
      // A java version is digits-only ("17"); anything else is a
      // corrupt entry, not a version worth trusting.
      if (strcmp(tool, "java") == 0) {
        int digits = 1;
        for (const char *c = v->str; *c != '\0'; c++) {
          if (!isdigit((unsigned char)*c)) {
            digits = 0;
            break;
          }
        }
        if (!digits) {
          continue;
        }
      }
      found = ccb_xstrdup(v->str);
      break;
    }
  }
  ccb_json_free(doc);
  return found;
}

typedef struct {
  char *tool;
  char *path;
  long long sec;
  long nsec;
  long long size;
  char *version;
} probe_entry;

void ccb_sdk_probe_cache_store(const char *build_dir, const char *tool, const char *path, const char *version) {
  if (build_dir == NULL || tool == NULL || path == NULL || version == NULL) {
    return;
  }
  long long sec = 0;
  long nsec = 0;
  long long size = 0;
  if (!probe_stat(path, &sec, &nsec, &size)) {
    return;
  }
  // Load existing entries; a missing or corrupt file starts fresh.
  probe_entry *entries = NULL;
  size_t count = 0;
  size_t cap = 0;
  char *cpath = probe_cache_path(build_dir);
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(cpath, &text, &len) == CCB_OK) {
    char *err = NULL;
    ccb_json_value *doc = ccb_json_parse(ccb_slice_from(text), &err);
    free(err);
    free(text);
    if (doc != NULL && doc->type == CCB_JSON_OBJ) {
      const ccb_json_value *ver = ccb_json_obj_get(doc, "version");
      const ccb_json_value *probes = ccb_json_obj_get(doc, "probes");
      if (ver != NULL && ver->type == CCB_JSON_NUM && ver->num == 1 &&
          probes != NULL && probes->type == CCB_JSON_ARR) {
        for (size_t i = 0; i < probes->count; i++) {
          const ccb_json_value *e = probes->items[i];
          if (e == NULL || e->type != CCB_JSON_OBJ) {
            continue;
          }
          const ccb_json_value *t = ccb_json_obj_get(e, "tool");
          const ccb_json_value *p = ccb_json_obj_get(e, "path");
          const ccb_json_value *ms = ccb_json_obj_get(e, "mtime_sec");
          const ccb_json_value *mn = ccb_json_obj_get(e, "mtime_nsec");
          const ccb_json_value *sz = ccb_json_obj_get(e, "size");
          const ccb_json_value *v = ccb_json_obj_get(e, "version");
          if (t == NULL || t->type != CCB_JSON_STR ||
              p == NULL || p->type != CCB_JSON_STR ||
              ms == NULL || ms->type != CCB_JSON_NUM ||
              mn == NULL || mn->type != CCB_JSON_NUM ||
              sz == NULL || sz->type != CCB_JSON_NUM ||
              v == NULL || v->type != CCB_JSON_STR) {
            continue;
          }
          if (count == cap) {
            cap = cap == 0 ? 4 : cap * 2;
            entries = ccb_xrealloc(entries, cap * sizeof(*entries));
          }
          entries[count].tool = ccb_xstrdup(t->str);
          entries[count].path = ccb_xstrdup(p->str);
          entries[count].sec = (long long)ms->num;
          entries[count].nsec = (long)mn->num;
          entries[count].size = (long long)sz->num;
          entries[count].version = ccb_xstrdup(v->str);
          count++;
        }
      }
    }
    ccb_json_free(doc);
  }
  // Upsert by (tool, path).
  size_t at = count;
  for (size_t i = 0; i < count; i++) {
    if (strcmp(entries[i].tool, tool) == 0 && strcmp(entries[i].path, path) == 0) {
      at = i;
      break;
    }
  }
  if (at == count) {
    if (count == cap) {
      cap = cap == 0 ? 4 : cap * 2;
      entries = ccb_xrealloc(entries, cap * sizeof(*entries));
    }
    entries[count].tool = ccb_xstrdup(tool);
    entries[count].path = ccb_xstrdup(path);
    count++;
    at = count - 1;
  } else {
    free(entries[at].version);
  }
  entries[at].sec = sec;
  entries[at].nsec = nsec;
  entries[at].size = size;
  entries[at].version = ccb_xstrdup(version);
  // Serialize. A failed write is silent: the next build just probes.
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "{\"version\":1,\"probes\":[");
  for (size_t i = 0; i < count; i++) {
    if (i != 0) {
      ccb_str_append(&s, ",");
    }
    ccb_str_append(&s, "{\"tool\":");
    ccb_json_escape(&s, entries[i].tool);
    ccb_str_append(&s, ",\"path\":");
    ccb_json_escape(&s, entries[i].path);
    ccb_str_appendf(&s, ",\"mtime_sec\":%lld,\"mtime_nsec\":%ld,\"size\":%lld,\"version\":",
      entries[i].sec, entries[i].nsec, entries[i].size);
    ccb_json_escape(&s, entries[i].version);
    ccb_str_append(&s, "}");
  }
  ccb_str_append(&s, "]}");
  ccb_write_file(cpath, s.buf, s.len);
  ccb_str_free(&s);
  for (size_t i = 0; i < count; i++) {
    free(entries[i].tool);
    free(entries[i].path);
    free(entries[i].version);
  }
  free(entries);
  free(cpath);
}

ccb_status ccb_sdk_resolve(const ccb_config *cfg, ccb_sdk **out, ccb_diag_list *diags) {
  *out = NULL;
  size_t diag_before = diags->count;
  ccb_sdk *s = ccb_xcalloc(1, sizeof(*s));

  // SDK home.
  const char *home = cfg->sdk_home;
  if (home == NULL) {
    home = getenv("ANDROID_HOME");
  }
  if (home == NULL) {
    home = getenv("ANDROID_SDK_ROOT");
  }
  char *default_home = NULL;
  if (home == NULL) {
    const char *h = getenv("HOME");
    if (h != NULL) {
      default_home = ccb_path_join(h, "Android/Sdk");
      home = default_home;
    }
  }
  if (home == NULL || !ccb_path_is_dir(home)) {
    ccb_sdk_diag(diags, "CCB-E2001", "Android SDK not found: set [sdk] home in ccb.toml or ANDROID_HOME");
    free(default_home);
    ccb_sdk_free(s);
    return CCB_ERR_TOOLCHAIN;
  }
  s->sdk_home = ccb_xstrdup(home);
  free(default_home);

  // Build-tools.
  s->build_tools_version = ccb_xstrdup(cfg->build_tools);
  s->build_tools_dir = ccb_path_join3(s->sdk_home, "build-tools", cfg->build_tools);
  // Tool overrides, highest precedence first: [sdk] key, then env var,
  // then the default SDK path. Overrides are resolved and validated
  // here, independent of the build-tools directory branch below, so a
  // broken override is reported even when build-tools is missing
  // (a CCB-E2002 must not mask it). An override path is used exactly
  // as given: a relative value is cwd-relative at invocation time.
  const char *aapt2_override = cfg->aapt2_override;
  const char *aapt2_label = "[sdk] aapt2";
  if (aapt2_override == NULL) {
    const char *env = ccb_nonempty_env("CCB_AAPT2");
    if (env != NULL) {
      aapt2_override = env;
      aapt2_label = "CCB_AAPT2";
    }
  }
  const char *zipalign_override = cfg->zipalign_override;
  const char *zipalign_label = "[sdk] zipalign";
  if (zipalign_override == NULL) {
    const char *env = ccb_nonempty_env("CCB_ZIPALIGN");
    if (env != NULL) {
      zipalign_override = env;
      zipalign_label = "CCB_ZIPALIGN";
    }
  }
  s->aapt2_source = ccb_xstrdup(aapt2_override != NULL ? (cfg->aapt2_override != NULL ? "config" : "env") : "default");
  s->zipalign_source = ccb_xstrdup(zipalign_override != NULL ? (cfg->zipalign_override != NULL ? "config" : "env") : "default");
  if (aapt2_override != NULL) {
    if (access(aapt2_override, X_OK) != 0) {
      ccb_sdk_diag_override(diags, aapt2_label, aapt2_override);
    } else {
      s->aapt2 = ccb_xstrdup(aapt2_override);
    }
  }
  if (zipalign_override != NULL) {
    if (access(zipalign_override, X_OK) != 0) {
      ccb_sdk_diag_override(diags, zipalign_label, zipalign_override);
    } else {
      s->zipalign = ccb_xstrdup(zipalign_override);
    }
  }
  if (!ccb_path_is_dir(s->build_tools_dir)) {
    ccb_sdk_diagf(diags, "CCB-E2002", "build-tools not installed", s->build_tools_dir);
  } else {
    static const char *tools[] = { "aapt2", "d8", "apksigner", "zipalign" };
    char **slots[] = { &s->aapt2, &s->d8, &s->apksigner, &s->zipalign };
    for (int i = 0; i < 4; i++) {
      // An overridden tool is already resolved (or already diagnosed)
      // above; never overwrite it with the default path.
      if ((strcmp(tools[i], "aapt2") == 0 && aapt2_override != NULL) ||
          (strcmp(tools[i], "zipalign") == 0 && zipalign_override != NULL)) {
        continue;
      }
      char *p = ccb_path_join(s->build_tools_dir, tools[i]);
      // Native tools only: aapt2 and zipalign are host binaries, so
      // an SDK copy can exist yet not run (x86-64 binary on Termux
      // aarch64). d8 and apksigner are Java and always run.
      int is_native = strcmp(tools[i], "aapt2") == 0 || strcmp(tools[i], "zipalign") == 0;
      const char *probe = strcmp(tools[i], "aapt2") == 0 ? "version" : NULL;
      if (access(p, X_OK) == 0 && (!is_native || ccb_sdk_tool_runnable(p, probe))) {
        *slots[i] = p;
        continue;
      }
      if (is_native) {
        // Fall back to a runnable copy on PATH (e.g. a Termux
        // package). Overrides never fall back: an explicit choice
        // that is broken fails instead.
        char *on_path = ccb_sdk_find_on_path(tools[i]);
        if (on_path != NULL && ccb_sdk_tool_runnable(on_path, probe)) {
          *slots[i] = on_path;
          if (strcmp(tools[i], "aapt2") == 0) {
            free(s->aapt2_source);
            s->aapt2_source = ccb_xstrdup("path");
          } else {
            free(s->zipalign_source);
            s->zipalign_source = ccb_xstrdup("path");
          }
          free(p);
          continue;
        }
        free(on_path);
      }
      if (access(p, X_OK) == 0) {
        ccb_sdk_diagf(diags, "CCB-E2009", "tool present but cannot run (wrong architecture?)", p);
      } else {
        ccb_sdk_diagf(diags, "CCB-E2009", "tool missing or not executable", p);
      }
      free(p);
    }
  }

  // Platform android.jar.
  {
    ccb_str plat;
    ccb_str_init(&plat);
    ccb_str_append(&plat, s->sdk_home);
    ccb_str_append(&plat, "/platforms/android-");
    ccb_str_appendf(&plat, "%ld", cfg->compile_sdk);
    char *plat_dir = ccb_str_steal(&plat);
    s->android_jar = ccb_path_join(plat_dir, "android.jar");
    if (!ccb_path_exists(s->android_jar)) {
      ccb_sdk_diagf(diags, "CCB-E2003", "platform android.jar not found", s->android_jar);
    }
    free(plat_dir);
  }

  // Java.
  {
    const char *java_home = getenv("JAVA_HOME");
    if (java_home != NULL) {
      char *p = ccb_path_join(java_home, "bin/java");
      if (access(p, X_OK) == 0) {
        s->java = p;
      } else {
        free(p);
      }
    }
    if (s->java == NULL) {
      s->java = ccb_sdk_find_on_path("java");
    }
    if (s->java == NULL) {
      ccb_sdk_diag(diags, "CCB-E2005", "java not found: set JAVA_HOME or put java 17+ on PATH");
    } else {
      // A cached probe for the unchanged binary skips the JVM spawn.
      char *cached = ccb_sdk_probe_cache_lookup(cfg->build_dir, "java", s->java);
      if (cached != NULL) {
        s->java_major = atoi(cached);
        free(cached);
        if (s->java_major < 17) {
          ccb_sdk_diag(diags, "CCB-E2005", "java must be 17 or newer");
        }
      } else {
        char *argv[] = { s->java, "-version", NULL };
        ccb_proc_result r;
        if (ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK) {
          s->java_major = ccb_parse_java_major(r.err);
          if (s->java_major < 17) {
            ccb_sdk_diag(diags, "CCB-E2005", "java must be 17 or newer");
          }
          ccb_proc_result_free(&r);
        } else {
          ccb_sdk_diag(diags, "CCB-E2005", "could not run java -version");
        }
      }
    }
    if (java_home != NULL) {
      char *p = ccb_path_join(java_home, "bin/javac");
      if (access(p, X_OK) == 0) {
        s->javac = p;
      } else {
        free(p);
      }
    }
    if (s->javac == NULL) {
      s->javac = ccb_sdk_find_on_path("javac");
    }
    if (s->javac == NULL) {
      ccb_sdk_diag(diags, "CCB-E2005", "javac not found: set JAVA_HOME or put javac on PATH");
    }
  }

  // Kotlin.
  {
    const char *kh = cfg->kotlin_home;
    if (kh == NULL) {
      kh = getenv("KOTLIN_HOME");
    }
    if (kh != NULL && ccb_path_is_dir(kh)) {
      s->kotlin_home = ccb_xstrdup(kh);
      s->kotlinc = ccb_path_join3(kh, "bin", "kotlinc");
      if (access(s->kotlinc, X_OK) != 0) {
        // Tolerate the common unzip layout where KOTLIN_HOME points at
        // the parent of the distribution's kotlinc/ directory.
        free(s->kotlinc);
        char *nested = ccb_path_join(kh, "kotlinc");
        s->kotlinc = ccb_path_join(nested, "bin/kotlinc");
        if (access(s->kotlinc, X_OK) == 0) {
          free(s->kotlin_home);
          s->kotlin_home = nested;
        } else {
          free(nested);
          s->kotlinc = NULL;
        }
      }
    }
    if (s->kotlinc == NULL) {
      char *on_path = ccb_sdk_find_on_path("kotlinc");
      if (on_path != NULL) {
        s->kotlinc = on_path;
      }
    }
    if (s->kotlinc == NULL && s->sdk_home != NULL) {
      // Setup-installed compiler: <sdk-home>/kotlinc. Last resort,
      // after explicit config, KOTLIN_HOME, and PATH.
      char *installed_home = ccb_path_join(s->sdk_home, "kotlinc");
      char *installed = ccb_path_join(installed_home, "bin/kotlinc");
      if (access(installed, X_OK) == 0) {
        s->kotlin_home = installed_home;
        s->kotlinc = installed;
      } else {
        free(installed_home);
        free(installed);
      }
    }
    if (s->kotlinc == NULL) {
      ccb_sdk_diag(diags, "CCB-E2004", "kotlinc not found: set [sdk] kotlin_home in ccb.toml or KOTLIN_HOME");
    } else {
      // A cached probe for the unchanged binary skips the JVM spawn.
      char *cached = ccb_sdk_probe_cache_lookup(cfg->build_dir, "kotlinc", s->kotlinc);
      if (cached != NULL) {
        s->kotlin_version = cached;
        if (s->kotlin_version[0] != '2') {
          ccb_sdk_diag(diags, "CCB-E2006", "kotlinc must be 2.x");
        }
      } else {
        char *argv[] = { s->kotlinc, "-version", NULL };
        ccb_proc_result r;
        if (ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK) {
          s->kotlin_version = ccb_sdk_parse_kotlin_version(r.err);
          if (s->kotlin_version == NULL || s->kotlin_version[0] != '2') {
            ccb_sdk_diag(diags, "CCB-E2006", "kotlinc must be 2.x");
          }
          ccb_proc_result_free(&r);
        } else {
          ccb_sdk_diag(diags, "CCB-E2004", "could not run kotlinc -version");
        }
      }
    }
  }

  // r8: explicit [sdk] r8_jar wins; else the setup-managed copy at
  // <sdk-home>/r8/r8.jar. Absent r8 is not a resolve failure: only
  // release+minify builds need it, and the build reports that.
  if (cfg->r8_jar != NULL) {
    s->r8_jar = ccb_xstrdup(cfg->r8_jar);
    s->r8_source = ccb_xstrdup("config");
  } else {
    char *cand = ccb_path_join3(s->sdk_home, "r8", "r8.jar");
    if (ccb_path_exists(cand)) {
      s->r8_jar = cand;
      s->r8_source = ccb_xstrdup("sdk-home");
    } else {
      free(cand);
    }
  }
  // NDK (only when the project declares one).
  if (cfg->has_ndk) {
    if (cfg->ndk_version == NULL) {
      ccb_sdk_diag(diags, "CCB-E2007", "ndk section present but sdk.ndk_version is not set");
    } else {
      s->ndk_home = ccb_path_join3(s->sdk_home, "ndk", cfg->ndk_version);
      if (!ccb_path_is_dir(s->ndk_home)) {
        ccb_sdk_diagf(diags, "CCB-E2007", "NDK not installed", s->ndk_home);
      } else {
        char *props = ccb_path_join(s->ndk_home, "source.properties");
        char *text = NULL;
        size_t len = 0;
        int rev_ok = 0;
        if (ccb_read_file(props, &text, &len) == CCB_OK) {
          // source.properties writes "Pkg.Revision = 27.0.12077973"
          // (spaces around '='), so parse the value, don't substring it.
          const char *m = strstr(text, "Pkg.Revision");
          if (m != NULL) {
            const char *eq = strchr(m, '=');
            if (eq != NULL) {
              eq++;
              while (*eq == ' ' || *eq == '\t') {
                eq++;
              }
              size_t vlen = 0;
              while (eq[vlen] != '\0' && eq[vlen] != '\n' && eq[vlen] != '\r' &&
                  eq[vlen] != ' ' && eq[vlen] != '\t') {
                vlen++;
              }
              if (strlen(cfg->ndk_version) == vlen &&
                  strncmp(eq, cfg->ndk_version, vlen) == 0) {
                rev_ok = 1;
              }
            }
          }
          free(text);
        }
        free(props);
        if (!rev_ok) {
          ccb_sdk_diagf(diags, "CCB-E2008", "NDK revision mismatch", s->ndk_home);
        }
        ccb_str clang;
        ccb_str_init(&clang);
        ccb_str_append(&clang, s->ndk_home);
        ccb_str_append(&clang, "/toolchains/llvm/prebuilt/");
        ccb_str_append(&clang, ccb_host_tag());
        ccb_str_append(&clang, "/bin/clang");
        s->ndk_clang = ccb_str_steal(&clang);
        if (access(s->ndk_clang, X_OK) != 0) {
          ccb_sdk_diagf(diags, "CCB-E2009", "NDK clang missing or not executable", s->ndk_clang);
        }
      }
    }
  }

  if (diags->count > diag_before) {
    ccb_sdk_free(s);
    return CCB_ERR_TOOLCHAIN;
  }
  *out = s;
  return CCB_OK;
}
