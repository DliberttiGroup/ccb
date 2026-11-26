// Tests for `ccb build --aab`: bundle layout, signatures, and that the
// default APK build is byte-identical across an AAB build and a clean
// rebuild. Needs a real SDK toolchain: ANDROID_HOME (else
// $HOME/android-sdk), KOTLIN_HOME (else $HOME/kotlin-2.4.21), JAVA_HOME
// (else $HOME/jdk17). Without a runnable aapt2 the black-box part
// prints a skip note and passes; BundleConfig encoding itself is
// covered by test_bundle.c.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include "ccb_hash.h"
#include "ccb_zip.h"
#include "ccb_bundle.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define PROJ B("proj")
#define APK_OUT B("proj/build/outputs/debug/app.apk")
#define AAB_OUT B("proj/build/outputs/debug/app.aab")

static void write_text(const char *path, const char *text) {
  CHECK(ccb_write_file(path, text, strlen(text)) == CCB_OK);
}

static void mkdir_or_fail(const char *path) {
  CHECK(ccb_mkdir_p(path) == CCB_OK);
}

static char *find_binary(int argc, char **argv) {
  if (argc > 1 && argv[1] != NULL && access(argv[1], X_OK) == 0) {
    // Resolve to an absolute path: the child runs with cwd set to the
    // project dir, so a relative argv[1] would resolve there and the
    // exec would fail silently with exit 127 and no output.
    char resolved[4096];
    if (realpath(argv[1], resolved) != NULL) {
      return ccb_xstrdup(resolved);
    }
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

// Resolve a toolchain home: env var first, else $HOME/<fallback>.
static char *tool_home(const char *env_name, const char *fallback) {
  const char *v = getenv(env_name);
  if (v != NULL && v[0] != '\0') {
    return ccb_xstrdup(v);
  }
  const char *home = getenv("HOME");
  if (home == NULL) {
    return NULL;
  }
  return ccb_path_join(home, fallback);
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

static int zip_has_prefix(const char *zip_path, const char *prefix) {
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(zip_path, &a) != CCB_OK) {
    return 0;
  }
  int found = 0;
  size_t plen = strlen(prefix);
  for (size_t i = 0; i < a->count; i++) {
    if (strncmp(a->entries[i].name, prefix, plen) == 0) {
      found = 1;
      break;
    }
  }
  ccb_zip_close(a);
  return found;
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "aab");
  char *bin = find_binary(argc, argv);
  char *sdk_home = tool_home("ANDROID_HOME", "android-sdk");
  char *java_home = tool_home("JAVA_HOME", "jdk17");
  char *kotlin_home = tool_home("KOTLIN_HOME", "kotlin-2.4.21");
  char *aapt2 = sdk_home == NULL ? NULL
    : ccb_path_join3(sdk_home, "build-tools/35.0.0", "aapt2");
  char *jarsigner = java_home == NULL ? NULL : ccb_path_join(java_home, "bin/jarsigner");
  if (bin == NULL || aapt2 == NULL || access(aapt2, X_OK) != 0 ||
      jarsigner == NULL || access(jarsigner, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping black-box AAB checks\n");
    free(bin);
    free(sdk_home);
    free(java_home);
    free(kotlin_home);
    free(aapt2);
    free(jarsigner);
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }
  char *sdk_env = NULL;
  char *java_env = NULL;
  char *kotlin_env = NULL;
  char *home_env = NULL;
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "ANDROID_HOME=");
    ccb_str_append(&s, sdk_home);
    sdk_env = ccb_str_steal(&s);
    ccb_str_init(&s);
    ccb_str_append(&s, "JAVA_HOME=");
    ccb_str_append(&s, java_home);
    java_env = ccb_str_steal(&s);
    ccb_str_init(&s);
    ccb_str_append(&s, "KOTLIN_HOME=");
    ccb_str_append(&s, kotlin_home);
    kotlin_env = ccb_str_steal(&s);
    ccb_str_init(&s);
    ccb_str_append(&s, "HOME=");
    ccb_str_append(&s, B("home"));
    home_env = ccb_str_steal(&s);
  }
  char *const env_extra[] = { sdk_env, java_env, kotlin_env, home_env, NULL };

  // Scaffold the hello fixture (same shape as `ccb init` output).
  ccb_remove_recursive(BASE);
  mkdir_or_fail(B("proj/src/main/kotlin/com/example/hello"));
  mkdir_or_fail(B("proj/src/main/res/values"));
  mkdir_or_fail(B("proj/src/main/res/layout"));
  write_text(B("proj/ccb.toml"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"hello\"\n"
    "package = \"com.example.hello\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\n"
    "kotlin = \"src/main/kotlin\"\n");
  write_text(B("proj/src/main/AndroidManifest.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  package=\"com.example.hello\">\n"
    "  <application android:label=\"@string/app_name\" android:theme=\"@style/AppTheme\">\n"
    "    <activity android:name=\".MainActivity\" android:exported=\"true\">\n"
    "      <intent-filter>\n"
    "        <action android:name=\"android.intent.action.MAIN\" />\n"
    "        <category android:name=\"android.intent.category.LAUNCHER\" />\n"
    "      </intent-filter>\n"
    "    </activity>\n"
    "  </application>\n"
    "</manifest>\n");
  write_text(B("proj/src/main/kotlin/com/example/hello/MainActivity.kt"),
    "package com.example.hello\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "\n"
    "class MainActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    setContentView(R.layout.activity_main)\n"
    "  }\n"
    "}\n");
  write_text(B("proj/src/main/res/values/strings.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"app_name\">hello</string>\n"
    "  <string name=\"hello\">Hello from CCB!</string>\n"
    "</resources>\n");
  write_text(B("proj/src/main/res/layout/activity_main.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  android:layout_width=\"match_parent\"\n"
    "  android:layout_height=\"match_parent\"\n"
    "  android:gravity=\"center\"\n"
    "  android:orientation=\"vertical\">\n"
    "  <TextView android:layout_width=\"wrap_content\"\n"
    "    android:layout_height=\"wrap_content\"\n"
    "    android:text=\"@string/hello\" />\n"
    "</LinearLayout>\n");
  // The scaffold needs a theme: init writes values/themes.xml too.
  write_text(B("proj/src/main/res/values/themes.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <style name=\"AppTheme\" parent=\"@android:style/Theme.Material.Light.NoActionBar\" />\n"
    "</resources>\n");

  ccb_proc_result r;
  char *apk_args[] = { bin, "build", NULL };
  CHECK(ccb_proc_run(apk_args, PROJ, env_extra, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  if (r.exit_code != 0) {
    fprintf(stderr, "APK build failed: %s\n%s\n", r.out, r.err);
  }
  ccb_proc_result_free(&r);
  CHECK(ccb_path_exists(APK_OUT));
  CHECK(zip_has(APK_OUT, "AndroidManifest.xml"));
  CHECK(zip_has(APK_OUT, "classes.dex"));
  CHECK(zip_has(APK_OUT, "resources.arsc"));
  CHECK(!zip_has(APK_OUT, "base/manifest/AndroidManifest.xml"));
  char h1[65];
  CHECK(ccb_sha256_file(APK_OUT, h1) == CCB_OK);

  // Build the bundle in the same project.
  char *aab_args[] = { bin, "build", "--aab", NULL };
  CHECK(ccb_proc_run(aab_args, PROJ, env_extra, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  if (r.exit_code != 0) {
    fprintf(stderr, "AAB build failed: %s\n%s\n", r.out, r.err);
  }
  ccb_proc_result_free(&r);
  CHECK(ccb_path_exists(AAB_OUT));
  CHECK(zip_has(AAB_OUT, "base/manifest/AndroidManifest.xml"));
  CHECK(zip_has(AAB_OUT, "base/dex/classes.dex"));
  CHECK(zip_has(AAB_OUT, "base/resources.pb"));
  CHECK(zip_has(AAB_OUT, "base/res/layout/activity_main.xml"));
  CHECK(zip_has(AAB_OUT, "BundleConfig.pb"));
  CHECK(zip_has(AAB_OUT, "META-INF/MANIFEST.MF"));
  CHECK(zip_has_prefix(AAB_OUT, "META-INF/"));
  CHECK(!zip_has(AAB_OUT, "AndroidManifest.xml"));
  CHECK(!zip_has(AAB_OUT, "resources.arsc"));
  // BundleConfig.pb decodes back to the version CCB writes.
  {
    ccb_zip_archive *a = NULL;
    CHECK(ccb_zip_open(AAB_OUT, &a) == CCB_OK);
    const ccb_zip_entry *e = ccb_zip_find(a, "BundleConfig.pb");
    CHECK(e != NULL);
    void *data = NULL;
    size_t len = 0;
    CHECK(ccb_zip_read_entry(a, e, &data, &len) == CCB_OK);
    char *ver = NULL;
    CHECK(ccb_bundle_config_decode_version(data, len, &ver) == CCB_OK);
    CHECK_STR_EQ(ver, "1.0.0");
    free(data);
    free(ver);
    ccb_zip_close(a);
  }
  // The bundle's JAR signature verifies.
  {
    char *js_args[] = { jarsigner, "-verify", (char *)AAB_OUT, NULL };
    CHECK(ccb_proc_run(js_args, PROJ, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    if (r.exit_code != 0) {
      fprintf(stderr, "jarsigner verify failed: %s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
  }
  // --json reports the bundle under the "aab" key.
  {
    char *json_args[] = { bin, "build", "--aab", "--json", NULL };
    CHECK(ccb_proc_run(json_args, PROJ, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(r.out != NULL && strstr(r.out, "\"aab\":") != NULL);
    ccb_proc_result_free(&r);
  }
  // Rebuilding the APK after the AAB build must be byte-identical:
  // the separate AAB cache keys and intermediates must not leak into
  // the APK path.
  CHECK(ccb_proc_run(apk_args, PROJ, env_extra, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  ccb_proc_result_free(&r);
  char h2[65];
  CHECK(ccb_sha256_file(APK_OUT, h2) == CCB_OK);
  CHECK_STR_EQ(h1, h2);
  // And a clean rebuild is byte-identical too.
  {
    char *clean_args[] = { bin, "clean", NULL };
    CHECK(ccb_proc_run(clean_args, PROJ, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    ccb_proc_result_free(&r);
    CHECK(ccb_proc_run(apk_args, PROJ, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    ccb_proc_result_free(&r);
    char h3[65];
    CHECK(ccb_sha256_file(APK_OUT, h3) == CCB_OK);
    CHECK_STR_EQ(h1, h3);
  }
  free(bin);
  free(sdk_home);
  free(java_home);
  free(kotlin_home);
  free(aapt2);
  free(jarsigner);
  free(sdk_env);
  free(java_env);
  free(kotlin_env);
  free(home_env);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
