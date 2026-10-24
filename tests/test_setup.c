// Tests for `ccb setup`: licenses are accepted before components are
// installed, a missing JDK fails fast with a clear message, and setup
// never reports success when components did not land. The fixture SDKs
// are entirely fake (scripted sdkmanager binaries), so no real Android
// SDK or network is required. The ccb binary is argv[1] when given,
// else a sibling `ccb` next to this test executable.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define BASE "/tmp/ccb_test_setup"
#define SDK_OK BASE "/sdk-ok"
#define SDK_BROKEN BASE "/sdk-broken"
#define SDK_INVALID BASE "/sdk-invalid"
#define PROJ_OK BASE "/proj-ok"
#define PROJ_BROKEN BASE "/proj-broken"
#define PROJ_INVALID BASE "/proj-invalid"

static void write_text(const char *path, const char *text) {
  CHECK(ccb_write_file(path, text, strlen(text)) == CCB_OK);
}

static void write_script(const char *path, const char *body) {
  write_text(path, body);
  CHECK(chmod(path, 0755) == 0);
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

static void write_config(const char *proj, const char *sdk_home) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "ccb_version = 1\n"
    "[project]\n"
    "name = \"hello\"\n"
    "package = \"com.example.hello\"\n"
    "version = \"1.0\"\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\n"
    "kotlin = \"src/main/kotlin\"\n"
    "[sdk]\n"
    "home = \"");
  ccb_str_append(&s, sdk_home);
  ccb_str_append(&s, "\"\nbuild_tools = \"35.0.0\"\n");
  char *cfg = ccb_path_join(proj, "ccb.toml");
  write_text(cfg, s.buf);
  free(cfg);
  ccb_str_free(&s);
}

// Read the first two lines of a calls log into line1/line2 (empty
// when absent). Returns 1 when at least one line was read.
static int read_calls(const char *path, char *line1, size_t n1, char *line2, size_t n2) {
  line1[0] = '\0';
  line2[0] = '\0';
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    return 0;
  }
  int got = 0;
  if (fgets(line1, (int)n1, f) != NULL) {
    got = 1;
    if (fgets(line2, (int)n2, f) == NULL) {
      line2[0] = '\0';
    }
  }
  fclose(f);
  return got;
}

int main(int argc, char **argv) {
  char *bin = find_binary(argc, argv);
  CHECK(bin != NULL);

  ccb_remove_recursive(BASE);
  CHECK(ccb_mkdir_p(SDK_OK "/cmdline-tools/latest/bin") == CCB_OK);
  CHECK(ccb_mkdir_p(SDK_BROKEN "/cmdline-tools/latest/bin") == CCB_OK);
  CHECK(ccb_mkdir_p(SDK_INVALID "/cmdline-tools/latest/bin") == CCB_OK);
  CHECK(ccb_mkdir_p(BASE "/java/bin") == CCB_OK);
  CHECK(ccb_mkdir_p(BASE "/empty-java") == CCB_OK);
  CHECK(ccb_mkdir_p(BASE "/shbin") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ_OK "/src/main/res/values") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ_OK "/src/main/kotlin") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ_BROKEN "/src/main/res/values") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ_BROKEN "/src/main/kotlin") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ_INVALID "/src/main/res/values") == CCB_OK);
  CHECK(ccb_mkdir_p(PROJ_INVALID "/src/main/kotlin") == CCB_OK);
  // Config load requires the manifest to exist as a file.
  write_text(PROJ_OK "/src/main/AndroidManifest.xml",
    "<manifest package=\"com.example.hello\" />\n");
  write_text(PROJ_BROKEN "/src/main/AndroidManifest.xml",
    "<manifest package=\"com.example.hello\" />\n");
  // PROJ_INVALID deliberately gets no manifest: its config is invalid.
  // Fake java answering -version in the expected format.
  write_script(BASE "/java/bin/java",
    "#!/bin/sh\necho 'openjdk version \"17.0.1\"' >&2\n");
  // shbin holds only sh, so a PATH pointing here has no java.
  CHECK(symlink("/bin/sh", BASE "/shbin/sh") == 0);

  // sdkmanager with real license semantics: installing components
  // creates their dirs only once licenses have been accepted;
  // before that it silently installs nothing (exit 0).
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "#!/bin/sh\necho \"$@\" >> \"" BASE "/ok-calls.log\"\n");
    ccb_str_append(&s, "if [ \"$1\" = \"--licenses\" ]; then\n");
    ccb_str_append(&s, "  touch \"" BASE "/ok-licensed\"\n  exit 0\nfi\n");
    ccb_str_append(&s, "if [ -f \"" BASE "/ok-licensed\" ]; then\n");
    ccb_str_append(&s, "  mkdir -p \"" SDK_OK "/platform-tools\" "
      "\"" SDK_OK "/platforms/android-35\" "
      "\"" SDK_OK "/build-tools/35.0.0\"\nfi\nexit 0\n");
    write_script(SDK_OK "/cmdline-tools/latest/bin/sdkmanager", s.buf);
    ccb_str_free(&s);
  }
  // sdkmanager that never installs anything, even after licenses.
  write_script(SDK_BROKEN "/cmdline-tools/latest/bin/sdkmanager",
    "#!/bin/sh\necho \"$@\" >> \"" BASE "/broken-calls.log\"\nexit 0\n");
  // Must never run: the invalid-config case fails before sdkmanager.
  write_script(SDK_INVALID "/cmdline-tools/latest/bin/sdkmanager",
    "#!/bin/sh\necho \"$@\" >> \"" BASE "/invalid-calls.log\"\nexit 0\n");

  write_config(PROJ_OK, SDK_OK);
  write_config(PROJ_BROKEN, SDK_BROKEN);
  write_config(PROJ_INVALID, SDK_INVALID);

  ccb_str jh, jh_empty, path_shbin;
  ccb_str_init(&jh);
  ccb_str_init(&jh_empty);
  ccb_str_init(&path_shbin);
  ccb_str_append(&jh, "JAVA_HOME=" BASE "/java");
  ccb_str_append(&jh_empty, "JAVA_HOME=" BASE "/empty-java");
  ccb_str_append(&path_shbin, "PATH=" BASE "/shbin");

  // 1. No JDK: setup fails fast with a clear message, before ever
  // invoking sdkmanager, and never claims success.
  {
    char *args[] = { bin, "--config", PROJ_OK "/ccb.toml", "setup", NULL };
    char *env[] = { jh_empty.buf, path_shbin.buf, "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code != 0);
    CHECK(strstr(r.err, "java not found") != NULL);
    CHECK(strstr(r.out, "SDK ready") == NULL);
    CHECK(!ccb_path_exists(BASE "/ok-calls.log"));
    ccb_proc_result_free(&r);
  }

  // 2. Fresh SDK, JDK present: licenses are accepted before the
  // install step, the components land, and setup reports ready.
  {
    char *args[] = { bin, "--config", PROJ_OK "/ccb.toml", "setup", NULL };
    char *env[] = { jh.buf, "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    ccb_proc_result_free(&r);
    char line1[512], line2[512];
    CHECK(read_calls(BASE "/ok-calls.log", line1, sizeof(line1), line2, sizeof(line2)));
    CHECK(strstr(line1, "--licenses") != NULL);
    CHECK(strstr(line2, "platform-tools") != NULL);
    CHECK(ccb_path_exists(SDK_OK "/platform-tools"));
    CHECK(ccb_path_exists(SDK_OK "/platforms/android-35"));
    CHECK(ccb_path_exists(SDK_OK "/build-tools/35.0.0"));
  }

  // 3. Install that silently installs nothing: setup must fail and
  // name the missing components instead of printing "SDK ready".
  {
    char *args[] = { bin, "--config", PROJ_BROKEN "/ccb.toml", "setup", NULL };
    char *env[] = { jh.buf, "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code != 0);
    CHECK(strstr(r.err, "still missing") != NULL);
    CHECK(strstr(r.err, "platform-tools") != NULL);
    CHECK(strstr(r.out, "SDK ready") == NULL);
    ccb_proc_result_free(&r);
  }

  // 4. Config found but invalid (manifest missing): setup fails with
  // the config diagnostic instead of silently falling back to the
  // default SDK home, and sdkmanager never runs.
  {
    char *args[] = { bin, "--config", PROJ_INVALID "/ccb.toml", "setup", NULL };
    char *env[] = { jh.buf, "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 2);
    CHECK(strstr(r.err, "manifest") != NULL);
    CHECK(strstr(r.out, "SDK ready") == NULL);
    CHECK(!ccb_path_exists(BASE "/invalid-calls.log"));
    ccb_proc_result_free(&r);
  }

  ccb_str_free(&jh);
  ccb_str_free(&jh_empty);
  ccb_str_free(&path_shbin);
  free(bin);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
