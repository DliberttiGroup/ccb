// Tests for [sdk] aapt2/zipalign overrides: precedence, validation,
// doctor/dry-run reporting, and direct invocation of a resolved tool.
// The fixture SDK is entirely fake (executable shell scripts), so no
// real Android SDK is required. The ccb binary is used only for the
// doctor/dry-run output checks: argv[1] when given, else a sibling
// `ccb` next to this test executable (the ctest layout).
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_diag.h"
#include "ccb_config.h"
#include "ccb_sdk.h"
#include "ccb_proc.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define SDK B("sdk")
#define BT B("sdk/build-tools/35.0.0")
#define PROJ B("proj")

static void write_text(const char *path, const char *text) {
  CHECK(ccb_write_file(path, text, strlen(text)) == CCB_OK);
}

static void write_script(const char *path, const char *body) {
  write_text(path, body);
  CHECK(chmod(path, 0755) == 0);
}

static char *saved_env(const char *name) {
  const char *v = getenv(name);
  return v == NULL ? NULL : ccb_xstrdup(v);
}

static void restore_env(const char *name, char *saved) {
  if (saved != NULL) {
    setenv(name, saved, 1);
  } else {
    unsetenv(name);
  }
}

static void set_env(const char *name, const char *value) {
  if (value == NULL) {
    unsetenv(name);
  } else {
    setenv(name, value, 1);
  }
}

// Write proj/ccb.toml. sdk_extra is verbatim extra [sdk] lines (or
// NULL); build_tools overrides the default version when non-NULL.
static void write_config(const char *sdk_extra, const char *build_tools) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "ccb_version = 1\n"
    "[project]\n"
    "name = \"hello\"\n"
    "package = \"com.example.hello\"\n"
    "version = \"1.0\"\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\n"
    "kotlin = \"src/main/kotlin\"\n"
    "[sdk]\n"
    "home = \"");
  ccb_str_append(&s, B("sdk"));
  ccb_str_append(&s, "\"\n");
  ccb_str_append(&s, "build_tools = \"");
  ccb_str_append(&s, build_tools == NULL ? "35.0.0" : build_tools);
  ccb_str_append(&s, "\"\n");
  if (sdk_extra != NULL) {
    ccb_str_append(&s, sdk_extra);
  }
  write_text(B("proj/ccb.toml"), s.buf);
  ccb_str_free(&s);
}

static int has_code(ccb_diag_list *d, const char *code) {
  for (size_t i = 0; i < d->count; i++) {
    if (strcmp(d->items[i].code, code) == 0) {
      return 1;
    }
  }
  return 0;
}

static int diag_contains(ccb_diag_list *d, const char *code, const char *needle) {
  for (size_t i = 0; i < d->count; i++) {
    if (strcmp(d->items[i].code, code) == 0 && d->items[i].message != NULL &&
        strstr(d->items[i].message, needle) != NULL) {
      return 1;
    }
  }
  return 0;
}

// Load the project config and resolve the SDK. Returns the resolve
// status; *out_sdk is NULL on failure.
static ccb_status resolve_proj(ccb_sdk **out_sdk, ccb_diag_list *diags) {
  *out_sdk = NULL;
  ccb_config *c = NULL;
  if (ccb_config_load(B("proj/ccb.toml"), NULL, &c, diags) != CCB_OK) {
    return CCB_ERR_CONFIG;
  }
  ccb_status rc = ccb_sdk_resolve(c, out_sdk, diags);
  ccb_config_free(c);
  return rc;
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

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "sdk_tools");
  char *save_java = saved_env("JAVA_HOME");
  char *save_kotlin = saved_env("KOTLIN_HOME");
  char *save_aapt2 = saved_env("CCB_AAPT2");
  char *save_zipalign = saved_env("CCB_ZIPALIGN");

  ccb_remove_recursive(BASE);
  CHECK(ccb_mkdir_p(BT) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk/platforms/android-35")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("java/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("kotlin/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("reltools")) == CCB_OK);
  // Fake default build-tools.
  write_script(B("sdk/build-tools/35.0.0/aapt2"), "#!/bin/sh\nexit 0\n");
  write_script(B("sdk/build-tools/35.0.0/d8"), "#!/bin/sh\nexit 0\n");
  write_script(B("sdk/build-tools/35.0.0/apksigner"), "#!/bin/sh\nexit 0\n");
  write_script(B("sdk/build-tools/35.0.0/zipalign"), "#!/bin/sh\nexit 0\n");
  write_text(B("sdk/platforms/android-35/android.jar"), "fake-jar");
  // Fake java/kotlinc answering -version in the expected formats.
  write_script(B("java/bin/java"),
    "#!/bin/sh\necho 'openjdk version \"17.0.1\"' >&2\n");
  write_script(B("java/bin/javac"), "#!/bin/sh\nexit 0\n");
  write_script(B("kotlin/bin/kotlinc"),
    "#!/bin/sh\necho 'info: kotlinc-jvm 2.4.21 (JRE 17)' >&2\n");
  write_text(B("proj/src/main/AndroidManifest.xml"),
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
  write_text(B("proj/src/main/res/values/strings.xml"),
    "<resources><string name=\"app_name\">hello</string></resources>\n");
  CHECK(ccb_mkdir_p(B("proj/src/main/kotlin/com/example/hello")) == CCB_OK);
  write_text(B("proj/src/main/kotlin/com/example/hello/MainActivity.kt"),
    "package com.example.hello\nclass MainActivity\n");
  // Override candidates.
  write_script(B("cfg-aapt2"), "#!/bin/sh\nexit 0\n");
  write_script(B("cfg-zipalign"), "#!/bin/sh\nexit 0\n");
  write_script(B("env-aapt2"), "#!/bin/sh\nexit 0\n");
  write_script(B("env-zipalign"), "#!/bin/sh\nexit 0\n");
  write_script(B("reltools/my-aapt2"), "#!/bin/sh\nexit 0\n");
  write_text(B("nonexec-aapt2"), "#!/bin/sh\nexit 0\n");
  CHECK(chmod(B("nonexec-aapt2"), 0644) == 0);
  // Logging script for the direct-invocation proof.
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "#!/bin/sh\necho invoked \"$@\" >> ");
    ccb_str_append(&s, B("invocations.log\n"));
    write_script(B("log-aapt2"), s.buf);
    ccb_str_free(&s);
  }

  set_env("JAVA_HOME", B("java"));
  set_env("KOTLIN_HOME", B("kotlin"));

  // Default: neither config nor env set.
  {
    write_config(NULL, NULL);
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    CHECK(sdk != NULL);
    if (sdk != NULL) {
      CHECK_STR_EQ(sdk->aapt2, B("sdk/build-tools/35.0.0/aapt2"));
      CHECK_STR_EQ(sdk->aapt2_source, "default");
      CHECK_STR_EQ(sdk->zipalign, B("sdk/build-tools/35.0.0/zipalign"));
      CHECK_STR_EQ(sdk->zipalign_source, "default");
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
  }

  // Env beats default.
  {
    write_config(NULL, NULL);
    set_env("CCB_AAPT2", B("env-aapt2"));
    set_env("CCB_ZIPALIGN", B("env-zipalign"));
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    if (sdk != NULL) {
      CHECK_STR_EQ(sdk->aapt2, B("env-aapt2"));
      CHECK_STR_EQ(sdk->aapt2_source, "env");
      CHECK_STR_EQ(sdk->zipalign, B("env-zipalign"));
      CHECK_STR_EQ(sdk->zipalign_source, "env");
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
  }

  // Config beats env.
  {
    ccb_str extra;
    ccb_str_init(&extra);
    ccb_str_append(&extra, "aapt2 = \"");
    ccb_str_append(&extra, B("cfg-aapt2\"\n"));
    ccb_str_append(&extra, "zipalign = \"");
    ccb_str_append(&extra, B("cfg-zipalign\"\n"));
    write_config(extra.buf, NULL);
    ccb_str_free(&extra);
    set_env("CCB_AAPT2", B("env-aapt2"));
    set_env("CCB_ZIPALIGN", B("env-zipalign"));
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    if (sdk != NULL) {
      CHECK_STR_EQ(sdk->aapt2, B("cfg-aapt2"));
      CHECK_STR_EQ(sdk->aapt2_source, "config");
      CHECK_STR_EQ(sdk->zipalign, B("cfg-zipalign"));
      CHECK_STR_EQ(sdk->zipalign_source, "config");
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
  }

  // Empty env values count as unset.
  {
    write_config(NULL, NULL);
    set_env("CCB_AAPT2", "");
    set_env("CCB_ZIPALIGN", "");
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    if (sdk != NULL) {
      CHECK_STR_EQ(sdk->aapt2, B("sdk/build-tools/35.0.0/aapt2"));
      CHECK_STR_EQ(sdk->aapt2_source, "default");
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
  }

  // Missing config override: CCB-E2009 naming source + path.
  {
    char cfg_extra[512];
    snprintf(cfg_extra, sizeof cfg_extra, "aapt2 = \"%s\"\n", B("no-such-aapt2"));
    write_config(cfg_extra, NULL);
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_ERR_TOOLCHAIN);
    CHECK(sdk == NULL);
    CHECK(has_code(&diags, "CCB-E2009"));
    CHECK(diag_contains(&diags, "CCB-E2009", "[sdk] aapt2"));
    CHECK(diag_contains(&diags, "CCB-E2009", B("no-such-aapt2")));
    ccb_diag_list_free(&diags);
  }

  // Missing env override: CCB-E2009 naming the env var + path.
  {
    write_config(NULL, NULL);
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", B("no-such-zipalign"));
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_ERR_TOOLCHAIN);
    CHECK(diag_contains(&diags, "CCB-E2009", "CCB_ZIPALIGN"));
    CHECK(diag_contains(&diags, "CCB-E2009", B("no-such-zipalign")));
    ccb_diag_list_free(&diags);
  }

  // Non-executable override file: same failure, no silent fallback.
  {
    char cfg_extra[512];
    snprintf(cfg_extra, sizeof cfg_extra, "aapt2 = \"%s\"\n", B("nonexec-aapt2"));
    write_config(cfg_extra, NULL);
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_ERR_TOOLCHAIN);
    CHECK(diag_contains(&diags, "CCB-E2009", "[sdk] aapt2"));
    CHECK(diag_contains(&diags, "CCB-E2009", B("nonexec-aapt2")));
    ccb_diag_list_free(&diags);
  }

  // A broken override is not masked by a missing build-tools dir:
  // both CCB-E2009 (override) and CCB-E2002 (build-tools) appear.
  {
    char cfg_extra[512];
    snprintf(cfg_extra, sizeof cfg_extra, "aapt2 = \"%s\"\n", B("no-such-aapt2"));
    write_config(cfg_extra, "99.0.0");
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_ERR_TOOLCHAIN);
    CHECK(has_code(&diags, "CCB-E2009"));
    CHECK(has_code(&diags, "CCB-E2002"));
    CHECK(diag_contains(&diags, "CCB-E2009", "[sdk] aapt2"));
    ccb_diag_list_free(&diags);
  }

  // Non-string config value: the existing CCB-E1007 type error.
  {
    write_config("aapt2 = 42\n", NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_config *c = NULL;
    CHECK(ccb_config_load(B("proj/ccb.toml"), NULL, &c, &diags) == CCB_ERR_CONFIG);
    CHECK(c == NULL);
    CHECK(has_code(&diags, "CCB-E1007"));
    ccb_diag_list_free(&diags);
  }

  // Relative override paths are used exactly as given (cwd-relative).
  {
    write_config("aapt2 = \"reltools/my-aapt2\"\n", NULL);
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", NULL);
    char cwd[4096];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    CHECK(chdir(BASE) == 0);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    if (sdk != NULL) {
      CHECK_STR_EQ(sdk->aapt2, "reltools/my-aapt2");
      CHECK_STR_EQ(sdk->aapt2_source, "config");
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
    CHECK(chdir(cwd) == 0);
  }

  // Direct invocation proof: the resolved override path really runs.
  {
    write_config(NULL, NULL);
    set_env("CCB_AAPT2", B("log-aapt2"));
    set_env("CCB_ZIPALIGN", NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    if (sdk != NULL) {
      char *args[] = { sdk->aapt2, "compile", "--dir", "x", NULL };
      ccb_proc_result r;
      CHECK(ccb_proc_run(args, NULL, NULL, &r) == CCB_OK);
      CHECK(r.exit_code == 0);
      ccb_proc_result_free(&r);
      char *log = NULL;
      size_t log_len = 0;
      CHECK(ccb_read_file(B("invocations.log"), &log, &log_len) == CCB_OK);
      if (log != NULL) {
        CHECK(strstr(log, "invoked compile --dir x") != NULL);
        free(log);
      }
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
  }

  // Doctor + dry-run reporting through the real binary.
  {
    char *bin = find_binary(argc, argv);
    CHECK(bin != NULL);
    if (bin != NULL) {
      // Restore the caller's env for the child tests; the child env
      // is set explicitly per invocation via env_extra below.
      restore_env("CCB_AAPT2", save_aapt2);
      restore_env("CCB_ZIPALIGN", save_zipalign);
      write_config(NULL, NULL);
      ccb_str jh, kh, ea, ez;
      ccb_str_init(&jh);
      ccb_str_init(&kh);
      ccb_str_init(&ea);
      ccb_str_init(&ez);
      ccb_str_append(&jh, "JAVA_HOME=");
      ccb_str_append(&jh, B("java"));
      ccb_str_append(&kh, "KOTLIN_HOME=");
      ccb_str_append(&kh, B("kotlin"));
      ccb_str_append(&ea, "CCB_AAPT2=");
      ccb_str_append(&ea, B("env-aapt2"));
      ccb_str_append(&ez, "CCB_ZIPALIGN=");
      ccb_str_append(&ez, B("env-zipalign"));
      // Doctor, text + JSON, with env overrides marked as env.
      {
        char *args[] = { bin, "--config", B("proj/ccb.toml"), "doctor", NULL };
        char *env[] = { jh.buf, kh.buf, ea.buf, ez.buf, NULL };
        ccb_proc_result r;
        CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
        CHECK(r.exit_code == 0);
        CHECK(strstr(r.out, B("env-aapt2")) != NULL);
        CHECK(strstr(r.out, "(env)") != NULL);
        CHECK(strstr(r.out, B("env-zipalign")) != NULL);
        ccb_proc_result_free(&r);
        char *jargs[] = { bin, "--json", "--config", B("proj/ccb.toml"), "doctor", NULL };
        CHECK(ccb_proc_run(jargs, NULL, env, &r) == CCB_OK);
        CHECK(r.exit_code == 0);
        CHECK(strstr(r.out, "\"aapt2_source\":\"env\"") != NULL);
        CHECK(strstr(r.out, "\"zipalign_source\":\"env\"") != NULL);
        CHECK(strstr(r.out, B("env-aapt2")) != NULL);
        ccb_proc_result_free(&r);
      }
      // Doctor with no override: default paths, source default.
      {
        char *env[] = { jh.buf, kh.buf, "CCB_AAPT2", "CCB_ZIPALIGN", NULL };
        char *args[] = { bin, "--config", B("proj/ccb.toml"), "doctor", NULL };
        ccb_proc_result r;
        CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
        CHECK(r.exit_code == 0);
        CHECK(strstr(r.out, B("sdk/build-tools/35.0.0/aapt2")) != NULL);
        CHECK(strstr(r.out, "(default)") != NULL);
        CHECK(strstr(r.out, "(env)") == NULL);
        ccb_proc_result_free(&r);
        char *jargs[] = { bin, "--json", "--config", B("proj/ccb.toml"), "doctor", NULL };
        CHECK(ccb_proc_run(jargs, NULL, env, &r) == CCB_OK);
        CHECK(strstr(r.out, "\"aapt2_source\":\"default\"") != NULL);
        ccb_proc_result_free(&r);
      }
      // Dry-run: the override paths appear in the aapt2 and zipalign
      // commands, and the default paths do not. Dry-run never creates
      // stage outputs, so the package step needs a pre-seeded
      // linked.ap_ (as a previous real link step would have left) to
      // reach the align step; without it the dry-run stops at package.
      {
        CHECK(ccb_mkdir_p(B("proj/build/intermediates")) == CCB_OK);
        write_text(B("proj/build/intermediates/linked.ap_"), "dummy");
        char *env[] = { jh.buf, kh.buf, ea.buf, ez.buf, NULL };
        char *args[] = { bin, "--config", B("proj/ccb.toml"), "--dry-run", "build", NULL };
        ccb_proc_result r;
        CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
        CHECK(strstr(r.out, B("env-aapt2")) != NULL);
        CHECK(strstr(r.out, B("env-zipalign")) != NULL);
        CHECK(strstr(r.out, B("sdk/build-tools/35.0.0/aapt2")) == NULL);
        CHECK(strstr(r.out, B("sdk/build-tools/35.0.0/zipalign")) == NULL);
        ccb_proc_result_free(&r);
      }
      ccb_str_free(&jh);
      ccb_str_free(&kh);
      ccb_str_free(&ea);
      ccb_str_free(&ez);
      free(bin);
    }
  }

  // PATH fallback: an SDK tool that cannot run (bad interpreter, so
  // exec fails, as with a wrong-architecture binary) falls back to
  // a runnable copy on PATH, and the source reports "path". A
  // missing SDK copy falls back the same way; when neither copy
  // runs, resolve fails naming the SDK copy.
  {
    char *save_path = saved_env("PATH");
    CHECK(ccb_mkdir_p(B("pathtools")) == CCB_OK);
    write_script(B("pathtools/aapt2"), "#!/bin/sh\nexit 0\n");
    write_script(B("pathtools/zipalign"), "#!/bin/sh\nexit 0\n");
    write_script(B("sdk/build-tools/35.0.0/aapt2"), "#!/nonexistent/interp\nexit 0\n");
    write_script(B("sdk/build-tools/35.0.0/zipalign"), "#!/nonexistent/interp\nexit 0\n");
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", NULL);
    set_env("PATH", B("pathtools"));
    write_config(NULL, NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    CHECK(sdk != NULL);
    CHECK(strcmp(sdk->aapt2, B("pathtools/aapt2")) == 0);
    CHECK(strcmp(sdk->aapt2_source, "path") == 0);
    CHECK(strcmp(sdk->zipalign, B("pathtools/zipalign")) == 0);
    CHECK(strcmp(sdk->zipalign_source, "path") == 0);
    ccb_sdk_free(sdk);
    ccb_diag_list_free(&diags);
    // Missing SDK copy also falls back to PATH.
    remove(B("sdk/build-tools/35.0.0/aapt2"));
    ccb_diag_list_init(&diags);
    sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    CHECK(sdk != NULL);
    CHECK(strcmp(sdk->aapt2, B("pathtools/aapt2")) == 0);
    CHECK(strcmp(sdk->aapt2_source, "path") == 0);
    ccb_sdk_free(sdk);
    ccb_diag_list_free(&diags);
    // Neither copy runs: resolve fails, naming the SDK copy.
    remove(B("pathtools/aapt2"));
    remove(B("pathtools/zipalign"));
    ccb_diag_list_init(&diags);
    sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) != CCB_OK);
    CHECK(sdk == NULL);
    CHECK(diag_contains(&diags, "CCB-E2009", "cannot run"));
    ccb_diag_list_free(&diags);
    restore_env("PATH", save_path);
    free(save_path);
    write_script(B("sdk/build-tools/35.0.0/aapt2"), "#!/bin/sh\nexit 0\n");
    write_script(B("sdk/build-tools/35.0.0/zipalign"), "#!/bin/sh\nexit 0\n");
  }

  // adb override through the real binary: a broken override fails
  // fast naming itself (logcat), and a runnable override is used
  // (install dry-run prints it).
  {
    char *bin = find_binary(argc, argv);
    CHECK(bin != NULL);
    if (bin != NULL) {
      write_text(B("broken-adb"), "not an executable\n");
      CHECK(chmod(B("broken-adb"), 0644) == 0);
      write_script(B("good-adb"), "#!/bin/sh\nexit 0\n");
      ccb_str bad, good;
      ccb_str_init(&bad);
      ccb_str_init(&good);
      ccb_str_append(&bad, "CCB_ADB=");
      ccb_str_append(&bad, B("broken-adb"));
      ccb_str_append(&good, "CCB_ADB=");
      ccb_str_append(&good, B("good-adb"));
      {
        char *args[] = { bin, "logcat", NULL };
        char *env[] = { bad.buf, NULL };
        ccb_proc_result r;
        CHECK(ccb_proc_run(args, BASE, env, &r) == CCB_OK);
        CHECK(r.exit_code == 3);
        CHECK(strstr(r.err, "CCB-E2009") != NULL);
        CHECK(strstr(r.err, B("broken-adb")) != NULL);
        ccb_proc_result_free(&r);
      }
      {
        char *args[] = { bin, "--dry-run", "--config", B("proj/ccb.toml"), "install", NULL };
        char *env[] = { good.buf, NULL };
        ccb_proc_result r;
        CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
        CHECK(r.exit_code == 0);
        CHECK(strstr(r.out, B("good-adb")) != NULL);
        ccb_proc_result_free(&r);
      }
      ccb_str_free(&bad);
      ccb_str_free(&good);
      free(bin);
    }
  }

  restore_env("JAVA_HOME", save_java);
  restore_env("KOTLIN_HOME", save_kotlin);
  restore_env("CCB_AAPT2", save_aapt2);
  restore_env("CCB_ZIPALIGN", save_zipalign);
  free(save_java);
  free(save_kotlin);
  free(save_aapt2);
  free(save_zipalign);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
