// Tests for the dex contents (bug-stdlib, bug-aar): a no-deps Kotlin
// project must ship kotlin-stdlib classes in the dex, and generated
// R classes (app + AAR library) must be in the dex too. Black-box:
// needs a real SDK toolchain, else prints a skip note and passes
// (same pattern as test_modules.c / test_aab.c). The toolchain is
// detected, not hardcoded to one SDK version, so the test runs
// wherever a usable SDK is installed.
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
#define LIB_PROJ B("libproj")
#define LIB_AAR B("lib.aar")
#define CACHE_PROJ B("cacheproj")

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

// Minimal Kotlin hello project (no [deps] at all). sdk_ver is the
// build-tools version ("36.0.0"); the platform is its major number.
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

// Minimal AAR: plain-text manifest + one attr in res/values. No
// classes.jar — the point is the generated library R class.
static void write_lib_aar(const char *path) {
  ccb_zip_writer w;
  CHECK(ccb_zip_writer_init(&w, path) == CCB_OK);
  const char *manifest =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "    package=\"com.example.lib\" />\n";
  CHECK(ccb_zip_writer_add_data(&w, "AndroidManifest.xml", manifest, strlen(manifest)) == CCB_OK);
  const char *values =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources><attr name=\"libAttr\" format=\"string\" /></resources>\n";
  CHECK(ccb_zip_writer_add_data(&w, "res/values/values.xml", values, strlen(values)) == CCB_OK);
  CHECK(ccb_zip_writer_finish(&w) == CCB_OK);
}

// .dex files directly under dir (malloc'd list; the caller frees).
static char **list_cache_dex(const char *dir, size_t *n) {
  char **files = NULL;
  *n = 0;
  if (ccb_collect_files(dir, ".dex", &files, n) != CCB_OK) {
    return NULL;
  }
  return files;
}

// The single .dex in dir, malloc'd, or NULL when there is not
// exactly one. The caller frees the result.
static char *single_cache_dex(const char *dir) {
  size_t n = 0;
  char **files = list_cache_dex(dir, &n);
  if (files == NULL || n != 1) {
    ccb_free_str_array(files, n);
    return NULL;
  }
  char *one = ccb_xstrdup(files[0]);
  ccb_free_str_array(files, n);
  return one;
}

// Rewrite jar src as dst with the same entries (directory entries
// skipped: d8 does not need them) plus one dummy entry, so the jar
// content — and therefore its hash — changes but every class stays.
static void rewrite_jar_with_dummy(const char *src, const char *dst) {
  ccb_zip_archive *a = NULL;
  CHECK(ccb_zip_open(src, &a) == CCB_OK);
  ccb_zip_writer w;
  CHECK(ccb_zip_writer_init(&w, dst) == CCB_OK);
  for (size_t i = 0; i < a->count; i++) {
    const ccb_zip_entry *e = &a->entries[i];
    size_t nlen = strlen(e->name);
    if (nlen > 0 && e->name[nlen - 1] == '/') {
      continue;
    }
    void *data = NULL;
    size_t len = 0;
    CHECK(ccb_zip_read_entry(a, e, &data, &len) == CCB_OK);
    CHECK(ccb_zip_writer_add_data(&w, e->name, data, len) == CCB_OK);
    free(data);
  }
  CHECK(ccb_zip_writer_add_data(&w, "META-INF/ccb-test.txt", "x", 1) == CCB_OK);
  CHECK(ccb_zip_writer_finish(&w) == CCB_OK);
  ccb_zip_close(a);
}

// A Kotlin home that behaves like real_home but ships the rewritten
// stdlib jar. bin/kotlinc is a COPY of the real script: the script
// resolves its home from its own path and follows symlinks, so a
// symlinked script would point back at the real home. lib/ entries
// are symlinks to the real ones, except the stdlib jar.
static void write_fake_kotlin_home(const char *fake, const char *real_home) {
  char *bin_dir = ccb_path_join(fake, "bin");
  mkdir_or_fail(bin_dir);
  free(bin_dir);
  char *real_kotlinc = ccb_path_join3(real_home, "bin", "kotlinc");
  char *fake_kotlinc = ccb_path_join3(fake, "bin", "kotlinc");
  char *script = NULL;
  size_t slen = 0;
  CHECK(ccb_read_file(real_kotlinc, &script, &slen) == CCB_OK);
  CHECK(ccb_write_file(fake_kotlinc, script, slen) == CCB_OK);
  CHECK(chmod(fake_kotlinc, 0755) == 0);
  free(script);
  free(real_kotlinc);
  free(fake_kotlinc);
  char *lib_dir = ccb_path_join(fake, "lib");
  mkdir_or_fail(lib_dir);
  char *real_lib = ccb_path_join(real_home, "lib");
  DIR *d = opendir(real_lib);
  CHECK(d != NULL);
  struct dirent *e = NULL;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') {
      continue;
    }
    char *target = ccb_path_join(real_lib, e->d_name);
    char *link = ccb_path_join(lib_dir, e->d_name);
    if (strcmp(e->d_name, "kotlin-stdlib.jar") == 0) {
      rewrite_jar_with_dummy(target, link);
    } else {
      CHECK(symlink(target, link) == 0);
    }
    free(target);
    free(link);
  }
  closedir(d);
  free(real_lib);
  free(lib_dir);
}

static int run_build(char *bin, const char *proj, char **env) {
  char *args[] = { bin, "build", NULL };
  ccb_proc_result r;
  if (ccb_proc_run(args, proj, env, &r) != CCB_OK) {
    return 0;
  }
  int ok = r.exit_code == 0;
  if (!ok) {
    fprintf(stderr, "build failed in %s:\n%s\n%s\n", proj, r.out, r.err);
  }
  ccb_proc_result_free(&r);
  return ok;
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "dex");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping dex-contents checks\n");
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
    fprintf(stderr, "note: no runnable SDK toolchain, skipping dex-contents checks\n");
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

  // 1. No-deps Kotlin project: the dex must contain the stdlib
  // (bug-stdlib) and the app's own R class (bug-aar, app side).
  // The hello app has only layout + string resources, so its R
  // has R$layout (aapt2 generates inner classes only for resource
  // types the app actually has — there is no R$attr here).
  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, NULL);
  CHECK(run_build(bin, HELLO_PROJ, env));
  char *hello_dex = ccb_path_join(HELLO_PROJ, "build/intermediates/dex");
  CHECK(dex_contains(hello_dex, "kotlin/jvm/internal/Intrinsics"));
  CHECK(dex_contains(hello_dex, "com/example/hello/R$layout"));
  free(hello_dex);

  // 2. Same project + a local AAR with one attr: the dex must
  // contain the library R class (bug-aar, library side).
  write_lib_aar(LIB_AAR);
  write_hello_project(LIB_PROJ, sdk_home, bt_ver, kotlin_home, LIB_AAR);
  CHECK(run_build(bin, LIB_PROJ, env));
  char *lib_dex = ccb_path_join(LIB_PROJ, "build/intermediates/dex");
  CHECK(dex_contains(lib_dex, "com/example/lib/R$attr"));
  CHECK(dex_contains(lib_dex, "kotlin/jvm/internal/Intrinsics"));
  free(lib_dex);

  // 3. Stdlib dex cache (opt-dex / opt-cache), under a fresh
  // XDG_CACHE_HOME so the test never touches the real cache.
  char *cache_home = ccb_path_join(BASE, "xdg");
  ccb_str xdg_env;
  ccb_str_init(&xdg_env);
  ccb_str_append(&xdg_env, "XDG_CACHE_HOME=");
  ccb_str_append(&xdg_env, cache_home);
  char *env2[] = { sdk_env.buf, java_env.buf, kot_env.buf, xdg_env.buf, NULL };
  char *cache_dex_dir = ccb_path_join3(cache_home, "ccb", "dex-stdlib");
  char *cache_proj_dex = ccb_path_join(CACHE_PROJ, "build/intermediates/dex");
  char *cache_proj_build = ccb_path_join(CACHE_PROJ, "build");

  // (a) A clean build populates exactly one cache entry, and the
  // dex still has the stdlib.
  write_hello_project(CACHE_PROJ, sdk_home, bt_ver, kotlin_home, NULL);
  CHECK(run_build(bin, CACHE_PROJ, env2));
  char *dex1 = single_cache_dex(cache_dex_dir);
  CHECK(dex1 != NULL);
  // NULL-guarded throughout: when the cache entry is missing the
  // test must FAIL, not crash on a NULL hash (that once segfaulted
  // here and skipped cleanup, leaving the tmpdir behind).
  char *hash1 = dex1 != NULL ? ccb_cache_hash_file(dex1) : NULL;
  CHECK(hash1 != NULL);
  struct stat st1;
  memset(&st1, 0, sizeof(st1));
  CHECK(dex1 != NULL && stat(dex1, &st1) == 0);
  CHECK(dex_contains(cache_proj_dex, "kotlin/jvm/internal/Intrinsics"));

  // (b) A second clean build reuses the entry: same file, same
  // hash, same mtime — d8 is not re-invoked for the stdlib.
  ccb_remove_recursive(cache_proj_build);
  CHECK(run_build(bin, CACHE_PROJ, env2));
  char *dex1b = single_cache_dex(cache_dex_dir);
  CHECK(dex1 != NULL && dex1b != NULL && strcmp(dex1b, dex1) == 0);
  char *hash1b = dex1b != NULL ? ccb_cache_hash_file(dex1b) : NULL;
  struct stat st1b;
  memset(&st1b, 0, sizeof(st1b));
  CHECK(dex1b != NULL && stat(dex1b, &st1b) == 0);
  CHECK(hash1 != NULL && hash1b != NULL && strcmp(hash1, hash1b) == 0);
  CHECK(st1.st_mtime == st1b.st_mtime);
  CHECK(dex_contains(cache_proj_dex, "kotlin/jvm/internal/Intrinsics"));

  // (c) A changed stdlib jar invalidates the cache: a second
  // entry appears, the first one untouched, the dex still good.
  char *fake_home = ccb_path_join(BASE, "kotlinc2");
  write_fake_kotlin_home(fake_home, kotlin_home);
  ccb_str fake_kot_env;
  ccb_str_init(&fake_kot_env);
  ccb_str_append(&fake_kot_env, "KOTLIN_HOME=");
  ccb_str_append(&fake_kot_env, fake_home);
  char *env3[] = { sdk_env.buf, java_env.buf, fake_kot_env.buf, xdg_env.buf, NULL };
  write_hello_project(CACHE_PROJ, sdk_home, bt_ver, fake_home, NULL);
  ccb_remove_recursive(cache_proj_build);
  CHECK(run_build(bin, CACHE_PROJ, env3));
  size_t ndex = 0;
  char **dexes = list_cache_dex(cache_dex_dir, &ndex);
  CHECK(dexes != NULL && ndex == 2);
  ccb_free_str_array(dexes, ndex);
  char *hash1c = dex1 != NULL ? ccb_cache_hash_file(dex1) : NULL;
  CHECK(hash1 != NULL && hash1c != NULL && strcmp(hash1, hash1c) == 0);
  CHECK(dex_contains(cache_proj_dex, "kotlin/jvm/internal/Intrinsics"));

  free(dex1);
  free(dex1b);
  free(hash1);
  free(hash1b);
  free(hash1c);
  free(cache_home);
  free(cache_dex_dir);
  free(cache_proj_dex);
  free(cache_proj_build);
  free(fake_home);
  ccb_str_free(&xdg_env);
  ccb_str_free(&fake_kot_env);

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
