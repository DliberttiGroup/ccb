// ccb_sdk implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_proc.h"
#include "ccb_sdk.h"
#include <ctype.h>
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

void ccb_sdk_free(ccb_sdk *sdk) {
  if (sdk == NULL) {
    return;
  }
  free(sdk->sdk_home);
  free(sdk->build_tools_dir);
  free(sdk->build_tools_version);
  free(sdk->aapt2);
  free(sdk->d8);
  free(sdk->apksigner);
  free(sdk->zipalign);
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

// Parse kotlinc -version stderr: "info: kotlinc-jvm 2.4.21 (JRE ...)".
static char *ccb_parse_kotlin_version(const char *text) {
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
  if (!ccb_path_is_dir(s->build_tools_dir)) {
    ccb_sdk_diagf(diags, "CCB-E2002", "build-tools not installed", s->build_tools_dir);
  } else {
    static const char *tools[] = { "aapt2", "d8", "apksigner", "zipalign" };
    char **slots[] = { &s->aapt2, &s->d8, &s->apksigner, &s->zipalign };
    for (int i = 0; i < 4; i++) {
      char *p = ccb_path_join(s->build_tools_dir, tools[i]);
      if (access(p, X_OK) != 0) {
        ccb_sdk_diagf(diags, "CCB-E2009", "tool missing or not executable", p);
        free(p);
      } else {
        *slots[i] = p;
      }
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
    if (s->kotlinc == NULL) {
      ccb_sdk_diag(diags, "CCB-E2004", "kotlinc not found: set [sdk] kotlin_home in ccb.toml or KOTLIN_HOME");
    } else {
      char *argv[] = { s->kotlinc, "-version", NULL };
      ccb_proc_result r;
      if (ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK) {
        s->kotlin_version = ccb_parse_kotlin_version(r.err);
        if (s->kotlin_version == NULL || s->kotlin_version[0] != '2') {
          ccb_sdk_diag(diags, "CCB-E2006", "kotlinc must be 2.x");
        }
        ccb_proc_result_free(&r);
      } else {
        ccb_sdk_diag(diags, "CCB-E2004", "could not run kotlinc -version");
      }
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
