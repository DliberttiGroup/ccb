// Tests for release minification (opt-apk): a build with
// [build] minify = true runs R8 with CCB-generated keep rules, so the
// dex keeps the app's classes (including manifest/layout entry
// points outside the app package) while dropping unused stdlib
// classes, and a build with the flag off ships the full stdlib
// unchanged. Black-box: needs a real SDK toolchain, an r8 jar, and
// keytool, else prints a skip note and passes (same pattern as
// test_dex_contents.c). The r8 jar is found via CCB_R8_JAR, the
// setup-managed <sdk-home>/r8/r8.jar, the conventional download
// location ~/downloads/r8-9.5.28.jar (SHA-verified against the pinned
// jar ccb setup installs), or the copy shipped with cmdline-tools.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cache.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define OFF_PROJ B("minoff")
#define ON_PROJ B("minon")

// Pinned r8 jar (see ccb_cli.c CCB_R8_SHA256): only the conventional
// download location is held to it; the setup-managed and
// cmdline-tools copies are whatever version is installed.
#define R8_PINNED_SHA256 "38a9ffaaebbd17d1514e3ecbff5a85b989bb13c551b9e224c7409514eea84ff5"

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

// An r8 jar to build with, or NULL when none is discoverable.
static char *find_r8_jar(const char *sdk_home) {
  const char *env = getenv("CCB_R8_JAR");
  if (env != NULL && env[0] != '\0' && access(env, R_OK) == 0) {
    return ccb_xstrdup(env);
  }
  char *cand = ccb_path_join3(sdk_home, "r8", "r8.jar");
  if (access(cand, R_OK) == 0) {
    return cand;
  }
  free(cand);
  const char *home = getenv("HOME");
  if (home != NULL) {
    cand = ccb_path_join3(home, "downloads", "r8-9.5.28.jar");
    if (access(cand, R_OK) == 0) {
      char *hash = ccb_cache_hash_file(cand);
      if (hash != NULL && strcmp(hash, R8_PINNED_SHA256) == 0) {
        free(hash);
        return cand;
      }
      free(hash);
    }
    free(cand);
    cand = ccb_path_join3(sdk_home, "cmdline-tools/latest/lib", "r8.jar");
    if (access(cand, R_OK) == 0) {
      return cand;
    }
    free(cand);
  }
  return NULL;
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

// True when the file at path contains needle as bytes.
static int file_contains(const char *path, const char *needle) {
  char *data = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &data, &len) != CCB_OK) {
    return 0;
  }
  int found = 0;
  size_t nlen = strlen(needle);
  for (size_t off = 0; off + nlen <= len; off++) {
    if (memcmp(data + off, needle, nlen) == 0) {
      found = 1;
      break;
    }
  }
  free(data);
  return found;
}

// Total size of the .dex files under dir, or -1 when dir is unreadable.
static long dex_total_size(const char *dir) {
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(dir, ".dex", &files, &nfiles) != CCB_OK) {
    return -1;
  }
  long total = 0;
  for (size_t i = 0; i < nfiles; i++) {
    struct stat st;
    if (stat(files[i], &st) == 0) {
      total += (long)st.st_size;
    }
  }
  ccb_free_str_array(files, nfiles);
  return total;
}

static int run_ccb(const char *proj, char **env, char **args) {
  ccb_proc_result r;
  if (ccb_proc_run(args, proj, env, &r) != CCB_OK) {
    return 0;
  }
  int ok = r.exit_code == 0;
  if (!ok) {
    fprintf(stderr, "%s failed in %s:\n%s\n%s\n", args[1], proj, r.out, r.err);
  }
  ccb_proc_result_free(&r);
  return ok;
}

static int make_keystore(const char *keytool, const char *proj, const char *ks_path) {
  char *args[] = { (char *)keytool, "-genkeypair", "-keystore", (char *)ks_path,
    "-alias", "test", "-keyalg", "RSA", "-keysize", "2048", "-validity", "365",
    "-storepass", "testpass123", "-keypass", "testpass123",
    "-dname", "CN=Test", "-noprompt", NULL };
  ccb_proc_result r;
  if (ccb_proc_run(args, proj, NULL, &r) != CCB_OK) {
    return 0;
  }
  int ok = r.exit_code == 0;
  if (!ok) {
    fprintf(stderr, "keytool failed:\n%s\n%s\n", r.out, r.err);
  }
  ccb_proc_result_free(&r);
  return ok;
}

// Hello project with two entry points outside the app package, each
// referenced only from XML: ExtraActivity from the manifest and
// MyView from the layout. When minify is set the project is a
// release build with minify on (plus signing and the r8 jar); else a
// plain debug build. sdk_ver is the build-tools version ("36.0.0");
// the platform is its major number.
static void write_minify_project(const char *proj, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home, int minify, const char *r8_jar) {
  char *kt_dir = ccb_path_join(proj, "src/main/kotlin/com/example/hello");
  mkdir_or_fail(kt_dir);
  free(kt_dir);
  char *other_dir = ccb_path_join(proj, "src/main/kotlin/com/example/other");
  mkdir_or_fail(other_dir);
  free(other_dir);
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
  if (minify) {
    ccb_str_append(&s, "r8_jar = \"");
    ccb_str_append(&s, r8_jar);
    ccb_str_append(&s, "\"\n\n[build]\nvariant = \"release\"\nminify = true\n"
      "proguard_rules = \"rules.pro\"\n\n[signing.release]\nstore = \"test.keystore\"\n"
      "alias = \"test\"\nstore_password_env = \"CCB_TEST_STORE_PW\"\n"
      "key_password_env = \"CCB_TEST_KEY_PW\"\n");
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
    "    <activity android:name=\"com.example.other.ExtraActivity\" android:exported=\"false\" />\n"
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
    "  <com.example.other.MyView android:layout_width=\"match_parent\"\n"
    "    android:layout_height=\"match_parent\" />\n"
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
  char *extra = ccb_path_join(proj, "src/main/kotlin/com/example/other/ExtraActivity.kt");
  write_text(extra,
    "package com.example.other\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "\n"
    "class ExtraActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "  }\n"
    "}\n");
  free(extra);
  char *view = ccb_path_join(proj, "src/main/kotlin/com/example/other/MyView.kt");
  write_text(view,
    "package com.example.other\n"
    "\n"
    "import android.content.Context\n"
    "import android.util.AttributeSet\n"
    "import android.view.View\n"
    "\n"
    "class MyView @JvmOverloads constructor(\n"
    "  context: Context, attrs: AttributeSet? = null\n"
    ") : View(context, attrs)\n");
  free(view);
  if (minify) {
    char *rules = ccb_path_join(proj, "rules.pro");
    write_text(rules, "# no extra rules\n");
    free(rules);
  }
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "r8min");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping r8-minify checks\n");
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
  char *keytool = java_home != NULL ?
    ccb_path_join(java_home, "bin/keytool") : NULL;
  char *r8_jar = sdk_home != NULL ? find_r8_jar(sdk_home) : NULL;
  if (bt_ver == NULL || java_home == NULL || kotlin_home == NULL ||
      kotlinc == NULL || access(kotlinc, X_OK) != 0 ||
      keytool == NULL || access(keytool, X_OK) != 0 || r8_jar == NULL) {
    fprintf(stderr, "note: no runnable SDK toolchain, keytool, or r8 jar, skipping r8-minify checks\n");
    free(bin);
    free(sdk_home);
    free(java_home);
    free(kotlin_home);
    free(bt_ver);
    free(kotlinc);
    free(keytool);
    free(r8_jar);
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
  char *env[] = { sdk_env.buf, java_env.buf, kot_env.buf,
    "CCB_TEST_STORE_PW=testpass123", "CCB_TEST_KEY_PW=testpass123", NULL };

  ccb_remove_recursive(BASE);

  // 1. Flag off (debug): the dex ships the full stdlib, including a
  // class nothing uses (kotlin.text.Regex), plus the app's classes.
  write_minify_project(OFF_PROJ, sdk_home, bt_ver, kotlin_home, 0, NULL);
  char *build_args[] = { bin, "build", NULL };
  CHECK(run_ccb(OFF_PROJ, env, build_args));
  char *off_dex = ccb_path_join(OFF_PROJ, "build/intermediates/dex");
  CHECK(dex_contains(off_dex, "com/example/hello/MainActivity"));
  CHECK(dex_contains(off_dex, "kotlin/text/Regex"));
  long off_size = dex_total_size(off_dex);
  CHECK(off_size > 1000000);
  CHECK(ccb_path_exists(B("minoff/build/outputs/debug/app.apk")));
  free(off_dex);

  // 2. Flag on (release + minify): ccb check passes, the build
  // succeeds, and the dex is far smaller but still holds the app's
  // classes -- including the two entry points that only XML
  // references -- while the unused stdlib class is gone.
  write_minify_project(ON_PROJ, sdk_home, bt_ver, kotlin_home, 1, r8_jar);
  char *ks_path = ccb_path_join(ON_PROJ, "test.keystore");
  CHECK(make_keystore(keytool, ON_PROJ, ks_path));
  free(ks_path);
  char *check_args[] = { bin, "check", NULL };
  CHECK(run_ccb(ON_PROJ, env, check_args));
  CHECK(run_ccb(ON_PROJ, env, build_args));
  char *on_dex = ccb_path_join(ON_PROJ, "build/intermediates/dex");
  CHECK(dex_contains(on_dex, "com/example/hello/MainActivity"));
  CHECK(dex_contains(on_dex, "com/example/other/ExtraActivity"));
  CHECK(dex_contains(on_dex, "com/example/other/MyView"));
  CHECK(!dex_contains(on_dex, "kotlin/text/Regex"));
  long on_size = dex_total_size(on_dex);
  CHECK(on_size > 0 && off_size > 0 && on_size * 2 < off_size);
  CHECK(ccb_path_exists(B("minon/build/outputs/release/app.apk")));
  free(on_dex);

  // 3. The generated keep rules name exactly the entry points the
  // project file does not cover: the out-of-package manifest
  // activity and layout view, under the app-package rule.
  char *keep_path = ccb_path_join(ON_PROJ, "build/intermediates/r8-keep.pro");
  CHECK(file_contains(keep_path, "-keep class com.example.hello.** { *; }"));
  CHECK(file_contains(keep_path, "-keep class com.example.other.ExtraActivity { *; }"));
  CHECK(file_contains(keep_path, "-keep class com.example.other.MyView { *; }"));
  free(keep_path);

  free(bin);
  free(sdk_home);
  free(java_home);
  free(kotlin_home);
  free(bt_ver);
  free(keytool);
  free(r8_jar);
  ccb_str_free(&sdk_env);
  ccb_str_free(&java_env);
  ccb_str_free(&kot_env);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
