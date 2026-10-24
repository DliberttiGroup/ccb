// test_probes: tool probe cache (build/intermediates/tool-probes.json).
// Unit tests cover lookup/store semantics; black-box tests prove a real
// build probes kotlinc -version once and then hits the cache.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_proc.h"
#include "ccb_sdk.h"
#include "ccb_str.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BASE "/tmp/ccb_test_probes"
#define SDK BASE "/sdk"
#define PROJ BASE "/proj"
#define KHOME BASE "/khome"
#define JAVADIR BASE "/java"
#define CACHE PROJ "/build/intermediates/tool-probes.json"
#define LOG BASE "/kotlinc.log"

static void write_text(const char *path, const char *text) {
  CHECK(ccb_write_file(path, text, strlen(text)) == CCB_OK);
}

static void write_script(const char *path, const char *body) {
  write_text(path, body);
  CHECK(chmod(path, 0755) == 0);
}

static char *find_binary(int argc, char **argv) {
  if (argc > 1 && argv[1] != NULL && access(argv[1], X_OK) == 0) {
    // The child runs with cwd=PROJ, so a relative argv[1] must be
    // absolutized against THIS process's cwd first.
    if (argv[1][0] == '/') {
      return ccb_xstrdup(argv[1]);
    }
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, cwd);
      ccb_str_append_c(&s, '/');
      ccb_str_append(&s, argv[1]);
      return ccb_str_steal(&s);
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

// Count log lines exactly equal to "-version".
static int count_version_probes(void) {
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(LOG, &text, &len) != CCB_OK) {
    return -1;
  }
  int n = 0;
  const char *p = text;
  while (*p != '\0') {
    const char *nl = strchr(p, '\n');
    size_t linelen = nl == NULL ? strlen(p) : (size_t)(nl - p);
    if (linelen == 8 && strncmp(p, "-version", 8) == 0) {
      n++;
    }
    if (nl == NULL) {
      break;
    }
    p = nl + 1;
  }
  free(text);
  return n;
}

// Replace needle with replacement in the cache file, in place.
static void cache_replace(const char *needle, const char *replacement) {
  char *text = NULL;
  size_t len = 0;
  CHECK(ccb_read_file(CACHE, &text, &len) == CCB_OK);
  char *at = strstr(text, needle);
  CHECK(at != NULL);
  if (at == NULL) {
    free(text);
    return;
  }
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append_n(&s, text, (size_t)(at - text));
  ccb_str_append(&s, replacement);
  ccb_str_append(&s, at + strlen(needle));
  CHECK(ccb_write_file(CACHE, s.buf, s.len) == CCB_OK);
  ccb_str_free(&s);
  free(text);
}

static void write_kotlinc_wrapper(const char *version) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "#!/bin/sh\necho \"$@\" >> " LOG "\n");
  ccb_str_append(&s, "if [ \"$1\" = \"-version\" ]; then echo 'info: kotlinc-jvm ");
  ccb_str_append(&s, version);
  ccb_str_append(&s, " (JRE 17.0.1)' >&2; fi\nexit 0\n");
  write_script(KHOME "/bin/kotlinc", s.buf);
  ccb_str_free(&s);
}

int main(int argc, char **argv) {
  ccb_remove_recursive(BASE);

  // Unit: round-trip, coexistence, upsert.
  CHECK(ccb_mkdir_p(BASE "/proj/build/intermediates") == CCB_OK);
  write_text(BASE "/tool", "tool-bytes-v1");
  CHECK(ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool") == NULL);
  ccb_sdk_probe_cache_store(BASE "/proj/build", "java", BASE "/tool", "17");
  char *v = ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool");
  CHECK(v != NULL);
  CHECK_STR_EQ(v, "17");
  free(v);
  ccb_sdk_probe_cache_store(BASE "/proj/build", "kotlinc", BASE "/tool", "2.4.21");
  v = ccb_sdk_probe_cache_lookup(BASE "/proj/build", "kotlinc", BASE "/tool");
  CHECK(v != NULL);
  CHECK_STR_EQ(v, "2.4.21");
  free(v);
  v = ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool");
  CHECK(v != NULL);
  CHECK_STR_EQ(v, "17");
  free(v);
  // Upsert replaces: a later lookup sees the new value, not the old.
  ccb_sdk_probe_cache_store(BASE "/proj/build", "java", BASE "/tool", "18");
  v = ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool");
  CHECK(v != NULL);
  CHECK_STR_EQ(v, "18");
  free(v);
  // Miss when the tool grows (size key).
  write_text(BASE "/tool", "tool-bytes-v1-longer");
  CHECK(ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool") == NULL);
  // Re-store, then miss on an mtime-only change (size key unchanged).
  ccb_sdk_probe_cache_store(BASE "/proj/build", "java", BASE "/tool", "17");
  v = ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool");
  CHECK(v != NULL);
  free(v);
  struct timespec ts[2];
  ts[0].tv_sec = 1000000000;
  ts[0].tv_nsec = 0;
  ts[1].tv_sec = 1000000000;
  ts[1].tv_nsec = 0;
  CHECK(utimensat(AT_FDCWD, BASE "/tool", ts, 0) == 0);
  CHECK(ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool") == NULL);
  // Corrupt file is a miss, and a later store replaces it wholesale.
  ccb_sdk_probe_cache_store(BASE "/proj/build", "java", BASE "/tool", "17");
  write_text(CACHE, "this is not json{");
  CHECK(ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool") == NULL);
  ccb_sdk_probe_cache_store(BASE "/proj/build", "java", BASE "/tool", "17");
  v = ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool");
  CHECK(v != NULL);
  CHECK_STR_EQ(v, "17");
  free(v);
  // Wrong document version is a miss.
  cache_replace("\"version\":1", "\"version\":2");
  CHECK(ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool") == NULL);
  ccb_sdk_probe_cache_store(BASE "/proj/build", "java", BASE "/tool", "17");
  // A non-digits java version is a corrupt entry, not a version.
  cache_replace("\"version\":\"17\"", "\"version\":\"abc\"");
  CHECK(ccb_sdk_probe_cache_lookup(BASE "/proj/build", "java", BASE "/tool") == NULL);
  ccb_sdk_probe_cache_store(BASE "/proj/build", "java", BASE "/tool", "17");
  // An empty kotlinc version is a corrupt entry.
  ccb_sdk_probe_cache_store(BASE "/proj/build", "kotlinc", BASE "/tool", "2.4.21");
  cache_replace("\"version\":\"2.4.21\"", "\"version\":\"\"");
  CHECK(ccb_sdk_probe_cache_lookup(BASE "/proj/build", "kotlinc", BASE "/tool") == NULL);
  // Store with no intermediates directory is a silent no-op.
  write_text(BASE "/tool2", "x");
  ccb_sdk_probe_cache_store(BASE "/nodoc", "java", BASE "/tool2", "17");
  CHECK(!ccb_path_exists(BASE "/nodoc/intermediates/tool-probes.json"));
  // NULL build dir means no cache, never a crash.
  CHECK(ccb_sdk_probe_cache_lookup(NULL, "java", BASE "/tool") == NULL);
  ccb_sdk_probe_cache_store(NULL, "java", BASE "/tool", "17");

  // Black-box: a real build probes once, then hits the cache.
  char *bin = find_binary(argc, argv);
  if (bin == NULL) {
    fprintf(stderr, "note: no ccb binary found, skipping black-box probe tests\n");
    TEST_MAIN_END();
  }
  CHECK(ccb_mkdir_p(SDK "/build-tools/35.0.0") == CCB_OK);
  CHECK(ccb_mkdir_p(SDK "/platforms/android-35") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ "/src/main/res/values") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ "/src/main/kotlin/com/example/hello") == CCB_OK);
  CHECK(ccb_mkdir_p(KHOME "/bin") == CCB_OK);
  CHECK(ccb_mkdir_p(JAVADIR "/bin") == CCB_OK);
  const char *tools[] = { "aapt2", "d8", "apksigner", "zipalign" };
  for (size_t i = 0; i < 4; i++) {
    char *p = ccb_path_join(SDK "/build-tools/35.0.0", tools[i]);
    write_script(p, "#!/bin/sh\nexit 0\n");
    free(p);
  }
  write_text(SDK "/platforms/android-35/android.jar", "");
  write_script(JAVADIR "/bin/java", "#!/bin/sh\necho 'openjdk version \"17.0.1\"' >&2\n");
  write_script(JAVADIR "/bin/javac", "#!/bin/sh\nexit 0\n");
  write_kotlinc_wrapper("2.4.21");
  write_text(PROJ "/src/main/AndroidManifest.xml", "<manifest package=\"com.example.hello\" />\n");
  write_text(PROJ "/src/main/kotlin/com/example/hello/MainActivity.kt",
    "package com.example.hello\nclass MainActivity\n");
  ccb_str cfg;
  ccb_str_init(&cfg);
  ccb_str_append(&cfg, "ccb_version = 1\n[project]\nname = \"hello\"\n"
    "package = \"com.example.hello\"\nversion = \"1.0\"\nmin_sdk = 26\n"
    "target_sdk = 35\ncompile_sdk = 35\n[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\nres = \"src/main/res\"\n"
    "kotlin = \"src/main/kotlin\"\n[sdk]\nhome = \"");
  ccb_str_append(&cfg, SDK);
  ccb_str_append(&cfg, "\"\nbuild_tools = \"35.0.0\"\n");
  write_text(PROJ "/ccb.toml", cfg.buf);
  ccb_str_free(&cfg);
  ccb_str jh;
  ccb_str_init(&jh);
  ccb_str_append(&jh, "JAVA_HOME=");
  ccb_str_append(&jh, JAVADIR);
  ccb_str kh;
  ccb_str_init(&kh);
  ccb_str_append(&kh, "KOTLIN_HOME=");
  ccb_str_append(&kh, KHOME);
  char *env[] = { jh.buf, kh.buf, "ANDROID_HOME", "CCB_AAPT2", "CCB_ZIPALIGN",
    "CCB_KOTLIN_URL", "CCB_KOTLIN_SHA256", NULL };
  // Build 1 probes once (the build itself may fail at a stub step;
  // only the probe count matters here).
  {
    char *args[] = { bin, "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ, env, &r) == CCB_OK);
    CHECK(r.exit_code != 127);
    ccb_proc_result_free(&r);
  }
  CHECK(count_version_probes() == 1);
  // The build recorded both probes.
  {
    char *text = NULL;
    size_t len = 0;
    CHECK(ccb_read_file(CACHE, &text, &len) == CCB_OK);
    CHECK(strstr(text, "\"tool\":\"java\"") != NULL);
    CHECK(strstr(text, "\"tool\":\"kotlinc\"") != NULL);
    free(text);
  }
  // Build 2 hits the cache: no new probe.
  {
    char *args[] = { bin, "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ, env, &r) == CCB_OK);
    CHECK(r.exit_code != 127);
    ccb_proc_result_free(&r);
  }
  CHECK(count_version_probes() == 1);
  // Check reads the cache too, and reports the cached version.
  {
    char *args[] = { bin, "check", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "kotlinc: 2.4.21") != NULL || strstr(r.err, "kotlinc: 2.4.21") != NULL);
    ccb_proc_result_free(&r);
  }
  CHECK(count_version_probes() == 1);
  // A replaced kotlinc re-probes (same size, new mtime) and the new
  // version is picked up, not the stale cached one.
  write_kotlinc_wrapper("2.4.22");
  {
    char *args[] = { bin, "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ, env, &r) == CCB_OK);
    CHECK(r.exit_code != 127);
    ccb_proc_result_free(&r);
  }
  CHECK(count_version_probes() == 2);
  {
    char *args[] = { bin, "check", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "kotlinc: 2.4.22") != NULL || strstr(r.err, "kotlinc: 2.4.22") != NULL);
    ccb_proc_result_free(&r);
  }
  CHECK(count_version_probes() == 2);
  // Deleting the cache falls back to probing.
  CHECK(remove(CACHE) == 0);
  {
    char *args[] = { bin, "check", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    ccb_proc_result_free(&r);
  }
  CHECK(count_version_probes() == 3);
  ccb_str_free(&jh);
  ccb_str_free(&kh);
  free(bin);
  TEST_MAIN_END();
}
