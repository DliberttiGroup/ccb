// Tests for d8 heap sizing (opt-d8-heap): the -JXmx flag passed to
// d8 must scale with the dex input size, never drop below the floor,
// and never exceed the cap. Two halves: the pure formula
// ccb_d8_heap_mb (exact values, no toolchain needed) and a dry-run
// build (proves the flag is really on the d8 command line and grows
// when the inputs grow). The dry-run half needs a real SDK toolchain
// to resolve, else prints a skip note and passes (same pattern as
// test_dex_contents.c). A dry-run executes nothing, so this test
// runs no JVM and stays fast.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include "ccb_pipeline.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define HELLO_PROJ B("hello")

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

// Highest build-tools version under sdk_home that has an aapt2, or
// NULL. The version string (e.g. "36.0.0") is malloc'd.
static char *find_build_tools(const char *sdk_home) {
  char *bt_dir = ccb_path_join(sdk_home, "build-tools");
  DIR *d = opendir(bt_dir);
  if (d == NULL) {
    free(bt_dir);
    return NULL;
  }
  char *best = NULL;
  struct dirent *e = NULL;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') {
      continue;
    }
    // Only build-tools CCB accepts (major 34..36): the runner image
    // ships newer ones (e.g. 37) with no matching platform, and
    // deriving compile_sdk from them fails config load (CCB-E1005).
    int major = 0;
    const char *mp = e->d_name;
    if (*mp < '0' || *mp > '9') {
      continue;
    }
    while (*mp >= '0' && *mp <= '9') {
      major = major * 10 + (*mp - '0');
      mp++;
    }
    if (major < 34 || major > 36) {
      continue;
    }
    char *cand = ccb_path_join3(bt_dir, e->d_name, "aapt2");
    int ok = access(cand, X_OK) == 0;
    free(cand);
    if (ok && (best == NULL || strcmp(e->d_name, best) > 0)) {
      free(best);
      best = ccb_xstrdup(e->d_name);
    }
  }
  closedir(d);
  free(bt_dir);
  return best;
}

// Minimal Kotlin hello project (no [deps] at all). sdk_ver is the
// build-tools version ("36.0.0"); the platform is its major number.
static void write_hello_project(const char *proj, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home) {
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

// Run `ccb build --dry-run` in proj, capturing stdout. Returns 1
// when the run exits 0, else 0 (and prints the output for diagnosis).
static int run_dry_run(char *bin, const char *proj, char **env, char **out) {
  char *args[] = { bin, "build", "--dry-run", NULL };
  ccb_proc_result r;
  if (ccb_proc_run(args, proj, env, &r) != CCB_OK) {
    return 0;
  }
  int ok = r.exit_code == 0;
  if (!ok) {
    fprintf(stderr, "dry-run failed in %s:\n%s\n%s\n", proj, r.out, r.err);
  }
  *out = ccb_xstrdup(r.out);
  ccb_proc_result_free(&r);
  return ok;
}

// Heap MB from the d8 line in dry-run output, or -1 when the line
// has no -JXmx flag. The flag must sit on the d8 command line
// itself: after the d8 path and before --min-api.
static long dry_run_d8_heap(const char *out) {
  const char *flag = strstr(out, "-JXmx");
  if (flag == NULL) {
    return -1;
  }
  const char *line = flag;
  while (line > out && line[-1] != '\n') {
    line--;
  }
  const char *end = strchr(flag, '\n');
  size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
  if (len >= 4096) {
    return -1;
  }
  char buf[4096];
  memcpy(buf, line, len);
  buf[len] = '\0';
  const char *d8 = strstr(buf, "/d8 ");
  const char *j = strstr(buf, "-JXmx");
  const char *api = strstr(buf, "--min-api");
  if (d8 == NULL || j == NULL || api == NULL || !(d8 < j && j < api)) {
    return -1;
  }
  long heap = 0;
  if (sscanf(j + 5, "%ld", &heap) != 1) {
    return -1;
  }
  return heap;
}

int main(int argc, char **argv) {
  // 1. The formula itself: floor, scaling, cap.
  CHECK(ccb_d8_heap_mb(0) == CCB_D8_HEAP_FLOOR_MB);
  CHECK(ccb_d8_heap_mb(1) == CCB_D8_HEAP_FLOOR_MB);
  CHECK(ccb_d8_heap_mb(1024 * 1024 - 1) == CCB_D8_HEAP_FLOOR_MB);
  CHECK(ccb_d8_heap_mb(1024 * 1024) == CCB_D8_HEAP_FLOOR_MB + 16);
  // Showcase warm inputs (2,639,430 bytes: app classes + cached
  // stdlib dex) -> 2 MiB -> 288; cold (1,912,693 bytes: app classes
  // + stdlib jar) -> 1 MiB -> 272.
  CHECK(ccb_d8_heap_mb(2639430) == 288);
  CHECK(ccb_d8_heap_mb(1912693) == 272);
  // Synthetic big input (7,143,205 bytes: 10,000 classes + stdlib
  // dex) -> 6 MiB -> 352.
  CHECK(ccb_d8_heap_mb(7143205) == 352);
  // Larger input -> larger heap.
  CHECK(ccb_d8_heap_mb(10ULL * 1024 * 1024) >
    ccb_d8_heap_mb(1024 * 1024));
  CHECK(ccb_d8_heap_mb(40ULL * 1024 * 1024) >
    ccb_d8_heap_mb(10ULL * 1024 * 1024));
  // Cap: 47 MiB -> 1008, 48 MiB -> cap, anything larger stays capped.
  CHECK(ccb_d8_heap_mb(47ULL * 1024 * 1024) == 1008);
  CHECK(ccb_d8_heap_mb(48ULL * 1024 * 1024) == CCB_D8_HEAP_CAP_MB);
  CHECK(ccb_d8_heap_mb(1024ULL * 1024 * 1024) == CCB_D8_HEAP_CAP_MB);

  // 2. The flag on the real d8 command line (dry-run: nothing
  // executes, no JVM runs). Needs a toolchain to resolve.
  ccb_test_tmpdir(BASE, sizeof BASE, "d8heap");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping dry-run heap checks\n");
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }
  char *sdk_home = env_or_home("ANDROID_HOME", "Android/Sdk");
  if (sdk_home == NULL) {
    sdk_home = env_or_home("ANDROID_HOME", "android-sdk");
  }
  char *java_home = env_or_home("JAVA_HOME", "jdk17");
  char *kotlin_home = env_or_home("KOTLIN_HOME", "kotlin-2.4.21/kotlinc");
  char *bt_ver = sdk_home != NULL ? find_build_tools(sdk_home) : NULL;
  char *kotlinc = kotlin_home != NULL ?
    ccb_path_join(kotlin_home, "bin/kotlinc") : NULL;
  if (bt_ver == NULL || java_home == NULL || kotlin_home == NULL ||
      kotlinc == NULL || access(kotlinc, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping dry-run heap checks\n");
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

  // Fresh XDG cache so no cached stdlib dex exists: the stdlib
  // input is the jar, whose size is stable across runs here.
  char *cache_home = ccb_path_join(BASE, "xdg");
  ccb_str sdk_env, java_env, kot_env, xdg_env;
  ccb_str_init(&sdk_env);
  ccb_str_init(&java_env);
  ccb_str_init(&kot_env);
  ccb_str_init(&xdg_env);
  ccb_str_append(&sdk_env, "ANDROID_HOME=");
  ccb_str_append(&sdk_env, sdk_home);
  ccb_str_append(&java_env, "JAVA_HOME=");
  ccb_str_append(&java_env, java_home);
  ccb_str_append(&kot_env, "KOTLIN_HOME=");
  ccb_str_append(&kot_env, kotlin_home);
  ccb_str_append(&xdg_env, "XDG_CACHE_HOME=");
  ccb_str_append(&xdg_env, cache_home);
  char *env[] = { sdk_env.buf, java_env.buf, kot_env.buf, xdg_env.buf, NULL };

  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home);

  // A dummy .class file in the classes dir, so the dex step runs at
  // all: on a fresh tree there are no compiled classes and dex skips
  // ("no classes or jars"). The dummy is never executed (dry-run
  // only); its content is irrelevant, only its size counts.
  char *classes_dir = ccb_path_join(HELLO_PROJ,
    "build/intermediates/classes/com/example/hello");
  mkdir_or_fail(classes_dir);
  free(classes_dir);
  char *dummy_class = ccb_path_join(HELLO_PROJ,
    "build/intermediates/classes/com/example/hello/Dummy.class");
  char zeros_small[1024];
  memset(zeros_small, 0, sizeof(zeros_small));
  CHECK(ccb_write_file(dummy_class, zeros_small, sizeof(zeros_small)) == CCB_OK);
  free(dummy_class);

  // (a) Small inputs (1 KiB of classes, stdlib jar only):
  // near the floor. The exact value depends on the installed
  // stdlib jar size (1-2 MiB -> 272-288), so assert the range.
  char *out_small = NULL;
  CHECK(run_dry_run(bin, HELLO_PROJ, env, &out_small));
  long small = out_small != NULL ? dry_run_d8_heap(out_small) : -1;
  CHECK(small >= CCB_D8_HEAP_FLOOR_MB && small <= 320);
  free(out_small);

  // (b) Same project plus a 40 MiB dummy class file in the classes
  // dir: the heap must grow a lot (40 MiB * 16 = +640) and stay
  // under the cap. The dummy is never executed (dry-run only).
  char *big_class = ccb_path_join(HELLO_PROJ,
    "build/intermediates/classes/com/example/hello/Big.class");
  size_t big_bytes = 40 * 1024 * 1024;
  char *zeros = calloc(1, big_bytes);
  CHECK(zeros != NULL);
  if (zeros != NULL) {
    CHECK(ccb_write_file(big_class, zeros, big_bytes) == CCB_OK);
    free(zeros);
  }
  free(big_class);
  char *out_big = NULL;
  CHECK(run_dry_run(bin, HELLO_PROJ, env, &out_big));
  long big = out_big != NULL ? dry_run_d8_heap(out_big) : -1;
  CHECK(big >= 880 && big <= 960);
  CHECK(big > small + 500);
  CHECK(big <= CCB_D8_HEAP_CAP_MB);
  free(out_big);

  free(bin);
  free(sdk_home);
  free(java_home);
  free(kotlin_home);
  free(bt_ver);
  free(cache_home);
  ccb_str_free(&sdk_env);
  ccb_str_free(&java_env);
  ccb_str_free(&kot_env);
  ccb_str_free(&xdg_env);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
