// Tests for the opt-in kotlinc daemon (opt-daemon): argv construction
// and output classification (pure, no toolchain), config keys, the
// daemon-client line on a dry-run build, and the live lifecycle on a
// real hello build — daemon reuse across two builds (same port),
// `daemon stop`, and a killed daemon falling back to a fresh one.
// The live half needs a real SDK toolchain to resolve, else prints a
// skip note and passes (same pattern as test_d8_heap.c). The stop
// poll waits for the daemon's own shutdown delay, so the live half
// takes about two minutes with the 60 s idle timeout used here.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_config.h"
#include "ccb_daemon.h"
#include "ccb_diag.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include <dirent.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define HELLO_PROJ B("hello")
// Idle timeout (s) used by the live fixture. Must exceed the
// post-kotlinc pipeline time, else the daemon is gone before the
// next build (see the header comment).
#define DAEMON_IDLE 180

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

// Minimal Kotlin hello project (no [deps] at all). daemon = 1 adds
// [build] kotlinc_daemon = true with the given idle seconds.
static void write_hello_project(const char *proj, const char *sdk_home,
    const char *bt_ver, const char *kotlin_home, int daemon, long idle) {
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
  if (daemon) {
    ccb_str_append(&s, "\n[build]\nkotlinc_daemon = true\n"
      "kotlinc_daemon_idle_seconds = ");
    ccb_str_appendf(&s, "%ld\n", idle);
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
    "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  android:layout_width=\"match_parent\"\n"
    "  android:layout_height=\"match_parent\"\n"
    "  android:orientation=\"vertical\">\n"
    "  <TextView android:layout_width=\"wrap_content\"\n"
    "    android:layout_height=\"wrap_content\"\n"
    "    android:text=\"@string/app_name\" />\n"
    "</LinearLayout>\n");
  free(layout);
  char *kt = ccb_path_join(proj,
    "src/main/kotlin/com/example/hello/MainActivity.kt");
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

// Run ccb with extra args in proj, capturing stdout+stderr. Returns
// the exit code, or -1 when the spawn itself failed.
static int run_ccb(char *bin, const char *proj, char **env,
    char **extra, ccb_proc_result *out) {
  size_t n = 0;
  while (extra[n] != NULL) {
    n++;
  }
  char **args = calloc(n + 2, sizeof(*args));
  args[0] = bin;
  for (size_t i = 0; i < n; i++) {
    args[i + 1] = extra[i];
  }
  args[n + 1] = NULL;
  int rc = -1;
  if (ccb_proc_run(args, proj, env, out) == CCB_OK) {
    rc = out->exit_code;
  }
  free(args);
  return rc;
}

static int contains(const char *text, const char *needle) {
  return text != NULL && strstr(text, needle) != NULL;
}

// Daemon port from "found daemon on port N" in text, or -1.
static long daemon_port(const char *text) {
  const char *p = text != NULL ? strstr(text, "found daemon on port ") : NULL;
  if (p == NULL) {
    return -1;
  }
  long port = -1;
  if (sscanf(p + strlen("found daemon on port "), "%ld", &port) != 1) {
    return -1;
  }
  return port;
}

// PID of a process whose cmdline contains needle, or -1. The daemon's
// cmdline carries --daemon-runFilesPath <run dir>, which is unique
// per project, so this identifies this project's daemon only.
static long find_daemon_pid(const char *needle) {
  DIR *d = opendir("/proc");
  if (d == NULL) {
    return -1;
  }
  long found = -1;
  struct dirent *e = NULL;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') {
      continue;
    }
    char path[512];
    snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
    char *cmd = NULL;
    size_t len = 0;
    if (ccb_read_file(path, &cmd, &len) != CCB_OK) {
      continue;
    }
    if (memmem(cmd, len, needle, strlen(needle)) != NULL) {
      found = atol(e->d_name);
      free(cmd);
      break;
    }
    free(cmd);
  }
  closedir(d);
  return found;
}

static int has_run_file(const char *run_dir) {
  DIR *d = opendir(run_dir);
  if (d == NULL) {
    return 0;
  }
  int found = 0;
  struct dirent *e = NULL;
  while ((e = readdir(d)) != NULL) {
    if (strstr(e->d_name, ".run") != NULL) {
      found = 1;
      break;
    }
  }
  closedir(d);
  return found;
}

int main(int argc, char **argv) {
  ccb_test_tmpdir(BASE, sizeof BASE, "kotlindaemon");
  // 1. Output classification (pure).
  CHECK(ccb_daemon_output_ok("Compilation succeeded, result code: 0\n",
    "") == 1);
  CHECK(ccb_daemon_output_ok("", "result code: 0") == 1);
  CHECK(ccb_daemon_output_ok("Compilation succeeded, result code: 1\n",
    "") == 0);
  CHECK(ccb_daemon_output_ok("", "") == 0);
  CHECK(ccb_daemon_output_broken("", "Unable to connect to daemon") == 1);
  CHECK(ccb_daemon_output_broken(
    "EXCEPTION: Failed connecting to the daemon in 4 retries", "") == 1);
  CHECK(ccb_daemon_output_broken(
    "The daemon has terminated unexpectedly on startup", "") == 1);
  CHECK(ccb_daemon_output_broken("",
    "Could not find or load main class X") == 1);
  CHECK(ccb_daemon_output_broken("Compilation succeeded, result code: 0",
    "") == 0);
  CHECK(ccb_daemon_output_has_errors("ERROR\tMain.kt:1 \tUnresolved\n",
    "") == 1);
  CHECK(ccb_daemon_output_has_errors("ok\nERROR\tMain.kt:1 \tbad\n", "") == 1);
  CHECK(ccb_daemon_output_has_errors("", "ERROR\tMain.kt:1 \tbad\n") == 1);
  CHECK(ccb_daemon_output_has_errors("no errors here", "") == 0);
  CHECK(ccb_daemon_output_has_errors("ERROR in a string literal", "") == 0);

  // 2. Argv + classpaths against a fake kotlin home (pure).
  char *fake_home = B("fake-kotlinc");
  char *fake_lib = B("fake-kotlinc/lib");
  mkdir_or_fail(fake_lib);
  static const char *jars[] = {
    "kotlin-compiler.jar",
    "kotlin-daemon.jar",
    "kotlin-daemon-client.jar",
    "kotlin-stdlib.jar",
    "kotlin-reflect.jar",
    "annotations-13.0.jar",
    "kotlin-script-runtime.jar",
  };
  for (size_t i = 0; i < sizeof(jars) / sizeof(*jars); i++) {
    char *p = ccb_path_join(fake_lib, jars[i]);
    write_text(p, "");
    free(p);
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  CHECK(ccb_daemon_client_classpath(ar, NULL) == NULL);
  CHECK(ccb_daemon_compiler_classpath(ar, NULL) == NULL);
  char *client_cp = ccb_daemon_client_classpath(ar, fake_home);
  char *compiler_cp = ccb_daemon_compiler_classpath(ar, fake_home);
  CHECK(client_cp != NULL);
  CHECK(compiler_cp != NULL);
  for (size_t i = 0; i < sizeof(jars) / sizeof(*jars); i++) {
    CHECK(contains(client_cp, jars[i]));
  }
  CHECK(!contains(compiler_cp, "kotlin-daemon-client.jar"));
  CHECK(contains(compiler_cp, "kotlin-daemon.jar"));
  char *run_dir = ccb_daemon_run_dir(ar, "/proj/build");
  CHECK_STR_EQ(run_dir, "/proj/build/kotlin-daemon");
  char *kot_argv[] = { (char *)"/bin/kotlinc", (char *)"-jvm-target",
    (char *)"17", (char *)"-d", (char *)"out", (char *)"A.kt", NULL };
  char **dav = ccb_daemon_compile_argv(ar, "/bin/java", fake_home,
    "/proj/build/kotlin-daemon", 120, kot_argv);
  CHECK(dav != NULL);
  int has_client = 0;
  int has_run = 0;
  int has_idle = 0;
  int has_delay = 0;
  int has_ipv4 = 0;
  int has_args = 0;
  for (size_t i = 0; dav[i] != NULL; i++) {
    if (strcmp(dav[i],
        "org.jetbrains.kotlin.daemon.client.KotlinCompilerClient") == 0) {
      has_client = 1;
    }
    if (strcmp(dav[i], "/proj/build/kotlin-daemon") == 0) {
      has_run = 1;
    }
    if (strcmp(dav[i], "--daemon-autoshutdownIdleSeconds=120") == 0) {
      has_idle = 1;
    }
    if (strcmp(dav[i], "--daemon-shutdownDelayMilliseconds=120000") == 0) {
      has_delay = 1;
    }
    if (strcmp(dav[i], "-Djava.net.preferIPv4Stack=true") == 0) {
      has_ipv4 = 1;
    }
    if (strcmp(dav[i], "A.kt") == 0) {
      has_args = 1;
    }
  }
  CHECK(has_client && has_run && has_idle && has_delay && has_ipv4);
  CHECK(has_args);
  CHECK_STR_EQ(dav[0], "/bin/java");
  char **sav = ccb_daemon_stop_argv(ar, "/bin/java", fake_home,
    "/proj/build/kotlin-daemon");
  CHECK(sav != NULL);
  int has_stop = 0;
  int has_dash_d = 0;
  for (size_t i = 0; sav[i] != NULL; i++) {
    if (strcmp(sav[i], "-Dkotlin.daemon.client.options=stop") == 0) {
      has_stop = 1;
    }
    if (strcmp(sav[i], "-d") == 0) {
      has_dash_d = 1;
    }
  }
  CHECK(has_stop);
  CHECK(!has_dash_d);
  // A missing daemon jar means no daemon argv: fall back to kotlinc.
  char *daemon_jar = ccb_path_join(fake_lib, "kotlin-daemon.jar");
  CHECK(remove(daemon_jar) == 0);
  free(daemon_jar);
  CHECK(ccb_daemon_client_classpath(ar, fake_home) == NULL);
  CHECK(ccb_daemon_compiler_classpath(ar, fake_home) == NULL);
  CHECK(ccb_daemon_compile_argv(ar, "/bin/java", fake_home,
    "/proj/build/kotlin-daemon", 120, kot_argv) == NULL);
  CHECK(ccb_daemon_stop_argv(ar, "/bin/java", fake_home,
    "/proj/build/kotlin-daemon") == NULL);

  // 3. Config keys (needs the fixture dirs, no toolchain).
  // The fixture's [sdk] paths must exist: config load requires them.
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary, skipping config/dry-run/live checks\n");
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
  if (sdk_home == NULL || !ccb_path_is_dir(sdk_home) ||
      kotlin_home == NULL || !ccb_path_is_dir(kotlin_home) ||
      bt_ver == NULL) {
    fprintf(stderr,
      "note: no SDK toolchain dirs, skipping config/dry-run/live checks\n");
    free(bin);
    free(sdk_home);
    free(java_home);
    free(kotlin_home);
    free(bt_ver);
    ccb_remove_recursive(BASE);
    TEST_MAIN_END();
  }
  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, 1, DAEMON_IDLE);
  char *toml = ccb_path_join(HELLO_PROJ, "ccb.toml");
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = NULL;
  CHECK(ccb_config_load(toml, NULL, &cfg, &diags) == CCB_OK);
  if (cfg != NULL) {
    CHECK(cfg->kotlinc_daemon == 1);
    CHECK(cfg->kotlinc_daemon_idle_seconds == DAEMON_IDLE);
    ccb_config_free(cfg);
  }
  ccb_diag_list_free(&diags);
  // Out-of-range idle timeouts are config errors.
  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, 1, 10);
  ccb_diag_list_init(&diags);
  cfg = NULL;
  CHECK(ccb_config_load(toml, NULL, &cfg, &diags) != CCB_OK);
  CHECK(cfg == NULL);
  ccb_diag_list_free(&diags);
  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, 1, 3601);
  ccb_diag_list_init(&diags);
  cfg = NULL;
  CHECK(ccb_config_load(toml, NULL, &cfg, &diags) != CCB_OK);
  CHECK(cfg == NULL);
  ccb_diag_list_free(&diags);
  free(toml);

  // 4. Dry-run + live builds need a runnable toolchain.
  char *kotlinc = kotlin_home != NULL ?
    ccb_path_join(kotlin_home, "bin/kotlinc") : NULL;
  if (bt_ver == NULL || java_home == NULL || kotlin_home == NULL ||
      kotlinc == NULL || access(kotlinc, X_OK) != 0) {
    fprintf(stderr,
      "note: no runnable SDK toolchain, skipping dry-run/live checks\n");
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

  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, 1, DAEMON_IDLE);
  char *dry_extra[] = { "build", "--dry-run", NULL };
  ccb_proc_result r;
  CHECK(run_ccb(bin, HELLO_PROJ, env, dry_extra, &r) == 0);
  CHECK(contains(r.out, "KotlinCompilerClient"));
  CHECK(contains(r.out, "--daemon-autoshutdownIdleSeconds=180"));
  CHECK(contains(r.out, "--daemon-shutdownDelayMilliseconds=180000"));
  CHECK(contains(r.out, "-Djava.net.preferIPv4Stack=true"));
  CHECK(contains(r.out, "build/kotlin-daemon"));
  ccb_proc_result_free(&r);
  // --no-kotlinc-daemon overrides the config: plain kotlinc line.
  char *off_extra[] = { "build", "--dry-run", "--no-kotlinc-daemon", NULL };
  CHECK(run_ccb(bin, HELLO_PROJ, env, off_extra, &r) == 0);
  CHECK(!contains(r.out, "KotlinCompilerClient"));
  CHECK(contains(r.out, "/bin/kotlinc "));
  ccb_proc_result_free(&r);
  // Default off (no [build] section): plain kotlinc line.
  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, 0, 60);
  CHECK(run_ccb(bin, HELLO_PROJ, env, dry_extra, &r) == 0);
  CHECK(!contains(r.out, "KotlinCompilerClient"));
  ccb_proc_result_free(&r);
  // --kotlinc-daemon opts in on a default-off project.
  char *on_extra[] = { "build", "--dry-run", "--kotlinc-daemon", NULL };
  CHECK(run_ccb(bin, HELLO_PROJ, env, on_extra, &r) == 0);
  CHECK(contains(r.out, "KotlinCompilerClient"));
  ccb_proc_result_free(&r);

  // 5. Live lifecycle: cold build, warm rebuild reusing the same
  // daemon (same port), daemon stop, killed daemon falls back.
  write_hello_project(HELLO_PROJ, sdk_home, bt_ver, kotlin_home, 1, DAEMON_IDLE);
  char *run_dir_path = ccb_path_join(HELLO_PROJ, "build/kotlin-daemon");
  char *apk_path = ccb_path_join(HELLO_PROJ,
    "build/outputs/debug/app.apk");
  char *kt_path = ccb_path_join(HELLO_PROJ,
    "src/main/kotlin/com/example/hello/MainActivity.kt");
  char *build_v[] = { "build", "--verbose", NULL };
  CHECK(run_ccb(bin, HELLO_PROJ, env, build_v, &r) == 0);
  CHECK(access(apk_path, R_OK) == 0);
  CHECK(has_run_file(run_dir_path));
  long port1 = daemon_port(r.err);
  if (port1 < 0) {
    port1 = daemon_port(r.out);
  }
  CHECK(port1 > 0);
  CHECK(contains(r.err, "connected to the daemon") ||
    contains(r.out, "connected to the daemon"));
  ccb_proc_result_free(&r);
  // One-file change: the daemon must recompile it (a touch alone
  // does not change the content-hash fingerprint).
  char *kt_text = NULL;
  size_t kt_len = 0;
  CHECK(ccb_read_file(kt_path, &kt_text, &kt_len) == CCB_OK);
  ccb_str changed;
  ccb_str_init(&changed);
  ccb_str_append(&changed, kt_text);
  ccb_str_append(&changed, "\n// daemon rebuild probe\n");
  free(kt_text);
  write_text(kt_path, changed.buf);
  ccb_str_free(&changed);
  CHECK(run_ccb(bin, HELLO_PROJ, env, build_v, &r) == 0);
  CHECK(access(apk_path, R_OK) == 0);
  long port2 = daemon_port(r.err);
  if (port2 < 0) {
    port2 = daemon_port(r.out);
  }
  CHECK(port2 > 0);
  CHECK(port1 == port2);
  ccb_proc_result_free(&r);
  // Stop: the daemon acknowledges shutdown, then exits within its
  // idle timeout (the stop RPC schedules shutdown after the delay).
  char *stop_extra[] = { "daemon", "stop", NULL };
  CHECK(run_ccb(bin, HELLO_PROJ, env, stop_extra, &r) == 0);
  CHECK(contains(r.out, "kotlinc daemon stopped"));
  ccb_proc_result_free(&r);
  int gone = 0;
  for (int i = 0; i < (DAEMON_IDLE + 30) / 2; i++) {
    if (find_daemon_pid(run_dir_path) < 0 && !has_run_file(run_dir_path)) {
      gone = 1;
      break;
    }
    sleep(2);
  }
  CHECK(gone);
  // Stale/crashed daemon: change a file and build (starts a daemon),
  // kill -9 it, change the file again, rebuild. The build must still
  // succeed (fresh daemon), never fail on the dead one.
  CHECK(ccb_read_file(kt_path, &kt_text, &kt_len) == CCB_OK);
  ccb_str_init(&changed);
  ccb_str_append(&changed, kt_text);
  ccb_str_append(&changed, "\n// daemon stale setup\n");
  free(kt_text);
  write_text(kt_path, changed.buf);
  ccb_str_free(&changed);
  CHECK(run_ccb(bin, HELLO_PROJ, env, build_v, &r) == 0);
  ccb_proc_result_free(&r);
  long pid = find_daemon_pid(run_dir_path);
  CHECK(pid > 0);
  if (pid > 0) {
    CHECK(kill((pid_t)pid, SIGKILL) == 0);
    sleep(1);
    CHECK(find_daemon_pid(run_dir_path) < 0);
  }
  CHECK(ccb_read_file(kt_path, &kt_text, &kt_len) == CCB_OK);
  ccb_str_init(&changed);
  ccb_str_append(&changed, kt_text);
  ccb_str_append(&changed, "\n// daemon stale probe\n");
  free(kt_text);
  write_text(kt_path, changed.buf);
  ccb_str_free(&changed);
  CHECK(run_ccb(bin, HELLO_PROJ, env, build_v, &r) == 0);
  CHECK(access(apk_path, R_OK) == 0);
  CHECK(contains(r.err, "connected to the daemon") ||
    contains(r.out, "connected to the daemon"));
  ccb_proc_result_free(&r);
  // Clean removes the build dir (and stops the daemon best-effort).
  char *clean_extra[] = { "clean", NULL };
  CHECK(run_ccb(bin, HELLO_PROJ, env, clean_extra, &r) == 0);
  ccb_proc_result_free(&r);
  char *build_dir = ccb_path_join(HELLO_PROJ, "build");
  CHECK(!ccb_path_exists(build_dir));
  free(build_dir);

  free(run_dir_path);
  free(apk_path);
  free(kt_path);
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
