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

#define BASE "/tmp/ccb_test_sdk_tools"
#define SDK BASE "/sdk"
#define BT SDK "/build-tools/35.0.0"
#define PROJ BASE "/proj"

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
    "home = \"" SDK "\"\n");
  ccb_str_append(&s, "build_tools = \"");
  ccb_str_append(&s, build_tools == NULL ? "35.0.0" : build_tools);
  ccb_str_append(&s, "\"\n");
  if (sdk_extra != NULL) {
    ccb_str_append(&s, sdk_extra);
  }
  write_text(PROJ "/ccb.toml", s.buf);
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
  if (ccb_config_load(PROJ "/ccb.toml", NULL, &c, diags) != CCB_OK) {
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
  char *save_java = saved_env("JAVA_HOME");
  char *save_kotlin = saved_env("KOTLIN_HOME");
  char *save_aapt2 = saved_env("CCB_AAPT2");
  char *save_zipalign = saved_env("CCB_ZIPALIGN");

  ccb_remove_recursive(BASE);
  CHECK(ccb_mkdir_p(BT) == CCB_OK);
  CHECK(ccb_mkdir_p(SDK "/platforms/android-35") == CCB_OK);
  CHECK(ccb_mkdir_p(BASE "/java/bin") == CCB_OK);
  CHECK(ccb_mkdir_p(BASE "/kotlin/bin") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ "/src/main/res/values") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ "/src/main/kotlin") == CCB_OK);
  CHECK(ccb_mkdir_p(BASE "/reltools") == CCB_OK);
  // Fake default build-tools.
  write_script(BT "/aapt2", "#!/bin/sh\nexit 0\n");
  write_script(BT "/d8", "#!/bin/sh\nexit 0\n");
  write_script(BT "/apksigner", "#!/bin/sh\nexit 0\n");
  write_script(BT "/zipalign", "#!/bin/sh\nexit 0\n");
  write_text(SDK "/platforms/android-35/android.jar", "fake-jar");
  // Fake java/kotlinc answering -version in the expected formats.
  write_script(BASE "/java/bin/java",
    "#!/bin/sh\necho 'openjdk version \"17.0.1\"' >&2\n");
  write_script(BASE "/java/bin/javac", "#!/bin/sh\nexit 0\n");
  write_script(BASE "/kotlin/bin/kotlinc",
    "#!/bin/sh\necho 'info: kotlinc-jvm 2.4.21 (JRE 17)' >&2\n");
  write_text(PROJ "/src/main/AndroidManifest.xml",
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
  write_text(PROJ "/src/main/res/values/strings.xml",
    "<resources><string name=\"app_name\">hello</string></resources>\n");
  CHECK(ccb_mkdir_p(PROJ "/src/main/kotlin/com/example/hello") == CCB_OK);
  write_text(PROJ "/src/main/kotlin/com/example/hello/MainActivity.kt",
    "package com.example.hello\nclass MainActivity\n");
  // Override candidates.
  write_script(BASE "/cfg-aapt2", "#!/bin/sh\nexit 0\n");
  write_script(BASE "/cfg-zipalign", "#!/bin/sh\nexit 0\n");
  write_script(BASE "/env-aapt2", "#!/bin/sh\nexit 0\n");
  write_script(BASE "/env-zipalign", "#!/bin/sh\nexit 0\n");
  write_script(BASE "/reltools/my-aapt2", "#!/bin/sh\nexit 0\n");
  write_text(BASE "/nonexec-aapt2", "#!/bin/sh\nexit 0\n");
  CHECK(chmod(BASE "/nonexec-aapt2", 0644) == 0);
  // Logging script for the direct-invocation proof.
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "#!/bin/sh\necho invoked \"$@\" >> " BASE "/invocations.log\n");
    write_script(BASE "/log-aapt2", s.buf);
    ccb_str_free(&s);
  }

  set_env("JAVA_HOME", BASE "/java");
  set_env("KOTLIN_HOME", BASE "/kotlin");

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
      CHECK_STR_EQ(sdk->aapt2, BT "/aapt2");
      CHECK_STR_EQ(sdk->aapt2_source, "default");
      CHECK_STR_EQ(sdk->zipalign, BT "/zipalign");
      CHECK_STR_EQ(sdk->zipalign_source, "default");
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
  }

  // Env beats default.
  {
    write_config(NULL, NULL);
    set_env("CCB_AAPT2", BASE "/env-aapt2");
    set_env("CCB_ZIPALIGN", BASE "/env-zipalign");
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    if (sdk != NULL) {
      CHECK_STR_EQ(sdk->aapt2, BASE "/env-aapt2");
      CHECK_STR_EQ(sdk->aapt2_source, "env");
      CHECK_STR_EQ(sdk->zipalign, BASE "/env-zipalign");
      CHECK_STR_EQ(sdk->zipalign_source, "env");
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
  }

  // Config beats env.
  {
    ccb_str extra;
    ccb_str_init(&extra);
    ccb_str_append(&extra, "aapt2 = \"" BASE "/cfg-aapt2\"\n");
    ccb_str_append(&extra, "zipalign = \"" BASE "/cfg-zipalign\"\n");
    write_config(extra.buf, NULL);
    ccb_str_free(&extra);
    set_env("CCB_AAPT2", BASE "/env-aapt2");
    set_env("CCB_ZIPALIGN", BASE "/env-zipalign");
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_OK);
    if (sdk != NULL) {
      CHECK_STR_EQ(sdk->aapt2, BASE "/cfg-aapt2");
      CHECK_STR_EQ(sdk->aapt2_source, "config");
      CHECK_STR_EQ(sdk->zipalign, BASE "/cfg-zipalign");
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
      CHECK_STR_EQ(sdk->aapt2, BT "/aapt2");
      CHECK_STR_EQ(sdk->aapt2_source, "default");
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&diags);
  }

  // Missing config override: CCB-E2009 naming source + path.
  {
    write_config("aapt2 = \"" BASE "/no-such-aapt2\"\n", NULL);
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_ERR_TOOLCHAIN);
    CHECK(sdk == NULL);
    CHECK(has_code(&diags, "CCB-E2009"));
    CHECK(diag_contains(&diags, "CCB-E2009", "[sdk] aapt2"));
    CHECK(diag_contains(&diags, "CCB-E2009", BASE "/no-such-aapt2"));
    ccb_diag_list_free(&diags);
  }

  // Missing env override: CCB-E2009 naming the env var + path.
  {
    write_config(NULL, NULL);
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", BASE "/no-such-zipalign");
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_ERR_TOOLCHAIN);
    CHECK(diag_contains(&diags, "CCB-E2009", "CCB_ZIPALIGN"));
    CHECK(diag_contains(&diags, "CCB-E2009", BASE "/no-such-zipalign"));
    ccb_diag_list_free(&diags);
  }

  // Non-executable override file: same failure, no silent fallback.
  {
    write_config("aapt2 = \"" BASE "/nonexec-aapt2\"\n", NULL);
    set_env("CCB_AAPT2", NULL);
    set_env("CCB_ZIPALIGN", NULL);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_sdk *sdk = NULL;
    CHECK(resolve_proj(&sdk, &diags) == CCB_ERR_TOOLCHAIN);
    CHECK(diag_contains(&diags, "CCB-E2009", "[sdk] aapt2"));
    CHECK(diag_contains(&diags, "CCB-E2009", BASE "/nonexec-aapt2"));
    ccb_diag_list_free(&diags);
  }

  // A broken override is not masked by a missing build-tools dir:
  // both CCB-E2009 (override) and CCB-E2002 (build-tools) appear.
  {
    write_config("aapt2 = \"" BASE "/no-such-aapt2\"\n", "99.0.0");
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
    CHECK(ccb_config_load(PROJ "/ccb.toml", NULL, &c, &diags) == CCB_ERR_CONFIG);
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
    set_env("CCB_AAPT2", BASE "/log-aapt2");
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
      CHECK(ccb_read_file(BASE "/invocations.log", &log, &log_len) == CCB_OK);
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
      ccb_str_append(&jh, "JAVA_HOME=" BASE "/java");
      ccb_str_append(&kh, "KOTLIN_HOME=" BASE "/kotlin");
      ccb_str_append(&ea, "CCB_AAPT2=" BASE "/env-aapt2");
      ccb_str_append(&ez, "CCB_ZIPALIGN=" BASE "/env-zipalign");
      // Doctor, text + JSON, with env overrides marked as env.
      {
        char *args[] = { bin, "--config", PROJ "/ccb.toml", "doctor", NULL };
        char *env[] = { jh.buf, kh.buf, ea.buf, ez.buf, NULL };
        ccb_proc_result r;
        CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
        CHECK(r.exit_code == 0);
        CHECK(strstr(r.out, BASE "/env-aapt2") != NULL);
        CHECK(strstr(r.out, "(env)") != NULL);
        CHECK(strstr(r.out, BASE "/env-zipalign") != NULL);
        ccb_proc_result_free(&r);
        char *jargs[] = { bin, "--json", "--config", PROJ "/ccb.toml", "doctor", NULL };
        CHECK(ccb_proc_run(jargs, NULL, env, &r) == CCB_OK);
        CHECK(r.exit_code == 0);
        CHECK(strstr(r.out, "\"aapt2_source\":\"env\"") != NULL);
        CHECK(strstr(r.out, "\"zipalign_source\":\"env\"") != NULL);
        CHECK(strstr(r.out, BASE "/env-aapt2") != NULL);
        ccb_proc_result_free(&r);
      }
      // Doctor with no override: default paths, source default.
      {
        char *env[] = { jh.buf, kh.buf, "CCB_AAPT2", "CCB_ZIPALIGN", NULL };
        char *args[] = { bin, "--config", PROJ "/ccb.toml", "doctor", NULL };
        ccb_proc_result r;
        CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
        CHECK(r.exit_code == 0);
        CHECK(strstr(r.out, BT "/aapt2") != NULL);
        CHECK(strstr(r.out, "(default)") != NULL);
        CHECK(strstr(r.out, "(env)") == NULL);
        ccb_proc_result_free(&r);
        char *jargs[] = { bin, "--json", "--config", PROJ "/ccb.toml", "doctor", NULL };
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
        CHECK(ccb_mkdir_p(PROJ "/build/intermediates") == CCB_OK);
        write_text(PROJ "/build/intermediates/linked.ap_", "dummy");
        char *env[] = { jh.buf, kh.buf, ea.buf, ez.buf, NULL };
        char *args[] = { bin, "--config", PROJ "/ccb.toml", "--dry-run", "build", NULL };
        ccb_proc_result r;
        CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
        CHECK(strstr(r.out, BASE "/env-aapt2") != NULL);
        CHECK(strstr(r.out, BASE "/env-zipalign") != NULL);
        CHECK(strstr(r.out, BT "/aapt2") == NULL);
        CHECK(strstr(r.out, BT "/zipalign") == NULL);
        ccb_proc_result_free(&r);
      }
      ccb_str_free(&jh);
      ccb_str_free(&kh);
      ccb_str_free(&ea);
      ccb_str_free(&ez);
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
