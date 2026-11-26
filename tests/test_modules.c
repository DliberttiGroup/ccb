// Tests for multi-module projects (requirement.md Q6): app + 2 library
// modules end to end, cycle + unknown-module errors, single-module
// behavior unchanged. Black-box: needs a real SDK toolchain, else
// prints a skip note and passes (same pattern as test_aab.c). The
// cycle/unknown checks need only the binary, so they run even without
// a toolchain.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include "ccb_hash.h"
#include "ccb_zip.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define PROJ B("proj")
#define APK_OUT B("proj/build/outputs/debug/app.apk")
#define DEX_DIR B("proj/build/intermediates/dex")
#define CYCLE_PROJ B("cycle")
#define UNKNOWN_PROJ B("unknown")
#define SINGLE_PROJ B("single")
#define SINGLE_APK B("single/build/outputs/debug/app.apk")

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

static int zip_has(const char *zip_path, const char *name) {
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(zip_path, &a) != CCB_OK) {
    return 0;
  }
  int found = ccb_zip_find(a, name) != NULL;
  ccb_zip_close(a);
  return found;
}

// True when any zip entry's basename is R.class or starts with R$.
static int zip_has_r_class(const char *zip_path) {
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(zip_path, &a) != CCB_OK) {
    return 0;
  }
  int found = 0;
  for (size_t i = 0; i < a->count; i++) {
    const char *base = strrchr(a->entries[i].name, '/');
    base = base == NULL ? a->entries[i].name : base + 1;
    if (strcmp(base, "R.class") == 0 || strncmp(base, "R$", 2) == 0) {
      found = 1;
      break;
    }
  }
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

static int str_contains(const char *hay, const char *needle) {
  return hay != NULL && needle != NULL && strstr(hay, needle) != NULL;
}

static void write_app_scaffold(const char *proj) {
  char *p = NULL;
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, proj);
  ccb_str_append(&s, "/src/main/kotlin/com/example/hello");
  p = ccb_str_steal(&s);
  mkdir_or_fail(p);
  free(p);
  ccb_str_init(&s);
  ccb_str_append(&s, proj);
  ccb_str_append(&s, "/src/main/res/values");
  p = ccb_str_steal(&s);
  mkdir_or_fail(p);
  free(p);
  ccb_str_init(&s);
  ccb_str_append(&s, proj);
  ccb_str_append(&s, "/src/main/res/layout");
  p = ccb_str_steal(&s);
  mkdir_or_fail(p);
  free(p);
  ccb_str path;
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/ccb.toml");
  write_text(path.buf,
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
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/src/main/AndroidManifest.xml");
  write_text(path.buf,
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
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/src/main/kotlin/com/example/hello/MainActivity.kt");
  write_text(path.buf,
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
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/src/main/res/values/strings.xml");
  write_text(path.buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"app_name\">hello</string>\n"
    "  <string name=\"hello\">Hello from CCB!</string>\n"
    "</resources>\n");
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/src/main/res/layout/activity_main.xml");
  write_text(path.buf,
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
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/src/main/res/values/themes.xml");
  write_text(path.buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <style name=\"AppTheme\" parent=\"@android:style/Theme.Material.Light.NoActionBar\" />\n"
    "</resources>\n");
  ccb_str_free(&path);
}

// A minimal project dir with a ccb.toml whose [modules] section is
// given verbatim, plus one manifest per module dir listed.
static void write_graph_project(const char *proj, const char *toml_modules,
    const char *const *mod_dirs, const char *const *mod_pkgs, size_t nmods) {
  mkdir_or_fail(proj);
  ccb_str path;
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/ccb.toml");
  ccb_str t;
  ccb_str_init(&t);
  ccb_str_append(&t,
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"graph\"\n"
    "package = \"com.example.graph\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"app/src/main/AndroidManifest.xml\"\n");
  ccb_str_append(&t, toml_modules);
  write_text(path.buf, t.buf);
  ccb_str_free(&t);
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/app/src/main");
  mkdir_or_fail(path.buf);
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/app/src/main/AndroidManifest.xml");
  write_text(path.buf,
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" "
    "package=\"com.example.graph\"><application /></manifest>\n");
  ccb_str_free(&path);
  for (size_t i = 0; i < nmods; i++) {
    ccb_str_init(&path);
    ccb_str_append(&path, proj);
    ccb_str_append(&path, "/");
    ccb_str_append(&path, mod_dirs[i]);
    ccb_str_append(&path, "/src/main");
    mkdir_or_fail(path.buf);
    ccb_str_free(&path);
    ccb_str_init(&path);
    ccb_str_append(&path, proj);
    ccb_str_append(&path, "/");
    ccb_str_append(&path, mod_dirs[i]);
    ccb_str_append(&path, "/src/main/AndroidManifest.xml");
    ccb_str m;
    ccb_str_init(&m);
    ccb_str_append(&m,
      "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"");
    ccb_str_append(&m, mod_pkgs[i]);
    ccb_str_append(&m, "\" />\n");
    write_text(path.buf, m.buf);
    ccb_str_free(&m);
    ccb_str_free(&path);
  }
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "mods");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping module checks\n");
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }

  // Cycle + unknown-module errors need only the binary (config load
  // fails before any toolchain resolution).
  ccb_remove_recursive(BASE);
  {
    const char *dirs[] = { "liba", "libb" };
    const char *pkgs[] = { "com.example.a", "com.example.b" };
    write_graph_project(CYCLE_PROJ,
      "[modules.a]\ndir = \"liba\"\ndepends = [\"b\"]\n\n"
      "[modules.b]\ndir = \"libb\"\ndepends = [\"a\"]\n",
      dirs, pkgs, 2);
    ccb_proc_result r;
    char *args[] = { bin, "check", NULL };
    CHECK(ccb_proc_run(args, CYCLE_PROJ, NULL, &r) == CCB_OK);
    CHECK(r.exit_code != 0);
    CHECK(str_contains(r.err, "CCB-E1010"));
    CHECK(str_contains(r.err, "a -> b -> a"));
    if (r.exit_code == 0) {
      fprintf(stderr, "cycle check unexpectedly passed: %s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
  }
  {
    const char *dirs[] = { "libcore" };
    const char *pkgs[] = { "com.example.core" };
    write_graph_project(UNKNOWN_PROJ,
      "[modules.core]\ndir = \"libcore\"\ndepends = [\"nope\"]\n",
      dirs, pkgs, 1);
    ccb_proc_result r;
    char *args[] = { bin, "check", NULL };
    CHECK(ccb_proc_run(args, UNKNOWN_PROJ, NULL, &r) == CCB_OK);
    CHECK(r.exit_code != 0);
    CHECK(str_contains(r.err, "CCB-E1011"));
    CHECK(str_contains(r.err, "nope"));
    ccb_proc_result_free(&r);
  }

  char *sdk_home = tool_home("ANDROID_HOME", "android-sdk");
  char *java_home = tool_home("JAVA_HOME", "jdk17");
  char *kotlin_home = tool_home("KOTLIN_HOME", "kotlin-2.4.21");
  char *aapt2 = sdk_home == NULL ? NULL
    : ccb_path_join3(sdk_home, "build-tools/35.0.0", "aapt2");
  char *jarsigner = java_home == NULL ? NULL : ccb_path_join(java_home, "bin/jarsigner");
  if (bin == NULL || aapt2 == NULL || access(aapt2, X_OK) != 0 ||
      jarsigner == NULL || access(jarsigner, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping black-box module checks\n");
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

  // Multi-module fixture: app + base + core, core depends on base.
  // The app module reuses the single-module scaffold under app/.
  ccb_remove_recursive(PROJ);
  mkdir_or_fail(B("proj/app/src/main/kotlin/com/example/hello"));
  mkdir_or_fail(B("proj/app/src/main/res/values"));
  mkdir_or_fail(B("proj/app/src/main/res/layout"));
  mkdir_or_fail(B("proj/libbase/src/main/kotlin/com/example/base"));
  mkdir_or_fail(B("proj/libbase/src/main/res/values"));
  mkdir_or_fail(B("proj/libcore/src/main/kotlin/com/example/core"));
  mkdir_or_fail(B("proj/libcore/src/main/res/values"));
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
    "manifest = \"app/src/main/AndroidManifest.xml\"\n"
    "res = \"app/src/main/res\"\n"
    "kotlin = \"app/src/main/kotlin\"\n"
    "\n"
    "[modules.base]\n"
    "dir = \"libbase\"\n"
    "\n"
    "[modules.core]\n"
    "dir = \"libcore\"\n"
    "depends = [\"base\"]\n");
  write_text(B("proj/app/src/main/AndroidManifest.xml"),
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
  write_text(B("proj/app/src/main/kotlin/com/example/hello/MainActivity.kt"),
    "package com.example.hello\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "import com.example.core.CoreHelper\n"
    "\n"
    "class MainActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    setContentView(R.layout.activity_main)\n"
    "    val unused = CoreHelper.greeting()\n"
    "  }\n"
    "}\n");
  write_text(B("proj/app/src/main/res/values/strings.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"app_name\">hello</string>\n"
    "  <string name=\"hello\">Hello from CCB!</string>\n"
    "</resources>\n");
  write_text(B("proj/app/src/main/res/layout/activity_main.xml"),
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
  write_text(B("proj/app/src/main/res/values/themes.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <style name=\"AppTheme\" parent=\"@android:style/Theme.Material.Light.NoActionBar\" />\n"
    "</resources>\n");
  write_text(B("proj/libbase/src/main/AndroidManifest.xml"),
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  package=\"com.example.base\">\n"
    "  <uses-permission android:name=\"android.permission.VIBRATE\" />\n"
    "  <application>\n"
    "    <activity android:name=\".LibActivity\" android:exported=\"false\" />\n"
    "  </application>\n"
    "</manifest>\n");
  write_text(B("proj/libbase/src/main/kotlin/com/example/base/BaseHelper.kt"),
    "package com.example.base\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "\n"
    "object BaseHelper {\n"
    "  fun hello(): String {\n"
    "    return \"base\"\n"
    "  }\n"
    "}\n"
    "\n"
    "class LibActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    val text = getString(R.string.base_text)\n"
    "    val unused = text\n"
    "  }\n"
    "}\n");
  write_text(B("proj/libbase/src/main/res/values/strings.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"base_text\">from base</string>\n"
    "</resources>\n");
  write_text(B("proj/libcore/src/main/AndroidManifest.xml"),
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  package=\"com.example.core\">\n"
    "</manifest>\n");
  write_text(B("proj/libcore/src/main/kotlin/com/example/core/CoreHelper.kt"),
    "package com.example.core\n"
    "\n"
    "import com.example.base.BaseHelper\n"
    "import com.example.base.R as BaseR\n"
    "\n"
    "object CoreHelper {\n"
    "  fun greeting(): String {\n"
    "    return BaseHelper.hello() + \" core\"\n"
    "  }\n"
    "  fun baseResId(): Int {\n"
    "    return BaseR.string.base_text\n"
    "  }\n"
    "}\n");
  write_text(B("proj/libcore/src/main/res/values/strings.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"core_text\">from core</string>\n"
    "</resources>\n");

  ccb_proc_result r;
  char *build_args[] = { bin, "build", NULL };
  CHECK(ccb_proc_run(build_args, PROJ, env_extra, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  if (r.exit_code != 0) {
    fprintf(stderr, "module build failed: %s\n%s\n", r.out, r.err);
  }
  ccb_proc_result_free(&r);
  CHECK(ccb_path_exists(APK_OUT));
  CHECK(zip_has(APK_OUT, "classes.dex"));
  CHECK(zip_has(APK_OUT, "resources.arsc"));
  // Merged manifest: library activity + permission visible.
  {
    char *xml_args[] = { aapt2, "dump", "xmltree", (char *)APK_OUT,
      "--file", "AndroidManifest.xml", NULL };
    CHECK(ccb_proc_run(xml_args, PROJ, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "com.example.base.LibActivity"));
    CHECK(str_contains(r.out, "android.permission.VIBRATE"));
    ccb_proc_result_free(&r);
  }
  // Library classes dexed; module AARs well-formed; no R classes in
  // module jars (the app link generates the real ones).
  CHECK(dex_contains(DEX_DIR, "Lcom/example/base/LibActivity;"));
  CHECK(dex_contains(DEX_DIR, "Lcom/example/core/CoreHelper;"));
  CHECK(zip_has(B("proj/build/intermediates/modules/base/base.aar"), "classes.jar"));
  CHECK(zip_has(B("proj/build/intermediates/modules/base/base.aar"), "AndroidManifest.xml"));
  CHECK(zip_has(B("proj/build/intermediates/modules/base/base.aar"), "res/values/strings.xml"));
  CHECK(zip_has(B("proj/build/intermediates/modules/base/classes.jar"),
    "com/example/base/BaseHelper.class"));
  CHECK(!zip_has_r_class(B("proj/build/intermediates/modules/base/classes.jar")));
  CHECK(!zip_has_r_class(B("proj/build/intermediates/modules/core/classes.jar")));
  // Explain + dry-run plan on a fresh tree (no intermediates yet).
  CHECK(ccb_remove_recursive(B("proj/build")) == CCB_OK);
  {
    char *explain_args[] = { bin, "build", "--explain", NULL };
    CHECK(ccb_proc_run(explain_args, PROJ, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "module base:"));
    CHECK(str_contains(r.out, "module core:"));
    ccb_proc_result_free(&r);
    char *dry_args[] = { bin, "build", "--dry-run", NULL };
    CHECK(ccb_proc_run(dry_args, PROJ, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "libbase"));
    CHECK(str_contains(r.out, "libcore"));
    ccb_proc_result_free(&r);
  }
  // Rebuild for real, then incremental: a semantic library change
  // must rebuild the dependent module, the app code, and dex.
  CHECK(ccb_proc_run(build_args, PROJ, env_extra, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  ccb_proc_result_free(&r);
  {
    char *noop_args[] = { bin, "build", NULL };
    CHECK(ccb_proc_run(noop_args, PROJ, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "up to date"));
    ccb_proc_result_free(&r);
  }
  write_text(B("proj/libbase/src/main/kotlin/com/example/base/BaseHelper.kt"),
    "package com.example.base\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "\n"
    "object BaseHelper {\n"
    "  fun hello(): String {\n"
    "    return \"base2\"\n"
    "  }\n"
    "}\n"
    "\n"
    "class LibActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    val text = getString(R.string.base_text)\n"
    "    val unused = text\n"
    "  }\n"
    "}\n");
  CHECK(ccb_proc_run(build_args, PROJ, env_extra, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  ccb_proc_result_free(&r);
  CHECK(dex_contains(DEX_DIR, "base2"));

  // Single-module project: no modules phase, byte-identical rebuilds.
  ccb_remove_recursive(SINGLE_PROJ);
  write_app_scaffold(SINGLE_PROJ);
  CHECK(ccb_proc_run(build_args, SINGLE_PROJ, env_extra, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  CHECK(!str_contains(r.out, "modules"));
  ccb_proc_result_free(&r);
  char h1[65];
  char h2[65];
  CHECK(ccb_sha256_file(SINGLE_APK, h1) == CCB_OK);
  CHECK(ccb_remove_recursive(B("single/build")) == CCB_OK);
  CHECK(ccb_proc_run(build_args, SINGLE_PROJ, env_extra, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  ccb_proc_result_free(&r);
  CHECK(ccb_sha256_file(SINGLE_APK, h2) == CCB_OK);
  CHECK_STR_EQ(h1, h2);

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
