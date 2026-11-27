// Tests for cache output validation (Step 2): a fingerprint match
// must not skip a step whose outputs are missing or corrupt.
// Black-box, like test_dex_contents.c: needs a real SDK toolchain,
// else prints a skip note and passes. Scenarios, each on a real
// `ccb build` of the minimal hello project:
//   1. B-1: delete build/intermediates/classes with the cache
//      intact -> the rebuild must re-run kotlinc, restore the
//      classes, and must NOT report "up to date".
//   2. Truncated classes.dex -> d8 re-runs and restores it.
//   3. Truncated .class file -> kotlinc re-runs and restores it.
//   4. Deleted generated R.java -> aapt2 link re-runs and
//      restores it.
//   5. B-2: delete an extracted AAR dir with the cache intact ->
//      the rebuild must re-extract it and succeed.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cache.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include "ccb_zip.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define HELLO_PROJ B("hello")
#define AAR_PROJ B("hello-aar")
#define LIB_AAR B("lib.aar")

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

// Run `ccb build` in proj, capturing stdout+stderr. Returns 1 when
// the process spawned; *exit_code and *combined (malloc'd) report
// the result. The caller frees *combined.
static int run_build_out(char *bin, const char *proj, char **env,
    int *exit_code, char **combined) {
  char *args[] = { bin, "build", NULL };
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

static int run_build(char *bin, const char *proj, char **env) {
  int code = -1;
  char *out = NULL;
  if (!run_build_out(bin, proj, env, &code, &out)) {
    return 0;
  }
  int ok = code == 0;
  if (!ok) {
    fprintf(stderr, "build failed in %s:\n%s\n", proj, out);
  }
  free(out);
  return ok;
}

// Minimal Kotlin hello project (same shape as test_dex_contents.c).
static void write_hello_project(const char *proj, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home, const char *aar_path) {
  char *kt_dir = ccb_path_join(proj, "src/main/kotlin/com/example/hello");
  mkdir_or_fail(kt_dir);
  free(kt_dir);
  char *values_dir = ccb_path_join(proj, "src/main/res/values");
  mkdir_or_fail(values_dir);
  free(values_dir);
  char *layout_dir = ccb_path_join(proj, "src/main/res/layout");
  mkdir_or_fail(layout_dir);
  free(layout_dir);
  char major[8];
  snprintf(major, sizeof(major), "%.*s", (int)strcspn(bt_ver, "."), bt_ver);
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "ccb_version = 1\n\n[project]\nname = \"hello\"\n"
    "package = \"com.example.hello\"\nversion = \"1.0\"\nversion_code = 1\n"
    "min_sdk = 26\ntarget_sdk = ");
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
  if (aar_path != NULL) {
    ccb_str_append(&s, "\n[deps]\naars = [\"");
    ccb_str_append(&s, aar_path);
    ccb_str_append(&s, "\"]\n");
  }
  char *cfg = ccb_path_join(proj, "ccb.toml");
  write_text(cfg, s.buf);
  free(cfg);
  ccb_str_free(&s);
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
  char *strings = ccb_path_join(proj, "src/main/res/values/strings.xml");
  write_text(strings,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources><string name=\"app_name\">hello</string></resources>\n");
  free(strings);
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
  char *kt = ccb_path_join(proj, "src/main/kotlin/com/example/hello/MainActivity.kt");
  write_text(kt,
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
  free(kt);
}

// Minimal AAR with a real classes.jar: compile one Lib class with
// the JDK javac, jar the .class with ccb_zip_writer, then zip the
// AAR the same way (manifest + classes.jar + one string resource).
static void write_lib_aar_with_classes(const char *aar_path,
    const char *java_home) {
  char *src_dir = ccb_path_join(BASE, "libsrc/com/example/lib");
  mkdir_or_fail(src_dir);
  free(src_dir);
  char *java = ccb_path_join(BASE, "libsrc/com/example/lib/Lib.java");
  write_text(java,
    "package com.example.lib;\n"
    "public class Lib { public static int answer() { return 42; } }\n");
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

static long file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;
  }
  return (long)st.st_size;
}

static int file_starts_with(const char *path, const unsigned char *magic,
    size_t n) {
  char *data = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &data, &len) != CCB_OK) {
    return 0;
  }
  int ok = len >= n && memcmp(data, magic, n) == 0;
  free(data);
  return ok;
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "cacheout");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping cache-output checks\n");
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
    fprintf(stderr, "note: no runnable SDK toolchain, skipping cache-output checks\n");
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

  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, NULL);
  char *apk = ccb_path_join(HELLO_PROJ, "build/outputs/debug/app.apk");
  char *classes_dir = ccb_path_join(HELLO_PROJ, "build/intermediates/classes");
  char *main_class = ccb_path_join(classes_dir,
    "com/example/hello/MainActivity.class");
  char *dex_dir = ccb_path_join(HELLO_PROJ, "build/intermediates/dex");
  char *dex_file = ccb_path_join(dex_dir, "classes.dex");
  char *r_java = ccb_path_join(HELLO_PROJ,
    "build/intermediates/gen/com/example/hello/R.java");
  CHECK(run_build(bin, HELLO_PROJ, env));
  CHECK(ccb_path_exists(apk));
  CHECK(ccb_path_exists(main_class));
  CHECK(file_size(dex_file) > 100000);

  // 1. B-1: delete the classes dir with the cache intact.
  int code = -1;
  char *out = NULL;
  CHECK(ccb_remove_recursive(classes_dir) == CCB_OK);
  CHECK(!ccb_path_exists(classes_dir));
  CHECK(run_build_out(bin, HELLO_PROJ, env, &code, &out));
  CHECK(code == 0);
  CHECK(!ccb_test_contains(out, "up to date:"));
  CHECK(ccb_test_contains(out, "kotlinc"));
  CHECK(ccb_path_exists(main_class));
  CHECK(ccb_path_exists(apk));
  free(out);

  // 2. Truncated dex: d8 must re-run and restore a whole dex.
  CHECK(ccb_write_file(dex_file, "dex\n038\0xx", 10) == CCB_OK);
  CHECK(file_size(dex_file) == 10);
  CHECK(run_build_out(bin, HELLO_PROJ, env, &code, &out));
  CHECK(code == 0);
  CHECK(ccb_test_contains(out, "d8"));
  CHECK(file_size(dex_file) > 100000);
  {
    static const unsigned char dex_magic[] = { 'd', 'e', 'x', '\n', '0' };
    CHECK(file_starts_with(dex_file, dex_magic, sizeof(dex_magic)));
  }
  free(out);

  // 3. Truncated .class: kotlinc must re-run and restore it.
  CHECK(ccb_write_file(main_class, "junk", 4) == CCB_OK);
  CHECK(run_build_out(bin, HELLO_PROJ, env, &code, &out));
  CHECK(code == 0);
  CHECK(ccb_test_contains(out, "kotlinc"));
  CHECK(file_size(main_class) > 100);
  {
    static const unsigned char class_magic[] = { 0xCA, 0xFE, 0xBA, 0xBE };
    CHECK(file_starts_with(main_class, class_magic, sizeof(class_magic)));
  }
  free(out);

  // 4. Deleted R.java: link must re-run and restore it.
  CHECK(unlink(r_java) == 0);
  CHECK(run_build_out(bin, HELLO_PROJ, env, &code, &out));
  CHECK(code == 0);
  CHECK(ccb_test_contains(out, "aapt2-link"));
  CHECK(ccb_path_exists(r_java));
  free(out);

  // 5. B-2: delete an extracted AAR dir with the cache intact.
  write_lib_aar_with_classes(LIB_AAR, java_home);
  write_hello_project(AAR_PROJ, sdk_home, bt_ver, kotlin_home, LIB_AAR);
  char *aar_apk = ccb_path_join(AAR_PROJ, "build/outputs/debug/app.apk");
  char *aar_extract = ccb_path_join(AAR_PROJ, "build/intermediates/aar/lib");
  char *aar_jar = ccb_path_join(aar_extract, "classes.jar");
  CHECK(run_build(bin, AAR_PROJ, env));
  CHECK(ccb_path_exists(aar_jar));
  CHECK(ccb_remove_recursive(aar_extract) == CCB_OK);
  CHECK(!ccb_path_exists(aar_extract));
  CHECK(run_build_out(bin, AAR_PROJ, env, &code, &out));
  CHECK(code == 0);
  CHECK(!ccb_test_contains(out, "up to date:"));
  CHECK(ccb_path_exists(aar_jar));
  CHECK(ccb_path_exists(aar_apk));
  free(out);

  free(aar_apk);
  free(aar_extract);
  free(aar_jar);
  free(apk);
  free(classes_dir);
  free(main_class);
  free(dex_dir);
  free(dex_file);
  free(r_java);
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
