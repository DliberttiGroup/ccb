// Tests for the native stage (requirement.md §5.5): [ndk] header
// dependency tracking (bug-header-deps), 16 KB page size for
// arm64-v8a, the [cmake] external build, kotlinc optional for
// Java-only projects, [paths] java_extra, and javac -encoding UTF-8.
// Black-box: needs a real SDK toolchain + NDK, else prints a skip
// note and passes (same pattern as test_modules.c). The config-error
// checks live in test_config.c and run even without a toolchain.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_proc.h"
#include "ccb_sdk.h"
#include "ccb_str.h"
#include "ccb_zip.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define NAT B("nat")
#define NAT_SO B("nat/build/intermediates/jni/arm64-v8a/libnat.so")
#define NAT_DEP B("nat/build/intermediates/jni/arm64-v8a/native.d")
#define CMK B("cmk")
#define CMK_SO B("cmk/build/intermediates/jni/arm64-v8a/libnativelib.so")
#define CMK_APK B("cmk/build/outputs/debug/app.apk")
#define JAVAONLY B("javaonly")
#define JAVAONLY_DEX B("javaonly/build/intermediates/dex")
#define VENDOR B("vendor")

static void write_text(const char *path, const char *text) {
  CHECK(ccb_write_file(path, text, strlen(text)) == CCB_OK);
}

static void mkdir_or_fail(const char *path) {
  CHECK(ccb_mkdir_p(path) == CCB_OK);
}

static char *find_binary(int argc, char **argv) {
  if (argc > 1 && argv[1] != NULL && access(argv[1], X_OK) == 0) {
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

static int str_contains(const char *hay, const char *needle) {
  return hay != NULL && needle != NULL && strstr(hay, needle) != NULL;
}

static void run_or_fail(char **args, const char *proj,
    char *const *env, const char *what) {
  ccb_proc_result r;
  CHECK(ccb_proc_run(args, proj, env, &r) == CCB_OK);
  if (r.exit_code != 0) {
    fprintf(stderr, "%s failed: %s\n%s\n", what, r.out, r.err);
  }
  CHECK(r.exit_code == 0);
  ccb_proc_result_free(&r);
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

// True when any .dex file under dir contains needle as bytes.
static int dex_contains(const char *dir, const char *needle) {
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(dir, ".dex", &files, &nfiles) != CCB_OK) {
    return 0;
  }
  int found = 0;
  size_t nlen = strlen(needle);
  for (size_t i = 0; i < nfiles && !found; i++) {
    char *data = NULL;
    size_t len = 0;
    if (ccb_read_file(files[i], &data, &len) != CCB_OK) {
      continue;
    }
    for (size_t off = 0; off + nlen <= len; off++) {
      if (memcmp(data + off, needle, nlen) == 0) {
        found = 1;
        break;
      }
    }
    free(data);
  }
  ccb_free_str_array(files, nfiles);
  return found;
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

// True when every PT_LOAD segment in the .so has 16 KB alignment.
// readelf -W keeps each program header on one line, so p_align is
// the last field of each LOAD line.
static int readelf_load_align_ok(const char *readelf, const char *so) {
  char *args[] = { (char *)readelf, "-W", "-l", (char *)so, NULL };
  ccb_proc_result r;
  if (ccb_proc_run(args, NULL, NULL, &r) != CCB_OK) {
    return 0;
  }
  int nload = 0;
  if (r.exit_code == 0) {
    char *p = r.out;
    while (*p != '\0') {
      char *eol = strchr(p, '\n');
      if (eol == NULL) {
        eol = p + strlen(p);
      }
      char *line = p;
      while (line < eol && (*line == ' ' || *line == '\t')) {
        line++;
      }
      if ((size_t)(eol - line) >= 4 && strncmp(line, "LOAD", 4) == 0) {
        nload++;
        char *last = eol;
        while (last > line && *(last - 1) != ' ' && *(last - 1) != '\t') {
          last--;
        }
        if ((size_t)(eol - last) != 6 || strncmp(last, "0x4000", 6) != 0) {
          nload = -1;
          break;
        }
      }
      p = *eol == '\n' ? eol + 1 : eol;
    }
  }
  ccb_proc_result_free(&r);
  return nload > 0;
}

// Shared app skeleton: manifest + res + a MainActivity. kt is 1 for
// a Kotlin activity, 0 for Java. java_extra is a [paths] java_extra
// value (or NULL); extra_toml is appended to ccb.toml.
static void write_app_skeleton(const char *dir, const char *pkg,
    const char *name, int kt, const char *java_extra,
    const char *extra_toml) {
  ccb_str kd;
  ccb_str_init(&kd);
  ccb_str_append(&kd, dir);
  ccb_str_append(&kd, kt ? "/src/main/kotlin/com/example/app"
    : "/src/main/java/com/example/app");
  mkdir_or_fail(kd.buf);
  ccb_str_free(&kd);
  ccb_str rd;
  ccb_str_init(&rd);
  ccb_str_append(&rd, dir);
  ccb_str_append(&rd, "/src/main/res/values");
  mkdir_or_fail(rd.buf);
  ccb_str_free(&rd);
  ccb_str ld;
  ccb_str_init(&ld);
  ccb_str_append(&ld, dir);
  ccb_str_append(&ld, "/src/main/res/layout");
  mkdir_or_fail(ld.buf);
  ccb_str_free(&ld);
  ccb_str path;
  ccb_str_init(&path);
  ccb_str_append(&path, dir);
  ccb_str_append(&path, "/ccb.toml");
  ccb_str t;
  ccb_str_init(&t);
  ccb_str_append(&t,
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"");
  ccb_str_append(&t, name);
  ccb_str_append(&t, "\"\npackage = \"");
  ccb_str_append(&t, pkg);
  ccb_str_append(&t,
    "\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\n");
  ccb_str_append(&t, kt ? "kotlin = \"src/main/kotlin\"\n"
    : "java = \"src/main/java\"\n");
  if (java_extra != NULL) {
    ccb_str_append(&t, "java_extra = [\"");
    ccb_str_append(&t, java_extra);
    ccb_str_append(&t, "\"]\n");
  }
  ccb_str_append(&t, extra_toml);
  write_text(path.buf, t.buf);
  ccb_str_free(&t);
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, dir);
  ccb_str_append(&path, "/src/main/AndroidManifest.xml");
  write_text(path.buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"com.example.app\">\n"
    "  <application android:label=\"@string/app_name\" android:theme=\"@style/AppTheme\">\n"
    "    <activity android:name=\".MainActivity\" android:exported=\"true\">\n"
    "      <intent-filter>\n"
    "        <action android:name=\"android.intent.action.MAIN\" />\n"
    "        <category android:name=\"android.intent.category.LAUNCHER\" />\n"
    "      </intent-filter>\n"
    "    </activity>\n"
    "  </application>\n"
    "</manifest>\n");
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, dir);
  ccb_str_append(&path, kt ? "/src/main/kotlin/com/example/app/MainActivity.kt"
    : "/src/main/java/com/example/app/MainActivity.java");
  if (kt) {
    write_text(path.buf,
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "  }\n"
      "}\n");
  } else {
    write_text(path.buf,
      "package com.example.app;\n"
      "\n"
      "import android.app.Activity;\n"
      "import android.os.Bundle;\n"
      "\n"
      "// 你好 — non-ASCII comment (UTF-8): must compile with -encoding UTF-8.\n"
      "public class MainActivity extends Activity {\n"
      "  @Override\n"
      "  protected void onCreate(Bundle savedInstanceState) {\n"
      "    super.onCreate(savedInstanceState);\n"
      "  }\n"
      "}\n");
  }
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, dir);
  ccb_str_append(&path, "/src/main/res/values/strings.xml");
  write_text(path.buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"app_name\">app</string>\n"
    "</resources>\n");
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, dir);
  ccb_str_append(&path, "/src/main/res/layout/activity_main.xml");
  write_text(path.buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  android:layout_width=\"match_parent\"\n"
    "  android:layout_height=\"match_parent\">\n"
    "  <TextView android:layout_width=\"wrap_content\"\n"
    "    android:layout_height=\"wrap_content\"\n"
    "    android:text=\"@string/app_name\" />\n"
    "</LinearLayout>\n");
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, dir);
  ccb_str_append(&path, "/src/main/res/values/themes.xml");
  write_text(path.buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <style name=\"AppTheme\" parent=\"@android:style/Theme.Material.Light.NoActionBar\" />\n"
    "</resources>\n");
  ccb_str_free(&path);
}

static void write_task_header(const char *path, int nfields) {
  ccb_str t;
  ccb_str_init(&t);
  ccb_str_append(&t,
    "#ifndef TASK_H\n"
    "#define TASK_H\n"
    "struct Task {\n"
    "  int a;\n"
    "  int b;\n");
  if (nfields > 2) {
    ccb_str_append(&t, "  int c;\n");
  }
  ccb_str_append(&t,
    "};\n"
    "int task_size(void);\n"
    "#endif\n");
  write_text(path, t.buf);
  ccb_str_free(&t);
}

// [ndk] fixture + header-deps proof: a header change must rebuild
// the native lib (bug-header-deps), and the .so must be 16 KB
// aligned for arm64-v8a.
static void test_native_header_deps(char *bin, char *const *env,
    const char *ndk_version, const char *readelf) {
  ccb_str extra;
  ccb_str_init(&extra);
  ccb_str_append(&extra, "\n[sdk]\nndk_version = \"");
  ccb_str_append(&extra, ndk_version);
  ccb_str_append(&extra, "\"\n\n[ndk]\nabis = [\"arm64-v8a\"]\nc_sources = [\"src/main/cpp/native.c\"]\n");
  write_app_skeleton(NAT, "com.example.app", "nat", 1, NULL, extra.buf);
  ccb_str_free(&extra);
  mkdir_or_fail(B("nat/src/main/cpp"));
  write_text(B("nat/src/main/cpp/native.c"),
    "#include \"task.h\"\n"
    "int task_size(void) {\n"
    "  return (int)sizeof(struct Task);\n"
    "}\n");
  write_task_header(B("nat/src/main/cpp/task.h"), 2);
  {
    char *args[] = { bin, "build", NULL };
    run_or_fail(args, NAT, env, "native build");
  }
  CHECK(ccb_path_exists(NAT_SO));
  CHECK(ccb_path_exists(NAT_DEP));
  {
    char *dep = NULL;
    size_t len = 0;
    CHECK(ccb_read_file(NAT_DEP, &dep, &len) == CCB_OK);
    CHECK(str_contains(dep, "task.h"));
    free(dep);
  }
  if (readelf != NULL) {
    CHECK(readelf_load_align_ok(readelf, NAT_SO));
  }
  // No change: the native step must skip.
  {
    char *args[] = { bin, "--explain", "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NAT, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "native-arm64-v8a: skip (inputs unchanged)"));
    ccb_proc_result_free(&r);
  }
  // Header change (struct grows 8 -> 12 bytes): the native step must
  // run, and the rebuilt .so must differ.
  char *before = NULL;
  size_t before_len = 0;
  CHECK(ccb_read_file(NAT_SO, &before, &before_len) == CCB_OK);
  write_task_header(B("nat/src/main/cpp/task.h"), 3);
  {
    char *args[] = { bin, "--explain", "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NAT, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "native-arm64-v8a: run (native inputs changed)"));
    ccb_proc_result_free(&r);
  }
  {
    char *args[] = { bin, "build", NULL };
    run_or_fail(args, NAT, env, "native rebuild after header change");
  }
  {
    char *after = NULL;
    size_t after_len = 0;
    CHECK(ccb_read_file(NAT_SO, &after, &after_len) == CCB_OK);
    CHECK(after_len != before_len ||
      memcmp(before, after, before_len) != 0);
    free(after);
  }
  free(before);
  if (readelf != NULL) {
    CHECK(readelf_load_align_ok(readelf, NAT_SO));
  }
}

// [cmake] fixture: an external CMakeLists.txt builds the .so, which
// is packaged exactly like an [ndk]-built lib.
static void test_cmake(char *bin, char *const *env,
    const char *ndk_version, const char *readelf) {
  ccb_str extra;
  ccb_str_init(&extra);
  ccb_str_append(&extra, "\n[sdk]\nndk_version = \"");
  ccb_str_append(&extra, ndk_version);
  ccb_str_append(&extra, "\"\n\n[ndk]\nabis = [\"arm64-v8a\"]\n\n[cmake]\nlists = \"cpp/CMakeLists.txt\"\n");
  write_app_skeleton(CMK, "com.example.app", "cmk", 0, NULL, extra.buf);
  ccb_str_free(&extra);
  mkdir_or_fail(B("cmk/cpp"));
  write_text(B("cmk/cpp/CMakeLists.txt"),
    "cmake_minimum_required(VERSION 3.22)\n"
    "project(nativelib)\n"
    "add_library(nativelib SHARED native.c)\n");
  write_text(B("cmk/cpp/native.c"),
    "int nativelib_value(void) {\n"
    "  return 42;\n"
    "}\n");
  {
    char *args[] = { bin, "build", NULL };
    run_or_fail(args, CMK, env, "cmake build");
  }
  CHECK(ccb_path_exists(CMK_SO));
  CHECK(zip_has(CMK_APK, "lib/arm64-v8a/libnativelib.so"));
  if (readelf != NULL) {
    CHECK(readelf_load_align_ok(readelf, CMK_SO));
  }
  // No change: the cmake step must skip.
  {
    char *args[] = { bin, "--explain", "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, CMK, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "native-arm64-v8a: skip (inputs unchanged)"));
    ccb_proc_result_free(&r);
  }
}

// Java-only fixture: builds with kotlinc nowhere to be found, and
// [paths] java_extra is compiled alongside java_dir.
static void test_java_only(char *bin, char *const *scrubbed_env) {
  write_app_skeleton(JAVAONLY, "com.example.app", "javaonly", 0,
    "../vendor", "");
  mkdir_or_fail(B("vendor/com/example/vendor"));
  write_text(B("vendor/com/example/vendor/Vendored.java"),
    "package com.example.vendor;\n"
    "\n"
    "// Vendored class, compiled from outside the project tree.\n"
    "public class Vendored {\n"
    "  public static String greeting() {\n"
    "    return \"vendored\";\n"
    "  }\n"
    "}\n");
  // `ccb check` with no kotlinc anywhere must pass and report none.
  {
    char *args[] = { bin, "check", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, JAVAONLY, scrubbed_env, &r) == CCB_OK);
    if (r.exit_code != 0) {
      fprintf(stderr, "java-only check failed: %s\n%s\n", r.out, r.err);
    }
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "kotlinc: none"));
    ccb_proc_result_free(&r);
  }
  // Full build with no kotlinc: the extra class lands in the dex.
  {
    char *args[] = { bin, "build", NULL };
    run_or_fail(args, JAVAONLY, scrubbed_env, "java-only build");
  }
  CHECK(dex_contains(JAVAONLY_DEX, "Lcom/example/vendor/Vendored;"));
  CHECK(dex_contains(JAVAONLY_DEX, "Lcom/example/app/MainActivity;"));
  // Control: a Kotlin project with no kotlinc must fail CCB-E2004.
  {
    char *args[] = { bin, "check", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NAT, scrubbed_env, &r) == CCB_OK);
    CHECK(r.exit_code != 0);
    CHECK(str_contains(r.err, "CCB-E2004"));
    ccb_proc_result_free(&r);
  }
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "native");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping native checks\n");
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }

  char *sdk_home = tool_home("ANDROID_HOME", "android-sdk");
  char *java_home = tool_home("JAVA_HOME", "jdk17");
  char *kotlin_home = tool_home("KOTLIN_HOME", "kotlin-2.4.21");
  char *aapt2 = sdk_home == NULL ? NULL
    : ccb_path_join3(sdk_home, "build-tools/35.0.0", "aapt2");
  char *jarsigner = java_home == NULL ? NULL : ccb_path_join(java_home, "bin/jarsigner");
  if (bin == NULL || aapt2 == NULL || access(aapt2, X_OK) != 0 ||
      jarsigner == NULL || access(jarsigner, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping black-box native checks\n");
    free(bin);
    free(sdk_home);
    free(java_home);
    free(kotlin_home);
    free(aapt2);
    free(jarsigner);
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }
  char *ndk_version = find_ndk_version(sdk_home);
  if (ndk_version == NULL) {
    fprintf(stderr, "note: no NDK installed, skipping black-box native checks\n");
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

  // Scrubbed env: kotlinc nowhere to be found (KOTLIN_HOME removed
  // entirely, PATH pointing at an empty dir the test owns: the
  // runner image preinstalls kotlinc on PATH, so scrubbing to
  // /usr/bin:/bin still finds it there). JAVA_HOME still points at
  // the JDK.
  char *scrubbed_kotlin = ccb_xstrdup("KOTLIN_HOME");
  char *scrubbed_path = NULL;
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "PATH=");
    ccb_str_append(&s, B("emptypath"));
    scrubbed_path = ccb_str_steal(&s);
  }
  char *const scrubbed_env[] = { sdk_env, java_env, home_env,
    scrubbed_kotlin, scrubbed_path, NULL };

  char *readelf = ccb_sdk_find_on_path("readelf");
  if (readelf == NULL) {
    fprintf(stderr, "note: no readelf, skipping 16 KB alignment checks\n");
  }

  ccb_remove_recursive(BASE);
  mkdir_or_fail(B("emptypath"));
  // Clean-room PATH: only the helpers the build-tools wrapper
  // scripts need (basename, dirname, expr, java). kotlinc must not
  // be reachable via PATH: the runner image preinstalls it, so
  // scrubbing to /usr/bin:/bin still finds it there.
  {
    const char *helpers[] = { "basename", "dirname", "expr" };
    for (size_t i = 0; i < 3; i++) {
      char *src = ccb_path_join("/usr/bin", helpers[i]);
      char *dst = ccb_path_join(B("emptypath"), helpers[i]);
      CHECK(access(src, X_OK) == 0);
      CHECK(symlink(src, dst) == 0);
      free(src);
      free(dst);
    }
    char *jsrc = ccb_path_join(java_home, "bin/java");
    char *jdst = ccb_path_join(B("emptypath"), "java");
    CHECK(access(jsrc, X_OK) == 0);
    CHECK(symlink(jsrc, jdst) == 0);
    free(jsrc);
    free(jdst);
  }
  test_native_header_deps(bin, env_extra, ndk_version, readelf);
  if (ccb_sdk_find_on_path("cmake") != NULL &&
      ccb_sdk_find_on_path("make") != NULL) {
    test_cmake(bin, env_extra, ndk_version, readelf);
  } else {
    fprintf(stderr, "note: no cmake/make on PATH, skipping cmake checks\n");
  }
  test_java_only(bin, scrubbed_env);

  free(bin);
  free(sdk_home);
  free(java_home);
  free(kotlin_home);
  free(aapt2);
  free(jarsigner);
  free(ndk_version);
  free(sdk_env);
  free(java_env);
  free(kotlin_env);
  free(home_env);
  free(scrubbed_kotlin);
  free(scrubbed_path);
  free(readelf);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
