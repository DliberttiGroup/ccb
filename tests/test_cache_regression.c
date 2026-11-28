// Cache/fingerprint regression tests (Step 3): black-box, like
// test_cache_outputs.c — needs a real SDK toolchain, else prints a
// skip note and passes. The exact run/skip signal is the --json task
// events (`result: done|skipped`); --explain is asserted as an upper
// bound on what reruns (exact only where explain is known exact —
// it over-predicts downstream of link, R-3, Step 4's fix). One case
// is not asserted against --explain at all: after an AAR content
// change, explain under-predicts compile-code/dex/package/align
// (it reads the classpath from the still-old extracted jar), so
// that scenario asserts the build's run set only.
// Scenarios:
//   hello project: cold build; no-op (byte-identical APK);
//     comment-only .kt change (only compile-code reruns);
//     real code change; version_code change; strings.xml change;
//     truncated compiled-res zip / linked.ap_ / aligned APK (each
//     fails loudly with its CCB-E code, never ships, then recovers
//     by rerunning exactly the affected step); truncated final APK
//     (self-heals: sign re-runs on its corrupt output and restores
//     the byte-identical APK); torn ccb-cache.json (rebuild all);
//     aapt2 path swap (tool fingerprint invalidates all);
//     debug keystore alias switch (sign reruns, cert changes).
//   AAR project: cold build; no-op; truncated extracted
//     classes.jar (fails loudly, CCB-E3005, then recovers by
//     rerunning compile-code, which restores the R.class the failed
//     build wiped); AAR content change (deps + compile-code
//     downstream rerun).
//   cmake project: cold build; no-op (native skips); adding a
//     [cmake] args entry (native reruns); no-op again.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cache.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include "ccb_zip.h"
#include "ccb_sdk.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define HELLO_PROJ B("hello")
#define AAR_PROJ B("hello-aar")
#define CMK_PROJ B("hello-cmk")
#define LIB_AAR B("lib.aar")
#define KEYSTORE B("ks")

// Task bits, from --json task events and --explain lines.
#define T_DEPS (1u << 0)
#define T_MERGE (1u << 1)
#define T_COMPILE_RES (1u << 2)
#define T_LINK (1u << 3)
#define T_CODE (1u << 4)
#define T_DEX (1u << 5)
#define T_PACKAGE (1u << 6)
#define T_ALIGN (1u << 7)
#define T_SIGN (1u << 8)
#define T_VERIFY (1u << 9)
#define T_NATIVE (1u << 10)
#define T_DEXCHECK (1u << 11)
#define T_LIBS (1u << 12)
#define T_MODULES (1u << 13)
#define CORE8 (T_COMPILE_RES | T_LINK | T_CODE | T_DEX | T_PACKAGE | \
  T_ALIGN | T_SIGN | T_VERIFY)

static const struct { const char *name; unsigned bit; } TASK_BITS[] = {
  { "deps", T_DEPS }, { "libraries", T_LIBS }, { "modules", T_MODULES },
  { "merge-manifest", T_MERGE }, { "compile-res", T_COMPILE_RES },
  { "link", T_LINK }, { "compile-code", T_CODE }, { "dex", T_DEX },
  { "dexcheck", T_DEXCHECK }, { "native", T_NATIVE },
  { "package", T_PACKAGE }, { "align", T_ALIGN }, { "sign", T_SIGN },
  { "verify", T_VERIFY },
};

static unsigned task_bit(const char *name) {
  // --explain names cmake native steps native-<abi>; the --json
  // event id is plain "native". Normalize both to T_NATIVE.
  if (strncmp(name, "native-", 7) == 0) {
    return T_NATIVE;
  }
  // Per-module tasks (Step 8): --json event ids are
  // "module-<name>", --explain lines are "module <name>".
  // Both count as the modules step.
  if (strncmp(name, "module-", 7) == 0 ||
      strncmp(name, "module ", 7) == 0) {
    return T_MODULES;
  }
  for (size_t i = 0; i < CCB_ARRAY_LEN(TASK_BITS); i++) {
    if (strcmp(TASK_BITS[i].name, name) == 0) {
      return TASK_BITS[i].bit;
    }
  }
  return 0;
}

static void write_text(const char *path, const char *text) {
  CHECK(ccb_write_file(path, text, strlen(text)) == CCB_OK);
}

static void mkdir_or_fail(const char *path) {
  CHECK(ccb_mkdir_p(path) == CCB_OK);
}

static char *find_binary(int argc, char **argv) {
  if (argc > 1 && argv[1] != NULL && access(argv[1], X_OK) == 0) {
    return ccb_xstrdup(argv[1]);
  }
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = '\0';
    char *dir = ccb_dirname(exe);
    char *cand = ccb_path_join(dir, "ccb");
    free(dir);
    if (access(cand, X_OK) == 0) {
      return cand;
    }
    free(cand);
  }
  return NULL;
}

static char *env_or_home(const char *env_name, const char *rel) {
  const char *v = getenv(env_name);
  if (v != NULL && v[0] != '\0') {
    return ccb_xstrdup(v);
  }
  const char *home = getenv("HOME");
  if (home == NULL) {
    return NULL;
  }
  return ccb_path_join(home, rel);
}

// First NDK revision installed under <sdk_home>/ndk, or NULL.
static char *find_ndk_version(const char *sdk_home) {
  char *dir = ccb_path_join(sdk_home, "ndk");
  DIR *d = opendir(dir);
  free(dir);
  if (d == NULL) {
    return NULL;
  }
  char *version = NULL;
  struct dirent *e = NULL;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') {
      continue;
    }
    version = ccb_xstrdup(e->d_name);
    break;
  }
  closedir(d);
  return version;
}

static int zip_has(const char *zip_path, const char *name) {
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(zip_path, &a) != CCB_OK) {
    return 0;
  }
  int found = ccb_zip_find(a, name) != NULL;
  ccb_zip_close(a);
  return found;
}

// Full ccb.toml for a hello project, regenerated per scenario so a
// single field can change without duplicate TOML sections.
typedef struct {
  int version_code;
  const char *aapt2;       // NULL = no [sdk] aapt2 override
  const char *aar_path;    // NULL = no [deps]
  const char *sign_store;  // NULL = no [signing.debug]
  const char *sign_alias;
  const char *ndk_version; // NULL = no [sdk] ndk_version
  const char *cmake_lists; // NULL = no [cmake]
  const char *cmake_args;  // NULL = no args line (one "-DFOO=bar")
} cfg_opts;

static void write_config(const char *proj, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home, const cfg_opts *o) {
  char major[8];
  snprintf(major, sizeof(major), "%.*s", (int)strcspn(bt_ver, "."), bt_ver);
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "ccb_version = 1\n\n[project]\nname = \"hello\"\n"
    "package = \"com.example.hello\"\nversion = \"1.0\"\nversion_code = ");
  ccb_str_appendf(&s, "%d\n", o->version_code);
  ccb_str_append(&s, "min_sdk = 26\ntarget_sdk = ");
  ccb_str_append(&s, major);
  ccb_str_append(&s, "\ncompile_sdk = ");
  ccb_str_append(&s, major);
  ccb_str_append(&s, "\n\n[paths]\nmanifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\nkotlin = \"src/main/kotlin\"\n\n[sdk]\nhome = \"");
  ccb_str_append(&s, sdk_home);
  ccb_str_append(&s, "\"\nbuild_tools = \"");
  ccb_str_append(&s, bt_ver);
  ccb_str_append(&s, "\"\nkotlin_home = \"");
  ccb_str_append(&s, kotlin_home);
  ccb_str_append(&s, "\"\n");
  if (o->aapt2 != NULL) {
    ccb_str_append(&s, "aapt2 = \"");
    ccb_str_append(&s, o->aapt2);
    ccb_str_append(&s, "\"\n");
  }
  if (o->ndk_version != NULL) {
    ccb_str_append(&s, "ndk_version = \"");
    ccb_str_append(&s, o->ndk_version);
    ccb_str_append(&s, "\"\n");
  }
  if (o->aar_path != NULL) {
    ccb_str_append(&s, "\n[deps]\naars = [\"");
    ccb_str_append(&s, o->aar_path);
    ccb_str_append(&s, "\"]\n");
  }
  if (o->sign_store != NULL) {
    ccb_str_append(&s, "\n[signing.debug]\nstore = \"");
    ccb_str_append(&s, o->sign_store);
    ccb_str_append(&s, "\"\nalias = \"");
    ccb_str_append(&s, o->sign_alias);
    ccb_str_append(&s, "\"\n");
  }
  if (o->cmake_lists != NULL) {
    ccb_str_append(&s, "\n[ndk]\nabis = [\"arm64-v8a\"]\n\n[cmake]\nlists = \"");
    ccb_str_append(&s, o->cmake_lists);
    ccb_str_append(&s, "\"\n");
    if (o->cmake_args != NULL) {
      ccb_str_append(&s, "args = [\"");
      ccb_str_append(&s, o->cmake_args);
      ccb_str_append(&s, "\"]\n");
    }
  }
  char *cfg = ccb_path_join(proj, "ccb.toml");
  write_text(cfg, s.buf);
  free(cfg);
  ccb_str_free(&s);
}

static void write_kt(const char *proj, const char *comment,
    const char *extra_field) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s,
    "package com.example.hello\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "\n"
    "class MainActivity : Activity() {\n");
  if (extra_field != NULL) {
    ccb_str_append(&s, "  private val ");
    ccb_str_append(&s, extra_field);
    ccb_str_append(&s, "\n");
  }
  ccb_str_append(&s,
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    setContentView(R.layout.activity_main)\n"
    "  }\n"
    "}\n");
  // A comment appended after the final brace shifts no code lines,
  // so the compiled classes are byte-identical: only compile-code
  // reruns. (Inserting the comment inside the class would shift the
  // LineNumberTable and change the .class bytes — a real code change
  // in everything but name.)
  if (comment != NULL) {
    ccb_str_append(&s, "// ");
    ccb_str_append(&s, comment);
    ccb_str_append(&s, "\n");
  }
  char *kt = ccb_path_join(proj,
    "src/main/kotlin/com/example/hello/MainActivity.kt");
  write_text(kt, s.buf);
  free(kt);
  ccb_str_free(&s);
}

static void write_strings(const char *proj, const char *value) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources><string name=\"app_name\">");
  ccb_str_append(&s, value);
  ccb_str_append(&s, "</string></resources>\n");
  char *strings = ccb_path_join(proj, "src/main/res/values/strings.xml");
  write_text(strings, s.buf);
  free(strings);
  ccb_str_free(&s);
}

// Minimal Kotlin hello project skeleton (same shape as
// test_cache_outputs.c): dirs, manifest, layout, default sources.
static void write_hello_project(const char *proj, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home, const cfg_opts *o) {
  char *kt_dir = ccb_path_join(proj, "src/main/kotlin/com/example/hello");
  mkdir_or_fail(kt_dir);
  free(kt_dir);
  char *values_dir = ccb_path_join(proj, "src/main/res/values");
  mkdir_or_fail(values_dir);
  free(values_dir);
  char *layout_dir = ccb_path_join(proj, "src/main/res/layout");
  mkdir_or_fail(layout_dir);
  free(layout_dir);
  write_config(proj, sdk_home, bt_ver, kotlin_home, o);
  char *man = ccb_path_join(proj, "src/main/AndroidManifest.xml");
  write_text(man,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  package=\"com.example.hello\">\n"
    "  <application android:label=\"@string/app_name\">\n"
    "    <activity android:name=\".MainActivity\" android:exported=\"true\">\n"
    "      <intent-filter>\n"
    "        <action android:name=\"android.intent.action.MAIN\" />\n"
    "        <category android:name=\"android.intent.category.LAUNCHER\" />\n"
    "      </intent-filter>\n"
    "    </activity>\n"
    "  </application>\n"
    "</manifest>\n");
  free(man);
  write_strings(proj, "hello");
  char *layout = ccb_path_join(proj, "src/main/res/layout/activity_main.xml");
  write_text(layout,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  android:layout_width=\"match_parent\"\n"
    "  android:layout_height=\"match_parent\"\n"
    "  android:orientation=\"vertical\">\n"
    "  <TextView android:layout_width=\"wrap_content\"\n"
    "    android:layout_height=\"wrap_content\"\n"
    "    android:text=\"@string/app_name\" />\n"
    "</LinearLayout>\n");
  free(layout);
  write_kt(proj, NULL, NULL);
}

// Minimal AAR with a real classes.jar, Lib.answer() returning the
// given value (same shape as test_cache_outputs.c).
static void write_lib_aar_with_classes(const char *aar_path,
    const char *java_home, int answer) {
  char *src_dir = ccb_path_join(BASE, "libsrc/com/example/lib");
  mkdir_or_fail(src_dir);
  free(src_dir);
  ccb_str src;
  ccb_str_init(&src);
  ccb_str_append(&src,
    "package com.example.lib;\n"
    "public class Lib { public static int answer() { return ");
  ccb_str_appendf(&src, "%d; } }\n", answer);
  char *java = ccb_path_join(BASE, "libsrc/com/example/lib/Lib.java");
  write_text(java, src.buf);
  ccb_str_free(&src);
  char *classes_dir = ccb_path_join(BASE, "libclasses");
  mkdir_or_fail(classes_dir);
  char *javac = ccb_path_join3(java_home, "bin", "javac");
  char *args[] = { javac, "-d", classes_dir, java, NULL };
  ccb_proc_result r;
  CHECK(ccb_proc_run(args, NULL, NULL, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  ccb_proc_result_free(&r);
  char *class_file = ccb_path_join(classes_dir, "com/example/lib/Lib.class");
  char *class_data = NULL;
  size_t class_len = 0;
  CHECK(ccb_read_file(class_file, &class_data, &class_len) == CCB_OK);
  CHECK(class_len > 100);
  char *jar_path = ccb_path_join(BASE, "lib-classes.jar");
  ccb_zip_writer jw;
  CHECK(ccb_zip_writer_init(&jw, jar_path) == CCB_OK);
  CHECK(ccb_zip_writer_add_data(&jw, "com/example/lib/Lib.class",
    class_data, class_len) == CCB_OK);
  CHECK(ccb_zip_writer_finish(&jw) == CCB_OK);
  free(class_data);
  char *jar_data = NULL;
  size_t jar_len = 0;
  CHECK(ccb_read_file(jar_path, &jar_data, &jar_len) == CCB_OK);
  ccb_zip_writer w;
  CHECK(ccb_zip_writer_init(&w, aar_path) == CCB_OK);
  const char *manifest =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "    package=\"com.example.lib\" />\n";
  CHECK(ccb_zip_writer_add_data(&w, "AndroidManifest.xml",
    manifest, strlen(manifest)) == CCB_OK);
  CHECK(ccb_zip_writer_add_data(&w, "classes.jar", jar_data, jar_len) == CCB_OK);
  const char *values =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources><string name=\"lib_string\">lib</string></resources>\n";
  CHECK(ccb_zip_writer_add_data(&w, "res/values/values.xml",
    values, strlen(values)) == CCB_OK);
  CHECK(ccb_zip_writer_finish(&w) == CCB_OK);
  free(jar_data);
  free(jar_path);
  free(class_file);
  free(classes_dir);
  free(javac);
  free(java);
}

// ---------- build runners and output parsers ----------

// Run `ccb build [extra]` in proj, capturing stdout+stderr. Returns
// 1 when the process spawned; *exit_code and *combined (malloc'd)
// report the result. The caller frees *combined.
static int run_build_out(char *bin, const char *proj, char **env,
    const char *extra, int *exit_code, char **combined) {
  char *args[5];
  args[0] = bin;
  args[1] = "build";
  int n = 2;
  if (extra != NULL) {
    args[n++] = (char *)extra;
  }
  args[n] = NULL;
  ccb_proc_result r;
  if (ccb_proc_run(args, proj, env, &r) != CCB_OK) {
    return 0;
  }
  *exit_code = r.exit_code;
  ccb_str s;
  ccb_str_init(&s);
  if (r.out != NULL) {
    ccb_str_append(&s, r.out);
  }
  if (r.err != NULL) {
    ccb_str_append(&s, r.err);
  }
  *combined = ccb_str_steal(&s);
  ccb_proc_result_free(&r);
  return 1;
}

// Parse --json task events into done/skipped masks. Also asserts
// dexcheck always runs (it has no fingerprint by design).
static void parse_json_tasks(const char *out, unsigned *done,
    unsigned *skipped) {
  *done = 0;
  *skipped = 0;
  const char *p = out;
  while ((p = strstr(p, "\"event\":\"task\"")) != NULL) {
    const char *t = strstr(p, "\"task\":\"");
    const char *r = t != NULL ? strstr(t, "\"result\":\"") : NULL;
    if (t == NULL || r == NULL) {
      p += 1;
      continue;
    }
    t += strlen("\"task\":\"");
    const char *te = strchr(t, '"');
    r += strlen("\"result\":\"");
    const char *re = strchr(r, '"');
    if (te == NULL || re == NULL || (size_t)(te - t) >= 32 ||
        (size_t)(re - r) >= 16) {
      p += 1;
      continue;
    }
    char id[32];
    char res[16];
    memcpy(id, t, (size_t)(te - t));
    id[te - t] = '\0';
    memcpy(res, r, (size_t)(re - r));
    res[re - r] = '\0';
    unsigned bit = task_bit(id);
    if (bit == 0) {
      fprintf(stderr, "unknown task event id '%s'\n", id);
    }
    CHECK(bit != 0);
    if (strcmp(res, "done") == 0) {
      *done |= bit;
    } else if (strcmp(res, "skipped") == 0) {
      *skipped |= bit;
    } else {
      fprintf(stderr, "unknown task result '%s' for '%s'\n", res, id);
      CHECK(0);
    }
    p = re;
  }
  CHECK((*done & T_DEXCHECK) != 0);
}

static int run_json(char *bin, const char *proj, char **env,
    unsigned *done, unsigned *skipped) {
  int code = -1;
  char *out = NULL;
  if (!run_build_out(bin, proj, env, "--json", &code, &out)) {
    return 0;
  }
  if (code != 0) {
    fprintf(stderr, "build failed in %s:\n%s\n", proj, out);
    free(out);
    return 0;
  }
  parse_json_tasks(out, done, skipped);
  free(out);
  return 1;
}

// Parse --explain per-step lines ("<task>: run/skip (...)") into a
// run mask. Plan lines are indented and ignored.
static unsigned parse_explain_runs(const char *out) {
  unsigned run = 0;
  const char *p = out;
  while (*p != '\0') {
    const char *eol = strchr(p, '\n');
    size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
    if (len > 0 && p[0] != ' ' && p[0] != '\t') {
      const char *colon = memchr(p, ':', len);
      if (colon != NULL) {
        size_t idlen = (size_t)(colon - p);
        const char *after = colon + 1;
        int is_run = 0;
        int is_skip = 0;
        if (strncmp(after, " run (", 6) == 0) {
          is_run = 1;
        } else if (strncmp(after, " skip (", 7) == 0) {
          is_skip = 1;
        }
        if ((is_run || is_skip) && idlen > 0 && idlen < 32) {
          char id[32];
          memcpy(id, p, idlen);
          id[idlen] = '\0';
          unsigned bit = task_bit(id);
          if (bit == 0) {
            fprintf(stderr, "unknown explain task '%s'\n", id);
          }
          CHECK(bit != 0);
          if (is_run) {
            run |= bit;
          }
        }
      }
    }
    if (eol == NULL) {
      break;
    }
    p = eol + 1;
  }
  return run;
}

static int run_explain(char *bin, const char *proj, char **env,
    unsigned *run) {
  int code = -1;
  char *out = NULL;
  if (!run_build_out(bin, proj, env, "--explain", &code, &out)) {
    return 0;
  }
  if (code != 0) {
    fprintf(stderr, "explain failed in %s:\n%s\n", proj, out);
    free(out);
    return 0;
  }
  *run = parse_explain_runs(out);
  free(out);
  return 1;
}

// Explain is an upper bound on what reruns: every task the real
// build ran must have been predicted run. When exact is set, the
// predicted run set must equal the actual done set (only for
// scenarios where explain is known exact — it over-predicts
// downstream of link, R-3). Only tasks in track are compared:
// trivially-absent steps (deps with no AARs, native with no [ndk])
// report done in --json but skip in --explain.
static void check_plan(unsigned explain_run, unsigned actual_done,
    unsigned track, int exact) {
  unsigned unpredicted = actual_done & track & ~explain_run;
  for (size_t i = 0; i < CCB_ARRAY_LEN(TASK_BITS); i++) {
    if ((unpredicted & TASK_BITS[i].bit) != 0) {
      fprintf(stderr, "explain missed rerun of '%s'\n", TASK_BITS[i].name);
    }
  }
  CHECK(unpredicted == 0);
  if (exact) {
    unsigned want = actual_done & track;
    unsigned got = explain_run & track;
    for (size_t i = 0; i < CCB_ARRAY_LEN(TASK_BITS); i++) {
      if (((want ^ got) & TASK_BITS[i].bit) != 0) {
        fprintf(stderr, "explain mismatch on '%s': actual %s, predicted %s\n",
          TASK_BITS[i].name,
          (want & TASK_BITS[i].bit) != 0 ? "run" : "skip",
          (got & TASK_BITS[i].bit) != 0 ? "run" : "skip");
      }
    }
    CHECK(want == got);
  }
}

// Run --explain then a --json build, and check the plan against the
// build. Returns 1 when both runs succeeded.
static int explain_and_build(char *bin, const char *proj, char **env,
    unsigned track, int exact, unsigned *done, unsigned *skipped) {
  unsigned run = 0;
  if (!run_explain(bin, proj, env, &run)) {
    return 0;
  }
  if (!run_json(bin, proj, env, done, skipped)) {
    return 0;
  }
  check_plan(run, *done, track, exact);
  return 1;
}

static char *apk_sha(const char *apk) {
  char *h = ccb_cache_hash_file(apk);
  CHECK(h != NULL);
  return h;
}

static long file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;
  }
  return (long)st.st_size;
}

// Backup a file's bytes (malloc'd, caller frees), or NULL on error.
static char *backup_file(const char *path, size_t *len_out) {
  char *data = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &data, &len) != CCB_OK) {
    return NULL;
  }
  *len_out = len;
  return data;
}

static void restore_file(const char *path, const char *data, size_t len) {
  CHECK(ccb_write_file(path, data, len) == CCB_OK);
}

// Corrupt a file in place with junk too short to be valid input.
static void truncate_file(const char *path) {
  CHECK(ccb_write_file(path, "CCB-TRUNCATED", 13) == CCB_OK);
  CHECK(file_size(path) == 13);
}

// Assert a human-mode build fails with the given code, never
// reporting "up to date" — i.e. a corrupt output fails loudly
// downstream instead of shipping silently.
static void check_loud_failure(char *bin, const char *proj, char **env,
    const char *code, int exit_want) {
  int exit_code = -1;
  char *out = NULL;
  CHECK(run_build_out(bin, proj, env, NULL, &exit_code, &out));
  CHECK(exit_code == exit_want);
  CHECK(ccb_test_contains(out, code));
  CHECK(!ccb_test_contains(out, "up to date:"));
  free(out);
}

// Assert a no-op --json build: every tracked task skipped.
static void check_noop(char *bin, const char *proj, char **env,
    unsigned track) {
  unsigned done = 0;
  unsigned skipped = 0;
  CHECK(run_json(bin, proj, env, &done, &skipped));
  CHECK((done & track) == 0);
  CHECK((skipped & track) == track);
}

// Assert a recovery build: after a corrupt output is restored, the
// build succeeds and reruns exactly the affected step(s) — the
// failure itself leaves the tree self-healing (a deleted R.java, a
// recorded corrupt input hash, a corrupt signed artifact), and the
// step that owns the output regenerates it. The following build must
// then be a full no-op (checked by the caller).
static void check_recovery(char *bin, const char *proj, char **env,
    unsigned track, unsigned want_done) {
  unsigned done = 0;
  unsigned skipped = 0;
  CHECK(run_json(bin, proj, env, &done, &skipped));
  CHECK((done & track) == want_done);
}

// Generate a debug keystore with two aliases (storepass/keypass
// "android", the debug signing passwords) and return its path.
static void gen_two_alias_keystore(const char *java_home) {
  char *keytool = ccb_path_join(java_home, "bin/keytool");
  const char *aliases[2] = { "AliasA", "AliasB" };
  const char *dns[2] = { "CN=AliasA", "CN=AliasB" };
  for (int i = 0; i < 2; i++) {
    char *args[] = { keytool, "-genkeypair", "-keystore", KEYSTORE,
      "-storepass", "android", "-keypass", "android", "-alias",
      (char *)aliases[i], "-keyalg", "RSA", "-keysize", "2048",
      "-validity", "10000", "-dname", (char *)dns[i], "-noprompt", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, NULL, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    ccb_proc_result_free(&r);
  }
  free(keytool);
}

// The "CN=..." DN of the APK's signer, via apksigner (a shell script
// that execs `java`, so java_home/bin must be on PATH).
static void check_cert_dn(const char *sdk_home, const char *bt_ver,
    const char *java_home, const char *apk, const char *want_dn) {
  ccb_str p;
  ccb_str_init(&p);
  ccb_str_append(&p, sdk_home);
  ccb_str_append(&p, "/build-tools/");
  ccb_str_append(&p, bt_ver);
  ccb_str_append(&p, "/apksigner");
  ccb_str path_env;
  ccb_str_init(&path_env);
  ccb_str_append(&path_env, "PATH=");
  ccb_str_append(&path_env, java_home);
  ccb_str_append(&path_env, "/bin:");
  const char *old_path = getenv("PATH");
  ccb_str_append(&path_env, old_path != NULL ? old_path : "/usr/bin:/bin");
  char *args[] = { p.buf, "verify", "--print-certs", (char *)apk, NULL };
  char *env[] = { path_env.buf, NULL };
  ccb_proc_result r;
  CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  ccb_str both;
  ccb_str_init(&both);
  if (r.out != NULL) {
    ccb_str_append(&both, r.out);
  }
  if (r.err != NULL) {
    ccb_str_append(&both, r.err);
  }
  CHECK(ccb_test_contains(both.buf, want_dn));
  ccb_str_free(&both);
  ccb_proc_result_free(&r);
  ccb_str_free(&path_env);
  ccb_str_free(&p);
}

// ---------- scenarios ----------

static void test_hello(char *bin, char **env, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home, const char *java_home) {
  cfg_opts o;
  memset(&o, 0, sizeof(o));
  o.version_code = 1;
  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  char *apk = ccb_path_join(HELLO_PROJ, "build/outputs/debug/app.apk");
  char *compiled_res = ccb_path_join(HELLO_PROJ,
    "build/intermediates/compiled_res/app.zip");
  char *linked = ccb_path_join(HELLO_PROJ, "build/intermediates/linked.ap_");
  char *aligned = ccb_path_join(HELLO_PROJ,
    "build/intermediates/app-aligned.apk");
  char *cache = ccb_path_join(HELLO_PROJ, "build/ccb-cache.json");
  unsigned done = 0;
  unsigned skipped = 0;

  // Cold build: all 8 core tasks run.
  CHECK(run_json(bin, HELLO_PROJ, env, &done, &skipped));
  CHECK((done & CORE8) == CORE8);
  CHECK(ccb_path_exists(apk));
  char *sha_cold = apk_sha(apk);

  // No-op: all 8 skip, APK byte-identical, explain exact.
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 1, &done, &skipped));
  CHECK((done & CORE8) == 0);
  CHECK((skipped & CORE8) == CORE8);
  char *sha_noop = apk_sha(apk);
  CHECK_STR_EQ(sha_cold, sha_noop);
  free(sha_noop);

  // Comment-only .kt change: only compile-code reruns (the dex is
  // byte-identical, so dex and everything downstream skip — explain
  // over-predicts dex here, so the plan check is subset-only).
  write_kt(HELLO_PROJ, "cache regression probe", NULL);
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 0, &done, &skipped));
  CHECK((done & CORE8) == T_CODE);
  char *sha_comment = apk_sha(apk);
  CHECK_STR_EQ(sha_cold, sha_comment);
  free(sha_comment);

  // Real code change: compile-code, dex, and the packaging tail.
  write_kt(HELLO_PROJ, "cache regression probe", "probeField = 42");
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 1, &done, &skipped));
  CHECK((done & CORE8) ==
    (T_CODE | T_DEX | T_PACKAGE | T_ALIGN | T_SIGN | T_VERIFY));

  // version_code change: link and the packaging tail only.
  o.version_code = 2;
  write_config(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 0, &done, &skipped));
  CHECK((done & CORE8) ==
    (T_LINK | T_PACKAGE | T_ALIGN | T_SIGN | T_VERIFY));

  // strings.xml value change: compile-res, link, packaging tail.
  write_strings(HELLO_PROJ, "hello2");
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 0, &done, &skipped));
  CHECK((done & CORE8) == (T_COMPILE_RES | T_LINK | T_PACKAGE |
    T_ALIGN | T_SIGN | T_VERIFY));

  // Truncated compiled-res zip: aapt2 link fails loudly (CCB-E3003),
  // never ships. The failed link deletes R.java, so restoring the
  // zip reruns link once, then the build is a no-op again.
  size_t len = 0;
  char *bak = backup_file(compiled_res, &len);
  CHECK(bak != NULL);
  truncate_file(compiled_res);
  check_loud_failure(bin, HELLO_PROJ, env, "CCB-E3003", 1);
  restore_file(compiled_res, bak, len);
  free(bak);
  check_recovery(bin, HELLO_PROJ, env, CORE8, T_LINK);
  check_noop(bin, HELLO_PROJ, env, CORE8);

  // Truncated linked.ap_: package fails loudly (CCB-E3007).
  // Restoring it reruns package once, then the build is a no-op.
  bak = backup_file(linked, &len);
  CHECK(bak != NULL);
  truncate_file(linked);
  check_loud_failure(bin, HELLO_PROJ, env, "CCB-E3007", 1);
  restore_file(linked, bak, len);
  free(bak);
  check_recovery(bin, HELLO_PROJ, env, CORE8, T_PACKAGE);
  check_noop(bin, HELLO_PROJ, env, CORE8);

  // Truncated aligned APK: sign fails loudly (CCB-E3009, exit 4).
  // The failed signer leaves the final APK empty (0 bytes); restoring
  // the aligned APK must re-sign (the empty output fails sign's
  // ZIP-magic check) and restore the byte-identical APK. Before the
  // Step 3 sign output validation fix, sign skipped over the empty
  // file and every build failed at verify forever.
  char *sha_before = apk_sha(apk);
  bak = backup_file(aligned, &len);
  CHECK(bak != NULL);
  truncate_file(aligned);
  check_loud_failure(bin, HELLO_PROJ, env, "CCB-E3009", 4);
  restore_file(aligned, bak, len);
  free(bak);
  check_recovery(bin, HELLO_PROJ, env, CORE8, T_SIGN);
  char *sha_after = apk_sha(apk);
  CHECK_STR_EQ(sha_before, sha_after);
  free(sha_before);
  free(sha_after);
  check_noop(bin, HELLO_PROJ, env, CORE8);

  // Truncated final APK: the signed artifact is sign's own output,
  // so the build self-heals instead of failing — sign re-runs (the
  // corrupt output fails its ZIP-magic check) and restores the
  // byte-identical APK. Same regression pin as above: before the
  // fix this failed loudly at verify (CCB-E3010) on every build,
  // forever, with no way to recover short of deleting the APK.
  sha_before = apk_sha(apk);
  truncate_file(apk);
  check_recovery(bin, HELLO_PROJ, env, CORE8, T_SIGN);
  sha_after = apk_sha(apk);
  CHECK_STR_EQ(sha_before, sha_after);
  free(sha_before);
  free(sha_after);
  check_noop(bin, HELLO_PROJ, env, CORE8);

  // Torn cache file: falls back to rebuilding everything.
  bak = backup_file(cache, &len);
  CHECK(bak != NULL && len > 40);
  CHECK(ccb_write_file(cache, bak, 40) == CCB_OK);
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 1, &done, &skipped));
  CHECK((done & CORE8) == CORE8);
  free(bak);

  // Toolchain change (aapt2 at a different path): the tool
  // fingerprint invalidates every task.
  ccb_str aapt2_real;
  ccb_str_init(&aapt2_real);
  ccb_str_append(&aapt2_real, sdk_home);
  ccb_str_append(&aapt2_real, "/build-tools/");
  ccb_str_append(&aapt2_real, bt_ver);
  ccb_str_append(&aapt2_real, "/aapt2");
  char *aapt2_link = ccb_path_join(HELLO_PROJ, "aapt2-link");
  CHECK(symlink(aapt2_real.buf, aapt2_link) == 0);
  o.aapt2 = aapt2_link;
  write_config(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 1, &done, &skipped));
  CHECK((done & CORE8) == CORE8);
  // Restoring the original path is itself a tool change, so it
  // reruns everything once more — the cache wipe is per-change, not
  // per-path — and only the build after that is a no-op.
  o.aapt2 = NULL;
  write_config(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 1, &done, &skipped));
  CHECK((done & CORE8) == CORE8);
  check_noop(bin, HELLO_PROJ, env, CORE8);
  ccb_str_free(&aapt2_real);
  free(aapt2_link);

  // Debug keystore alias switch: only sign and verify rerun, and
  // the APK is really signed with the new key (this is the
  // regression pin for the Step 3 sign-fingerprint fix — before the
  // fix, sign skipped and the old signature shipped).
  gen_two_alias_keystore(java_home);
  o.sign_store = KEYSTORE;
  o.sign_alias = "AliasA";
  write_config(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  CHECK(run_json(bin, HELLO_PROJ, env, &done, &skipped));
  CHECK((done & CORE8) == (T_SIGN | T_VERIFY));
  char *sha_a = apk_sha(apk);
  check_cert_dn(sdk_home, bt_ver, java_home, apk, "CN=AliasA");
  o.sign_alias = "AliasB";
  write_config(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  CHECK(explain_and_build(bin, HELLO_PROJ, env, CORE8, 1, &done, &skipped));
  CHECK((done & CORE8) == (T_SIGN | T_VERIFY));
  char *sha_b = apk_sha(apk);
  CHECK(strcmp(sha_a, sha_b) != 0);
  check_cert_dn(sdk_home, bt_ver, java_home, apk, "CN=AliasB");
  free(sha_a);
  free(sha_b);
  free(sha_cold);

  free(apk);
  free(compiled_res);
  free(linked);
  free(aligned);
  free(cache);
}

static void test_aar(char *bin, char **env, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home, const char *java_home) {
  write_lib_aar_with_classes(LIB_AAR, java_home, 42);
  cfg_opts o;
  memset(&o, 0, sizeof(o));
  o.version_code = 1;
  o.aar_path = LIB_AAR;
  write_hello_project(AAR_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  char *apk = ccb_path_join(AAR_PROJ, "build/outputs/debug/app.apk");
  char *aar_jar = ccb_path_join(AAR_PROJ,
    "build/intermediates/aar/lib/classes.jar");
  unsigned track = CORE8 | T_DEPS | T_MERGE;
  unsigned done = 0;
  unsigned skipped = 0;

  // Cold build: deps and merge-manifest run, all 8 core tasks run.
  CHECK(run_json(bin, AAR_PROJ, env, &done, &skipped));
  CHECK((done & track) == track);
  CHECK(ccb_path_exists(aar_jar));
  CHECK(ccb_path_exists(apk));

  // No-op: everything tracked skips, explain exact.
  CHECK(explain_and_build(bin, AAR_PROJ, env, track, 1, &done, &skipped));
  CHECK((done & track) == 0);
  CHECK((skipped & track) == track);

  // Truncated extracted classes.jar: javac fails loudly
  // (CCB-E3005), never ships. The failed compile wiped the classes
  // dir and never wrote R.class, so restoring the jar reruns
  // compile-code once (the missing R.class fails its output check)
  // and restores the byte-identical APK; then the build is a no-op.
  // Before the Step 3 R.class fix, compile-code skipped and every
  // build after shipped an APK without R.class.
  size_t len = 0;
  char *bak = backup_file(aar_jar, &len);
  CHECK(bak != NULL);
  truncate_file(aar_jar);
  check_loud_failure(bin, AAR_PROJ, env, "CCB-E3005", 1);
  restore_file(aar_jar, bak, len);
  free(bak);
  check_recovery(bin, AAR_PROJ, env, track, T_CODE);
  check_noop(bin, AAR_PROJ, env, track);

  // AAR content change (Lib.answer 42 -> 43): deps reruns, the
  // manifest merge and resource steps skip (the AAR manifest and
  // resources are unchanged), compile-code and the tail rerun.
  // No --explain assertion here: explain under-predicts this case
  // (it evaluates the classpath from the still-old extracted jar,
  // so it says compile-code/dex/package/align skip while the build
  // runs them). That staleness is R-3, owned by Step 4's --explain
  // fix; the build's run set below is the regression assertion.
  write_lib_aar_with_classes(LIB_AAR, java_home, 43);
  CHECK(run_json(bin, AAR_PROJ, env, &done, &skipped));
  CHECK((done & track) == (T_DEPS | T_CODE | T_DEX | T_PACKAGE |
    T_ALIGN | T_SIGN | T_VERIFY));
  CHECK((skipped & (T_MERGE | T_COMPILE_RES | T_LINK)) ==
    (T_MERGE | T_COMPILE_RES | T_LINK));

  free(apk);
  free(aar_jar);
}

static void test_cmake(char *bin, char **env, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home,
    const char *ndk_version) {
  cfg_opts o;
  memset(&o, 0, sizeof(o));
  o.version_code = 1;
  o.ndk_version = ndk_version;
  o.cmake_lists = "cpp/CMakeLists.txt";
  write_hello_project(CMK_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  char *cpp_dir = ccb_path_join(CMK_PROJ, "cpp");
  mkdir_or_fail(cpp_dir);
  free(cpp_dir);
  char *lists = ccb_path_join(CMK_PROJ, "cpp/CMakeLists.txt");
  write_text(lists,
    "cmake_minimum_required(VERSION 3.22)\n"
    "project(nativelib)\n"
    "add_library(nativelib SHARED native.c)\n");
  free(lists);
  char *native_c = ccb_path_join(CMK_PROJ, "cpp/native.c");
  write_text(native_c,
    "int nativelib_value(void) {\n"
    "  return 42;\n"
    "}\n");
  free(native_c);
  char *so = ccb_path_join(CMK_PROJ,
    "build/intermediates/jni/arm64-v8a/libnativelib.so");
  char *apk = ccb_path_join(CMK_PROJ, "build/outputs/debug/app.apk");
  unsigned track = CORE8 | T_NATIVE;
  unsigned done = 0;
  unsigned skipped = 0;

  // Cold build: native runs, the .so is packaged.
  CHECK(run_json(bin, CMK_PROJ, env, &done, &skipped));
  CHECK((done & T_NATIVE) != 0);
  CHECK(ccb_path_exists(so));
  CHECK(zip_has(apk, "lib/arm64-v8a/libnativelib.so"));

  // No-op: native skips, explain exact.
  CHECK(explain_and_build(bin, CMK_PROJ, env, track, 1, &done, &skipped));
  CHECK((done & track) == 0);
  CHECK((skipped & track) == track);

  // Adding a [cmake] args entry: native reruns (configure inputs
  // changed — the regression pin for the Step 3 cmake-fingerprint
  // fix). Whether the tail reruns depends on the rebuilt .so bytes,
  // so the plan check is subset-only here.
  o.cmake_args = "-DFOO=bar";
  write_config(CMK_PROJ, sdk_home, bt_ver, kotlin_home, &o);
  CHECK(explain_and_build(bin, CMK_PROJ, env, track, 0, &done, &skipped));
  CHECK((done & T_NATIVE) != 0);
  CHECK(ccb_path_exists(so));

  // No-op again: native skips.
  check_noop(bin, CMK_PROJ, env, track);

  free(so);
  free(apk);
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "cachereg");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping cache-regression checks\n");
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }
  char *sdk_home = env_or_home("ANDROID_HOME", "Android/Sdk");
  if (sdk_home == NULL) {
    sdk_home = env_or_home("ANDROID_HOME", "android-sdk");
  }
  char *java_home = env_or_home("JAVA_HOME", "jdk17");
  char *kotlin_home = env_or_home("KOTLIN_HOME", "kotlin-2.4.21/kotlinc");
  char *bt_ver = sdk_home != NULL ? ccb_test_find_build_tools(sdk_home) : NULL;
  char *kotlinc = kotlin_home != NULL ?
    ccb_path_join(kotlin_home, "bin/kotlinc") : NULL;
  if (bt_ver == NULL || java_home == NULL || kotlin_home == NULL ||
      kotlinc == NULL || access(kotlinc, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping cache-regression checks\n");
    free(bin);
    free(sdk_home);
    free(java_home);
    free(kotlin_home);
    free(bt_ver);
    free(kotlinc);
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }
  free(kotlinc);

  ccb_str sdk_env, java_env, kot_env;
  ccb_str_init(&sdk_env);
  ccb_str_init(&java_env);
  ccb_str_init(&kot_env);
  ccb_str_append(&sdk_env, "ANDROID_HOME=");
  ccb_str_append(&sdk_env, sdk_home);
  ccb_str_append(&java_env, "JAVA_HOME=");
  ccb_str_append(&java_env, java_home);
  ccb_str_append(&kot_env, "KOTLIN_HOME=");
  ccb_str_append(&kot_env, kotlin_home);
  char *env[] = { sdk_env.buf, java_env.buf, kot_env.buf, NULL };

  ccb_remove_recursive(BASE);

  test_hello(bin, env, sdk_home, bt_ver, kotlin_home, java_home);
  test_aar(bin, env, sdk_home, bt_ver, kotlin_home, java_home);

  char *ndk_version = find_ndk_version(sdk_home);
  char *cmake = ccb_sdk_find_on_path("cmake");
  char *make = ccb_sdk_find_on_path("make");
  if (ndk_version != NULL && cmake != NULL && make != NULL) {
    test_cmake(bin, env, sdk_home, bt_ver, kotlin_home, ndk_version);
  } else {
    fprintf(stderr, "note: no cmake/make/NDK, skipping cmake checks\n");
  }
  free(ndk_version);
  free(cmake);
  free(make);

  free(bin);
  free(sdk_home);
  free(java_home);
  free(kotlin_home);
  free(bt_ver);
  ccb_str_free(&sdk_env);
  ccb_str_free(&java_env);
  ccb_str_free(&kot_env);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
