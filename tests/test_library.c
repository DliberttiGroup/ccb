// Tests for standalone library projects (requirement.md §4.3/§5.11):
// a [library] project builds an .aar (classes.jar, manifest, res,
// R.txt); an app consumes it from a local path (project dir or built
// .aar) and from Maven coordinates; app resources win collisions with
// a warning; `ccb check` reports each library. Black-box: needs a
// real SDK toolchain, else prints a skip note and passes (same pattern
// as test_modules.c). The config-error checks need only the binary, so
// they run even without a toolchain.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_config.h"
#include "ccb_diag.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_proc.h"
#include "ccb_str.h"
#include "ccb_zip.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define LIB B("lib")
#define LIB_AAR B("lib/build/mylib-1.2.aar")
#define APP B("app")
#define APP_APK B("app/build/outputs/debug/app.apk")
#define APP_DEX B("app/build/intermediates/dex")
#define APPFILE B("appfile")
#define APPFILE_APK B("appfile/build/outputs/debug/app.apk")
#define APPFILE_DEX B("appfile/build/intermediates/dex")
#define APPMAVEN B("appmaven")
#define APPMAVEN_APK B("appmaven/build/outputs/debug/app.apk")
#define APPMAVEN_DEX B("appmaven/build/intermediates/dex")
#define REPO B("repo")
#define MCACHE B("mcache")

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

// Contents of one zip entry as a NUL-terminated string, or NULL.
static char *zip_read_text(const char *zip_path, const char *name) {
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(zip_path, &a) != CCB_OK) {
    return NULL;
  }
  const ccb_zip_entry *e = ccb_zip_find(a, name);
  if (e == NULL) {
    ccb_zip_close(a);
    return NULL;
  }
  void *data = NULL;
  size_t len = 0;
  if (ccb_zip_read_entry(a, e, &data, &len) != CCB_OK) {
    ccb_zip_close(a);
    return NULL;
  }
  char *text = ccb_xmalloc(len + 1);
  memcpy(text, data, len);
  text[len] = '\0';
  free(data);
  ccb_zip_close(a);
  return text;
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

static int run_check(char *bin, const char *proj, ccb_proc_result *r) {
  char *args[] = { bin, "check", NULL };
  if (ccb_proc_run(args, proj, NULL, r) != CCB_OK) {
    return 0;
  }
  return 1;
}

// A minimal project dir: ccb.toml + manifest, no sources. toml is the
// full ccb.toml text; pkg is the manifest package.
static void write_min_project(const char *proj, const char *toml,
    const char *pkg) {
  mkdir_or_fail(proj);
  ccb_str path;
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/ccb.toml");
  write_text(path.buf, toml);
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/src/main");
  mkdir_or_fail(path.buf);
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, proj);
  ccb_str_append(&path, "/src/main/AndroidManifest.xml");
  ccb_str m;
  ccb_str_init(&m);
  ccb_str_append(&m,
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"");
  ccb_str_append(&m, pkg);
  ccb_str_append(&m, "\" />\n");
  write_text(path.buf, m.buf);
  ccb_str_free(&m);
  ccb_str_free(&path);
}

static const char *MIN_APP_TOML =
  "ccb_version = 1\n"
  "\n"
  "[project]\n"
  "name = \"app\"\n"
  "package = \"com.example.app\"\n"
  "version = \"1.0\"\n"
  "version_code = 1\n"
  "min_sdk = 26\n"
  "target_sdk = 35\n"
  "compile_sdk = 35\n"
  "\n"
  "[paths]\n"
  "manifest = \"src/main/AndroidManifest.xml\"\n";

static void check_fails_with(char *bin, const char *proj,
    const char *code) {
  ccb_proc_result r;
  CHECK(run_check(bin, proj, &r));
  CHECK(r.exit_code != 0);
  if (!str_contains(r.err, code)) {
    fprintf(stderr, "expected %s in check output for %s:\n%s\n%s\n",
      code, proj, r.out, r.err);
  }
  CHECK(str_contains(r.err, code));
  ccb_proc_result_free(&r);
}

static void test_config_errors(char *bin) {
  // [library] takes no keys.
  write_min_project(B("cfg1"), MIN_APP_TOML, "com.example.app");
  write_text(B("cfg1/ccb.toml"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"lib\"\n"
    "package = \"com.example.lib\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[library]\n"
    "foo = 1\n");
  check_fails_with(bin, B("cfg1"), "CCB-E1001");

  // A library project cannot declare modules.
  write_min_project(B("cfg2"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"lib\"\n"
    "package = \"com.example.lib\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[library]\n"
    "\n"
    "[modules.x]\n"
    "dir = \"libx\"\n",
    "com.example.lib");
  check_fails_with(bin, B("cfg2"), "CCB-E1005");

  // A library project cannot declare [deps] jars.
  write_min_project(B("cfg3"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"lib\"\n"
    "package = \"com.example.lib\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[deps]\n"
    "jars = [\"x.jar\"]\n"
    "\n"
    "[library]\n",
    "com.example.lib");
  check_fails_with(bin, B("cfg3"), "CCB-E1005");

  // A valid library project dir for the path cases below.
  write_min_project(B("libok"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"mylib\"\n"
    "package = \"com.example.mylib\"\n"
    "version = \"1.2\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[library]\n",
    "com.example.mylib");
  // A non-library dir (valid app project, no [library] table).
  write_min_project(B("notlib"), MIN_APP_TOML, "com.example.app");

  // [deps.library.l] without path.
  write_min_project(B("cfg4"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"app\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[deps.library.l]\n",
    "com.example.app");
  check_fails_with(bin, B("cfg4"), "CCB-E1004");

  // Path does not exist.
  write_min_project(B("cfg5"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"app\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[deps.library.l]\n"
    "path = \"../nope\"\n",
    "com.example.app");
  check_fails_with(bin, B("cfg5"), "CCB-E1006");

  // Path is a directory but not a library project.
  write_min_project(B("cfg6"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"app\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[deps.library.l]\n"
    "path = \"../notlib\"\n",
    "com.example.app");
  check_fails_with(bin, B("cfg6"), "CCB-E1005");

  // Path is a file but not an .aar.
  write_min_project(B("cfg7"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"app\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[deps.library.l]\n"
    "path = \"../libok/ccb.toml\"\n",
    "com.example.app");
  check_fails_with(bin, B("cfg7"), "CCB-E1005");

  // Library package equals the app package.
  write_min_project(B("libsamepkg"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"mylib\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.2\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[library]\n",
    "com.example.app");
  write_min_project(B("cfg8"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"app\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[deps.library.l]\n"
    "path = \"../libsamepkg\"\n",
    "com.example.app");
  check_fails_with(bin, B("cfg8"), "CCB-E1005");

  // Library name collides with a module name.
  write_min_project(B("cfg9"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"app\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[modules.mylib]\n"
    "dir = \"libmod\"\n"
    "\n"
    "[deps.library.mylib]\n"
    "path = \"../libok\"\n",
    "com.example.app");
  check_fails_with(bin, B("cfg9"), "CCB-E1005");

  // Library project declaring its own Maven deps.
  write_min_project(B("libdeps"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"mylib\"\n"
    "package = \"com.example.mylib\"\n"
    "version = \"1.2\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[deps]\n"
    "maven = [\"com.example:other:1.0\"]\n"
    "\n"
    "[library]\n",
    "com.example.mylib");
  write_min_project(B("cfg10"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"app\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "\n"
    "[deps.library.l]\n"
    "path = \"../libdeps\"\n",
    "com.example.app");
  check_fails_with(bin, B("cfg10"), "CCB-E1005");
}

// Write the library fixture (code + resources + manifest).
static void write_lib_fixture(void) {
  mkdir_or_fail(B("lib/src/main/kotlin/com/example/mylib"));
  mkdir_or_fail(B("lib/src/main/res/values"));
  write_text(B("lib/ccb.toml"),
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"mylib\"\n"
    "package = \"com.example.mylib\"\n"
    "version = \"1.2\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\n"
    "kotlin = \"src/main/kotlin\"\n"
    "\n"
    "[library]\n");
  write_text(B("lib/src/main/AndroidManifest.xml"),
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"com.example.mylib\">\n"
    "  <uses-permission android:name=\"android.permission.VIBRATE\" />\n"
    "  <application>\n"
    "    <activity android:name=\".LibActivity\" android:exported=\"false\" />\n"
    "  </application>\n"
    "</manifest>\n");
  write_text(B("lib/src/main/kotlin/com/example/mylib/Lib.kt"),
    "package com.example.mylib\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "\n"
    "object Lib {\n"
    "  fun greeting(): String {\n"
    "    return \"from lib \" + R.string.lib_text\n"
    "  }\n"
    "}\n"
    "\n"
    "class LibActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    val text = getString(R.string.lib_text)\n"
    "    val unused = text\n"
    "  }\n"
    "}\n");
  write_text(B("lib/src/main/res/values/strings.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"lib_text\">from lib</string>\n"
    "  <string name=\"shared\">LIB_SHARED</string>\n"
    "</resources>\n");
}

// Write the app fixture under dir, consuming the library via dep_toml
// (the [deps...] section text, verbatim).
static void write_app_fixture(const char *dir, const char *dep_toml) {
  ccb_str kd;
  ccb_str_init(&kd);
  ccb_str_append(&kd, dir);
  ccb_str_append(&kd, "/src/main/kotlin/com/example/app");
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
    "name = \"app\"\n"
    "package = \"com.example.app\"\n"
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
  ccb_str_append(&t, dep_toml);
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
  ccb_str_append(&path, "/src/main/kotlin/com/example/app/MainActivity.kt");
  write_text(path.buf,
    "package com.example.app\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "import com.example.mylib.Lib\n"
    "\n"
    "class MainActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    setContentView(R.layout.activity_main)\n"
    "    val unused = Lib.greeting()\n"
    "  }\n"
    "}\n");
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, dir);
  ccb_str_append(&path, "/src/main/res/values/strings.xml");
  write_text(path.buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"app_name\">app</string>\n"
    "  <string name=\"hello\">Hello</string>\n"
    "  <string name=\"shared\">APP_SHARED</string>\n"
    "</resources>\n");
  ccb_str_free(&path);
  ccb_str_init(&path);
  ccb_str_append(&path, dir);
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
  ccb_str_append(&path, dir);
  ccb_str_append(&path, "/src/main/res/values/themes.xml");
  write_text(path.buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <style name=\"AppTheme\" parent=\"@android:style/Theme.Material.Light.NoActionBar\" />\n"
    "</resources>\n");
  ccb_str_free(&path);
}

static void run_or_fail(char **args, const char *proj,
    char *const *env, const char *what) {
  ccb_proc_result r;
  CHECK(ccb_proc_run(args, proj, env, &r) == CCB_OK);
  CHECK(r.exit_code == 0);
  if (r.exit_code != 0) {
    fprintf(stderr, "%s failed: %s\n%s\n", what, r.out, r.err);
  }
  ccb_proc_result_free(&r);
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "lib");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping library checks\n");
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }

  // Config errors need only the binary.
  ccb_remove_recursive(BASE);
  test_config_errors(bin);

  char *sdk_home = tool_home("ANDROID_HOME", "android-sdk");
  char *java_home = tool_home("JAVA_HOME", "jdk17");
  char *kotlin_home = tool_home("KOTLIN_HOME", "kotlin-2.4.21");
  char *aapt2 = sdk_home == NULL ? NULL
    : ccb_path_join3(sdk_home, "build-tools/35.0.0", "aapt2");
  char *jarsigner = java_home == NULL ? NULL : ccb_path_join(java_home, "bin/jarsigner");
  if (bin == NULL || aapt2 == NULL || access(aapt2, X_OK) != 0 ||
      jarsigner == NULL || access(jarsigner, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping black-box library checks\n");
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

  // Standalone library build.
  write_lib_fixture();
  write_app_fixture(APP, "\n[deps.library.mylib]\npath = \"../lib\"\n");
  {
    char *args[] = { bin, "build", NULL };
    run_or_fail(args, LIB, env_extra, "library build");
  }
  CHECK(ccb_path_exists(LIB_AAR));
  CHECK(zip_has(LIB_AAR, "classes.jar"));
  CHECK(zip_has(LIB_AAR, "AndroidManifest.xml"));
  CHECK(zip_has(LIB_AAR, "R.txt"));
  CHECK(zip_has(LIB_AAR, "res/values/strings.xml"));
  {
    char *rtxt = zip_read_text(LIB_AAR, "R.txt");
    CHECK(rtxt != NULL);
    CHECK(str_contains(rtxt, "string lib_text"));
    CHECK(str_contains(rtxt, "string shared"));
    free(rtxt);
  }
  CHECK(zip_has(B("lib/build/intermediates/libraries/mylib/classes.jar"),
    "com/example/mylib/Lib.class"));
  CHECK(!zip_has_r_class(
    B("lib/build/intermediates/libraries/mylib/classes.jar")));
  {
    ccb_proc_result r;
    char *args[] = { bin, "check", NULL };
    CHECK(ccb_proc_run(args, LIB, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "library mylib 1.2 (com.example.mylib)"));
    CHECK(str_contains(r.out, "classes.jar, 1 res files, 2 R symbols"));
    ccb_proc_result_free(&r);
    char *jargs[] = { bin, "check", "--json", NULL };
    CHECK(ccb_proc_run(jargs, LIB, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "\"library\":{\"name\":\"mylib\""));
    CHECK(str_contains(r.out, "\"res_files\":1"));
    ccb_proc_result_free(&r);
    char *bargs[] = { bin, "build", NULL };
    CHECK(ccb_proc_run(bargs, LIB, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "up to date"));
    ccb_proc_result_free(&r);
  }

  // App consuming the library project by path.
  {
    ccb_proc_result r;
    char *args[] = { bin, "build", NULL };
    CHECK(ccb_proc_run(args, APP, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    if (r.exit_code != 0) {
      fprintf(stderr, "app build failed: %s\n%s\n", r.out, r.err);
    }
    CHECK(str_contains(r.err,
      "resource 'string/shared' defined by app and library mylib; app wins"));
    ccb_proc_result_free(&r);
  }
  CHECK(ccb_path_exists(APP_APK));
  CHECK(dex_contains(APP_DEX, "Lcom/example/mylib/Lib;"));
  CHECK(dex_contains(APP_DEX, "Lcom/example/mylib/R$string;"));
  CHECK(dex_contains(APP_DEX, "Lcom/example/app/MainActivity;"));
  {
    ccb_proc_result r;
    char *xml_args[] = { aapt2, "dump", "xmltree", (char *)APP_APK,
      "--file", "AndroidManifest.xml", NULL };
    CHECK(ccb_proc_run(xml_args, APP, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "com.example.mylib.LibActivity"));
    CHECK(str_contains(r.out, "android.permission.VIBRATE"));
    ccb_proc_result_free(&r);
    char *res_args[] = { aapt2, "dump", "resources", (char *)APP_APK, NULL };
    CHECK(ccb_proc_run(res_args, APP, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "() \"APP_SHARED\""));
    CHECK(str_contains(r.out, "() \"from lib\""));
    ccb_proc_result_free(&r);
    char *cargs[] = { bin, "check", NULL };
    CHECK(ccb_proc_run(cargs, APP, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out,
      "library mylib 1.2 (com.example.mylib) path"));
    CHECK(str_contains(r.out, "classes.jar, 1 res files, 2 R symbols"));
    ccb_proc_result_free(&r);
  }
  // Explain + dry-run plan on a fresh tree.
  CHECK(ccb_remove_recursive(B("app/build")) == CCB_OK);
  {
    ccb_proc_result r;
    char *explain_args[] = { bin, "build", "--explain", NULL };
    CHECK(ccb_proc_run(explain_args, APP, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "library mylib:"));
    ccb_proc_result_free(&r);
    char *dry_args[] = { bin, "build", "--dry-run", NULL };
    CHECK(ccb_proc_run(dry_args, APP, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "mylib"));
    CHECK(str_contains(r.out, "-R"));
    ccb_proc_result_free(&r);
  }
  // Rebuild for real, then incremental: a library resource change
  // rebuilds the app and the new value wins.
  {
    char *args[] = { bin, "build", NULL };
    run_or_fail(args, APP, env_extra, "app rebuild");
  }
  write_text(B("lib/src/main/res/values/strings.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"lib_text\">from lib2</string>\n"
    "  <string name=\"shared\">LIB_SHARED</string>\n"
    "</resources>\n");
  {
    char *args[] = { bin, "build", NULL };
    run_or_fail(args, APP, env_extra, "app rebuild after lib change");
  }
  {
    ccb_proc_result r;
    char *res_args[] = { aapt2, "dump", "resources", (char *)APP_APK, NULL };
    CHECK(ccb_proc_run(res_args, APP, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "() \"from lib2\""));
    ccb_proc_result_free(&r);
  }

  // Same library consumed as a built .aar file.
  mkdir_or_fail(B("appfile/libs"));
  {
    char *data = NULL;
    size_t len = 0;
    CHECK(ccb_read_file(LIB_AAR, &data, &len) == CCB_OK);
    CHECK(ccb_write_file(B("appfile/libs/mylib-1.2.aar"), data, len) == CCB_OK);
    free(data);
  }
  write_app_fixture(APPFILE,
    "\n[deps.library.mylib]\npath = \"libs/mylib-1.2.aar\"\n");
  {
    char *args[] = { bin, "build", NULL };
    run_or_fail(args, APPFILE, env_extra, "file-library app build");
  }
  CHECK(ccb_path_exists(APPFILE_APK));
  CHECK(dex_contains(APPFILE_DEX, "Lcom/example/mylib/Lib;"));

  // Same library consumed from Maven coordinates: publish the built
  // AAR into a fixture repo, prefetch in-process, then build.
  {
    char *dir = B("repo/com/example/mylib/1.2");
    mkdir_or_fail(dir);
    char *data = NULL;
    size_t len = 0;
    CHECK(ccb_read_file(LIB_AAR, &data, &len) == CCB_OK);
    CHECK(ccb_write_file(B("repo/com/example/mylib/1.2/mylib-1.2.aar"),
      data, len) == CCB_OK);
    free(data);
    write_text(B("repo/com/example/mylib/1.2/mylib-1.2.pom"),
      "<project><modelVersion>4.0.0</modelVersion>"
      "<groupId>com.example</groupId><artifactId>mylib</artifactId>"
      "<version>1.2</version><packaging>aar</packaging></project>");
  }
  write_app_fixture(APPMAVEN,
    "\n[deps]\nmaven = [\"com.example:mylib:1.2\"]\n");
  {
    const char *old_repos = getenv("CCB_MAVEN_REPOS");
    const char *old_cache = getenv("CCB_MAVEN_CACHE");
    char *saved_repos = old_repos != NULL ? ccb_xstrdup(old_repos) : NULL;
    char *saved_cache = old_cache != NULL ? ccb_xstrdup(old_cache) : NULL;
    ccb_str r;
    ccb_str_init(&r);
    ccb_str_append(&r, "file://");
    ccb_str_append(&r, REPO);
    char *repos_env = ccb_str_steal(&r);
    setenv("CCB_MAVEN_REPOS", repos_env, 1);
    free(repos_env);
    setenv("CCB_MAVEN_CACHE", MCACHE, 1);
    char *cfg_path = ccb_path_join(APPMAVEN, "ccb.toml");
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_config *cfg = NULL;
    CHECK(ccb_config_load(cfg_path, NULL, &cfg, &diags) == CCB_OK);
    free(cfg_path);
    if (cfg != NULL) {
      ccb_maven_result out;
      CHECK(ccb_maven_resolve(cfg, 1, 0, 0, &out, &diags) == CCB_OK);
      CHECK(out.aars_count == 1);
      ccb_maven_result_free(&out);
      ccb_config_free(cfg);
    }
    ccb_diag_list_free(&diags);
    if (saved_repos != NULL) {
      setenv("CCB_MAVEN_REPOS", saved_repos, 1);
      free(saved_repos);
    } else {
      unsetenv("CCB_MAVEN_REPOS");
    }
    if (saved_cache != NULL) {
      setenv("CCB_MAVEN_CACHE", saved_cache, 1);
      free(saved_cache);
    } else {
      unsetenv("CCB_MAVEN_CACHE");
    }
  }
  char *cache_env = NULL;
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "CCB_MAVEN_CACHE=");
    ccb_str_append(&s, MCACHE);
    cache_env = ccb_str_steal(&s);
  }
  char *const env_maven[] = { sdk_env, java_env, kotlin_env, home_env,
    cache_env, NULL };
  {
    ccb_proc_result r;
    char *cargs[] = { bin, "check", NULL };
    CHECK(ccb_proc_run(cargs, APPMAVEN, env_maven, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(str_contains(r.out, "library com.example:mylib:1.2 maven"));
    ccb_proc_result_free(&r);
    char *args[] = { bin, "build", NULL };
    CHECK(ccb_proc_run(args, APPMAVEN, env_maven, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    if (r.exit_code != 0) {
      fprintf(stderr, "maven app build failed: %s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
  }
  CHECK(ccb_path_exists(APPMAVEN_APK));
  CHECK(dex_contains(APPMAVEN_DEX, "Lcom/example/mylib/Lib;"));
  CHECK(dex_contains(APPMAVEN_DEX, "Lcom/example/mylib/R$string;"));

  free(cache_env);
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
