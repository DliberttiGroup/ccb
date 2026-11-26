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
#include "ccb_zip.h"
#include "ccb_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)
#define SDK_OK B("sdk-ok")
#define SDK_BROKEN B("sdk-broken")
#define SDK_INVALID B("sdk-invalid")
#define SDK_PKG B("sdk-pkg")
#define SDK_APT B("sdk-apt")
#define SDK_PTY B("sdk-pty")
#define SDK_KOT B("sdk-kot")
#define SDK_NDK B("sdk-ndk")
#define SDK_R8 B("sdk-r8")
#define SDK_UPG B("sdk-upg")
#define SDK_SKIP B("sdk-skip")
#define PROJ_OK B("proj-ok")
#define PROJ_BROKEN B("proj-broken")
#define PROJ_INVALID B("proj-invalid")
#define PROJ_PKG B("proj-pkg")
#define PROJ_APT B("proj-apt")
#define PROJ_PTY B("proj-pty")
#define PROJ_KOT B("proj-kot")
#define PROJ_NDK B("proj-ndk")
#define PROJ_R8 B("proj-r8")
#define PROJ_UPG B("proj-upg")
#define PROJ_SKIP B("proj-skip")

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

static void write_config(const char *proj, const char *sdk_home, const char *ndk_version, int minify) {
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
  if (ndk_version != NULL) {
    ccb_str_append(&s, "ndk_version = \"");
    ccb_str_append(&s, ndk_version);
    ccb_str_append(&s, "\"\n");
  }
  if (minify) {
    ccb_str_append(&s, "[build]\nvariant = \"release\"\nminify = true\nproguard_rules = \"proguard-rules.pro\"\n");
  }
  if (ndk_version != NULL) {
    ccb_str_append(&s, "[ndk]\nabis = [\"arm64-v8a\"]\n");
  }
  char *cfg = ccb_path_join(proj, "ccb.toml");
  write_text(cfg, s.buf);
  free(cfg);
  ccb_str_free(&s);
}

// Fake Termux pkg: "install" drops a fake java for openjdk-17, or a
// runnable stub for aapt2/zipalign/android-tools.
static void write_fake_pkg(const char *path) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "#!/bin/sh\nDIR=${0%/*}\nif [ \"$1\" = \"install\" ]; then\n"
    "  case \"$3\" in\n"
    "    openjdk-17) /bin/cat \"");
  ccb_str_append(&s, B("java-script"));
  ccb_str_append(&s, "\" > \"$DIR/java\"; /bin/chmod +x \"$DIR/java\";;\n"
    "    aapt2) printf '#!/bin/sh\nexit 0\n' > \"$DIR/aapt2\"; /bin/chmod +x \"$DIR/aapt2\";;\n"
    "    zipalign) printf '#!/bin/sh\nexit 0\n' > \"$DIR/zipalign\"; /bin/chmod +x \"$DIR/zipalign\";;\n"
    "    android-tools) printf '#!/bin/sh\nexit 0\n' > \"$DIR/adb\"; /bin/chmod +x \"$DIR/adb\";;\n"
    "  esac\nfi\nexit 0\n");
  write_script(path, s.buf);
  ccb_str_free(&s);
}

// Like write_sdkmanager_ok, but an "ndk;<version>" argument also
// creates the NDK dir.
static void write_sdkmanager_ndk(const char *sdk, const char *log, const char *marker, const char *version) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "#!/bin/sh\necho \"$@\" >> \"");
  ccb_str_append(&s, log);
  ccb_str_append(&s, "\"\nif [ \"$1\" = \"--licenses\" ]; then\n  touch \"");
  ccb_str_append(&s, marker);
  ccb_str_append(&s, "\"\n  exit 0\nfi\nif [ -f \"");
  ccb_str_append(&s, marker);
  ccb_str_append(&s, "\" ]; then\n  mkdir -p \"");
  ccb_str_append(&s, sdk);
  ccb_str_append(&s, "/platform-tools\" \"");
  ccb_str_append(&s, sdk);
  ccb_str_append(&s, "/platforms/android-35\" \"");
  ccb_str_append(&s, sdk);
  ccb_str_append(&s, "/build-tools/35.0.0\"\n  case \"$*\" in *\"ndk;\"*) mkdir -p \"");
  ccb_str_append(&s, sdk);
  ccb_str_append(&s, "/ndk/");
  ccb_str_append(&s, version);
  ccb_str_append(&s, "\";; esac\nfi\nexit 0\n");
  char *path = ccb_path_join(sdk, "cmdline-tools/latest/bin/sdkmanager");
  write_script(path, s.buf);
  free(path);
  ccb_str_free(&s);
}

// sdkmanager with real license semantics: installing components
// creates their dirs only once licenses have been accepted;
// before that it silently installs nothing (exit 0).
static void write_sdkmanager_ok(const char *sdk, const char *log, const char *marker) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "#!/bin/sh\necho \"$@\" >> \"");
  ccb_str_append(&s, log);
  ccb_str_append(&s, "\"\nif [ \"$1\" = \"--licenses\" ]; then\n  touch \"");
  ccb_str_append(&s, marker);
  ccb_str_append(&s, "\"\n  exit 0\nfi\nif [ -f \"");
  ccb_str_append(&s, marker);
  ccb_str_append(&s, "\" ]; then\n  mkdir -p \"");
  ccb_str_append(&s, sdk);
  ccb_str_append(&s, "/platform-tools\" \"");
  ccb_str_append(&s, sdk);
  ccb_str_append(&s, "/platforms/android-35\" \"");
  ccb_str_append(&s, sdk);
  ccb_str_append(&s, "/build-tools/35.0.0\"\nfi\nexit 0\n");
  char *path = ccb_path_join(sdk, "cmdline-tools/latest/bin/sdkmanager");
  write_script(path, s.buf);
  free(path);
  ccb_str_free(&s);
}
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
  ccb_test_tmpdir(BASE, sizeof BASE, "setup");
  char *bin = find_binary(argc, argv);
  CHECK(bin != NULL);

  ccb_remove_recursive(BASE);
  CHECK(ccb_mkdir_p(B("sdk-ok/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-broken/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-invalid/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-pkg/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-apt/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-pty/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-kot/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-kot/build-tools/35.0.0")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-kot/platforms/android-35")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("kbin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("kbin-old")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("khome/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("java/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("empty-java")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("shbin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("aptbin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("termux-prefix/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("termux-prefix2/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("termux-prefix3/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-ok/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-ok/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-broken/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-broken/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-invalid/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-invalid/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-pkg/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-pkg/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-apt/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-apt/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-pty/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-pty/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-kot/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-kot/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("nocfg")) == CCB_OK);
  // Config load requires the manifest to exist as a file.
  write_text(B("proj-ok/src/main/AndroidManifest.xml"),
    "<manifest package=\"com.example.hello\" />\n");
  write_text(B("proj-broken/src/main/AndroidManifest.xml"),
    "<manifest package=\"com.example.hello\" />\n");
  // PROJ_INVALID deliberately gets no manifest: its config is invalid.
  // Fake java answering -version in the expected format.
  write_script(B("java/bin/java"),
    "#!/bin/sh\necho 'openjdk version \"17.0.1\"' >&2\n");
  // shbin holds only sh and yes, so a PATH pointing here has no java.
  CHECK(symlink("/bin/sh", B("shbin/sh")) == 0);
  CHECK(symlink("/usr/bin/yes", B("shbin/yes")) == 0);
  CHECK(symlink("/bin/sh", B("aptbin/sh")) == 0);
  CHECK(symlink("/usr/bin/yes", B("aptbin/yes")) == 0);
  // The fake sdkmanager needs these externals under restricted PATHs.
  CHECK(symlink("/usr/bin/touch", B("shbin/touch")) == 0);
  CHECK(symlink("/usr/bin/mkdir", B("shbin/mkdir")) == 0);
  CHECK(symlink("/usr/bin/touch", B("aptbin/touch")) == 0);
  CHECK(symlink("/usr/bin/mkdir", B("aptbin/mkdir")) == 0);
  // The kotlinc install needs curl + unzip under restricted PATHs.
  CHECK(symlink("/usr/bin/curl", B("shbin/curl")) == 0);
  CHECK(symlink("/usr/bin/unzip", B("shbin/unzip")) == 0);
  // Reusable fake java script: fake installers copy it into place.
  write_script(B("java-script"),
    "#!/bin/sh\necho 'openjdk version \"17.0.1\"' >&2\n");
  // Fake javac (existence only; resolve never runs it).
  write_script(B("java/bin/javac"), "#!/bin/sh\nexit 0\n");
  // Fake kotlinc binaries: a usable 2.x and an old 1.9.
  write_script(B("kbin/kotlinc"),
    "#!/bin/sh\necho 'info: kotlinc-jvm 2.4.21 (JRE 17.0.1)' >&2\n");
  write_script(B("kbin-old/kotlinc"),
    "#!/bin/sh\necho 'info: kotlinc-jvm 1.9.24 (JRE 17.0.1)' >&2\n");
  // KOTLIN_HOME layout for the pre-kotlinc cases: they test the SDK
  // flow, so they get a usable kotlinc and skip the install step.
  write_script(B("khome/bin/kotlinc"),
    "#!/bin/sh\necho 'info: kotlinc-jvm 2.4.21 (JRE 17.0.1)' >&2\n");
  // Fake Termux pkg: "install" drops a fake java next to itself.
  write_fake_pkg(B("termux-prefix/bin/pkg"));
  write_fake_pkg(B("termux-prefix2/bin/pkg"));
  write_fake_pkg(B("termux-prefix3/bin/pkg"));
  // Fake apt-get (update is a no-op; install drops a fake java for
  // openjdk-17-jdk, or a runnable adb stub for adb) and a fake sudo
  // that just execs its arguments.
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "#!/bin/sh\nDIR=${0%/*}\nif [ \"$1\" = \"install\" ]; then\n"
      "  case \"$3\" in\n"
      "    openjdk-17-jdk) /bin/cat \"");
    ccb_str_append(&s, B("java-script"));
    ccb_str_append(&s, "\" > \"$DIR/java\"; /bin/chmod +x \"$DIR/java\";;\n"
      "    adb) printf '#!/bin/sh\nexit 0\n' > \"$DIR/adb\"; /bin/chmod +x \"$DIR/adb\";;\n"
      "  esac\nfi\nexit 0\n");
    write_script(B("aptbin/apt-get"), s.buf);
    ccb_str_free(&s);
  }
  write_script(B("aptbin/sudo"), "#!/bin/sh\nexec \"$@\"\n");

  write_sdkmanager_ok(SDK_OK, B("ok-calls.log"), B("ok-licensed"));
  write_sdkmanager_ok(SDK_PKG, B("pkg-calls.log"), B("pkg-licensed"));
  write_sdkmanager_ok(SDK_APT, B("apt-calls.log"), B("apt-licensed"));
  write_sdkmanager_ok(SDK_PTY, B("pty-calls.log"), B("pty-licensed"));
  write_sdkmanager_ok(SDK_KOT, B("kot-calls.log"), B("kot-licensed"));
  // Stub build tools + android.jar so `ccb check` can pass against
  // the kotlinc fixture SDK (resolve checks existence only).
  write_script(B("sdk-kot/build-tools/35.0.0/aapt2"), "#!/bin/sh\nexit 0\n");
  write_script(B("sdk-kot/build-tools/35.0.0/d8"), "#!/bin/sh\nexit 0\n");
  write_script(B("sdk-kot/build-tools/35.0.0/apksigner"), "#!/bin/sh\nexit 0\n");
  write_script(B("sdk-kot/build-tools/35.0.0/zipalign"), "#!/bin/sh\nexit 0\n");
  write_text(B("sdk-kot/platforms/android-35/android.jar"), "");
  // SDKs/projects for the NDK, r8, and upgrade cases.
  CHECK(ccb_mkdir_p(B("sdk-ndk/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-ndk/build-tools/35.0.0")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-ndk/platforms/android-35")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-r8/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-r8/build-tools/35.0.0")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-r8/platforms/android-35")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-upg/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-upg/build-tools/35.0.0")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-upg/platforms/android-35")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-ndk/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-ndk/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-r8/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-r8/src/main/kotlin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-upg/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-upg/src/main/kotlin")) == CCB_OK);
  write_text(B("proj-ndk/src/main/AndroidManifest.xml"), "<manifest package=\"com.example.hello\" />\n");
  write_text(B("proj-r8/src/main/AndroidManifest.xml"), "<manifest package=\"com.example.hello\" />\n");
  write_text(B("proj-upg/src/main/AndroidManifest.xml"), "<manifest package=\"com.example.hello\" />\n");
  write_config(PROJ_NDK, SDK_NDK, "27.0.12077973", 0);
  write_config(PROJ_R8, SDK_R8, NULL, 1);
  write_text(B("proj-r8/proguard-rules.pro"), "");
  write_config(PROJ_UPG, SDK_UPG, NULL, 0);
  write_sdkmanager_ndk(SDK_NDK, B("ndk-calls.log"), B("ndk-licensed"), "27.0.12077973");
  write_sdkmanager_ok(SDK_R8, B("r8-calls.log"), B("r8-licensed"));
  write_sdkmanager_ok(SDK_UPG, B("upg-calls.log"), B("upg-licensed"));
  {
    const char *sdks[] = { SDK_NDK, SDK_R8, SDK_UPG };
    const char *tools[] = { "aapt2", "d8", "apksigner", "zipalign" };
    for (size_t i = 0; i < 3; i++) {
      for (size_t j = 0; j < 4; j++) {
        char *bt = ccb_path_join3(sdks[i], "build-tools/35.0.0", tools[j]);
        write_script(bt, "#!/bin/sh\nexit 0\n");
        free(bt);
      }
      char *jar = ccb_path_join(sdks[i], "platforms/android-35/android.jar");
      write_text(jar, "");
      free(jar);
    }
  }
  // Stale SDK-home kotlinc for the upgrade case.
  CHECK(ccb_mkdir_p(B("sdk-upg/kotlinc/bin")) == CCB_OK);
  write_script(B("sdk-upg/kotlinc/bin/kotlinc"),
    "#!/bin/sh\necho 'info: kotlinc-jvm 1.9.24 (JRE 17.0.1)' >&2\n");
  // Fixture r8 jar + a java that answers both -version and the R8
  // --version probe.
  write_text(B("r8-fixture.jar"), "fake r8 jar\n");
  CHECK(ccb_mkdir_p(B("r8-java/bin")) == CCB_OK);
  write_script(B("r8-java/bin/java"),
    "#!/bin/sh\nfor a in \"$@\"; do\n"
    "  if [ \"$a\" = \"com.android.tools.r8.R8\" ]; then echo \"R8 9.5.28\"; exit 0; fi\n"
    "done\necho 'openjdk version \"17.0.1\"' >&2\n");
  write_script(B("r8-java/bin/javac"), "#!/bin/sh\nexit 0\n");
  // Rendiix prefix: tool-aware pkg + apt-cache (no official
  // candidate for zipalign) + wget + apt.
  CHECK(ccb_mkdir_p(B("termux-prefix-rendiix/bin")) == CCB_OK);
  write_fake_pkg(B("termux-prefix-rendiix/bin/pkg"));
  write_script(B("termux-prefix-rendiix/bin/apt-cache"),
    "#!/bin/sh\necho \"zipalign:\"\necho \"  Installed: (none)\"\necho \"  Candidate: (none)\"\nexit 0\n");
  write_script(B("termux-prefix-rendiix/bin/wget"),
    "#!/bin/sh\ntouch \"$2/rendiix.gpg\"\nexit 0\n");
  {
    char script[512];
    snprintf(script, sizeof script, "#!/bin/sh\ntouch \"%s\"\nexit 0\n", B("apt-updated"));
    write_script(B("termux-prefix-rendiix/bin/apt"), script);
  }
  // Fixture kotlinc zip (real layout: kotlinc/bin/kotlinc) + its hash.
  {
    ccb_zip_writer w;
    CHECK(ccb_zip_writer_init(&w, B("kotlin-fixture.zip")) == CCB_OK);
    const char *script =
      "#!/bin/sh\necho 'info: kotlinc-jvm 2.4.21 (JRE 17.0.1)' >&2\n";
    CHECK(ccb_zip_writer_add_data(&w, "kotlinc/bin/kotlinc", script, strlen(script)) == CCB_OK);
    CHECK(ccb_zip_writer_finish(&w) == CCB_OK);
  }
  char kot_sha[65];
  CHECK(ccb_sha256_file(B("kotlin-fixture.zip"), kot_sha) == CCB_OK);
  // sdkmanager that never installs anything, even after licenses.
  {
    char script[512];
    snprintf(script, sizeof script, "#!/bin/sh\necho \"$@\" >> \"%s\"\nexit 0\n", B("broken-calls.log"));
    write_script(B("sdk-broken/cmdline-tools/latest/bin/sdkmanager"), script);
  }
  // Skip fixture: all three components already on disk, and a
  // sdkmanager that fails loudly if invoked (bug-setup: setup must
  // not call it at all here, e.g. behind a proxy that blocks it).
  CHECK(ccb_mkdir_p(B("sdk-skip/cmdline-tools/latest/bin")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-skip/platform-tools")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-skip/platforms/android-35")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("sdk-skip/build-tools/35.0.0")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-skip/src/main/res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("proj-skip/src/main/kotlin")) == CCB_OK);
  {
    char script[512];
    snprintf(script, sizeof script, "#!/bin/sh\necho \"$@\" >> \"%s\"\nexit 1\n", B("skip-calls.log"));
    write_script(B("sdk-skip/cmdline-tools/latest/bin/sdkmanager"), script);
  }
  write_text(B("proj-skip/src/main/AndroidManifest.xml"),
    "<manifest package=\"com.example.hello\" />\n");
  write_config(PROJ_SKIP, SDK_SKIP, NULL, 0);
  // Must never run: the invalid-config case fails before sdkmanager.
  {
    char script[512];
    snprintf(script, sizeof script, "#!/bin/sh\necho \"$@\" >> \"%s\"\nexit 0\n", B("invalid-calls.log"));
    write_script(B("sdk-invalid/cmdline-tools/latest/bin/sdkmanager"), script);
  }

  write_text(B("proj-pkg/src/main/AndroidManifest.xml"),
    "<manifest package=\"com.example.hello\" />\n");
  write_text(B("proj-apt/src/main/AndroidManifest.xml"),
    "<manifest package=\"com.example.hello\" />\n");
  write_text(B("proj-pty/src/main/AndroidManifest.xml"),
    "<manifest package=\"com.example.hello\" />\n");
  write_text(B("proj-kot/src/main/AndroidManifest.xml"),
    "<manifest package=\"com.example.hello\" />\n");

  write_config(PROJ_OK, SDK_OK, NULL, 0);
  write_config(PROJ_BROKEN, SDK_BROKEN, NULL, 0);
  write_config(PROJ_INVALID, SDK_INVALID, NULL, 0);
  write_config(PROJ_PKG, SDK_PKG, NULL, 0);
  write_config(PROJ_APT, SDK_APT, NULL, 0);
  write_config(PROJ_PTY, SDK_PTY, NULL, 0);
  write_config(PROJ_KOT, SDK_KOT, NULL, 0);

  ccb_str jh, jh_empty, path_shbin, khome;
  ccb_str_init(&jh);
  ccb_str_init(&jh_empty);
  ccb_str_init(&path_shbin);
  ccb_str_init(&khome);
  ccb_str_append(&jh, "JAVA_HOME=");
  ccb_str_append(&jh, B("java"));
  ccb_str_append(&jh_empty, "JAVA_HOME=");
  ccb_str_append(&jh_empty, B("empty-java"));
  ccb_str_append(&path_shbin, "PATH=");
  ccb_str_append(&path_shbin, B("shbin"));
  ccb_str_append(&khome, "KOTLIN_HOME=");
  ccb_str_append(&khome, B("khome"));

  // 1. No JDK, no installer: setup fails fast with a clear message,
  // before ever invoking sdkmanager, and never claims success.
  {
    char *args[] = { bin, "--config", B("proj-ok/ccb.toml"), "setup", NULL };
    char *env[] = { jh_empty.buf, path_shbin.buf, "ANDROID_HOME", "PREFIX", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code != 0);
    CHECK(strstr(r.err, "java not found") != NULL);
    CHECK(strstr(r.out, "SDK ready") == NULL);
    CHECK(!ccb_path_exists(B("ok-calls.log")));
    ccb_proc_result_free(&r);
  }

  // 2. Fresh SDK, JDK present: licenses are accepted before the
  // install step, the components land, and setup reports ready.
  {
    char *args[] = { bin, "--config", B("proj-ok/ccb.toml"), "setup", NULL };
    char *env[] = { jh.buf, khome.buf, "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    ccb_proc_result_free(&r);
    char line1[512], line2[512];
    CHECK(read_calls(B("ok-calls.log"), line1, sizeof(line1), line2, sizeof(line2)));
    CHECK(strstr(line1, "--licenses") != NULL);
    CHECK(strstr(line2, "platform-tools") != NULL);
    CHECK(ccb_path_exists(B("sdk-ok/platform-tools")));
    CHECK(ccb_path_exists(B("sdk-ok/platforms/android-35")));
    CHECK(ccb_path_exists(B("sdk-ok/build-tools/35.0.0")));
  }

  // 2b. Components already present: setup skips the licenses and
  // sdkmanager steps entirely (bug-setup) and still reports ready.
  // The fixture sdkmanager exits 1 if invoked, so any call fails
  // the setup run outright.
  {
    char *args[] = { bin, "--config", B("proj-skip/ccb.toml"), "setup", NULL };
    char *env[] = { jh.buf, khome.buf, "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    CHECK(!ccb_path_exists(B("skip-calls.log")));
    ccb_proc_result_free(&r);
  }

  // 3. Install that silently installs nothing: setup must fail and
  // name the missing components instead of printing "SDK ready".
  {
    char *args[] = { bin, "--config", B("proj-broken/ccb.toml"), "setup", NULL };
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
    char *args[] = { bin, "--config", B("proj-invalid/ccb.toml"), "setup", NULL };
    char *env[] = { jh.buf, "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 2);
    CHECK(strstr(r.err, "manifest") != NULL);
    CHECK(strstr(r.out, "SDK ready") == NULL);
    CHECK(!ccb_path_exists(B("invalid-calls.log")));
    ccb_proc_result_free(&r);
  }

  // 5. Termux + --yes: no JDK, fake pkg installs one, setup then
  // completes normally and reports ready.
  {
    ccb_str prefix, path_pkg;
    ccb_str_init(&prefix);
    ccb_str_init(&path_pkg);
    ccb_str_append(&prefix, "PREFIX=");
    ccb_str_append(&prefix, B("termux-prefix"));
    ccb_str_append(&path_pkg, "PATH=");
    ccb_str_append(&path_pkg, B("termux-prefix/bin:"));
    ccb_str_append(&path_pkg, B("shbin"));
    char *args[] = { bin, "--config", B("proj-pkg/ccb.toml"), "--yes", "setup", NULL };
    char *env[] = { prefix.buf, path_pkg.buf, khome.buf, "JAVA_HOME", "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    // The final pass warns about the fixture SDK's missing
    // android.jar even though setup itself succeeded.
    CHECK(strstr(r.err, "warning:") != NULL);
    ccb_proc_result_free(&r);
    CHECK(ccb_path_exists(B("termux-prefix/bin/java")));
    CHECK(ccb_path_exists(B("termux-prefix/bin/aapt2")));
    CHECK(ccb_path_exists(B("termux-prefix/bin/zipalign")));
    CHECK(ccb_path_exists(B("termux-prefix/bin/adb")));
    CHECK(ccb_path_exists(B("sdk-pkg/platform-tools")));
    CHECK(ccb_path_exists(B("sdk-pkg/platforms/android-35")));
    CHECK(ccb_path_exists(B("sdk-pkg/build-tools/35.0.0")));
    ccb_str_free(&prefix);
    ccb_str_free(&path_pkg);
  }

  // 6. Distro apt-get + --yes (via fake sudo when non-root): same
  // flow, full success.
  {
    ccb_str path_apt;
    ccb_str_init(&path_apt);
    ccb_str_append(&path_apt, "PATH=");
    ccb_str_append(&path_apt, B("aptbin"));
    char *args[] = { bin, "--config", B("proj-apt/ccb.toml"), "--yes", "setup", NULL };
    char *env[] = { path_apt.buf, khome.buf, "PREFIX", "JAVA_HOME", "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    ccb_proc_result_free(&r);
    CHECK(ccb_path_exists(B("aptbin/java")));
    CHECK(ccb_path_exists(B("aptbin/adb")));
    CHECK(ccb_path_exists(B("sdk-apt/platform-tools")));
    CHECK(ccb_path_exists(B("sdk-apt/platforms/android-35")));
    CHECK(ccb_path_exists(B("sdk-apt/build-tools/35.0.0")));
    ccb_str_free(&path_apt);
  }

  // 7. Installer present but no --yes and no TTY: setup refuses to
  // install unattended, names --yes, and installs nothing.
  {
    ccb_str prefix2, path_pkg2;
    ccb_str_init(&prefix2);
    ccb_str_init(&path_pkg2);
    ccb_str_append(&prefix2, "PREFIX=");
    ccb_str_append(&prefix2, B("termux-prefix2"));
    ccb_str_append(&path_pkg2, "PATH=");
    ccb_str_append(&path_pkg2, B("termux-prefix2/bin:"));
    ccb_str_append(&path_pkg2, B("shbin"));
    char *args[] = { bin, "--config", B("proj-pkg/ccb.toml"), "setup", NULL };
    char *env[] = { prefix2.buf, path_pkg2.buf, "JAVA_HOME", "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code != 0);
    CHECK(strstr(r.err, "java not found") != NULL);
    CHECK(strstr(r.err, "--yes") != NULL);
    CHECK(strstr(r.out, "SDK ready") == NULL);
    CHECK(!ccb_path_exists(B("termux-prefix2/bin/java")));
    ccb_proc_result_free(&r);
    ccb_str_free(&prefix2);
    ccb_str_free(&path_pkg2);
  }

  // 8. Interactive prompt under a pty: answering "y" installs via
  // fake pkg and setup completes. Skipped when `script` is absent.
  if (access("/usr/bin/script", X_OK) == 0) {
    ccb_str prefix3, path_pkg3, cmd;
    ccb_str_init(&prefix3);
    ccb_str_init(&path_pkg3);
    ccb_str_init(&cmd);
    ccb_str_append(&prefix3, "PREFIX=");
    ccb_str_append(&prefix3, B("termux-prefix3"));
    ccb_str_append(&path_pkg3, "PATH=");
    ccb_str_append(&path_pkg3, B("termux-prefix3/bin:"));
    ccb_str_append(&path_pkg3, B("shbin"));
    ccb_str_append(&cmd, "printf 'y\\n' | /usr/bin/script -qec \"");
    ccb_str_append(&cmd, bin);
    ccb_str_append(&cmd, " --config ");
    ccb_str_append(&cmd, B("proj-pty/ccb.toml setup\" /dev/null"));
    char *args[] = { "/bin/sh", "-c", cmd.buf, NULL };
    char *env[] = { prefix3.buf, path_pkg3.buf, khome.buf, "JAVA_HOME", "ANDROID_HOME", "TERM=xterm", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    ccb_proc_result_free(&r);
    CHECK(ccb_path_exists(B("termux-prefix3/bin/java")));
    CHECK(ccb_path_exists(B("sdk-pty/platform-tools")));
    CHECK(ccb_path_exists(B("sdk-pty/platforms/android-35")));
    CHECK(ccb_path_exists(B("sdk-pty/build-tools/35.0.0")));
    ccb_str_free(&prefix3);
    ccb_str_free(&path_pkg3);
    ccb_str_free(&cmd);
  } else {
    fprintf(stderr, "note: skipping pty prompt test, /usr/bin/script not found\n");
  }

  // K1. Usable kotlinc on PATH: setup reports it and installs nothing.
  {
    ccb_str path_k;
    ccb_str_init(&path_k);
    ccb_str_append(&path_k, "PATH=");
    ccb_str_append(&path_k, B("kbin:"));
    ccb_str_append(&path_k, B("shbin"));
    char *args[] = { bin, "--config", B("proj-kot/ccb.toml"), "setup", NULL };
    char *env[] = { path_k.buf, jh.buf, "KOTLIN_HOME", "ANDROID_HOME",
      "CCB_KOTLIN_URL", "CCB_KOTLIN_SHA256", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    // The final pass resolves the whole fixture toolchain.
    CHECK(strstr(r.err, "toolchain check: ok") != NULL);
    CHECK(strstr(r.err, "found") != NULL);
    ccb_proc_result_free(&r);
    CHECK(!ccb_path_exists(B("sdk-kot/kotlinc")));
    ccb_str_free(&path_k);
  }

  // K2. No kotlinc anywhere: setup installs the fixture compiler
  // (file:// URL + computed hash) into the SDK home and reports ready.
  {
    ccb_str url, sha;
    ccb_str_init(&url);
    ccb_str_init(&sha);
    ccb_str_append(&url, "CCB_KOTLIN_URL=file://");
    ccb_str_append(&url, B("kotlin-fixture.zip"));
    ccb_str_append(&sha, "CCB_KOTLIN_SHA256=");
    ccb_str_append(&sha, kot_sha);
    char *args[] = { bin, "--config", B("proj-kot/ccb.toml"), "setup", NULL };
    char *env[] = { path_shbin.buf, jh.buf, "KOTLIN_HOME", "ANDROID_HOME",
      url.buf, sha.buf, NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    CHECK(strstr(r.err, "installed") != NULL);
    ccb_proc_result_free(&r);
    CHECK(access(B("sdk-kot/kotlinc/bin/kotlinc"), X_OK) == 0);
    ccb_str_free(&url);
    ccb_str_free(&sha);
  }

  // K3. Old kotlinc on PATH: setup warns it is not 2.x and installs
  // the fixture compiler over the SDK-home one.
  {
    ccb_str path_old, url, sha;
    ccb_str_init(&path_old);
    ccb_str_init(&url);
    ccb_str_init(&sha);
    ccb_str_append(&path_old, "PATH=");
    ccb_str_append(&path_old, B("kbin-old:"));
    ccb_str_append(&path_old, B("shbin"));
    ccb_str_append(&url, "CCB_KOTLIN_URL=file://");
    ccb_str_append(&url, B("kotlin-fixture.zip"));
    ccb_str_append(&sha, "CCB_KOTLIN_SHA256=");
    ccb_str_append(&sha, kot_sha);
    char *args[] = { bin, "--config", B("proj-kot/ccb.toml"), "setup", NULL };
    char *env[] = { path_old.buf, jh.buf, "KOTLIN_HOME", "ANDROID_HOME",
      url.buf, sha.buf, NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    CHECK(strstr(r.err, "not 2.x") != NULL);
    ccb_proc_result_free(&r);
    CHECK(access(B("sdk-kot/kotlinc/bin/kotlinc"), X_OK) == 0);
    ccb_str_free(&path_old);
    ccb_str_free(&url);
    ccb_str_free(&sha);
  }

  // K4. Fallback resolution: with no kotlinc on PATH and no
  // KOTLIN_HOME, `ccb check` passes using the setup-installed
  // <sdk-home>/kotlinc.
  {
    char *args[] = { bin, "--config", B("proj-kot/ccb.toml"), "check", NULL };
    char *env[] = { path_shbin.buf, jh.buf, "KOTLIN_HOME", "ANDROID_HOME",
      "CCB_KOTLIN_URL", "CCB_KOTLIN_SHA256", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    ccb_proc_result_free(&r);
  }

  // K5. No config anywhere (defaults path, cfg NULL): setup must not
  // crash in the kotlinc probe. Regression test: the probe used to
  // dereference cfg unconditionally (SIGSEGV on the phone).
  {
    ccb_str sdk_kot;
    ccb_str_init(&sdk_kot);
    ccb_str_append(&sdk_kot, "ANDROID_HOME=");
    ccb_str_append(&sdk_kot, B("sdk-kot"));
    char *args[] = { bin, "setup", NULL };
    char *env[] = { path_shbin.buf, jh.buf, khome.buf, sdk_kot.buf,
      "CCB_KOTLIN_URL", "CCB_KOTLIN_SHA256", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, B("nocfg"), env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    ccb_proc_result_free(&r);
    ccb_str_free(&sdk_kot);
  }

  // NDK. A project declaring [ndk] gets its revision installed.
  {
    char *args[] = { bin, "--config", B("proj-ndk/ccb.toml"), "setup", NULL };
    char *env[] = { path_shbin.buf, jh.buf, khome.buf, "ANDROID_HOME", "PREFIX", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    CHECK(strstr(r.err, "ndk 27.0.12077973 installed") != NULL);
    ccb_proc_result_free(&r);
    CHECK(ccb_path_is_dir(B("sdk-ndk/ndk/27.0.12077973")));
  }

  // R8. A minify project without [sdk] r8_jar gets the official jar
  // installed into the SDK home.
  {
    char r8_sha[65];
    CHECK(ccb_sha256_file(B("r8-fixture.jar"), r8_sha) == CCB_OK);
    ccb_str url, sha, jh_r8;
    ccb_str_init(&url);
    ccb_str_init(&sha);
    ccb_str_init(&jh_r8);
    ccb_str_append(&url, "CCB_R8_URL=file://");
    ccb_str_append(&url, B("r8-fixture.jar"));
    ccb_str_append(&sha, "CCB_R8_SHA256=");
    ccb_str_append(&sha, r8_sha);
    ccb_str_append(&jh_r8, "JAVA_HOME=");
    ccb_str_append(&jh_r8, B("r8-java"));
    char *args[] = { bin, "--config", B("proj-r8/ccb.toml"), "setup", NULL };
    char *env[] = { path_shbin.buf, jh_r8.buf, khome.buf, url.buf, sha.buf,
      "ANDROID_HOME", "PREFIX", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    CHECK(strstr(r.err, "r8 9.5.28 installed") != NULL);
    ccb_proc_result_free(&r);
    CHECK(ccb_path_exists(B("sdk-r8/r8/r8.jar")));
    ccb_str_free(&url);
    ccb_str_free(&sha);
    ccb_str_free(&jh_r8);
  }

  // RENDIIX. On Termux, zipalign (absent from the official repos)
  // comes from the rendiix repo, added only after apt-cache shows
  // no official candidate.
  {
    ccb_str prefix, path_rendiix;
    ccb_str_init(&prefix);
    ccb_str_init(&path_rendiix);
    ccb_str_append(&prefix, "PREFIX=");
    ccb_str_append(&prefix, B("termux-prefix-rendiix"));
    ccb_str_append(&path_rendiix, "PATH=");
    ccb_str_append(&path_rendiix, B("termux-prefix-rendiix/bin:"));
    ccb_str_append(&path_rendiix, B("shbin"));
    char *args[] = { bin, "--config", B("proj-pkg/ccb.toml"), "--yes", "setup", NULL };
    char *env[] = { prefix.buf, path_rendiix.buf, khome.buf, jh.buf, "ANDROID_HOME", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    ccb_proc_result_free(&r);
    CHECK(ccb_path_exists(B("termux-prefix-rendiix/etc/apt/sources.list.d/rendiix.list")));
    CHECK(ccb_path_exists(B("termux-prefix-rendiix/etc/apt/trusted.gpg.d/rendiix.gpg")));
    CHECK(ccb_path_exists(B("apt-updated")));
    CHECK(ccb_path_exists(B("termux-prefix-rendiix/bin/aapt2")));
    CHECK(ccb_path_exists(B("termux-prefix-rendiix/bin/zipalign")));
    CHECK(ccb_path_exists(B("termux-prefix-rendiix/bin/adb")));
    char *list_text = NULL;
    size_t list_len = 0;
    CHECK(ccb_read_file(B("termux-prefix-rendiix/etc/apt/sources.list.d/rendiix.list"),
      &list_text, &list_len) == CCB_OK);
    CHECK(strcmp(list_text, "deb https://rendiix.github.io android-tools termux\n") == 0);
    free(list_text);
    ccb_str_free(&prefix);
    ccb_str_free(&path_rendiix);
  }

  // UPG. --upgrade reinstalls a stale SDK-home kotlinc to the pinned
  // version; user-managed kotlins are never touched.
  {
    ccb_str url, sha;
    ccb_str_init(&url);
    ccb_str_init(&sha);
    ccb_str_append(&url, "CCB_KOTLIN_URL=file://");
    ccb_str_append(&url, B("kotlin-fixture.zip"));
    ccb_str_append(&sha, "CCB_KOTLIN_SHA256=");
    ccb_str_append(&sha, kot_sha);
    char *args[] = { bin, "--config", B("proj-upg/ccb.toml"), "--upgrade", "setup", NULL };
    char *env[] = { path_shbin.buf, jh.buf, url.buf, sha.buf,
      "KOTLIN_HOME", "ANDROID_HOME", "PREFIX", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "SDK ready") != NULL);
    CHECK(strstr(r.err, "upgrading SDK-home kotlinc 1.9.24 -> 2.4.21") != NULL);
    ccb_proc_result_free(&r);
    char *kargv[] = { B("sdk-upg/kotlinc/bin/kotlinc"), "-version", NULL };
    CHECK(ccb_proc_run(kargv, NULL, env, &r) == CCB_OK);
    CHECK(strstr(r.err, "2.4.21") != NULL);
    ccb_proc_result_free(&r);
    ccb_str_free(&url);
    ccb_str_free(&sha);
  }

  // PRINT-ENV. --print-env puts the exports on stdout and the ready
  // line on stderr.
  {
    char *args[] = { bin, "--config", B("proj-upg/ccb.toml"), "--print-env", "setup", NULL };
    char *env[] = { path_shbin.buf, jh.buf, "KOTLIN_HOME", "ANDROID_HOME", "PREFIX", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, NULL, env, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    char needle[512];
    snprintf(needle, sizeof needle, "export ANDROID_HOME=\"%s\"", B("sdk-upg"));
    CHECK(strstr(r.out, needle) != NULL);
    snprintf(needle, sizeof needle, "export PATH=\"%s/kotlinc/bin:$PATH\"", B("sdk-upg"));
    CHECK(strstr(r.out, needle) != NULL);
    CHECK(strstr(r.out, "SDK ready") == NULL);
    CHECK(strstr(r.err, "SDK ready") != NULL);
    ccb_proc_result_free(&r);
  }

  ccb_str_free(&jh);
  ccb_str_free(&jh_empty);
  ccb_str_free(&path_shbin);
  ccb_str_free(&khome);
  free(bin);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
