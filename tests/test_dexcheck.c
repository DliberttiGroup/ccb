// Tests for the post-build dex check (change-dexcheck): a build
// must fail before packaging when the dex references a class it
// does not define. Black-box, like test_dex_contents.c: needs a
// real SDK toolchain, else prints a skip note and passes. The
// toolchain is detected, not hardcoded to one SDK version.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_dexcheck.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define HELLO_PROJ B("hello")
#define NEG_PROJ B("neg")
#define USER_SRC B("usersrc")
#define USER_OUT B("userclasses")
#define USER_JAR B("user.jar")
#define USER_DEX B("userdex")
#define MISSING_DEX B("missingdex")
#define MULTI_DEX B("multidex")
#define APP_DEX B("appdex")

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

// Run a command in cwd, capturing output. Returns 1 when the
// process ran and exited 0.
static int run_cmd(char **args, const char *cwd, char **env,
    ccb_proc_result *out) {
  if (ccb_proc_run(args, cwd, env, out) != CCB_OK) {
    return 0;
  }
  int ok = out->exit_code == 0;
  return ok;
}

// Minimal Kotlin hello project. Same shape as test_dex_contents.c.
static void write_hello_project(const char *proj, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home, const char *jar) {
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
  if (jar != NULL) {
    ccb_str_append(&s, "\n[deps]\njars = [\"");
    ccb_str_append(&s, jar);
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
  // greet() takes a non-null parameter, so kotlinc emits an
  // Intrinsics.checkNotNullParameter call — the class the dex
  // references when the stdlib is absent (bug-stdlib).
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
    "  fun greet(name: String): String {\n"
    "    return \"Hello, \" + name\n"
    "  }\n"
    "}\n");
  free(kt);
}

static int missing_has(char **missing, size_t n, const char *name) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(missing[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "dexcheck");
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping dexcheck checks\n");
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
  char *javac = java_home != NULL ?
    ccb_path_join(java_home, "bin/javac") : NULL;
  char *bt_root = sdk_home != NULL ?
    ccb_path_join(sdk_home, "build-tools") : NULL;
  char *d8 = (bt_root != NULL && bt_ver != NULL) ?
    ccb_path_join3(bt_root, bt_ver, "d8") : NULL;
  free(bt_root);
  if (bt_ver == NULL || java_home == NULL || kotlin_home == NULL ||
      kotlinc == NULL || access(kotlinc, X_OK) != 0 ||
      javac == NULL || access(javac, X_OK) != 0 ||
      d8 == NULL || access(d8, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping dexcheck checks\n");
    free(bin);
    free(sdk_home);
    free(java_home);
    free(kotlin_home);
    free(bt_ver);
    free(kotlinc);
    free(javac);
    free(d8);
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }
  free(kotlinc);
  char major[8];
  snprintf(major, sizeof(major), "%.*s", (int)strcspn(bt_ver, "."), bt_ver);
  char ver_name[16];
  snprintf(ver_name, sizeof(ver_name), "android-%s", major);
  char *plat_dir = ccb_path_join(sdk_home, "platforms");
  char *ver_dir = ccb_path_join(plat_dir, ver_name);
  free(plat_dir);
  char *android_jar = ccb_path_join(ver_dir, "android.jar");
  free(ver_dir);

  ccb_str sdk_env, java_env, kot_env, path_env;
  ccb_str_init(&sdk_env);
  ccb_str_init(&java_env);
  ccb_str_init(&kot_env);
  ccb_str_init(&path_env);
  ccb_str_append(&sdk_env, "ANDROID_HOME=");
  ccb_str_append(&sdk_env, sdk_home);
  ccb_str_append(&java_env, "JAVA_HOME=");
  ccb_str_append(&java_env, java_home);
  ccb_str_append(&kot_env, "KOTLIN_HOME=");
  ccb_str_append(&kot_env, kotlin_home);
  // d8 is a wrapper script that execs plain `java` from PATH; the
  // pipeline prefixes the JDK bin itself (ccb_jvm_env), but direct
  // d8 calls in this test need it here.
  ccb_str_append(&path_env, "PATH=");
  ccb_str_append(&path_env, java_home);
  ccb_str_append(&path_env, "/bin:");
  const char *old_path = getenv("PATH");
  ccb_str_append(&path_env, old_path != NULL ? old_path : "/usr/bin:/bin");
  char *env[] = { sdk_env.buf, java_env.buf, kot_env.buf, path_env.buf, NULL };

  ccb_remove_recursive(BASE);

  // 1. Positive: a no-deps Kotlin hello build passes dexcheck
  // (stdlib auto-added, app classes present) and ships an APK.
  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, NULL);
  {
    char *args[] = { bin, "build", NULL };
    ccb_proc_result r;
    CHECK(run_cmd(args, HELLO_PROJ, env, &r));
    if (r.exit_code != 0) {
      fprintf(stderr, "hello build failed:\n%s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
  }
  {
    char *apk = ccb_path_join(HELLO_PROJ, "build/outputs/debug/app.apk");
    CHECK(access(apk, R_OK) == 0);
    free(apk);
  }

  // 2. AAB build of the same project also passes (dexcheck sits
  // before the shared package step).
  {
    char *args[] = { bin, "build", "--aab", NULL };
    ccb_proc_result r;
    CHECK(run_cmd(args, HELLO_PROJ, env, &r));
    if (r.exit_code != 0) {
      fprintf(stderr, "hello aab build failed:\n%s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
  }
  {
    char *aab = ccb_path_join(HELLO_PROJ, "build/outputs/debug/app.aab");
    CHECK(access(aab, R_OK) == 0);
    free(aab);
  }

  // 3. Helper level: d8 on app classes WITHOUT the stdlib — the
  // historical bug-stdlib case in miniature. The dex references
  // Intrinsics but does not define it. kotlin.Metadata is also
  // reported: it is only an annotation reference, but d8 keeps it
  // in type_ids, and annotation-only references count by design.
  mkdir_or_fail(APP_DEX);
  {
    char *cls = ccb_path_join(HELLO_PROJ,
      "build/intermediates/classes/com/example/hello/MainActivity.class");
    char *args[] = { d8, "--min-api", "26", "--lib", android_jar,
      "--output", APP_DEX, cls, NULL };
    ccb_proc_result r;
    CHECK(run_cmd(args, BASE, env, &r));
    if (r.exit_code != 0) {
      fprintf(stderr, "d8 on app classes failed:\n%s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
    free(cls);
  }
  {
    char *dex = ccb_path_join(APP_DEX, "classes.dex");
    char **missing = NULL;
    size_t nmissing = 0;
    char *err = NULL;
    CHECK(ccb_dexcheck_file(dex, &missing, &nmissing, &err) == CCB_OK);
    CHECK(err == NULL);
    CHECK(missing_has(missing, nmissing, "kotlin.jvm.internal.Intrinsics"));
    CHECK(missing_has(missing, nmissing, "kotlin.Metadata"));
    ccb_free_str_array(missing, nmissing);
    free(dex);
  }

  // 4. Helper level, normalization: a class referencing Missing
  // directly, as Missing[], and only primitives / platform types.
  // d8 tolerates the missing reference; dexcheck must report
  // exactly one missing class, the element type, dotted.
  mkdir_or_fail(USER_SRC);
  mkdir_or_fail(USER_OUT);
  {
    char *missing_java = ccb_path_join(USER_SRC, "Missing.java");
    write_text(missing_java,
      "package com.example;\n"
      "public class Missing { public static String hello() { return \"hi\"; } }\n");
    free(missing_java);
    char *user_java = ccb_path_join(USER_SRC, "User.java");
    write_text(user_java,
      "package com.example;\n"
      "public class User {\n"
      "  public Missing field;\n"
      "  public Missing[] arr;\n"
      "  public int count;\n"
      "  public String use() { return Missing.hello(); }\n"
      "  public java.util.ArrayList<String> list() { return new java.util.ArrayList<>(); }\n"
      "}\n");
    free(user_java);
  }
  {
    char *mjava = ccb_path_join(USER_SRC, "Missing.java");
    char *ujava = ccb_path_join(USER_SRC, "User.java");
    char *args[] = { javac, "-d", USER_OUT, mjava, ujava, NULL };
    ccb_proc_result r;
    CHECK(run_cmd(args, BASE, env, &r));
    if (r.exit_code != 0) {
      fprintf(stderr, "javac failed:\n%s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
    free(mjava);
    free(ujava);
  }
  mkdir_or_fail(USER_DEX);
  {
    char *uclass = ccb_path_join(USER_OUT, "com/example/User.class");
    char *args[] = { d8, "--min-api", "26", "--lib", android_jar,
      "--output", USER_DEX, uclass, NULL };
    ccb_proc_result r;
    CHECK(run_cmd(args, BASE, env, &r));
    if (r.exit_code != 0) {
      fprintf(stderr, "d8 on User failed:\n%s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
    free(uclass);
  }
  {
    char *dex = ccb_path_join(USER_DEX, "classes.dex");
    char **missing = NULL;
    size_t nmissing = 0;
    char *err = NULL;
    CHECK(ccb_dexcheck_file(dex, &missing, &nmissing, &err) == CCB_OK);
    CHECK(err == NULL);
    CHECK(nmissing == 1);
    if (nmissing == 1) {
      CHECK_STR_EQ(missing[0], "com.example.Missing");
    }
    ccb_free_str_array(missing, nmissing);
    free(dex);
  }

  // 5. Malformed dex files are build errors, never crashes.
  {
    char *bad = ccb_path_join(BASE, "bad.dex");
    write_text(bad, "not a dex file at all, far too short");
    char **missing = NULL;
    size_t nmissing = 0;
    char *err = NULL;
    CHECK(ccb_dexcheck_file(bad, &missing, &nmissing, &err) == CCB_ERR_BUILD);
    CHECK(err != NULL);
    CHECK(missing == NULL && nmissing == 0);
    free(err);
    free(bad);
  }
  {
    // Truncated copy of a real dex.
    char *src = ccb_path_join(USER_DEX, "classes.dex");
    char *data = NULL;
    size_t len = 0;
    CHECK(ccb_read_file(src, &data, &len) == CCB_OK);
    char *trunc = ccb_path_join(BASE, "trunc.dex");
    // The read above can fail (e.g. the build that should have
    // produced the dex failed); a NULL data must fail the CHECK,
    // not reach ccb_write_file and segfault.
    if (data != NULL) {
      CHECK(ccb_write_file(trunc, data, 50) == CCB_OK);
      free(data);
      char **missing = NULL;
      size_t nmissing = 0;
      char *err = NULL;
      CHECK(ccb_dexcheck_file(trunc, &missing, &nmissing, &err) == CCB_ERR_BUILD);
      CHECK(err != NULL);
      free(err);
    }
    free(trunc);
    free(src);
  }

  // 6. Negative real build: the hello project + a [deps] jar whose
  // only class references Missing. d8 succeeds; dexcheck must
  // fail the build with CCB-E3025 naming com.example.Missing,
  // and no APK may ship.
  {
    char *uclass = ccb_path_join(USER_OUT, "com/example/User.class");
    // jar cf needs a cwd inside the classes dir.
    char *args[] = { "jar", "cf", USER_JAR, "com/example/User.class", NULL };
    ccb_proc_result r;
    // jar lives in the JDK bin dir.
    char *jarbin = ccb_path_join(java_home, "bin/jar");
    args[0] = jarbin;
    CHECK(run_cmd(args, USER_OUT, env, &r));
    if (r.exit_code != 0) {
      fprintf(stderr, "jar failed:\n%s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
    free(jarbin);
    free(uclass);
  }
  write_hello_project(NEG_PROJ, sdk_home, bt_ver, kotlin_home, USER_JAR);
  {
    char *args[] = { bin, "build", NULL };
    ccb_proc_result r;
    int ran = ccb_proc_run(args, NEG_PROJ, env, &r) == CCB_OK;
    CHECK(ran);
    CHECK(r.exit_code != 0);
    CHECK(ccb_test_contains(r.out, "CCB-E3025") || ccb_test_contains(r.err, "CCB-E3025"));
    CHECK(ccb_test_contains(r.out, "com.example.Missing") ||
      ccb_test_contains(r.err, "com.example.Missing"));
    if (r.exit_code == 0) {
      fprintf(stderr, "negative build unexpectedly passed:\n%s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
  }
  {
    char *apk = ccb_path_join(NEG_PROJ, "build/outputs/debug/app.apk");
    CHECK(access(apk, R_OK) != 0);
    free(apk);
  }

  // 7. Multidex: User's dex references Missing, Missing's dex
  // defines it. A directory holding both must report nothing
  // missing — a class defined in classes2.dex satisfies a
  // reference from classes.dex. The single-file dir still reports
  // it (the per-file API is unchanged).
  mkdir_or_fail(MISSING_DEX);
  mkdir_or_fail(MULTI_DEX);
  {
    char *mclass = ccb_path_join(USER_OUT, "com/example/Missing.class");
    char *args[] = { d8, "--min-api", "26", "--lib", android_jar,
      "--output", MISSING_DEX, mclass, NULL };
    ccb_proc_result r;
    CHECK(run_cmd(args, BASE, env, &r));
    if (r.exit_code != 0) {
      fprintf(stderr, "d8 on Missing failed:\n%s\n%s\n", r.out, r.err);
    }
    ccb_proc_result_free(&r);
    free(mclass);
  }
  {
    char *src = ccb_path_join(USER_DEX, "classes.dex");
    char *data = NULL;
    size_t len = 0;
    CHECK(ccb_read_file(src, &data, &len) == CCB_OK);
    char *dst = ccb_path_join(MULTI_DEX, "classes.dex");
    CHECK(ccb_write_file(dst, data, len) == CCB_OK);
    free(data);
    free(src);
    free(dst);
    src = ccb_path_join(MISSING_DEX, "classes.dex");
    CHECK(ccb_read_file(src, &data, &len) == CCB_OK);
    dst = ccb_path_join(MULTI_DEX, "classes2.dex");
    CHECK(ccb_write_file(dst, data, len) == CCB_OK);
    free(data);
    free(src);
    free(dst);
  }
  {
    char **missing = NULL;
    size_t nmissing = 0;
    char *err = NULL;
    CHECK(ccb_dexcheck_dir(MULTI_DEX, &missing, &nmissing, &err) == CCB_OK);
    CHECK(err == NULL);
    CHECK(nmissing == 0);
    ccb_free_str_array(missing, nmissing);
  }
  {
    char **missing = NULL;
    size_t nmissing = 0;
    char *err = NULL;
    CHECK(ccb_dexcheck_dir(USER_DEX, &missing, &nmissing, &err) == CCB_OK);
    CHECK(err == NULL);
    CHECK(nmissing == 1);
    if (nmissing == 1) {
      CHECK_STR_EQ(missing[0], "com.example.Missing");
    }
    ccb_free_str_array(missing, nmissing);
  }

  free(bin);
  free(sdk_home);
  free(java_home);
  free(kotlin_home);
  free(bt_ver);
  free(javac);
  free(d8);
  free(android_jar);
  ccb_str_free(&sdk_env);
  ccb_str_free(&java_env);
  ccb_str_free(&kot_env);
  ccb_str_free(&path_env);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
