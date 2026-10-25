// ccb_cli implementation, part 1: arg parsing, diag output, init, setup.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_json.h"
#include "ccb_net.h"
#include "ccb_maven.h"
#include "ccb_diag.h"
#include "ccb_log.h"
#include "ccb_proc.h"
#include "ccb_config.h"
#include "ccb_sdk.h"
#include "ccb_pipeline.h"
#include "ccb_preview.h"
#include "ccb_preview_lsp.h"
#include "ccb_preview_tree.h"
#include "ccb_preview_walk.h"
#include "ccb_preview_watch.h"
#include "ccb_preview_gate.h"
#include "ccb_preview_diff.h"
#include "ccb_cli.h"
#include <unistd.h>
#include <sys/stat.h>

// SHA-256 of the official cmdline-tools Linux zip used by `ccb setup`
// to bootstrap an SDK. Verified against the complete download
// (commandlinetools-linux-11076708_latest.zip, 153607504 bytes).
#define CCB_CMDLINE_SHA256 "2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258"
#define CCB_KOTLIN_VERSION "2.4.21"
#define CCB_KOTLIN_URL "https://github.com/JetBrains/kotlin/releases/download/v2.4.21/kotlin-compiler-2.4.21.zip"
#define CCB_KOTLIN_SHA256 "7cc140e76daf416a0424a5557f99afd7ca89ebd463ea1101261e3e01c33e75cd"
#define CCB_R8_VERSION "9.5.28"
#define CCB_R8_URL "https://dl.google.com/dl/android/maven2/com/android/tools/r8/9.5.28/r8-9.5.28.jar"
#define CCB_R8_SHA256 "38a9ffaaebbd17d1514e3ecbff5a85b989bb13c551b9e224c7409514eea84ff5"

static const char *CCB_USAGE =
  "usage: ccb [--json] [--verbose] [--config PATH] [--variant debug|release]\n"
  "           [--dry-run] [--explain] <command> [args]\n"
  "\n"
  "commands:\n"
  "  init [dir] [--package PKG] [--name NAME]  scaffold a new project\n"
  "  setup [--yes] [--upgrade] [--print-env]  install missing SDK components\n"
  "  check [--preview-gate] [--no-preview-gate]\n"
  "                                            validate config + toolchain\n"
  "  build [--preview-gate] [--no-preview-gate] [--aab]\n"
  "                                            build the APK (or .aab with --aab)\n"
  "  clean                                     remove the build directory\n"
  "  doctor                                    report toolchain status\n"
  "  install                                   adb install the built APK\n"
  "  run                                       install + launch the app\n"
  "  logcat                                    stream device logs\n"
  "  preview [--ascii] [--width N] [--device phone|tablet] [--strict]\n"
  "          [--screen NAME] [--output FILE] [--update-goldens] [--walk]\n"
  "          [--watch] [--diff DIR] [--storyboard A,B] [--tree-live]\n"
  "          [screen]                              render a UI preview\n"
  "  lsp [--scenario NAME] [--trace FILE]     preview logic-tree server\n"
  "                                            (JSON-RPC over stdio)\n"
  "  version                                   print the version\n";

typedef struct {
  int json;
  int verbose;
  int dry_run;
  int explain;
  int yes;
  int upgrade;
  int print_env;
  int aab;
  const char *config_path;
  const char *variant;
  const char *command;
  const char *positional;
  const char *init_package;
  const char *init_name;
  int preview_ascii;
  int preview_strict;
  int preview_walk;
  int preview_watch;
  // Chunk 4: --tree-live (live logic-tree client, spawns ccb lsp).
  int preview_tree_live;
  long preview_width;
  int preview_width_set;
  const char *preview_device;
  const char *preview_screen;
  const char *preview_output;
  int preview_update_goldens;
  const char *preview_diff;
  const char *preview_storyboard;
  const char *preview_scenario;
  // Chunk 2: --trace FILE (render with the trace overlay) and
  // --record FILE (walk only: write the walk's trace).
  const char *preview_trace;
  const char *preview_record;
  int preview_gate_on;
  int preview_gate_off;
} ccb_args;

static void ccb_print_diags_human(ccb_diag_list *diags) {
  for (size_t i = 0; i < diags->count; i++) {
    ccb_diag *d = &diags->items[i];
    fprintf(stderr, "error %s [%s]: %s\n", d->code, d->phase, d->message);
    if (d->file != NULL) {
      fprintf(stderr, "  at %s", d->file);
      if (d->line > 0) {
        fprintf(stderr, ":%ld", d->line);
      }
      fprintf(stderr, "\n");
    }
    if (d->command != NULL) {
      fprintf(stderr, "  command: %s\n", d->command);
    }
    if (d->suggestion != NULL) {
      fprintf(stderr, "  suggestion: %s\n", d->suggestion);
    }
  }
}

static void ccb_print_diags_json(ccb_diag_list *diags) {
  for (size_t i = 0; i < diags->count; i++) {
    ccb_diag *d = &diags->items[i];
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "{\"event\":\"diagnostic\",\"code\":");
    ccb_json_escape(&s, d->code);
    ccb_str_append(&s, ",\"phase\":");
    ccb_json_escape(&s, d->phase);
    ccb_str_append(&s, ",\"message\":");
    ccb_json_escape(&s, d->message);
    if (d->file != NULL) {
      ccb_str_append(&s, ",\"file\":");
      ccb_json_escape(&s, d->file);
      ccb_str_appendf(&s, ",\"line\":%ld", d->line);
    }
    if (d->command != NULL) {
      ccb_str_append(&s, ",\"command\":");
      ccb_json_escape(&s, d->command);
    }
    if (d->suggestion != NULL) {
      ccb_str_append(&s, ",\"suggestion\":");
      ccb_json_escape(&s, d->suggestion);
    }
    ccb_str_append(&s, "}\n");
    fputs(s.buf, stdout);
    ccb_str_free(&s);
  }
}

static void ccb_print_diags(ccb_args *a, ccb_diag_list *diags) {
  if (a->json) {
    ccb_print_diags_json(diags);
  } else {
    ccb_print_diags_human(diags);
  }
}

// Load config for commands that need it. Prints diags, returns NULL on
// failure (caller maps to exit code 2).
static ccb_config *ccb_load_config_or_die(ccb_args *a, ccb_diag_list *diags) {
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    ccb_diag d = ccb_diag_make("CCB-E1009", "config", "cannot determine working directory");
    ccb_diag_list_push(diags, &d);
    return NULL;
  }
  char *path = NULL;
  if (ccb_config_discover(cwd, a->config_path, &path) != CCB_OK) {
    ccb_diag d = ccb_diag_make("CCB-E1009", "config",
      a->config_path != NULL ? "config file not found" : "no ccb.toml found in this directory or any parent");
    ccb_diag_list_push(diags, &d);
    return NULL;
  }
  ccb_config *cfg = NULL;
  if (ccb_config_load(path, a->variant, &cfg, diags) != CCB_OK) {
    free(path);
    return NULL;
  }
  free(path);
  return cfg;
}

// ---------- init ----------

typedef struct {
  const char *rel;
  const char *content;
} ccb_scaffold_file;

static int ccb_write_scaffold_file(const char *dir, const char *rel, const char *content, int *created) {
  char *path = ccb_path_join(dir, rel);
  if (ccb_path_exists(path)) {
    free(path);
    return 0;
  }
  char *parent = ccb_dirname(path);
  if (ccb_mkdir_p(parent) != CCB_OK) {
    free(parent);
    free(path);
    return -1;
  }
  free(parent);
  if (ccb_write_file(path, content, strlen(content)) != CCB_OK) {
    free(path);
    return -1;
  }
  free(path);
  *created = 1;
  return 0;
}

static int ccb_cmd_init(ccb_args *a) {
  const char *dir = a->positional == NULL ? "." : a->positional;
  const char *package = a->init_package == NULL ? "com.example.hello" : a->init_package;
  const char *name = a->init_name == NULL ? "hello" : a->init_name;
  if (strcmp(dir, ".") != 0 && ccb_mkdir_p(dir) != CCB_OK) {
    fprintf(stderr, "error: cannot create directory %s\n", dir);
    return 2;
  }
  char *toml_path = ccb_path_join(dir, "ccb.toml");
  if (ccb_path_exists(toml_path)) {
    fprintf(stderr, "error: %s is already a CCB project (ccb.toml exists)\n", dir);
    free(toml_path);
    return 2;
  }
  free(toml_path);
  // Package path for the Kotlin source tree.
  ccb_str pkg_path;
  ccb_str_init(&pkg_path);
  for (const char *c = package; *c != '\0'; c++) {
    ccb_str_append_c(&pkg_path, *c == '.' ? '/' : *c);
  }
  ccb_str toml;
  ccb_str_init(&toml);
  ccb_str_append(&toml, "ccb_version = 1\n\n[project]\nname = \"");
  ccb_str_append(&toml, name);
  ccb_str_append(&toml, "\"\npackage = \"");
  ccb_str_append(&toml, package);
  ccb_str_append(&toml, "\"\nversion = \"1.0\"\nversion_code = 1\nmin_sdk = 26\n"
    "target_sdk = 35\ncompile_sdk = 35\n\n[paths]\nmanifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\nkotlin = \"src/main/kotlin\"\n");
  ccb_str manifest;
  ccb_str_init(&manifest);
  ccb_str_append(&manifest, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  package=\"");
  ccb_str_append(&manifest, package);
  ccb_str_append(&manifest, "\">\n"
    "  <application android:label=\"@string/app_name\" android:theme=\"@style/AppTheme\">\n"
    "    <activity android:name=\".MainActivity\" android:exported=\"true\">\n"
    "      <intent-filter>\n"
    "        <action android:name=\"android.intent.action.MAIN\" />\n"
    "        <category android:name=\"android.intent.category.LAUNCHER\" />\n"
    "      </intent-filter>\n"
    "    </activity>\n"
    "  </application>\n"
    "</manifest>\n");
  ccb_str activity;
  ccb_str_init(&activity);
  ccb_str_append(&activity, "package ");
  ccb_str_append(&activity, package);
  ccb_str_append(&activity, "\n\nimport android.app.Activity\nimport android.os.Bundle\n\n"
    "class MainActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    setContentView(R.layout.activity_main)\n"
    "  }\n"
    "}\n");
  ccb_str kt_path;
  ccb_str_init(&kt_path);
  ccb_str_append(&kt_path, "src/main/kotlin/");
  ccb_str_append(&kt_path, pkg_path.buf);
  ccb_str_append(&kt_path, "/MainActivity.kt");
  static const char *layout =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  android:layout_width=\"match_parent\"\n"
    "  android:layout_height=\"match_parent\"\n"
    "  android:gravity=\"center\"\n"
    "  android:orientation=\"vertical\">\n"
    "  <TextView android:layout_width=\"wrap_content\"\n"
    "    android:layout_height=\"wrap_content\"\n"
    "    android:text=\"@string/hello\" />\n"
    "</LinearLayout>\n";
  ccb_str strings;
  ccb_str_init(&strings);
  ccb_str_append(&strings, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n"
    "  <string name=\"app_name\">");
  ccb_str_append(&strings, name);
  ccb_str_append(&strings, "</string>\n"
    "  <string name=\"hello\">Hello from CCB!</string>\n</resources>\n");
  static const char *themes =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n"
    "  <style name=\"AppTheme\" parent=\"@android:style/Theme.Material.Light.NoActionBar\" />\n"
    "</resources>\n";
  static const char *gitignore = "/build/\n";
  ccb_str readme;
  ccb_str_init(&readme);
  ccb_str_append(&readme, "# ");
  ccb_str_append(&readme, name);
  ccb_str_append(&readme, "\n\nBuilt with CCB. Build the debug APK with:\n\n```\nccb build\n```\n");
  const char *rels[] = {
    "ccb.toml",
    "src/main/AndroidManifest.xml",
    NULL, // MainActivity.kt path (dynamic)
    "src/main/res/layout/activity_main.xml",
    "src/main/res/values/strings.xml",
    "src/main/res/values/themes.xml",
    ".gitignore",
    "README.md"
  };
  const char *contents[] = {
    toml.buf, manifest.buf, activity.buf, layout, strings.buf, themes, gitignore, readme.buf
  };
  int created_count = 0;
  int skipped_count = 0;
  for (int i = 0; i < 8; i++) {
    const char *rel = rels[i] == NULL ? kt_path.buf : rels[i];
    int created = 0;
    if (ccb_write_scaffold_file(dir, rel, contents[i], &created) != 0) {
      fprintf(stderr, "error: cannot write %s/%s\n", dir, rel);
      ccb_str_free(&pkg_path);
      ccb_str_free(&toml);
      ccb_str_free(&manifest);
      ccb_str_free(&activity);
      ccb_str_free(&kt_path);
      ccb_str_free(&strings);
      ccb_str_free(&readme);
      return 2;
    }
    if (created) {
      created_count++;
      if (!a->json) {
        printf("created %s/%s\n", dir, rel);
      }
    } else {
      skipped_count++;
    }
  }
  if (a->json) {
    printf("{\"event\":\"init\",\"dir\":");
    ccb_str s;
    ccb_str_init(&s);
    ccb_json_escape(&s, dir);
    printf("%s,\"created\":%d,\"skipped\":%d}\n", s.buf, created_count, skipped_count);
    ccb_str_free(&s);
  } else if (skipped_count > 0) {
    printf("%d files already existed and were left untouched\n", skipped_count);
  }
  if (!a->json) {
    printf("run `ccb build` in %s to build the app\n", dir);
  }
  ccb_str_free(&pkg_path);
  ccb_str_free(&toml);
  ccb_str_free(&manifest);
  ccb_str_free(&activity);
  ccb_str_free(&kt_path);
  ccb_str_free(&strings);
  ccb_str_free(&readme);
  return 0;
}

// ---------- setup ----------

static char *ccb_find_sdkmanager(const char *sdk_home) {
  char *p = ccb_path_join3(sdk_home, "cmdline-tools/latest/bin", "sdkmanager");
  if (access(p, X_OK) == 0) {
    return p;
  }
  free(p);
  p = ccb_path_join3(sdk_home, "cmdline-tools/bin", "sdkmanager");
  if (access(p, X_OK) == 0) {
    return p;
  }
  free(p);
  return NULL;
}

// Download a file with curl (follows redirects) to a temp path.
// Shared implementation lives in ccb_net; this wrapper keeps the
// existing setup call sites unchanged.
static int ccb_download(const char *url, const char *dest) {
  return ccb_net_download(url, dest);
}

// Like ccb_download, but resumes a partial file when the server
// supports byte ranges.
static int ccb_download_resume(const char *url, const char *dest) {
  char *curl = ccb_sdk_find_on_path("curl");
  if (curl == NULL) {
    return -1;
  }
  char *argv[] = { curl, "-sL", "--fail", "-C", "-", "-o", (char *)dest, (char *)url, NULL };
  ccb_proc_result r;
  ccb_status rc = ccb_proc_run(argv, NULL, NULL, &r);
  int ok = rc == CCB_OK && r.exit_code == 0;
  ccb_proc_result_free(&r);
  free(curl);
  return ok ? 0 : -1;
}

static int ccb_copy_file(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (in == NULL) {
    return -1;
  }
  FILE *out = fopen(dst, "wb");
  if (out == NULL) {
    fclose(in);
    return -1;
  }
  char buf[65536];
  size_t n;
  int ok = 1;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      ok = 0;
      break;
    }
  }
  if (ferror(in)) {
    ok = 0;
  }
  if (fclose(out) != 0) {
    ok = 0;
  }
  fclose(in);
  if (!ok) {
    remove(dst);
  }
  return ok ? 0 : -1;
}

// Shared download cache: $XDG_CACHE_HOME/ccb/downloads, else
// ~/.cache/ccb/downloads. NULL when neither base is available.
static char *ccb_cache_dir(void) {
  char *root = ccb_net_cache_root();
  if (root == NULL) {
    return NULL;
  }
  char *dir = ccb_path_join(root, "downloads");
  free(root);
  return dir;
}

// Download with a shared cache. Pinned downloads (expected_sha
// given) are cached by URL basename and reused when the cached
// copy's hash matches, so the ~250MB of setup downloads is fetched
// once per version even across SDK homes. A partial download is
// resumed on the next run; a server that rejects ranges falls back
// to a fresh download. Returns 0 on success, -1 on download
// failure, -2 on checksum failure. Unpinned downloads and file://
// URLs are never cached (a custom URL without a hash cannot be
// told apart from a different file with the same basename).
static int ccb_download_cached(const char *url, const char *dest, const char *expected_sha) {
  if (expected_sha == NULL || strncmp(url, "file://", 7) == 0) {
    return ccb_download(url, dest);
  }
  char *dir = ccb_cache_dir();
  if (dir == NULL || ccb_mkdir_p(dir) != CCB_OK) {
    // No cache available: download directly, but still verify.
    free(dir);
    if (ccb_download(url, dest) != 0) {
      return -1;
    }
    char hex0[65];
    if (ccb_sha256_file(dest, hex0) != CCB_OK || strcmp(hex0, expected_sha) != 0) {
      remove(dest);
      return -2;
    }
    return 0;
  }
  const char *base = strrchr(url, '/');
  base = base != NULL ? base + 1 : url;
  if (base[0] == '\0') {
    free(dir);
    if (ccb_download(url, dest) != 0) {
      return -1;
    }
    char hex0[65];
    if (ccb_sha256_file(dest, hex0) != CCB_OK || strcmp(hex0, expected_sha) != 0) {
      remove(dest);
      return -2;
    }
    return 0;
  }
  char *cached = ccb_path_join(dir, base);
  free(dir);
  char hex[65];
  if (ccb_path_exists(cached) && ccb_sha256_file(cached, hex) == CCB_OK &&
      strcmp(hex, expected_sha) == 0) {
    int rc = ccb_copy_file(cached, dest);
    free(cached);
    return rc;
  }
  ccb_str t;
  ccb_str_init(&t);
  ccb_str_append(&t, cached);
  ccb_str_append(&t, ".tmp");
  char *tmp = ccb_str_steal(&t);
  if (ccb_download_resume(url, tmp) != 0) {
    remove(tmp);
    if (ccb_download(url, tmp) != 0) {
      remove(tmp);
      free(tmp);
      free(cached);
      return -1;
    }
  }
  if (ccb_sha256_file(tmp, hex) != CCB_OK || strcmp(hex, expected_sha) != 0) {
    remove(tmp);
    free(tmp);
    free(cached);
    return -2;
  }
  if (rename(tmp, cached) != 0) {
    // The cache store failed (permissions?): the download itself
    // is verified, so use it directly.
    int rc = ccb_copy_file(tmp, dest);
    remove(tmp);
    free(tmp);
    free(cached);
    return rc;
  }
  free(tmp);
  int rc = ccb_copy_file(cached, dest);
  free(cached);
  return rc;
}

// ---------- setup: JDK install ----------

typedef enum {
  CCB_JDKINST_NONE,
  CCB_JDKINST_TERMUX,
  CCB_JDKINST_APT,
  CCB_JDKINST_DNF,
  CCB_JDKINST_YUM,
  CCB_JDKINST_PACMAN,
  CCB_JDKINST_ZYPPER,
  CCB_JDKINST_APK
} ccb_jdkinst;

typedef struct {
  ccb_jdkinst kind;
  const char *display;  // "pkg", "apt-get", ... (string literal, not owned)
  char *path;           // resolved installer binary (owned)
  char *sudo_path;      // resolved sudo (owned, NULL unless needs_sudo)
  int needs_sudo;       // distro installer, not running as root
} ccb_jdkinst_info;

static void ccb_detect_jdkinst(ccb_jdkinst_info *out) {
  out->kind = CCB_JDKINST_NONE;
  out->display = NULL;
  out->path = NULL;
  out->sudo_path = NULL;
  out->needs_sudo = 0;
  // Termux: $PREFIX with a pkg binary.
  const char *prefix = getenv("PREFIX");
  if (prefix != NULL) {
    char *pkg = ccb_path_join(prefix, "bin/pkg");
    if (access(pkg, X_OK) == 0) {
      out->kind = CCB_JDKINST_TERMUX;
      out->display = "pkg";
      out->path = pkg;
      return;
    }
    free(pkg);
  }
  static const struct {
    ccb_jdkinst kind;
    const char *name;
  } cands[] = {
    { CCB_JDKINST_APT, "apt-get" },
    { CCB_JDKINST_DNF, "dnf" },
    { CCB_JDKINST_YUM, "yum" },
    { CCB_JDKINST_PACMAN, "pacman" },
    { CCB_JDKINST_ZYPPER, "zypper" },
    { CCB_JDKINST_APK, "apk" },
  };
  for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
    char *p = ccb_sdk_find_on_path(cands[i].name);
    if (p == NULL) {
      continue;
    }
    if (getuid() != 0) {
      char *sudo = ccb_sdk_find_on_path("sudo");
      if (sudo == NULL) {
        // No way to gain root: no distro installer is usable.
        free(p);
        return;
      }
      out->sudo_path = sudo;
      out->needs_sudo = 1;
    }
    out->kind = cands[i].kind;
    out->display = cands[i].name;
    out->path = p;
    return;
  }
}

// Run one installer step: [sudo] <installer> <cmd...>. Prints the
// step's stderr on failure. Returns 0 on success, -1 otherwise.
static int ccb_run_jdkinst_cmd(const ccb_jdkinst_info *inst, char *const cmd[]) {
  size_t n = 0;
  while (cmd[n] != NULL) {
    n++;
  }
  char **argv = ccb_xcalloc(n + 3, sizeof(char *));
  size_t k = 0;
  if (inst->needs_sudo) {
    argv[k++] = inst->sudo_path;
  }
  argv[k++] = inst->path;
  for (size_t i = 0; i < n; i++) {
    argv[k++] = cmd[i];
  }
  argv[k] = NULL;
  ccb_proc_result r;
  ccb_status rc = ccb_proc_run(argv, NULL, NULL, &r);
  int ok = rc == CCB_OK && r.exit_code == 0;
  if (!ok) {
    fputs(r.err != NULL ? r.err : "", stderr);
  }
  ccb_proc_result_free(&r);
  free(argv);
  return ok ? 0 : -1;
}

static int ccb_install_jdk(const ccb_jdkinst_info *inst) {
  switch (inst->kind) {
    case CCB_JDKINST_TERMUX: {
      char *cmd[] = { "install", "-y", "openjdk-17", NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    case CCB_JDKINST_APT: {
      char *upd[] = { "update", NULL };
      if (ccb_run_jdkinst_cmd(inst, upd) != 0) {
        return -1;
      }
      char *cmd[] = { "install", "-y", "openjdk-17-jdk", NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    case CCB_JDKINST_DNF:
    case CCB_JDKINST_YUM: {
      char *cmd[] = { "install", "-y", "java-17-openjdk-devel", NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    case CCB_JDKINST_PACMAN: {
      char *cmd[] = { "-Sy", "--noconfirm", "jdk17-openjdk", NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    case CCB_JDKINST_ZYPPER: {
      char *cmd[] = { "install", "-y", "java-17-openjdk-devel", NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    case CCB_JDKINST_APK: {
      char *cmd[] = { "add", "openjdk17", NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    default:
      return -1;
  }
}

// Returns 1 when the user answered yes, 0 otherwise (no, EOF, no TTY).
static int ccb_prompt_yes(const char *question) {
  if (!isatty(STDIN_FILENO)) {
    return 0;
  }
  fprintf(stderr, "%s [y/N] ", question);
  fflush(stderr);
  char line[128];
  if (fgets(line, sizeof(line), stdin) == NULL) {
    return 0;
  }
  return line[0] == 'y' || line[0] == 'Y';
}

// Derive a JAVA_HOME for a resolved java path: Termux's JDK dir
// first, else the java path's parent of bin/. NULL when neither
// applies. Owned.
static char *ccb_derive_java_home(const char *java_path) {
  const char *prefix = getenv("PREFIX");
  if (prefix != NULL) {
    char *cand = ccb_path_join3(prefix, "lib/jvm/java-17-openjdk", "bin/java");
    int ok = access(cand, X_OK) == 0;
    free(cand);
    if (ok) {
      return ccb_path_join(prefix, "lib/jvm/java-17-openjdk");
    }
  }
  const char *suffix = "/bin/java";
  size_t plen = strlen(java_path);
  size_t slen = strlen(suffix);
  if (plen > slen && strcmp(java_path + plen - slen, suffix) == 0) {
    return ccb_xstrndup(java_path, plen - slen);
  }
  return NULL;
}

// Point JAVA_HOME at the resolved JDK when it is unset and a JDK home
// can be derived: Termux's JDK dir first, else the java path's parent
// of bin/. Only affects this process and its children (sdkmanager).
static void ccb_configure_java_home(const char *java_path) {
  if (getenv("JAVA_HOME") != NULL) {
    return;
  }
  char *home = ccb_derive_java_home(java_path);
  if (home != NULL) {
    setenv("JAVA_HOME", home, 1);
    free(home);
  }
}

// ---------- setup: kotlinc install ----------

// First executable kotlinc candidate for a home dir, mirroring the
// resolver: bin/kotlinc, then <home>/kotlinc/bin/kotlinc. NULL when
// the home is unset, not a dir, or holds no executable kotlinc.
static char *ccb_setup_kotlinc_candidate(const char *home) {
  if (home == NULL || !ccb_path_is_dir(home)) {
    return NULL;
  }
  char *direct = ccb_path_join3(home, "bin", "kotlinc");
  if (access(direct, X_OK) == 0) {
    return direct;
  }
  free(direct);
  char *nested_home = ccb_path_join(home, "kotlinc");
  char *nested = ccb_path_join(nested_home, "bin/kotlinc");
  free(nested_home);
  if (access(nested, X_OK) == 0) {
    return nested;
  }
  free(nested);
  return NULL;
}

// Probe for a usable kotlinc 2.x in resolver order: [sdk] kotlin_home,
// KOTLIN_HOME, PATH, <sdk-home>/kotlinc (the setup-installed one).
// Returns 1 when usable, 0 when none found, -1 when one was found
// but is not 2.x. On 1 and -1, *path_out holds its path (owned) and
// *version_out its parsed version (owned, NULL when unparseable).
static int ccb_setup_probe_kotlinc(const ccb_config *cfg, const char *sdk_home,
    char **path_out, char **version_out) {
  *path_out = NULL;
  *version_out = NULL;
  char *cand = ccb_setup_kotlinc_candidate(cfg != NULL ? cfg->kotlin_home : NULL);
  if (cand == NULL) {
    cand = ccb_setup_kotlinc_candidate(getenv("KOTLIN_HOME"));
  }
  if (cand == NULL) {
    cand = ccb_sdk_find_on_path("kotlinc");
  }
  if (cand == NULL && sdk_home != NULL) {
    char *home = ccb_path_join(sdk_home, "kotlinc");
    char *bin = ccb_path_join(home, "bin/kotlinc");
    free(home);
    if (access(bin, X_OK) == 0) {
      cand = bin;
    } else {
      free(bin);
    }
  }
  if (cand == NULL) {
    return 0;
  }
  char *argv[] = { cand, "-version", NULL };
  ccb_proc_result r;
  char *ver = NULL;
  if (ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK) {
    ver = ccb_sdk_parse_kotlin_version(r.err);
  }
  ccb_proc_result_free(&r);
  *path_out = cand;
  *version_out = ver;
  return ver != NULL && ver[0] == '2' ? 1 : -1;
}

// Download the official kotlinc compiler into <sdk-home>/kotlinc and
// verify it runs and reports 2.x. CCB_KOTLIN_URL overrides the
// download source (a custom URL skips the pinned checksum unless
// CCB_KOTLIN_SHA256 is also set); CCB_KOTLIN_SHA256 overrides the
// expected hash. Returns 0 on success, -1 on failure (error printed).
static int ccb_setup_install_kotlinc(const char *sdk_home) {
  const char *url = getenv("CCB_KOTLIN_URL");
  int url_custom = url != NULL;
  if (url == NULL) {
    url = CCB_KOTLIN_URL;
  }
  const char *sha = getenv("CCB_KOTLIN_SHA256");
  if (sha == NULL && !url_custom) {
    sha = CCB_KOTLIN_SHA256;
  }
  fprintf(stderr, "downloading the official kotlinc compiler (%s)...\n",
    CCB_KOTLIN_VERSION);
  char *tmp = ccb_path_join(sdk_home, "kotlin.zip");
  int drc = ccb_download_cached(url, tmp, sha);
  if (drc != 0) {
    fprintf(stderr, "error: %s: %s\n",
      drc == -2 ? "downloaded kotlinc archive failed checksum verification" : "download failed", url);
    remove(tmp);
    free(tmp);
    return -1;
  }
  if (sha == NULL) {
    fprintf(stderr, "note: checksum verification skipped for custom kotlinc URL\n");
  }
  char *unzip_dir = ccb_path_join(sdk_home, "kotlin-tmp");
  ccb_remove_recursive(unzip_dir);
  if (ccb_mkdir_p(unzip_dir) != CCB_OK) {
    free(unzip_dir);
    remove(tmp);
    free(tmp);
    return -1;
  }
  char *unzip = ccb_sdk_find_on_path("unzip");
  char *unzip_argv[] = { unzip == NULL ? "unzip" : unzip, "-q", tmp, "-d", unzip_dir, NULL };
  ccb_proc_result r;
  int unzip_ok = unzip != NULL && ccb_proc_run(unzip_argv, NULL, NULL, &r) == CCB_OK && r.exit_code == 0;
  ccb_proc_result_free(&r);
  free(unzip);
  remove(tmp);
  free(tmp);
  if (!unzip_ok) {
    fprintf(stderr, "error: cannot unzip kotlinc archive (is `unzip` on PATH?)\n");
    ccb_remove_recursive(unzip_dir);
    free(unzip_dir);
    return -1;
  }
  char *src = ccb_path_join(unzip_dir, "kotlinc");
  char *dst = ccb_path_join(sdk_home, "kotlinc");
  if (!ccb_path_is_dir(src)) {
    fprintf(stderr, "error: kotlinc archive did not contain a kotlinc/ directory\n");
    free(src);
    free(dst);
    ccb_remove_recursive(unzip_dir);
    free(unzip_dir);
    return -1;
  }
  ccb_remove_recursive(dst);
  if (rename(src, dst) != 0) {
    fprintf(stderr, "error: cannot install kotlinc into %s\n", dst);
    free(src);
    free(dst);
    ccb_remove_recursive(unzip_dir);
    free(unzip_dir);
    return -1;
  }
  free(src);
  ccb_remove_recursive(unzip_dir);
  free(unzip_dir);
  // The official zip preserves exec bits; make the entry point
  // executable regardless so a mode-stripping unzip cannot break it.
  char *bin = ccb_path_join(dst, "bin/kotlinc");
  chmod(bin, 0755);
  char *argv[] = { bin, "-version", NULL };
  ccb_proc_result vr;
  char *ver = NULL;
  int ok = 0;
  if (ccb_proc_run(argv, NULL, NULL, &vr) == CCB_OK) {
    ver = ccb_sdk_parse_kotlin_version(vr.err);
    ok = ver != NULL && ver[0] == '2';
  }
  ccb_proc_result_free(&vr);
  if (!ok) {
    fprintf(stderr, "error: installed kotlinc at %s failed verification\n", dst);
    free(bin);
    free(dst);
    free(ver);
    return -1;
  }
  fprintf(stderr, "kotlinc %s installed at %s\n", ver, dst);
  free(bin);
  free(dst);
  free(ver);
  return 0;
}

// ---------- setup: r8 install ----------

// Install the official r8 jar into <sdk-home>/r8/r8.jar, the
// setup-managed location the build picks up when [sdk] r8_jar is
// unset. Returns 0 on success, -1 otherwise.
static int ccb_setup_install_r8(const char *sdk_home) {
  const char *url = getenv("CCB_R8_URL");
  int url_custom = url != NULL;
  if (url == NULL) {
    url = CCB_R8_URL;
  }
  const char *sha = getenv("CCB_R8_SHA256");
  if (sha == NULL && !url_custom) {
    sha = CCB_R8_SHA256;
  }
  fprintf(stderr, "downloading the official r8 jar (%s)...\n", CCB_R8_VERSION);
  char *dir = ccb_path_join(sdk_home, "r8");
  if (ccb_mkdir_p(dir) != CCB_OK) {
    free(dir);
    return -1;
  }
  char *dst = ccb_path_join(dir, "r8.jar");
  free(dir);
  int drc = ccb_download_cached(url, dst, sha);
  if (drc != 0) {
    fprintf(stderr, "error: %s: %s\n",
      drc == -2 ? "downloaded r8 jar failed checksum verification" : "download failed", url);
    remove(dst);
    free(dst);
    return -1;
  }
  if (sha == NULL) {
    fprintf(stderr, "note: checksum verification skipped for custom r8 URL\n");
  }
  // Verify the jar actually runs as R8 (needs java; setup already
  // ensured a JDK). The jar's manifest Main-Class is broken, so
  // invoke the class explicitly, as the build does.
  char *java = NULL;
  int major = 0;
  if (ccb_sdk_find_java(&java, &major) == CCB_OK) {
    char *argv[] = { java, "-cp", dst, "com.android.tools.r8.R8", "--version", NULL };
    ccb_proc_result r;
    int ok = 0;
    if (ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK && r.exit_code == 0) {
      ok = (strstr(r.out, "R8 ") != NULL) || (strstr(r.err, "R8 ") != NULL);
    }
    ccb_proc_result_free(&r);
    free(java);
    if (!ok) {
      fprintf(stderr, "error: installed r8 jar at %s failed verification\n", dst);
      free(dst);
      return -1;
    }
  }
  fprintf(stderr, "r8 %s installed at %s\n", CCB_R8_VERSION, dst);
  free(dst);
  return 0;
}

// ---------- setup: NDK install ----------

// Install one NDK revision through sdkmanager, then verify the dir.
// Returns 0 on success, -1 otherwise.
static int ccb_setup_install_ndk(const char *sdk_home, const char *sdkmanager, const char *version) {
  ccb_str pkg;
  ccb_str_init(&pkg);
  ccb_str_append(&pkg, "ndk;");
  ccb_str_append(&pkg, version);
  char *argv[] = { (char *)sdkmanager, pkg.buf, NULL };
  ccb_proc_result r;
  ccb_status prc = ccb_proc_run(argv, NULL, NULL, &r);
  int ok = prc == CCB_OK && r.exit_code == 0;
  if (!ok) {
    fprintf(stderr, "error: sdkmanager failed to install %s\n%s\n", pkg.buf, r.err);
  }
  ccb_proc_result_free(&r);
  ccb_str_free(&pkg);
  if (!ok) {
    return -1;
  }
  char *dir = ccb_path_join3(sdk_home, "ndk", version);
  int exists = ccb_path_is_dir(dir);
  free(dir);
  if (!exists) {
    fprintf(stderr, "error: sdkmanager finished but ndk/%s is still missing\n", version);
    return -1;
  }
  fprintf(stderr, "ndk %s installed\n", version);
  return 0;
}

// ---------- setup: host tools (Termux + distro packages) ----------

static int ccb_env_nonempty(const char *name) {
  const char *v = getenv(name);
  return v != NULL && v[0] != '\0';
}

// 1 when the tool at path exists, is executable, and runs.
static int ccb_tool_runs(const char *path, const char *arg) {
  return access(path, X_OK) == 0 && ccb_sdk_tool_runnable(path, arg);
}

// 1 when a runnable copy of the tool is on PATH.
static int ccb_path_tool_runs(const char *name, const char *arg) {
  char *p = ccb_sdk_find_on_path(name);
  if (p == NULL) {
    return 0;
  }
  int ok = ccb_tool_runs(p, arg);
  free(p);
  return ok;
}

// rendiix's repo: the only known source for zipalign on Termux (the
// official repos do not carry it). Used ONLY when the official repos
// have no candidate for the package.
#define CCB_RENDIIX_LIST "etc/apt/sources.list.d/rendiix.list"
#define CCB_RENDIIX_LINE "deb https://rendiix.github.io android-tools termux"
#define CCB_RENDIIX_KEY_URL "https://rendiix.github.io/rendiix.gpg"

static int ccb_termux_repo_present(void) {
  const char *prefix = getenv("PREFIX");
  if (prefix == NULL) {
    return 0;
  }
  char *list = ccb_path_join(prefix, CCB_RENDIIX_LIST);
  int ok = ccb_path_exists(list);
  free(list);
  return ok;
}

// Add the rendiix repo (list file + key + apt update). Idempotent:
// when the list file already exists, nothing is done. On failure the
// list file is removed again so a retry redoes the whole add.
static int ccb_termux_add_rendiix(const char *pkg) {
  if (ccb_termux_repo_present()) {
    return 0;
  }
  const char *prefix = getenv("PREFIX");
  if (prefix == NULL) {
    return -1;
  }
  char *deps[] = { (char *)pkg, "install", "-y", "gnupg", "wget", NULL };
  ccb_proc_result r;
  int ok = ccb_proc_run(deps, NULL, NULL, &r) == CCB_OK && r.exit_code == 0;
  if (!ok) {
    fputs(r.err != NULL ? r.err : "", stderr);
  }
  ccb_proc_result_free(&r);
  if (!ok) {
    return -1;
  }
  char *list = ccb_path_join(prefix, CCB_RENDIIX_LIST);
  char *list_dir = ccb_path_join(prefix, "etc/apt/sources.list.d");
  if (ccb_mkdir_p(list_dir) != CCB_OK) {
    free(list_dir);
    free(list);
    return -1;
  }
  free(list_dir);
  FILE *f = fopen(list, "w");
  if (f == NULL) {
    free(list);
    return -1;
  }
  fputs(CCB_RENDIIX_LINE "\n", f);
  if (fclose(f) != 0) {
    free(list);
    return -1;
  }
  char *keydir = ccb_path_join(prefix, "etc/apt/trusted.gpg.d");
  if (ccb_mkdir_p(keydir) != CCB_OK) {
    free(keydir);
    remove(list);
    free(list);
    return -1;
  }
  char *wget = ccb_sdk_find_on_path("wget");
  char *wargv[] = { wget == NULL ? "wget" : wget, "-qP", keydir, (char *)CCB_RENDIIX_KEY_URL, NULL };
  ok = wget != NULL && ccb_proc_run(wargv, NULL, NULL, &r) == CCB_OK && r.exit_code == 0;
  if (!ok) {
    fputs(r.err != NULL ? r.err : "", stderr);
  }
  ccb_proc_result_free(&r);
  free(wget);
  free(keydir);
  if (!ok) {
    remove(list);
    free(list);
    return -1;
  }
  char *apt = ccb_sdk_find_on_path("apt");
  char *uargv[] = { apt == NULL ? "apt" : apt, "update", NULL };
  ok = apt != NULL && ccb_proc_run(uargv, NULL, NULL, &r) == CCB_OK && r.exit_code == 0;
  if (!ok) {
    fputs(r.err != NULL ? r.err : "", stderr);
  }
  ccb_proc_result_free(&r);
  free(apt);
  if (!ok) {
    remove(list);
    free(list);
    return -1;
  }
  free(list);
  return 0;
}

// 1 when the currently enabled repos offer the package, 0 when they
// do not, -1 when apt-cache is unavailable. Runs BEFORE any
// third-party repo is added, so a candidate means the official repos
// carry it.
static int ccb_termux_pkg_candidate(const char *pkg) {
  char *apt_cache = ccb_sdk_find_on_path("apt-cache");
  if (apt_cache == NULL) {
    return -1;
  }
  char *argv[] = { apt_cache, "policy", (char *)pkg, NULL };
  ccb_proc_result r;
  int found = 0;
  if (ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK && r.exit_code == 0) {
    const char *p = strstr(r.out, "Candidate:");
    if (p != NULL) {
      p += strlen("Candidate:");
      while (*p == ' ' || *p == '\t') {
        p++;
      }
      found = strncmp(p, "(none)", 6) != 0 && *p != '\0' && *p != '\n';
    }
  }
  ccb_proc_result_free(&r);
  free(apt_cache);
  return found;
}

// Install one system package for a tool: Termux via pkg, Linux via
// the detected distro installer. needs_repo (Termux only) adds the
// rendiix repo first, but only when the official repos have no
// candidate. Returns 0 on success.
static int ccb_setup_install_system_tool(const ccb_jdkinst_info *inst, const char *pkg_name, int needs_repo) {
  if (inst->kind == CCB_JDKINST_TERMUX) {
    if (needs_repo && ccb_termux_pkg_candidate(pkg_name) == 0) {
      if (ccb_termux_add_rendiix(inst->path) != 0) {
        return -1;
      }
    }
    char *cmd[] = { "install", "-y", (char *)pkg_name, NULL };
    return ccb_run_jdkinst_cmd(inst, cmd);
  }
  switch (inst->kind) {
    case CCB_JDKINST_APT: {
      char *upd[] = { "update", NULL };
      if (ccb_run_jdkinst_cmd(inst, upd) != 0) {
        return -1;
      }
      char *cmd[] = { "install", "-y", (char *)pkg_name, NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    case CCB_JDKINST_DNF:
    case CCB_JDKINST_YUM:
    case CCB_JDKINST_ZYPPER: {
      char *cmd[] = { "install", "-y", (char *)pkg_name, NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    case CCB_JDKINST_PACMAN: {
      char *cmd[] = { "-Sy", "--noconfirm", (char *)pkg_name, NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    case CCB_JDKINST_APK: {
      char *cmd[] = { "add", (char *)pkg_name, NULL };
      return ccb_run_jdkinst_cmd(inst, cmd);
    }
    default:
      return -1;
  }
}

// Forward declaration: defined with install/run/logcat below.
static char *ccb_find_adb(ccb_config *cfg, ccb_diag_list *diags);

// After every install step, resolve the toolchain and report what
// still needs attention (e.g. an old kotlinc on PATH shadowing the
// SDK-home install). Informational only: setup already did what it
// can; the remaining items need user action. adb is outside the
// toolchain resolve, so it is checked separately.
static void ccb_setup_final_pass(ccb_config *cfg, long compile_sdk, const char *build_tools) {
  ccb_config *pass_cfg = cfg;
  ccb_config defcfg;
  if (cfg == NULL) {
    memset(&defcfg, 0, sizeof(defcfg));
    defcfg.compile_sdk = compile_sdk;
    defcfg.build_tools = ccb_xstrdup(build_tools);
    pass_cfg = &defcfg;
  }
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_sdk *sdk = NULL;
  if (ccb_sdk_resolve(pass_cfg, &sdk, &diags) == CCB_OK) {
    fprintf(stderr, "toolchain check: ok\n");
    ccb_sdk_free(sdk);
  } else {
    for (size_t i = 0; i < diags.count; i++) {
      ccb_diag *d = &diags.items[i];
      fprintf(stderr, "warning: %s: %s\n", d->code, d->message);
      if (d->suggestion != NULL) {
        fprintf(stderr, "  suggestion: %s\n", d->suggestion);
      }
    }
    fprintf(stderr, "warning: setup finished with the above gaps; fix them, then rerun `ccb setup` or `ccb check`.\n");
  }
  ccb_diag_list_free(&diags);
  ccb_diag_list adiags;
  ccb_diag_list_init(&adiags);
  char *adb = ccb_find_adb(pass_cfg, &adiags);
  if (adb == NULL) {
    if (adiags.count > 0) {
      for (size_t i = 0; i < adiags.count; i++) {
        fprintf(stderr, "warning: %s: %s\n", adiags.items[i].code, adiags.items[i].message);
      }
    } else {
      fprintf(stderr, "warning: adb not found: install platform-tools or put adb on PATH to use `ccb install` / `ccb run`.\n");
    }
  }
  free(adb);
  ccb_diag_list_free(&adiags);
  if (cfg == NULL) {
    free(defcfg.build_tools);
  }
}

static int ccb_cmd_setup(ccb_args *a) {
  // sdkmanager is a Java program. When no usable JDK is present, offer
  // to install one (Termux pkg or the distro package manager) instead
  // of failing at the sdkmanager step with a generic error.
  {
    char *java = NULL;
    int java_major = 0;
    if (ccb_sdk_find_java(&java, &java_major) != CCB_OK) {
      ccb_jdkinst_info inst;
      ccb_detect_jdkinst(&inst);
      if (inst.kind == CCB_JDKINST_NONE) {
        fprintf(stderr,
          "error: java not found: `ccb setup` runs sdkmanager, which needs a JDK 17+.\n"
          "No supported package installer found (Termux pkg, apt-get, dnf, yum, pacman, zypper, apk).\n"
          "Install a JDK 17+ manually and set JAVA_HOME, or put java 17+ on PATH.\n");
        return 3;
      }
      int go = 0;
      if (a->yes || a->upgrade) {
        go = 1;
      } else if (isatty(STDIN_FILENO)) {
        ccb_str q;
        ccb_str_init(&q);
        ccb_str_append(&q, "Java 17+ not found. Install OpenJDK 17 via ");
        ccb_str_append(&q, inst.display);
        ccb_str_append(&q, "?");
        go = ccb_prompt_yes(q.buf);
        ccb_str_free(&q);
        if (!go) {
          fprintf(stderr, "setup aborted: Java 17+ is required.\n");
        }
      } else {
        fprintf(stderr,
          "error: java not found: `ccb setup` runs sdkmanager, which needs a JDK 17+.\n"
          "Run `ccb setup` in a terminal to install OpenJDK 17 via %s, or pass --yes to install automatically.\n",
          inst.display);
      }
      if (!go) {
        free(inst.path);
        free(inst.sudo_path);
        return 3;
      }
      fprintf(stderr, "Installing OpenJDK 17 via %s...\n", inst.display);
      int irc = ccb_install_jdk(&inst);
      free(inst.path);
      free(inst.sudo_path);
      if (irc != 0) {
        fprintf(stderr, "error: failed to install OpenJDK 17 via %s.\n", inst.display);
        return 3;
      }
      if (ccb_sdk_find_java(&java, &java_major) != CCB_OK) {
        fprintf(stderr,
          "error: installed OpenJDK 17 via %s but java 17+ is still not found.\n"
          "Set JAVA_HOME or put java 17+ on PATH, then run `ccb setup` again.\n",
          inst.display);
        return 3;
      }
    }
    ccb_configure_java_home(java);
    free(java);
  }
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  // Config is optional for setup: defaults apply when no ccb.toml
  // exists. But a config that was found (or explicitly requested)
  // and failed to load is an error: silently falling back to the
  // default SDK home would provision the wrong SDK.
  if (cfg == NULL) {
    int found = 0;
    if (a->config_path != NULL) {
      found = 1;
    } else {
      char cwd[4096];
      char *discovered = NULL;
      if (getcwd(cwd, sizeof(cwd)) != NULL &&
          ccb_config_discover(cwd, NULL, &discovered) == CCB_OK) {
        found = 1;
        free(discovered);
      }
    }
    if (found) {
      ccb_print_diags(a, &diags);
      ccb_diag_list_free(&diags);
      return 2;
    }
    // No config anywhere: defaults apply.
  }
  ccb_diag_list_free(&diags);
  long compile_sdk = 35;
  const char *build_tools = "35.0.0";
  const char *sdk_home_cfg = NULL;
  if (cfg != NULL) {
    compile_sdk = cfg->compile_sdk;
    build_tools = cfg->build_tools;
    sdk_home_cfg = cfg->sdk_home;
  }
  const char *sdk_home = sdk_home_cfg;
  if (sdk_home == NULL) {
    sdk_home = getenv("ANDROID_HOME");
  }
  if (sdk_home == NULL) {
    sdk_home = getenv("ANDROID_SDK_ROOT");
  }
  char *default_home = NULL;
  if (sdk_home == NULL) {
    const char *home = getenv("HOME");
    if (home == NULL) {
      fprintf(stderr, "error: cannot determine an SDK location (no HOME, no ANDROID_HOME)\n");
      ccb_config_free(cfg);
      return 3;
    }
    default_home = ccb_path_join(home, "Android/Sdk");
    sdk_home = default_home;
  }
  char *sdkmanager = ccb_find_sdkmanager(sdk_home);
  if (sdkmanager == NULL) {
    // Bootstrap: download the official cmdline-tools archive.
    fprintf(stderr, "cmdline-tools not found, downloading the official archive...\n");
    if (ccb_mkdir_p(sdk_home) != CCB_OK) {
      fprintf(stderr, "error: cannot create SDK directory %s\n", sdk_home);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    char *tmp = ccb_path_join(sdk_home, "cmdline-tools.zip");
    const char *url = "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip";
    int drc = ccb_download_cached(url, tmp, CCB_CMDLINE_SHA256);
    if (drc != 0) {
      fprintf(stderr, "error: %s: %s\n",
        drc == -2 ? "downloaded cmdline-tools archive failed checksum verification" : "download failed",
        url);
      remove(tmp);
      free(tmp);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    char *unzip_dir = ccb_path_join(sdk_home, "cmdline-tools-tmp");
    ccb_remove_recursive(unzip_dir);
    if (ccb_mkdir_p(unzip_dir) != CCB_OK) {
      free(unzip_dir);
      free(tmp);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    char *unzip = ccb_sdk_find_on_path("unzip");
    char *unzip_argv[] = { unzip == NULL ? "unzip" : unzip, "-q", tmp, "-d", unzip_dir, NULL };
    ccb_proc_result r;
    int unzip_ok = unzip != NULL && ccb_proc_run(unzip_argv, NULL, NULL, &r) == CCB_OK && r.exit_code == 0;
    ccb_proc_result_free(&r);
    free(unzip);
    remove(tmp);
    free(tmp);
    if (!unzip_ok) {
      fprintf(stderr, "error: cannot unzip cmdline-tools archive\n");
      free(unzip_dir);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    char *src = ccb_path_join(unzip_dir, "cmdline-tools");
    char *dst = ccb_path_join(sdk_home, "cmdline-tools/latest");
    char *dst_parent = ccb_dirname(dst);
    if (ccb_mkdir_p(dst_parent) != CCB_OK) {
      free(dst_parent);
      free(src);
      free(dst);
      free(unzip_dir);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    free(dst_parent);
    if (rename(src, dst) != 0) {
      fprintf(stderr, "error: cannot install cmdline-tools into %s\n", dst);
      free(src);
      free(dst);
      free(unzip_dir);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    free(src);
    free(dst);
    ccb_remove_recursive(unzip_dir);
    free(unzip_dir);
    sdkmanager = ccb_find_sdkmanager(sdk_home);
    if (sdkmanager == NULL) {
      fprintf(stderr, "error: cmdline-tools installed but sdkmanager not found\n");
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
  }
  // Accept licenses BEFORE installing: on a fresh SDK an install
  // attempted before license acceptance installs nothing, and the
  // old order then reported success anyway (see the verification
  // below). A licenses failure is only a warning here: the
  // post-install verification is the arbiter of success.
  {
    char *lic_cmd = NULL;
    {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "yes | \"");
      ccb_str_append(&s, sdkmanager);
      ccb_str_append(&s, "\" --licenses >/dev/null");
      lic_cmd = ccb_str_steal(&s);
    }
    // execve does no PATH lookup, so resolve sh explicitly.
    char *sh = ccb_sdk_find_on_path("sh");
    if (sh == NULL) {
      fprintf(stderr, "warning: could not accept SDK licenses (sh not found); continuing\n");
    } else {
      char *sh_argv[] = { sh, "-c", lic_cmd, NULL };
      ccb_proc_result lr;
      ccb_status lrc = ccb_proc_run(sh_argv, NULL, NULL, &lr);
      if (lrc != CCB_OK || lr.exit_code != 0) {
        fprintf(stderr, "warning: sdkmanager --licenses failed; continuing\n");
      }
      ccb_proc_result_free(&lr);
      free(sh);
    }
    free(lic_cmd);
  }
  // Install the components this project needs.
  ccb_str platform;
  ccb_str_init(&platform);
  ccb_str_append(&platform, "platforms;android-");
  ccb_str_appendf(&platform, "%ld", compile_sdk);
  ccb_str bt;
  ccb_str_init(&bt);
  ccb_str_append(&bt, "build-tools;");
  ccb_str_append(&bt, build_tools);
  char *components[] = { "platform-tools", platform.buf, bt.buf, NULL };
  char *argv[] = { sdkmanager, components[0], components[1], components[2], NULL };
  ccb_proc_result r;
  ccb_status prc = ccb_proc_run(argv, NULL, NULL, &r);
  if (prc != CCB_OK || r.exit_code != 0) {
    fprintf(stderr, "error: sdkmanager failed to install components\n%s\n", r.err);
    fprintf(stderr,
      "note: sdkmanager needs network access to Google's repository. It has no flags for\n"
      "authenticated proxies, so behind an authenticated egress proxy it cannot fetch;\n"
      "in that case provision the SDK by direct download instead (see docs/setup.md).\n");
    ccb_proc_result_free(&r);
    ccb_str_free(&platform);
    ccb_str_free(&bt);
    free(sdkmanager);
    free(default_home);
    ccb_config_free(cfg);
    return 3;
  }
  ccb_proc_result_free(&r);
  // Verify the components actually landed before claiming success:
  // an install that silently installed nothing must not print
  // "SDK ready".
  {
    const char *missing[3];
    int nmissing = 0;
    char *pt = ccb_path_join(sdk_home, "platform-tools");
    if (!ccb_path_exists(pt)) {
      missing[nmissing++] = "platform-tools";
    }
    free(pt);
    ccb_str ps;
    ccb_str_init(&ps);
    ccb_str_append(&ps, "platforms/android-");
    ccb_str_appendf(&ps, "%ld", compile_sdk);
    char *plat = ccb_path_join(sdk_home, ps.buf);
    ccb_str_free(&ps);
    if (!ccb_path_exists(plat)) {
      missing[nmissing++] = "platform";
    }
    free(plat);
    char *btd = ccb_path_join3(sdk_home, "build-tools", build_tools);
    if (!ccb_path_exists(btd)) {
      missing[nmissing++] = "build-tools";
    }
    free(btd);
    if (nmissing > 0) {
      fprintf(stderr, "error: ccb setup finished but the following components are still missing:");
      for (int i = 0; i < nmissing; i++) {
        fprintf(stderr, " %s", missing[i]);
      }
      fprintf(stderr, "\nCheck the sdkmanager output above, then run `ccb setup` again.\n");
      ccb_str_free(&platform);
      ccb_str_free(&bt);
      free(sdkmanager);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
  }
  // Kotlin: when no usable kotlinc 2.x resolves, install the official
  // compiler into the SDK home, where the resolver picks it up as a
  // last resort (after [sdk] kotlin_home, KOTLIN_HOME, and PATH).
  // Automatic, like the SDK component install: the destination is
  // CCB's own directory, not a system package.
  {
    int upgraded = 0;
    if (a->upgrade) {
      // Upgrade only the setup-managed copy: when the SDK-home
      // kotlinc exists and is not the pinned version, reinstall it.
      // A user-managed kotlinc (config, KOTLIN_HOME, PATH) is never
      // touched, and SDK component versions stay pinned by config.
      char *home_bin = ccb_path_join3(sdk_home, "kotlinc", "bin/kotlinc");
      if (access(home_bin, X_OK) == 0) {
        char *argv[] = { home_bin, "-version", NULL };
        ccb_proc_result vr;
        char *ver = NULL;
        if (ccb_proc_run(argv, NULL, NULL, &vr) == CCB_OK) {
          ver = ccb_sdk_parse_kotlin_version(vr.err);
        }
        ccb_proc_result_free(&vr);
        if (ver == NULL || strcmp(ver, CCB_KOTLIN_VERSION) != 0) {
          fprintf(stderr, "upgrading SDK-home kotlinc %s -> %s\n",
            ver != NULL ? ver : "unversioned", CCB_KOTLIN_VERSION);
          if (ccb_setup_install_kotlinc(sdk_home) != 0) {
            free(ver);
            free(home_bin);
            ccb_str_free(&platform);
            ccb_str_free(&bt);
            free(sdkmanager);
            free(default_home);
            ccb_config_free(cfg);
            return 3;
          }
          upgraded = 1;
        }
        free(ver);
      }
      free(home_bin);
    }
    if (!upgraded) {
    char *kpath = NULL;
    char *kver = NULL;
    int kprobe = ccb_setup_probe_kotlinc(cfg, sdk_home, &kpath, &kver);
    if (kprobe == 1) {
      fprintf(stderr, "kotlinc %s found at %s\n", kver, kpath);
    } else {
      if (kprobe == -1) {
        fprintf(stderr,
          "warning: found kotlinc %s at %s, which is not 2.x; installing %s into the SDK home.\n"
          "Builds will keep picking the old one until it is removed or [sdk] kotlin_home / KOTLIN_HOME points at %s/kotlinc.\n",
          kver != NULL ? kver : "unversioned", kpath, CCB_KOTLIN_VERSION, sdk_home);
      }
      if (ccb_setup_install_kotlinc(sdk_home) != 0) {
        free(kpath);
        free(kver);
        ccb_str_free(&platform);
        ccb_str_free(&bt);
        free(sdkmanager);
        free(default_home);
        ccb_config_free(cfg);
        return 3;
      }
    }
    free(kpath);
    free(kver);
    }
  }
  // NDK: a project declaring [ndk] needs its revision installed, or
  // check fails after setup says ready.
  if (cfg != NULL && cfg->has_ndk && cfg->ndk_version != NULL) {
    char *ndk_dir = ccb_path_join3(sdk_home, "ndk", cfg->ndk_version);
    if (!ccb_path_is_dir(ndk_dir)) {
      fprintf(stderr, "ndk %s not found, installing via sdkmanager...\n", cfg->ndk_version);
      if (ccb_setup_install_ndk(sdk_home, sdkmanager, cfg->ndk_version) != 0) {
        free(ndk_dir);
        ccb_str_free(&platform);
        ccb_str_free(&bt);
        free(sdkmanager);
        free(default_home);
        ccb_config_free(cfg);
        return 3;
      }
    } else {
      fprintf(stderr, "ndk %s found at %s\n", cfg->ndk_version, ndk_dir);
    }
    free(ndk_dir);
  }
  // r8: only for minify projects without an explicit jar. The
  // setup-managed copy lives at <sdk-home>/r8/r8.jar, where the
  // build picks it up.
  if (cfg != NULL && cfg->minify && cfg->r8_jar == NULL) {
    char *r8_path = ccb_path_join3(sdk_home, "r8", "r8.jar");
    int need = !ccb_path_exists(r8_path);
    if (!need && a->upgrade && getenv("CCB_R8_URL") == NULL && getenv("CCB_R8_SHA256") == NULL) {
      char hex[65];
      need = ccb_sha256_file(r8_path, hex) != CCB_OK || strcmp(hex, CCB_R8_SHA256) != 0;
    }
    if (need) {
      if (ccb_setup_install_r8(sdk_home) != 0) {
        free(r8_path);
        ccb_str_free(&platform);
        ccb_str_free(&bt);
        free(sdkmanager);
        free(default_home);
        ccb_config_free(cfg);
        return 3;
      }
    } else {
      fprintf(stderr, "r8 found at %s\n", r8_path);
    }
    free(r8_path);
  }
  // Host tools: on Termux, the SDK's native tools can exist yet not
  // run (x86-64 binaries on aarch64). When no runnable copy resolves
  // (SDK or PATH) and no override pins the choice, install the
  // platform package. Termux: aapt2 and android-tools from the
  // official repos, zipalign from the rendiix repo (only when the
  // official repos have no candidate). Linux: adb from the distro
  // package manager. aapt2/zipalign on Linux are SDK-native on
  // x86-64; on aarch64 Linux there is no package fix, so the final
  // pass warns instead. Best effort: declining, or lacking an
  // installer, only skips the installs; a failed install errors.
  {
    int ov_aapt2 = (cfg != NULL && cfg->aapt2_override != NULL) || ccb_env_nonempty("CCB_AAPT2");
    int ov_zipalign = (cfg != NULL && cfg->zipalign_override != NULL) || ccb_env_nonempty("CCB_ZIPALIGN");
    int ov_adb = (cfg != NULL && cfg->adb_override != NULL) || ccb_env_nonempty("CCB_ADB");
    char *bt_dir = ccb_path_join3(sdk_home, "build-tools", build_tools);
    char *sdk_aapt2 = ccb_path_join(bt_dir, "aapt2");
    char *sdk_zipalign = ccb_path_join(bt_dir, "zipalign");
    char *sdk_adb = ccb_path_join(sdk_home, "platform-tools/adb");
    free(bt_dir);
    int need_aapt2 = !ov_aapt2 && !ccb_tool_runs(sdk_aapt2, "version") && !ccb_path_tool_runs("aapt2", "version");
    int need_zipalign = !ov_zipalign && !ccb_tool_runs(sdk_zipalign, NULL) && !ccb_path_tool_runs("zipalign", NULL);
    int need_adb = !ov_adb && !ccb_tool_runs(sdk_adb, "version") && !ccb_path_tool_runs("adb", "version");
    free(sdk_aapt2);
    free(sdk_zipalign);
    free(sdk_adb);
    ccb_jdkinst_info inst;
    ccb_detect_jdkinst(&inst);
    struct ccb_tool_need { const char *tool; const char *pkg; const char *arg; int needs_repo; int needed; } tools[3] = {
      { "aapt2", "aapt2", "version", 0, need_aapt2 },
      { "zipalign", "zipalign", NULL, 1, need_zipalign },
      { "adb", "android-tools", "version", 0, need_adb },
    };
    if (inst.kind != CCB_JDKINST_TERMUX) {
      // Linux: only adb has a package fix.
      tools[0].needed = 0;
      tools[1].needed = 0;
      if (inst.kind == CCB_JDKINST_APT) {
        tools[2].pkg = "adb";
      }
    }
    int any = tools[0].needed || tools[1].needed || tools[2].needed;
    if (any && inst.kind != CCB_JDKINST_NONE) {
      int go = 0;
      if (a->yes || a->upgrade) {
        go = 1;
      } else if (isatty(STDIN_FILENO)) {
        ccb_str q;
        ccb_str_init(&q);
        ccb_str_append(&q, "Install missing host tools via ");
        ccb_str_append(&q, inst.display);
        ccb_str_append(&q, " (");
        int first = 1;
        for (int i = 0; i < 3; i++) {
          if (!tools[i].needed) {
            continue;
          }
          if (!first) {
            ccb_str_append(&q, ", ");
          }
          ccb_str_append(&q, tools[i].tool);
          first = 0;
        }
        ccb_str_append(&q, ")?");
        if (inst.kind == CCB_JDKINST_TERMUX && tools[1].needed && !ccb_termux_repo_present()) {
          ccb_str_append(&q, " zipalign is not in the official Termux repos, so this adds the rendiix repository (https://rendiix.github.io) for it.");
        }
        go = ccb_prompt_yes(q.buf);
        ccb_str_free(&q);
        if (!go) {
          fprintf(stderr, "skipping host tool installs; the final check below lists what is still missing.\n");
        }
      } else {
        fprintf(stderr, "note: run `ccb setup` in a terminal or pass --yes to install the missing host tools.\n");
      }
      if (go) {
        for (int i = 0; i < 3; i++) {
          if (!tools[i].needed) {
            continue;
          }
          fprintf(stderr, "Installing %s via %s...\n", tools[i].tool, inst.display);
          if (ccb_setup_install_system_tool(&inst, tools[i].pkg, tools[i].needs_repo) != 0) {
            fprintf(stderr, "error: failed to install %s via %s.\n", tools[i].tool, inst.display);
            free(inst.path);
            free(inst.sudo_path);
            ccb_str_free(&platform);
            ccb_str_free(&bt);
            free(sdkmanager);
            free(default_home);
            ccb_config_free(cfg);
            return 3;
          }
          if (!ccb_path_tool_runs(tools[i].tool, tools[i].arg)) {
            fprintf(stderr, "error: installed %s via %s but no runnable %s found on PATH.\n", tools[i].tool, inst.display, tools[i].tool);
            free(inst.path);
            free(inst.sudo_path);
            ccb_str_free(&platform);
            ccb_str_free(&bt);
            free(sdkmanager);
            free(default_home);
            ccb_config_free(cfg);
            return 3;
          }
        }
      }
    }
    free(inst.path);
    free(inst.sudo_path);
  }
  // Maven prefetch: the one network step setup owns for dependencies.
  // Builds never download; a project that declares [deps] maven gets
  // its artifacts into the local cache here, and a failure stops setup
  // instead of leaving a half-prefetched cache behind as "ready".
  if (cfg != NULL && cfg->maven_count > 0) {
    ccb_maven_result mv;
    if (ccb_maven_resolve(cfg, 1, a->verbose, 0, &mv, &diags) != CCB_OK) {
      ccb_print_diags(a, &diags);
      ccb_diag_list_free(&diags);
      ccb_str_free(&platform);
      ccb_str_free(&bt);
      free(sdkmanager);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    fprintf(stderr, "maven: prefetched %zu artifacts (%zu aar, %zu jar)\n",
      mv.aars_count + mv.jars_count, mv.aars_count, mv.jars_count);
    // The lockfile records exactly what was prefetched; builds and
    // checks enforce it from here on. A lockfile that cannot be
    // written fails setup: a half-ready project must not read "ready".
    if (ccb_maven_write_lockfile(cfg, &mv, &diags) != CCB_OK) {
      ccb_print_diags(a, &diags);
      ccb_diag_list_free(&diags);
      ccb_maven_result_free(&mv);
      ccb_str_free(&platform);
      ccb_str_free(&bt);
      free(sdkmanager);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    fprintf(stderr, "maven: lockfile written to ccb.lock\n");
    ccb_maven_result_free(&mv);
  }
  ccb_setup_final_pass(cfg, compile_sdk, build_tools);
  if (a->print_env) {
    printf("export ANDROID_HOME=\"%s\"\n", sdk_home);
    const char *jh = getenv("JAVA_HOME");
    if (jh != NULL) {
      printf("export JAVA_HOME=\"%s\"\n", jh);
    } else {
      char *java = NULL;
      int major = 0;
      if (ccb_sdk_find_java(&java, &major) == CCB_OK) {
        char *derived = ccb_derive_java_home(java);
        if (derived != NULL) {
          printf("export JAVA_HOME=\"%s\"\n", derived);
          free(derived);
        }
        free(java);
      }
    }
    char *kbin = ccb_path_join3(sdk_home, "kotlinc", "bin");
    if (ccb_path_is_dir(kbin)) {
      printf("export PATH=\"%s:$PATH\"\n", kbin);
    }
    free(kbin);
    fprintf(stderr, "SDK ready at %s (platform android-%ld, build-tools %s)\n", sdk_home, compile_sdk, build_tools);
  } else {
    printf("SDK ready at %s (platform android-%ld, build-tools %s)\n", sdk_home, compile_sdk, build_tools);
  }
  ccb_str_free(&platform);
  ccb_str_free(&bt);
  free(sdkmanager);
  free(default_home);
  ccb_config_free(cfg);
  return 0;
}

// ---------- check / build / clean / doctor ----------

// Preview build gate, shared by `check` and `build`. Runs only when
// enabled (explicit flag beats config); on failure the concise lines
// go to stderr and the command exits 1. A gate-run failure (2) is a
// config error and exits 2. Returns -1 when the gate is off (the
// caller proceeds normally); 0 means the gate passed. In --json mode
// the failure still surfaces as the stderr lines plus the nonzero
// exit code, not as a JSON event.
static int ccb_run_preview_gate(ccb_args *a, ccb_config *cfg) {
  ccb_str failed;
  int grc = 0;
  if (!pv_gate_enabled(cfg, a->preview_gate_on, a->preview_gate_off)) {
    return -1;
  }
  ccb_str_init(&failed);
  grc = pv_gate_run(cfg, 0, 0, a->preview_strict, &failed);
  if (grc == 1) {
    fputs(failed.buf != NULL ? failed.buf : "", stderr);
    ccb_str_free(&failed);
    return 1;
  }
  if (grc == 2) {
    fprintf(stderr, "error: preview gate could not run\n");
    ccb_str_free(&failed);
    return 2;
  }
  ccb_str_free(&failed);
  return 0;
}

static int ccb_cmd_check(ccb_args *a) {
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  if (a->preview_gate_on && a->preview_gate_off) {
    fprintf(stderr, "error: --preview-gate cannot be combined with "
      "--no-preview-gate\n");
    return 2;
  }
  ccb_diag_list_init(&diags);
  cfg = ccb_load_config_or_die(a, &diags);
  if (cfg == NULL) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 2;
  }
  ccb_sdk *sdk = NULL;
  if (ccb_sdk_resolve(cfg, &sdk, &diags) != CCB_OK) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    ccb_config_free(cfg);
    return 3;
  }
  // Maven dependencies (cache-only): a missing prefetch is reported
  // here, not as a surprise at build time. The counts print with the
  // rest of the report below, after the ok line.
  size_t mv_aars = 0;
  size_t mv_jars = 0;
  int have_maven = 0;
  if (cfg->maven_count > 0) {
    ccb_maven_result mv;
    if (ccb_maven_resolve(cfg, 0, a->verbose, 0, &mv, &diags) != CCB_OK) {
      ccb_print_diags(a, &diags);
      ccb_sdk_free(sdk);
      ccb_config_free(cfg);
      ccb_diag_list_free(&diags);
      return 3;
    }
    mv_aars = mv.aars_count;
    mv_jars = mv.jars_count;
    have_maven = 1;
    ccb_maven_result_free(&mv);
  }
  // Preview build gate (opt-in): runs after toolchain validation so a
  // failing gate does not mask a broken toolchain, and a broken
  // toolchain does not mask gate failures either way — the gate only
  // runs when the toolchain resolved.
  {
    int gate = ccb_run_preview_gate(a, cfg);
    if (gate == 1 || gate == 2) {
      ccb_sdk_free(sdk);
      ccb_config_free(cfg);
      ccb_diag_list_free(&diags);
      return gate;
    }
  }
  if (a->json) {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "{\"event\":\"check\",\"ok\":true,\"project\":");
    ccb_json_escape(&s, cfg->name);
    ccb_str_append(&s, ",\"package\":");
    ccb_json_escape(&s, cfg->package);
    ccb_str_append(&s, ",\"variant\":");
    ccb_json_escape(&s, cfg->variant);
    ccb_str_append(&s, ",\"sdk\":");
    ccb_json_escape(&s, sdk->sdk_home);
    ccb_str_append(&s, ",\"build_tools\":");
    ccb_json_escape(&s, sdk->build_tools_version);
    ccb_str_append(&s, ",\"aapt2\":");
    ccb_json_escape(&s, sdk->aapt2);
    ccb_str_append(&s, ",\"aapt2_source\":");
    ccb_json_escape(&s, sdk->aapt2_source == NULL ? "default" : sdk->aapt2_source);
    ccb_str_append(&s, ",\"zipalign\":");
    ccb_json_escape(&s, sdk->zipalign);
    ccb_str_append(&s, ",\"zipalign_source\":");
    ccb_json_escape(&s, sdk->zipalign_source == NULL ? "default" : sdk->zipalign_source);
    ccb_str_append(&s, ",\"kotlin\":");
    ccb_json_escape(&s, sdk->kotlin_version);
    ccb_str_appendf(&s, ",\"java\":%d", sdk->java_major);
    if (have_maven && cfg->maven_exclude_count > 0) {
      ccb_str_append(&s, ",\"maven_excluded\":[");
      for (size_t i = 0; i < cfg->maven_exclude_count; i++) {
        if (i > 0) {
          ccb_str_append_c(&s, ',');
        }
        ccb_json_escape(&s, cfg->maven_exclude[i]);
      }
      ccb_str_append_c(&s, ']');
    }
    ccb_str_append_c(&s, '}');
    printf("%s\n", s.buf);
    ccb_str_free(&s);
  } else {
    printf("ok: %s (%s, variant %s)\n", cfg->name, cfg->package, cfg->variant);
    printf("  sdk: %s\n  build-tools: %s\n  aapt2: %s (%s)\n  zipalign: %s (%s)\n  kotlinc: %s\n  java: %d\n",
      sdk->sdk_home, sdk->build_tools_version,
      sdk->aapt2, sdk->aapt2_source == NULL ? "default" : sdk->aapt2_source,
      sdk->zipalign, sdk->zipalign_source == NULL ? "default" : sdk->zipalign_source,
      sdk->kotlin_version, sdk->java_major);
    if (cfg->has_ndk) {
      printf("  ndk: %s\n", sdk->ndk_home);
    }
    if (have_maven) {
      printf("  maven: %zu artifacts (%zu aar, %zu jar)\n",
        mv_aars + mv_jars, mv_aars, mv_jars);
    }
    if (have_maven && cfg->maven_exclude_count > 0) {
      printf("  maven: excluded ");
      for (size_t i = 0; i < cfg->maven_exclude_count; i++) {
        if (i > 0) {
          printf(", ");
        }
        printf("%s", cfg->maven_exclude[i]);
      }
      printf("\n");
    }
  }
  ccb_sdk_free(sdk);
  ccb_config_free(cfg);
  ccb_diag_list_free(&diags);
  return 0;
}

static int ccb_cmd_build(ccb_args *a) {
  ccb_build_opts opts;
  if (a->preview_gate_on && a->preview_gate_off) {
    fprintf(stderr, "error: --preview-gate cannot be combined with "
      "--no-preview-gate\n");
    return 2;
  }
  memset(&opts, 0, sizeof(opts));
  opts.json = a->json;
  opts.verbose = a->verbose;
  opts.dry_run = a->dry_run;
  opts.explain = a->explain;
  opts.aab = a->aab;
  opts.variant_override = a->variant;
  opts.config_path = a->config_path;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  // Preview build gate (opt-in), fail-fast before any pipeline stage:
  // a failing gate never starts a build. Config load here is only for
  // the gate; when it fails, the gate is skipped and the pipeline
  // below reports the config error normally.
  {
    ccb_diag_list gdiags;
    ccb_config *gcfg = NULL;
    ccb_diag_list_init(&gdiags);
    gcfg = ccb_load_config_or_die(a, &gdiags);
    if (gcfg != NULL) {
      int gate = ccb_run_preview_gate(a, gcfg);
      ccb_config_free(gcfg);
      ccb_diag_list_free(&gdiags);
      if (gate == 1 || gate == 2) {
        ccb_diag_list_free(&diags);
        return gate;
      }
    } else {
      ccb_diag_list_free(&gdiags);
    }
  }
  ccb_build_result result;
  ccb_status rc = ccb_pipeline_build(&opts, &result, &diags);
  if (rc != CCB_OK) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return ccb_exit_code(rc);
  }
  if (!a->dry_run && !a->explain) {
    if (a->json) {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "{\"event\":\"build_done\",\"");
      ccb_str_append(&s, result.aab ? "aab" : "apk");
      ccb_str_append(&s, "\":");
      ccb_json_escape(&s, result.apk_path);
      ccb_str_appendf(&s, ",\"steps_run\":%d,\"steps_skipped\":%d,\"up_to_date\":%s}",
        result.steps_run, result.steps_skipped, result.up_to_date ? "true" : "false");
      printf("%s\n", s.buf);
      ccb_str_free(&s);
    } else if (result.up_to_date) {
      printf("up to date: %s\n", result.apk_path);
    } else {
      printf("built %s\n", result.apk_path);
    }
  }
  ccb_build_result_free(&result);
  ccb_diag_list_free(&diags);
  return 0;
}

static int ccb_cmd_clean(ccb_args *a) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  char *build_dir = NULL;
  if (cfg != NULL) {
    build_dir = ccb_xstrdup(cfg->build_dir);
    ccb_config_free(cfg);
  } else {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      ccb_diag_list_free(&diags);
      return 2;
    }
    build_dir = ccb_path_join(cwd, "build");
  }
  ccb_diag_list_free(&diags);
  if (!ccb_path_exists(build_dir)) {
    if (!a->json) {
      printf("nothing to clean\n");
    } else {
      printf("{\"event\":\"clean\",\"removed\":false}\n");
    }
    free(build_dir);
    return 0;
  }
  if (ccb_remove_recursive(build_dir) != CCB_OK) {
    fprintf(stderr, "error: cannot remove %s\n", build_dir);
    free(build_dir);
    return 1;
  }
  if (a->json) {
    printf("{\"event\":\"clean\",\"removed\":true}\n");
  } else {
    printf("removed %s\n", build_dir);
  }
  free(build_dir);
  return 0;
}

static int ccb_cmd_doctor(ccb_args *a) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  int problems = 0;
  int cfg_state = 0; // 0 = loaded, 1 = not found, 2 = invalid
  if (cfg == NULL) {
    // Distinguish "no config file" from "config file is invalid": the
    // fallback message must not claim the file is missing when it is
    // present but broken.
    for (size_t i = 0; i < diags.count; i++) {
      if (strcmp(diags.items[i].code, "CCB-E1009") == 0) {
        cfg_state = 1;
      }
    }
    if (cfg_state == 0) {
      cfg_state = 2;
    }
    if (!a->json) {
      if (cfg_state == 1) {
        printf("no ccb.toml found; reporting environment toolchain only\n");
      } else {
        ccb_print_diags(a, &diags);
        printf("config invalid; reporting environment toolchain only\n");
      }
    }
    ccb_diag_list_free(&diags);
    ccb_diag_list_init(&diags);
  }
  if (cfg != NULL) {
    ccb_sdk *sdk = NULL;
    if (ccb_sdk_resolve(cfg, &sdk, &diags) != CCB_OK) {
      problems = 1;
      ccb_print_diags(a, &diags);
    } else if (a->json) {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "{\"event\":\"doctor\",\"project\":");
      ccb_json_escape(&s, cfg->name);
      ccb_str_append(&s, ",\"package\":");
      ccb_json_escape(&s, cfg->package);
      ccb_str_append(&s, ",\"sdk\":");
      ccb_json_escape(&s, sdk->sdk_home);
      ccb_str_append(&s, ",\"build_tools\":");
      ccb_json_escape(&s, sdk->build_tools_version);
      ccb_str_append(&s, ",\"aapt2\":");
      ccb_json_escape(&s, sdk->aapt2);
      ccb_str_append(&s, ",\"aapt2_source\":");
      ccb_json_escape(&s, sdk->aapt2_source == NULL ? "default" : sdk->aapt2_source);
      ccb_str_append(&s, ",\"zipalign\":");
      ccb_json_escape(&s, sdk->zipalign);
      ccb_str_append(&s, ",\"zipalign_source\":");
      ccb_json_escape(&s, sdk->zipalign_source == NULL ? "default" : sdk->zipalign_source);
      ccb_str_append(&s, ",\"kotlin\":");
      ccb_json_escape(&s, sdk->kotlin_version);
      ccb_str_appendf(&s, ",\"java\":%d", sdk->java_major);
      if (cfg->has_ndk) {
        ccb_str_append(&s, ",\"ndk\":");
        ccb_json_escape(&s, sdk->ndk_home);
      } else {
        ccb_str_append(&s, ",\"ndk\":null");
      }
      ccb_str_append(&s, "}\n");
      fputs(s.buf, stdout);
      ccb_str_free(&s);
      ccb_sdk_free(sdk);
    } else {
      printf("project: %s (%s)\n", cfg->name, cfg->package);
      printf("sdk: %s\n", sdk->sdk_home);
      printf("build-tools: %s (%s)\n", sdk->build_tools_version, sdk->build_tools_dir);
      printf("aapt2: %s (%s)\n", sdk->aapt2, sdk->aapt2_source == NULL ? "default" : sdk->aapt2_source);
      printf("zipalign: %s (%s)\n", sdk->zipalign, sdk->zipalign_source == NULL ? "default" : sdk->zipalign_source);
      printf("platform: android-%ld\n", cfg->compile_sdk);
      printf("kotlinc: %s (%s)\n", sdk->kotlin_version, sdk->kotlinc);
      printf("java: %d (%s)\n", sdk->java_major, sdk->java);
      if (cfg->has_ndk) {
        printf("ndk: %s\n", sdk->ndk_home);
      }
      ccb_sdk_free(sdk);
    }
    ccb_config_free(cfg);
  } else {
    // Environment-only probes.
    char *java = NULL;
    const char *jh = getenv("JAVA_HOME");
    if (jh != NULL) {
      char *p = ccb_path_join(jh, "bin/java");
      if (access(p, X_OK) == 0) {
        java = p;
      } else {
        free(p);
      }
    }
    if (java == NULL) {
      java = ccb_sdk_find_on_path("java");
    }
    char *kotlinc = NULL;
    const char *kh = getenv("KOTLIN_HOME");
    if (kh != NULL) {
      // Same tolerance as toolchain resolution: accept the nested
      // distribution layout (KOTLIN_HOME/kotlinc/bin/kotlinc).
      char *p = ccb_path_join3(kh, "bin", "kotlinc");
      if (access(p, X_OK) == 0) {
        kotlinc = p;
      } else {
        free(p);
        p = ccb_path_join3(kh, "kotlinc/bin", "kotlinc");
        if (access(p, X_OK) == 0) {
          kotlinc = p;
        } else {
          free(p);
        }
      }
    }
    if (kotlinc == NULL) {
      kotlinc = ccb_sdk_find_on_path("kotlinc");
    }
    const char *sdk_home = getenv("ANDROID_HOME");
    if (sdk_home == NULL) {
      sdk_home = getenv("ANDROID_SDK_ROOT");
    }
    if (a->json) {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "{\"event\":\"doctor\",\"config\":");
      ccb_str_append(&s, cfg_state == 1 ? "null" : "\"invalid\"");
      ccb_str_append(&s, ",\"java\":");
      if (java != NULL) {
        ccb_json_escape(&s, java);
      } else {
        ccb_str_append(&s, "null");
      }
      ccb_str_append(&s, ",\"kotlinc\":");
      if (kotlinc != NULL) {
        ccb_json_escape(&s, kotlinc);
      } else {
        ccb_str_append(&s, "null");
      }
      ccb_str_append(&s, ",\"sdk\":");
      if (sdk_home != NULL) {
        ccb_json_escape(&s, sdk_home);
      } else {
        ccb_str_append(&s, "null");
      }
      ccb_str_append(&s, "}\n");
      fputs(s.buf, stdout);
      ccb_str_free(&s);
    } else {
      printf("java: %s\n", java == NULL ? "MISSING" : java);
      printf("kotlinc: %s\n", kotlinc == NULL ? "MISSING" : kotlinc);
      printf("sdk: %s\n", sdk_home == NULL ? "MISSING (set ANDROID_HOME)" : sdk_home);
    }
    if (java == NULL || kotlinc == NULL || sdk_home == NULL) {
      problems = 1;
    }
    free(java);
    free(kotlinc);
  }
  ccb_diag_list_free(&diags);
  return problems ? 3 : 0;
}

// ---------- install / run / logcat ----------

// adb lives under the SDK's platform-tools; no full toolchain resolve is
// needed just to find it (that would waste a kotlinc -version probe).
// Precedence: [sdk] adb, then CCB_ADB, then the SDK's platform-tools
// adb, then adb on PATH. An explicit override that is missing or
// cannot run pushes a diagnostic (when diags is given) and returns
// NULL instead of silently falling back. SDK and PATH candidates must
// actually run: an SDK adb can exist yet be unrunnable (x86-64 binary
// on Termux aarch64), in which case the PATH copy wins.
static char *ccb_find_adb(ccb_config *cfg, ccb_diag_list *diags) {
  const char *ov = cfg != NULL ? cfg->adb_override : NULL;
  const char *label = "[sdk] adb";
  if (ov == NULL) {
    const char *env = getenv("CCB_ADB");
    if (env != NULL && env[0] != '\0') {
      ov = env;
      label = "CCB_ADB";
    }
  }
  if (ov != NULL) {
    if (access(ov, X_OK) == 0 && ccb_sdk_tool_runnable(ov, "version")) {
      return ccb_xstrdup(ov);
    }
    if (diags != NULL) {
      ccb_str m;
      ccb_str_init(&m);
      ccb_str_append(&m, label);
      ccb_str_append(&m, " points at ");
      ccb_str_append(&m, ov);
      ccb_str_append(&m, " but it is missing, not executable, or cannot run on this machine");
      ccb_diag d = ccb_diag_make("CCB-E2009", "device", m.buf);
      ccb_str_free(&m);
      ccb_diag_list_push(diags, &d);
    }
    return NULL;
  }
  const char *sdk_home = NULL;
  if (cfg != NULL) {
    sdk_home = cfg->sdk_home;
  }
  if (sdk_home == NULL) {
    sdk_home = getenv("ANDROID_HOME");
  }
  if (sdk_home == NULL) {
    sdk_home = getenv("ANDROID_SDK_ROOT");
  }
  if (sdk_home != NULL) {
    char *adb = ccb_path_join(sdk_home, "platform-tools/adb");
    if (access(adb, X_OK) == 0 && ccb_sdk_tool_runnable(adb, "version")) {
      return adb;
    }
    free(adb);
  }
  char *on_path = ccb_sdk_find_on_path("adb");
  if (on_path != NULL && ccb_sdk_tool_runnable(on_path, "version")) {
    return on_path;
  }
  free(on_path);
  return NULL;
}

static void ccb_device_diag(ccb_diag_list *diags, const char *code, const char *msg) {
  ccb_diag d = ccb_diag_make(code, "device", msg);
  d.suggestion = ccb_xstrdup(
    "connect a device: run 'adb devices' to check. For this VM's phone, use wireless debugging "
    "over the private Tailscale network (see docs/device-setup.md); adb over the public internet is not used.");
  ccb_diag_list_push(diags, &d);
}

static int ccb_cmd_install(ccb_args *a) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  if (cfg == NULL) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 2;
  }
  if (a->variant != NULL) {
    free(cfg->variant);
    cfg->variant = ccb_xstrdup(a->variant);
  }
  char *out = ccb_path_join3(cfg->build_dir, "outputs", cfg->variant);
  char *apk_path = ccb_path_join(out, "app.apk");
  free(out);
  size_t ndiags_before = diags.count;
  char *adb = ccb_find_adb(cfg, &diags);
  if (a->dry_run) {
    printf("%s install -r %s\n", adb == NULL ? "adb" : adb, apk_path);
    free(adb);
    free(apk_path);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 0;
  }
  if (!ccb_path_exists(apk_path)) {
    ccb_diag d = ccb_diag_make("CCB-E5002", "install", "no APK to install");
    d.suggestion = ccb_xstrdup("run `ccb build` first, then `ccb install`");
    ccb_diag_list_push(&diags, &d);
    ccb_print_diags(a, &diags);
    free(apk_path);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 1;
  }
  if (adb == NULL) {
    if (diags.count == ndiags_before) {
      ccb_diag d = ccb_diag_make("CCB-E2009", "install", "adb not found");
      d.suggestion = ccb_xstrdup("run `ccb setup` to install platform-tools, or put adb on PATH");
      ccb_diag_list_push(&diags, &d);
    }
    ccb_print_diags(a, &diags);
    free(apk_path);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 3;
  }
  char *argv[] = { adb, "install", "-r", apk_path, NULL };
  ccb_proc_result r;
  ccb_status prc = ccb_proc_run(argv, NULL, NULL, &r);
  int code = prc == CCB_OK ? r.exit_code : -1;
  if (a->verbose || code != 0) {
    fputs(r.out, stdout);
    fputs(r.err, stderr);
  }
  ccb_proc_result_free(&r);
  free(adb);
  free(apk_path);
  ccb_config_free(cfg);
  if (code != 0) {
    ccb_device_diag(&diags, "CCB-E5001", "adb install failed");
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 5;
  }
  ccb_diag_list_free(&diags);
  printf("installed\n");
  return 0;
}

// Find the MAIN/LAUNCHER activity android:name in a text manifest.
// Returns a malloc'd name (".MainActivity" style kept as-is) or NULL.
static char *ccb_manifest_launcher(const char *manifest_text) {
  const char *p = manifest_text;
  while ((p = strstr(p, "<activity")) != NULL) {
    const char *block_end = strstr(p, "</activity>");
    const char *next_activity = strstr(p + 1, "<activity");
    const char *end = block_end;
    if (end == NULL || (next_activity != NULL && next_activity < end)) {
      end = next_activity == NULL ? p + strlen(p) : next_activity;
    } else {
      end = block_end + strlen("</activity>");
    }
    size_t blen = (size_t)(end - p);
    char *block = ccb_xstrndup(p, blen);
    int is_launcher = strstr(block, "android.intent.action.MAIN") != NULL &&
      strstr(block, "android.intent.category.LAUNCHER") != NULL;
    char *name = NULL;
    if (is_launcher) {
      const char *n = strstr(block, "android:name=\"");
      if (n != NULL) {
        n += 14;
        const char *q = strchr(n, '"');
        if (q != NULL) {
          name = ccb_xstrndup(n, (size_t)(q - n));
        }
      }
    }
    free(block);
    if (name != NULL) {
      return name;
    }
    p = end;
  }
  return NULL;
}

static int ccb_cmd_run(ccb_args *a) {
  // Step 1: build. `run` always builds first so install + launch act on
  // the current sources. In dry-run mode the build prints its commands
  // and nothing is executed.
  ccb_build_opts opts;
  memset(&opts, 0, sizeof(opts));
  opts.json = a->json;
  opts.verbose = a->verbose;
  opts.dry_run = a->dry_run || a->explain;
  opts.variant_override = a->variant;
  opts.config_path = a->config_path;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_build_result result;
  ccb_status brc = ccb_pipeline_build(&opts, &result, &diags);
  if (brc != CCB_OK) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return ccb_exit_code(brc);
  }
  ccb_build_result_free(&result);
  // Steps 2-3: install + launch.
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  if (cfg == NULL) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 2;
  }
  if (a->variant != NULL) {
    free(cfg->variant);
    cfg->variant = ccb_xstrdup(a->variant);
  }
  char *out = ccb_path_join3(cfg->build_dir, "outputs", cfg->variant);
  char *apk_path = ccb_path_join(out, "app.apk");
  free(out);
  size_t ndiags_before = diags.count;
  char *adb = ccb_find_adb(cfg, &diags);
  if (a->dry_run || a->explain) {
    printf("%s install -r %s\n", adb == NULL ? "adb" : adb, apk_path);
    char *mtext = NULL;
    size_t mlen = 0;
    if (ccb_read_file(cfg->manifest, &mtext, &mlen) == CCB_OK) {
      char *activity = ccb_manifest_launcher(mtext);
      free(mtext);
      if (activity != NULL) {
        ccb_str component;
        ccb_str_init(&component);
        ccb_str_append(&component, cfg->package);
        ccb_str_append_c(&component, '/');
        if (activity[0] == '.') {
          ccb_str_append(&component, cfg->package);
          ccb_str_append(&component, activity);
        } else {
          ccb_str_append(&component, activity);
        }
        free(activity);
        printf("%s shell am start -n %s\n", adb == NULL ? "adb" : adb, component.buf);
        ccb_str_free(&component);
      } else {
        printf("# no MAIN/LAUNCHER activity found in the manifest; launch step skipped\n");
      }
    }
    free(adb);
    free(apk_path);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 0;
  }
  if (adb == NULL) {
    if (diags.count == ndiags_before) {
      ccb_diag d = ccb_diag_make("CCB-E2009", "install", "adb not found");
      d.suggestion = ccb_xstrdup("run `ccb setup` to install platform-tools, or put adb on PATH");
      ccb_diag_list_push(&diags, &d);
    }
    ccb_print_diags(a, &diags);
    free(apk_path);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 3;
  }
  char *install_argv[] = { adb, "install", "-r", apk_path, NULL };
  ccb_proc_result r;
  ccb_status prc = ccb_proc_run(install_argv, NULL, NULL, &r);
  int code = prc == CCB_OK ? r.exit_code : -1;
  if (a->verbose || code != 0) {
    fputs(r.out, stdout);
    fputs(r.err, stderr);
  }
  ccb_proc_result_free(&r);
  if (code != 0) {
    ccb_device_diag(&diags, "CCB-E5001", "adb install failed");
    ccb_print_diags(a, &diags);
    free(adb);
    free(apk_path);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 5;
  }
  char *mtext = NULL;
  size_t mlen = 0;
  if (ccb_read_file(cfg->manifest, &mtext, &mlen) != CCB_OK) {
    fprintf(stderr, "error: cannot read manifest\n");
    free(adb);
    free(apk_path);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 2;
  }
  char *activity = ccb_manifest_launcher(mtext);
  free(mtext);
  if (activity == NULL) {
    fprintf(stderr, "error: no MAIN/LAUNCHER activity found in the manifest\n");
    free(adb);
    free(apk_path);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 1;
  }
  ccb_str component;
  ccb_str_init(&component);
  ccb_str_append(&component, cfg->package);
  ccb_str_append_c(&component, '/');
  if (activity[0] == '.') {
    ccb_str_append(&component, cfg->package);
    ccb_str_append(&component, activity);
  } else {
    ccb_str_append(&component, activity);
  }
  free(activity);
  char *launch_argv[] = { adb, "shell", "am", "start", "-n", component.buf, NULL };
  prc = ccb_proc_run(launch_argv, NULL, NULL, &r);
  code = prc == CCB_OK ? r.exit_code : -1;
  if (a->verbose || code != 0) {
    fputs(r.out, stdout);
    fputs(r.err, stderr);
  }
  ccb_proc_result_free(&r);
  free(adb);
  ccb_str_free(&component);
  free(apk_path);
  ccb_config_free(cfg);
  if (code != 0) {
    ccb_device_diag(&diags, "CCB-E5001", "adb launch failed");
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 5;
  }
  ccb_diag_list_free(&diags);
  printf("launched\n");
  return 0;
}

static int ccb_cmd_logcat(ccb_args *a) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  // logcat works without a project too; only the config load may fail.
  ccb_diag_list_free(&diags);
  ccb_diag_list adiags;
  ccb_diag_list_init(&adiags);
  char *adb = ccb_find_adb(cfg, &adiags);
  if (cfg != NULL) {
    ccb_config_free(cfg);
  }
  if (adb == NULL) {
    if (adiags.count > 0) {
      ccb_print_diags(a, &adiags);
    } else {
      fprintf(stderr, "error: adb not found (install platform-tools or put adb on PATH)\n");
    }
    ccb_diag_list_free(&adiags);
    return 3;
  }
  ccb_diag_list_free(&adiags);
  // adb logcat blocks forever on "- waiting for device -" when nothing
  // is attached, so check first and fail fast with a real diagnostic.
  int have_device = 0;
  {
    char *dev_argv[] = { adb, "devices", NULL };
    ccb_proc_result r;
    if (ccb_proc_run(dev_argv, NULL, NULL, &r) == CCB_OK) {
      // Skip the "List of devices attached" header; a device line looks
      // like "<serial>\tdevice".
      char *line = r.out;
      while (line != NULL) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
          *nl = '\0';
        }
        if (strstr(line, "List of devices") == NULL && line[0] != '\0') {
          char *tab = strchr(line, '\t');
          if (tab != NULL && strcmp(tab + 1, "device") == 0) {
            have_device = 1;
          }
        }
        line = nl == NULL ? NULL : nl + 1;
      }
    }
    ccb_proc_result_free(&r);
  }
  if (!have_device) {
    ccb_diag_list diags2;
    ccb_diag_list_init(&diags2);
    ccb_device_diag(&diags2, "CCB-E5001", "no device connected for logcat");
    ccb_print_diags(a, &diags2);
    ccb_diag_list_free(&diags2);
    free(adb);
    return 5;
  }
  char *argv[] = { adb, "logcat", NULL };
  int code = ccb_proc_run_inherit(argv);
  free(adb);
  if (code != 0) {
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_device_diag(&diags, "CCB-E5001", "adb logcat failed");
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 5;
  }
  return 0;
}

// ---------- preview ----------

// Device width presets for `ccb preview --device`: phone is the
// existing default frame width; tablet is a wider frame. An explicit
// --width always wins over --device (resolved by the caller).
#define CCB_PREVIEW_PHONE_WIDTH 40
#define CCB_PREVIEW_TABLET_WIDTH 80

// Write data to a file for --output. Returns 0 on success, -1 on
// failure (message already printed to stderr).
static int ccb_preview_write_file(const char *path, const char *data,
    size_t len) {
  if (ccb_write_file(path, data, len) != CCB_OK) {
    fprintf(stderr, "error: cannot write %s\n", path);
    return -1;
  }
  return 0;
}

// Resolve the golden directory for --update-goldens. The test-only
// CCB_GOLDEN_DIR override wins when set; otherwise walk up from cwd
// looking for tests/golden/preview. Returns a malloc'd path or NULL
// (message already printed to stderr).
static char *ccb_preview_golden_dir(const char *cwd) {
  const char *env = getenv("CCB_GOLDEN_DIR");
  if (env != NULL && env[0] != '\0') {
    return ccb_xstrdup(env);
  }
  {
    char *dir = ccb_xstrdup(cwd);
    for (;;) {
      char *cand = ccb_path_join3(dir, "tests", "golden/preview");
      // ccb_path_join3 joins with single separators, so the third
      // argument's embedded slash survives as a path component.
      if (ccb_path_exists(cand)) {
        free(dir);
        return cand;
      }
      free(cand);
      {
        char *parent = ccb_dirname(dir);
        if (strcmp(parent, dir) == 0) {
          free(parent);
          break;
        }
        free(dir);
        dir = parent;
      }
    }
    free(dir);
  }
  fprintf(stderr, "error: cannot locate golden directory "
    "(run from inside the CCB tree, or set CCB_GOLDEN_DIR)\n");
  return NULL;
}

// Regenerate the golden files from the current render: one frame file
// per screen (<Screen>.txt), logic-tree.txt, and preview.json (the
// JSON form with no trailing newline, matching the checked-in file).
// Frames use the --ascii flag and no color, matching the goldens.
// Returns 0 on success, 2 on any write failure.
static int ccb_preview_update_goldens(pv_project *proj, int ascii,
    const char *golden_dir) {
  size_t i = 0;
  ccb_str out;
  ccb_str_init(&out);
  for (i = 0; i < proj->nscreens; i++) {
    ccb_str name;
    char *path = NULL;
    ccb_str_init(&out);
    pv_render_screen(&out, proj->screens[i], proj->frame_width, ascii, 0);
    ccb_str_init(&name);
    ccb_str_append(&name, proj->screens[i]->name != NULL ?
      proj->screens[i]->name : "?");
    ccb_str_append(&name, ".txt");
    path = ccb_path_join(golden_dir, name.buf);
    ccb_str_free(&name);
    if (ccb_preview_write_file(path, out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : "")) != 0) {
      free(path);
      ccb_str_free(&out);
      return 2;
    }
    free(path);
    ccb_str_free(&out);
  }
  ccb_str_init(&out);
  pv_render_logic_tree(&out, proj);
  {
    char *path = ccb_path_join(golden_dir, "logic-tree.txt");
    int rc = ccb_preview_write_file(path,
      out.buf != NULL ? out.buf : "",
      strlen(out.buf != NULL ? out.buf : ""));
    free(path);
    ccb_str_free(&out);
    if (rc != 0) {
      return 2;
    }
  }
  ccb_str_init(&out);
  pv_project_json(&out, proj);
  {
    char *path = ccb_path_join(golden_dir, "preview.json");
    int rc = ccb_preview_write_file(path,
      out.buf != NULL ? out.buf : "",
      strlen(out.buf != NULL ? out.buf : ""));
    free(path);
    ccb_str_free(&out);
    if (rc != 0) {
      return 2;
    }
  }
  printf("updated goldens in %s\n", golden_dir);
  return 0;
}

// ---------- preview watch ----------

// Free the watch path copies (all NULL-safe). The watch loop takes
// its own copies of the dirs, so the caller's copies are freed at the
// watch branch; the early exits above free them instead.
static void ccb_preview_watch_paths_free(char *toml, char *dir,
    char *styles, char *flow) {
  free(toml);
  free(dir);
  free(styles);
  free(flow);
}

// Render one preview, exactly as a single `ccb preview` does: human
// frames + logic tree, or the JSON object (with its trailing newline).
// --output writes the file and leaves stdout empty. Diagnostics go to
// stderr in human mode and ride inside the JSON object in JSON mode.
// Returns 0 (ok), 1 (preview errors), or 2 (output write failure).
// The caller keeps owning proj.
static int ccb_preview_render_once(ccb_args *a, pv_project *proj,
    pv_screen *screen, long errors) {
  size_t i = 0;
  if (a->json) {
    ccb_str out;
    ccb_str_init(&out);
    pv_project_json(&out, proj);
    // The file carries exactly what stdout would have carried.
    ccb_str_append(&out, "\n");
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : ""));
      ccb_str_free(&out);
      return rc == 0 ? (errors > 0 ? 1 : 0) : 2;
    }
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
  } else {
    int color = 0;
    const char *term = NULL;
    ccb_str out;
    // A file is not a terminal: never write ANSI colors into it.
    if (!a->preview_ascii && a->preview_output == NULL &&
        getenv("NO_COLOR") == NULL) {
      term = getenv("TERM");
      color = term == NULL || strcmp(term, "dumb") != 0;
    }
    ccb_str_init(&out);
    if (screen != NULL) {
      pv_render_screen(&out, screen, proj->frame_width,
        a->preview_ascii, color);
    } else {
      for (i = 0; i < proj->nscreens; i++) {
        pv_render_screen(&out, proj->screens[i], proj->frame_width,
          a->preview_ascii, color);
      }
    }
    pv_render_logic_tree(&out, proj);
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : ""));
      ccb_str_free(&out);
      // Diagnostics in human form, to stderr like other commands.
      ccb_print_diags_human(&proj->diags);
      return rc == 0 ? (errors > 0 ? 1 : 0) : 2;
    }
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
    // Diagnostics in human form, to stderr like other commands.
    ccb_print_diags_human(&proj->diags);
  }
  return errors > 0 ? 1 : 0;
}

// One watch render, with the refresh framing for re-renders (first is
// 0): TTY stdout clears first, piped human mode gets a delimiter line
// on stdout, JSONL mode gets neither (each render stays one JSON
// object + newline), and --output mode reports on stderr instead
// (stdout stays empty). A selected screen that vanished since the
// last render prints a diagnostic naming it and renders nothing; the
// loop keeps watching. Returns the single-render exit code; the loop
// ignores it (preview errors surface via diagnostics, and a signal
// stop still exits 0).
static int ccb_preview_watch_render(ccb_args *a, pv_project *proj,
    const char *selected_name, long errors, int first) {
  pv_screen *screen = NULL;
  size_t i = 0;
  if (selected_name != NULL) {
    for (i = 0; i < proj->nscreens; i++) {
      if (proj->screens[i]->name != NULL &&
          strcmp(proj->screens[i]->name, selected_name) == 0) {
        screen = proj->screens[i];
        break;
      }
    }
    if (screen == NULL) {
      fprintf(stderr, "error: screen '%s' no longer exists; still watching\n",
        selected_name);
      return 0;
    }
  }
  if (!first) {
    if (a->preview_output != NULL) {
      fprintf(stderr, "preview refreshed: %s\n", a->preview_output);
    } else if (isatty(STDOUT_FILENO)) {
      fputs("\033[2J\033[H", stdout);
    } else if (!a->json) {
      fputs("--- ccb preview refresh ---\n", stdout);
    }
  }
  return ccb_preview_render_once(a, proj, screen, errors);
}

// NULL-aware dir comparison for the watched-dirs-moved check.
static int ccb_preview_watch_dirs_changed(const char *old_dir,
    const char *new_dir) {
  if (old_dir == NULL || new_dir == NULL) {
    return old_dir != new_dir;
  }
  return strcmp(old_dir, new_dir) != 0;
}

// The watch loop. Borrows toml_path, config_dir, styles_dir, flow_dir
// and selected_name (NULL renders every screen); takes ownership of
// proj and frees it on every exit path. Signals are installed once
// the initial render is out: the handlers only set pv_watch_stop, the
// loop checks it every tick and exits 0 on stop (nothing terminal
// related to restore, since watch mode never enters raw mode, but
// stdout is flushed first). A reload or load failure after a change
// (a half-saved TOML, broken CSS/Kotlin, a [preview] section removed)
// prints diagnostics and keeps watching on the last good render; a
// broken save never kills the loop. Returns 0 on a signal stop, 2 on
// a watch setup or wait failure.
static int ccb_preview_watch_loop(ccb_args *a, const char *toml_path,
    const char *config_dir, const char *styles_dir, const char *flow_dir,
    long eff_width, pv_project *proj, long errors,
    const char *selected_name) {
  pv_watch_ctx *ctx = NULL;
  pv_watch_snapshot *snap = NULL;
  pv_watch_debounce deb;
  char *cur_styles = styles_dir == NULL ? NULL : ccb_xstrdup(styles_dir);
  char *cur_flow = flow_dir == NULL ? NULL : ccb_xstrdup(flow_dir);
  int used_inotify = 0;
  int rc = 0;
  memset(&deb, 0, sizeof(deb));
  // Initial render, exactly like a single preview.
  ccb_preview_watch_render(a, proj, selected_name, errors, 1);
  fflush(stdout);
  pv_watch_install_signals();
  if (pv_watch_stop) {
    goto watch_done;
  }
  if (pv_watch_begin(config_dir, cur_styles, cur_flow, toml_path, &ctx,
      &used_inotify) != CCB_OK) {
    fprintf(stderr, "error: cannot watch preview inputs\n");
    rc = 2;
    goto watch_done;
  }
  CCB_UNUSED(used_inotify);
  if (pv_watch_snapshot_take(config_dir, cur_styles, cur_flow, toml_path,
      &snap) != CCB_OK) {
    fprintf(stderr, "error: cannot scan preview inputs\n");
    rc = 2;
    goto watch_done;
  }
  for (;;) {
    int w = 0;
    if (pv_watch_stop) {
      break;
    }
    w = pv_watch_wait(ctx, 50);
    if (pv_watch_stop) {
      break;
    }
    if (w < 0) {
      fprintf(stderr, "error: preview watch failed\n");
      rc = 2;
      break;
    }
    if (w == 1) {
      pv_watch_debounce_event(&deb, pv_watch_now_ms());
    }
    if (!pv_watch_debounce_due(&deb, pv_watch_now_ms())) {
      continue;
    }
    pv_watch_debounce_reset(&deb);
    {
      pv_watch_snapshot *next = NULL;
      ccb_diag_list rdiags;
      ccb_config *ncfg = NULL;
      pv_project *nproj = NULL;
      long nerrors = 0;
      if (pv_watch_snapshot_take(config_dir, cur_styles, cur_flow,
          toml_path, &next) != CCB_OK) {
        // A scan failure (a directory briefly missing) is not a
        // reason to quit; the next tick rescans.
        continue;
      }
      if (!pv_watch_snapshot_changed(snap, next)) {
        pv_watch_snapshot_free(next);
        continue;
      }
      // Adopt the new baseline before reloading, so a broken save
      // does not re-report on every tick.
      pv_watch_snapshot_free(snap);
      snap = next;
      // Reload config from the watched toml path: [preview] paths
      // may themselves have changed.
      ccb_diag_list_init(&rdiags);
      if (ccb_config_load(toml_path, a->variant, &ncfg, &rdiags) != CCB_OK) {
        ccb_print_diags_human(&rdiags);
        ccb_diag_list_free(&rdiags);
        continue;
      }
      // A successful load leaves no diagnostics to print; the call
      // is harmless when the list is empty and surfaces any warning
      // the loader did emit.
      ccb_print_diags_human(&rdiags);
      ccb_diag_list_free(&rdiags);
      if (!ncfg->has_preview || (ncfg->preview_styles == NULL &&
          ncfg->preview_flow == NULL)) {
        fprintf(stderr, "error: preview no longer applicable to this "
          "project; still watching\n");
        ccb_config_free(ncfg);
        continue;
      }
      {
        ccb_diag_list ldiags;
        ccb_status lrc = CCB_OK;
        ccb_diag_list_init(&ldiags);
        lrc = pv_project_load(ncfg, eff_width, a->preview_ascii,
          a->preview_strict, &ldiags, &nproj);
        if (lrc != CCB_OK || nproj == NULL) {
          ccb_print_diags_human(&ldiags);
          ccb_diag_list_free(&ldiags);
          ccb_config_free(ncfg);
          continue;
        }
        // Success: ownership of ldiags transferred to nproj; the
        // caller's list is empty and must not be freed again.
      }
      nerrors = pv_project_diagnose(nproj);
      // The watched dirs may have moved: re-point the watch at the
      // new dirs so later changes there are seen. The new context is
      // begun before the old one ends, so a begin failure keeps the
      // old watch (and the fresh render) intact.
      if (ccb_preview_watch_dirs_changed(cur_styles,
          ncfg->preview_styles) ||
          ccb_preview_watch_dirs_changed(cur_flow,
          ncfg->preview_flow)) {
        pv_watch_ctx *nctx = NULL;
        if (pv_watch_begin(config_dir, ncfg->preview_styles,
            ncfg->preview_flow, toml_path, &nctx,
            &used_inotify) == CCB_OK) {
          pv_watch_end(ctx);
          ctx = nctx;
          free(cur_styles);
          free(cur_flow);
          cur_styles = ncfg->preview_styles == NULL ? NULL :
            ccb_xstrdup(ncfg->preview_styles);
          cur_flow = ncfg->preview_flow == NULL ? NULL :
            ccb_xstrdup(ncfg->preview_flow);
          // Re-baseline against the new dirs so the move itself does
          // not trigger one spurious refresh.
          {
            pv_watch_snapshot *fresh = NULL;
            if (pv_watch_snapshot_take(config_dir, cur_styles, cur_flow,
                toml_path, &fresh) == CCB_OK) {
              pv_watch_snapshot_free(snap);
              snap = fresh;
            }
          }
        } else {
          fprintf(stderr, "error: cannot watch new preview directories; "
            "still watching the old ones\n");
        }
      }
      ccb_config_free(ncfg);
      pv_project_free(proj);
      proj = nproj;
      errors = nerrors;
      ccb_preview_watch_render(a, proj, selected_name, errors, 0);
      fflush(stdout);
    }
  }
watch_done:
  if (snap != NULL) {
    pv_watch_snapshot_free(snap);
  }
  if (ctx != NULL) {
    pv_watch_end(ctx);
  }
  pv_project_free(proj);
  free(cur_styles);
  free(cur_flow);
  fflush(stdout);
  return rc;
}

// ---------- preview storyboard ----------

// Parse a --storyboard list ("A,B,C", names trimmed of surrounding
// whitespace, repeats allowed) into the journey's screens, in order.
// Returns NULL on any error (message already printed to stderr):
// an empty list, an empty or whitespace-only element, or a name
// that is not a screen in the project. All are usage errors (exit 2).
static pv_screen **ccb_preview_storyboard_parse(pv_project *proj,
    const char *list, size_t *out_n) {
  pv_screen **screens = NULL;
  size_t n = 0;
  size_t cap = 0;
  const char *p = NULL;
  *out_n = 0;
  if (list == NULL || list[0] == '\0') {
    fprintf(stderr, "error: --storyboard requires a non-empty list of "
      "screen names\n");
    return NULL;
  }
  p = list;
  for (;;) {
    const char *seg_start = p;
    const char *seg_end = NULL;
    const char *s = NULL;
    const char *e = NULL;
    size_t i = 0;
    pv_screen *found = NULL;
    while (*p != '\0' && *p != ',') {
      p++;
    }
    seg_end = p;
    s = seg_start;
    e = seg_end;
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' ||
        *s == '\r')) {
      s++;
    }
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
        e[-1] == '\r')) {
      e--;
    }
    if (s == e) {
      fprintf(stderr, "error: --storyboard has an empty element\n");
      free(screens);
      return NULL;
    }
    for (i = 0; i < proj->nscreens; i++) {
      const char *name = proj->screens[i]->name;
      if (name != NULL && strlen(name) == (size_t)(e - s) &&
          memcmp(name, s, (size_t)(e - s)) == 0) {
        found = proj->screens[i];
        break;
      }
    }
    if (found == NULL) {
      fprintf(stderr, "error: --storyboard: no screen named '%.*s'\n",
        (int)(e - s), s);
      free(screens);
      return NULL;
    }
    if (n == cap) {
      size_t ncap = cap == 0 ? 4 : cap * 2;
      screens = ccb_xrealloc(screens, ncap * sizeof(*screens));
      cap = ncap;
    }
    screens[n++] = found;
    if (*p == '\0') {
      break;
    }
    p++; // consume ','
  }
  *out_n = n;
  return screens;
}

// Render one storyboard: human mode prints each journey screen's
// frame preceded by "Journey N/M: <Name>", then the normal logic
// tree, then diagnostics to stderr; --json prints the same JSON
// object as a plain preview but with "screens" filtered to the
// journey screens, in journey order (repeats as separate entries).
// --output writes the render to the file and leaves stdout empty.
// Returns 0 (ok), 1 (preview errors), or 2 (output write failure).
static int ccb_preview_render_storyboard(ccb_args *a, pv_project *proj,
    pv_screen **journey, size_t njourney, long errors) {
  size_t i = 0;
  if (a->json) {
    ccb_str out;
    ccb_str_init(&out);
    pv_project_json_screens(&out, proj, journey, njourney);
    ccb_str_append(&out, "\n");
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : ""));
      ccb_str_free(&out);
      return rc == 0 ? (errors > 0 ? 1 : 0) : 2;
    }
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
  } else {
    int color = 0;
    const char *term = NULL;
    ccb_str out;
    if (!a->preview_ascii && a->preview_output == NULL &&
        getenv("NO_COLOR") == NULL) {
      term = getenv("TERM");
      color = term == NULL || strcmp(term, "dumb") != 0;
    }
    ccb_str_init(&out);
    for (i = 0; i < njourney; i++) {
      ccb_str_appendf(&out, "Journey %zu/%zu: %s\n", i + 1, njourney,
        journey[i]->name != NULL ? journey[i]->name : "?");
      pv_render_screen(&out, journey[i], proj->frame_width,
        a->preview_ascii, color);
    }
    pv_render_logic_tree(&out, proj);
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : ""));
      ccb_str_free(&out);
      ccb_print_diags_human(&proj->diags);
      return rc == 0 ? (errors > 0 ? 1 : 0) : 2;
    }
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
    ccb_print_diags_human(&proj->diags);
  }
  return errors > 0 ? 1 : 0;
}

static int ccb_cmd_preview(ccb_args *a) {
  char cwd[4096];
  char *path = NULL;
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  pv_project *proj = NULL;
  ccb_status lrc = CCB_OK;
  long errors = 0;
  const char *selected = NULL;
  pv_screen *selected_screen = NULL;
  size_t i = 0;
  // --dry-run and --explain are ignored for preview: it never builds.
  CCB_UNUSED(a->dry_run);
  CCB_UNUSED(a->explain);
  // Conflicting destinations fail loudly instead of guessing.
  if (a->preview_output != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --output cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_update_goldens && a->preview_screen != NULL) {
    fprintf(stderr, "error: --screen cannot be combined with "
      "--update-goldens (goldens cover every screen)\n");
    return 2;
  }
  if (a->preview_walk && a->preview_output != NULL) {
    fprintf(stderr, "error: --walk cannot be combined with "
      "--output (walk mode is interactive)\n");
    return 2;
  }
  if (a->preview_walk && a->preview_update_goldens) {
    fprintf(stderr, "error: --walk cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_watch && a->preview_update_goldens) {
    fprintf(stderr, "error: --watch cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_watch && a->preview_walk) {
    fprintf(stderr, "error: --watch cannot be combined with "
      "--walk\n");
    return 2;
  }
  // Chunk 4: tree-live is interactive like walk, so it rejects the
  // other output modes the same way. --scenario/--trace compose
  // (they are passed to the spawned server); --screen and a
  // positional screen select the initial screen.
  if (a->preview_tree_live && a->preview_walk) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--walk\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_watch) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--watch\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_output != NULL) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--output (tree-live mode is interactive)\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_update_goldens) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_diff != NULL) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--diff\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_storyboard != NULL) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--storyboard\n");
    return 2;
  }
  if (a->preview_diff != NULL && a->preview_watch) {
    fprintf(stderr, "error: --diff cannot be combined with --watch\n");
    return 2;
  }
  if (a->preview_diff != NULL && a->preview_walk) {
    fprintf(stderr, "error: --diff cannot be combined with --walk\n");
    return 2;
  }
  if (a->preview_diff != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --diff cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_screen != NULL) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--screen\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->positional != NULL) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--screen (a positional screen counts as --screen)\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_walk) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--walk\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_watch) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--watch\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_diff != NULL) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--diff\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  // Chunk 1: a scenario applies to the plain render (human or
  // --json) only. Walk takes it later, and watch/diff/storyboard/
  // goldens each own their own project load; failing loudly beats
  // silently rendering without the scenario.
  if (a->preview_scenario != NULL && a->preview_walk) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--walk (walk takes --scenario in a later chunk)\n");
    return 2;
  }
  if (a->preview_scenario != NULL && a->preview_watch) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--watch\n");
    return 2;
  }
  if (a->preview_scenario != NULL && a->preview_diff != NULL) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--diff\n");
    return 2;
  }
  if (a->preview_scenario != NULL && a->preview_storyboard != NULL) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--storyboard\n");
    return 2;
  }
  if (a->preview_scenario != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--update-goldens (goldens cover the no-scenario baseline)\n");
    return 2;
  }
  // Chunk 2: --record only makes sense with --walk (it records the
  // walk), and --trace is a render overlay, so it rejects the other
  // render modes the same way --scenario does. --trace composes
  // with --scenario (scenario applies at load, trace after), and
  // with --json/--screen/--output like any plain render.
  if (a->preview_record != NULL && !a->preview_walk) {
    fprintf(stderr, "error: --record requires --walk\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_walk) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--walk (record a walk with --record instead)\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_watch) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--watch\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_diff != NULL) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--diff\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_storyboard != NULL) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--storyboard\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--update-goldens (goldens cover the no-trace baseline)\n");
    return 2;
  }
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    fprintf(stderr, "error: cannot determine working directory\n");
    return 2;
  }
  if (ccb_config_discover(cwd, a->config_path, &path) != CCB_OK) {
    if (a->config_path == NULL) {
      // No project here at all: preview simply does not apply.
      // A diff, though, needs something to compare against, so it
      // is a usage error here rather than "not applicable".
      if (a->preview_diff != NULL) {
        fprintf(stderr, "error: --diff requires a project: no ccb.toml "
          "found in this directory or any parent\n");
        return 2;
      }
      // With --output the message goes to the file, not stdout.
      if (a->preview_output != NULL) {
        if (ccb_preview_write_file(a->preview_output,
            "preview not applicable to this project\n",
            strlen("preview not applicable to this project\n")) != 0) {
          return 2;
        }
        return 0;
      }
      printf("preview not applicable to this project\n");
      return 0;
    }
    // An explicit --config pointing at a missing file is a config error.
    ccb_diag_list_init(&diags);
    {
      ccb_diag d = ccb_diag_make("CCB-E1009", "config",
        "config file not found");
      ccb_diag_list_push(&diags, &d);
    }
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 2;
  }
  ccb_diag_list_init(&diags);
  if (ccb_config_load(path, a->variant, &cfg, &diags) != CCB_OK) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    free(path);
    return 2;
  }
  // Watch mode keeps the discovered toml path, its directory, and the
  // resolved preview dirs alive for the loop's lifetime: the loop
  // re-loads config from the toml path on every change, and the
  // [preview] paths may themselves move. All four are NULL when
  // --watch is not set; the early exits below free them.
  char *watch_toml = NULL;
  char *watch_config_dir = NULL;
  char *watch_styles = NULL;
  char *watch_flow = NULL;
  if (a->preview_watch) {
    watch_toml = ccb_xstrdup(path);
    watch_config_dir = ccb_dirname(path);
    watch_styles = cfg->preview_styles == NULL ? NULL :
      ccb_xstrdup(cfg->preview_styles);
    watch_flow = cfg->preview_flow == NULL ? NULL :
      ccb_xstrdup(cfg->preview_flow);
  }
  free(path);
  if (a->preview_diff != NULL) {
    // Diff runs its own project load: it must report a project without
    // a usable [preview] section as exit 2, not the plain preview's
    // exit-0 "not applicable", so it branches before the
    // not-applicable early return below. --screen overrides the
    // positional screen, as in the normal render path.
    const char *diff_screen = a->preview_screen != NULL ?
      a->preview_screen : a->positional;
    long diff_width = a->preview_width;
    ccb_str out, err;
    int drc = 0;
    if (!a->preview_width_set && a->preview_device != NULL) {
      diff_width = strcmp(a->preview_device, "tablet") == 0 ?
        CCB_PREVIEW_TABLET_WIDTH : CCB_PREVIEW_PHONE_WIDTH;
    }
    ccb_str_init(&out);
    ccb_str_init(&err);
    drc = pv_diff_run(cfg, a->preview_diff, diff_screen, diff_width,
      a->preview_ascii, a->preview_strict, a->json, &out, &err);
    if (err.buf != NULL && err.buf[0] != '\0') {
      fputs(err.buf, stderr);
    }
    if (drc == 0 || drc == 1) {
      if (a->json) {
        ccb_str_append(&out, "\n");
      }
      if (a->preview_output != NULL) {
        int wrc = ccb_preview_write_file(a->preview_output,
          out.buf != NULL ? out.buf : "",
          strlen(out.buf != NULL ? out.buf : ""));
        ccb_str_free(&out);
        ccb_str_free(&err);
        ccb_config_free(cfg);
        ccb_diag_list_free(&diags);
        ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
          watch_styles, watch_flow);
        return wrc == 0 ? drc : 2;
      }
      fputs(out.buf != NULL ? out.buf : "", stdout);
    }
    ccb_str_free(&out);
    ccb_str_free(&err);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
      watch_styles, watch_flow);
    return drc;
  }
  if (!cfg->has_preview || (cfg->preview_styles == NULL &&
      cfg->preview_flow == NULL)) {
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        "preview not applicable to this project\n",
        strlen("preview not applicable to this project\n"));
      ccb_config_free(cfg);
      ccb_diag_list_free(&diags);
      ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
        watch_styles, watch_flow);
      return rc == 0 ? 0 : 2;
    }
    printf("preview not applicable to this project\n");
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
      watch_styles, watch_flow);
    return 0;
  }
  // Effective frame width: an explicit --width always wins over
  // --device; otherwise the device preset applies (phone = the
  // default 40, tablet = 80); with neither, 0 lets load use its
  // default. Hoisted out of the block so the watch loop reuses the
  // same width on every reload.
  long eff_width = a->preview_width;
  if (!a->preview_width_set && a->preview_device != NULL) {
    eff_width = strcmp(a->preview_device, "tablet") == 0 ?
      CCB_PREVIEW_TABLET_WIDTH : CCB_PREVIEW_PHONE_WIDTH;
  }
  // Chunk 1: resolve the scenario by name before the config is
  // freed. An unknown name is a usage error (exit 2), like an
  // unknown --screen — distinct from a scenario entry that matches
  // nothing, which warns (CCB-P1022) during the load.
  const ccb_scenario *scenario = NULL;
  if (a->preview_scenario != NULL) {
    scenario = ccb_config_scenario(cfg, a->preview_scenario);
    if (scenario == NULL) {
      fprintf(stderr, "error: unknown scenario '%s'\n",
        a->preview_scenario);
      fprintf(stderr, "available scenarios:");
      for (i = 0; i < cfg->preview_scenario_count; i++) {
        fprintf(stderr, " %s",
          cfg->preview_scenarios[i].name != NULL ?
          cfg->preview_scenarios[i].name : "?");
      }
      fprintf(stderr, "\n");
      ccb_config_free(cfg);
      ccb_diag_list_free(&diags);
      ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
        watch_styles, watch_flow);
      return 2;
    }
  }
  // Chunk 4: tree-live spawns the server instead of loading a
  // project locally, so it branches before the load. The scenario
  // was already validated above; the trace is validated here
  // (read + parse, exit 2 on failure) so a bad --trace is a usage
  // error, not a server that dies mid-session. The server root is
  // re-discovered rather than kept from the freed toml path, so
  // this branch owns no extra lifetime.
  if (a->preview_tree_live) {
    char *root_path = NULL;
    char *root_dir = NULL;
    const char *start = a->preview_screen != NULL ?
      a->preview_screen : a->positional;
    int trc = 0;
    if (ccb_config_discover(cwd, a->config_path, &root_path) !=
        CCB_OK) {
      fprintf(stderr, "error: no ccb.toml found in this directory "
        "or any parent\n");
      ccb_config_free(cfg);
      ccb_diag_list_free(&diags);
      ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
        watch_styles, watch_flow);
      return 2;
    }
    root_dir = ccb_dirname(root_path);
    free(root_path);
    if (a->preview_trace != NULL) {
      char *ttext = NULL;
      size_t tlen = 0;
      char *terr = NULL;
      pv_trace *trace = NULL;
      if (ccb_read_file(a->preview_trace, &ttext, &tlen) != CCB_OK) {
        fprintf(stderr, "error: cannot read trace file '%s'\n",
          a->preview_trace);
        free(root_dir);
        ccb_config_free(cfg);
        ccb_diag_list_free(&diags);
        ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
          watch_styles, watch_flow);
        return 2;
      }
      trace = pv_trace_parse(ttext, &terr);
      free(ttext);
      if (trace == NULL) {
        fprintf(stderr, "error: invalid trace file '%s': %s\n",
          a->preview_trace, terr != NULL ? terr : "parse error");
        free(terr);
        free(root_dir);
        ccb_config_free(cfg);
        ccb_diag_list_free(&diags);
        ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
          watch_styles, watch_flow);
        return 2;
      }
      pv_trace_free(trace);
    }
    trc = pv_tree_run(NULL, root_dir, a->preview_scenario,
      a->preview_trace, start);
    free(root_dir);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
      watch_styles, watch_flow);
    return trc;
  }
  lrc = pv_project_load_scenario(cfg, eff_width, a->preview_ascii,
    a->preview_strict, &diags, scenario, &proj);
  if (lrc == CCB_ERR_CONFIG) {
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        "preview not applicable to this project\n",
        strlen("preview not applicable to this project\n"));
      ccb_diag_list_free(&diags);
      ccb_config_free(cfg);
      ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
        watch_styles, watch_flow);
      return rc == 0 ? 0 : 2;
    }
    printf("preview not applicable to this project\n");
    ccb_diag_list_free(&diags);
    ccb_config_free(cfg);
    ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
      watch_styles, watch_flow);
    return 0;
  }
  if (lrc != CCB_OK || proj == NULL) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    ccb_config_free(cfg);
    ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
      watch_styles, watch_flow);
    return 2;
  }
  // From here the diag list belongs to the project; the caller's list
  // was left empty by pv_project_load, so it must not be freed again.
  ccb_config_free(cfg);
  // Chunk 2: apply the trace after load (marks and argument fills
  // do not affect layout, so post-load is fine) and before
  // diagnose, so trace warnings print with the rest. A missing or
  // malformed trace file is a usage error (exit 2); a step that
  // matches nothing warns (CCB-P1023) during apply and rendering
  // continues. A scenario applied at load keeps its values: the
  // trace only fills slots still unknown.
  if (a->preview_trace != NULL) {
    char *ttext = NULL;
    size_t tlen = 0;
    char *terr = NULL;
    pv_trace *trace = NULL;
    if (ccb_read_file(a->preview_trace, &ttext, &tlen) != CCB_OK) {
      fprintf(stderr, "error: cannot read trace file '%s'\n",
        a->preview_trace);
      pv_project_free(proj);
      ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
        watch_styles, watch_flow);
      return 2;
    }
    trace = pv_trace_parse(ttext, &terr);
    free(ttext);
    if (trace == NULL) {
      fprintf(stderr, "error: invalid trace file '%s': %s\n",
        a->preview_trace, terr != NULL ? terr : "parse error");
      free(terr);
      pv_project_free(proj);
      ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
        watch_styles, watch_flow);
      return 2;
    }
    pv_trace_apply(proj, trace, &proj->diags);
    pv_trace_free(trace);
  }
  errors = pv_project_diagnose(proj);
  if (a->preview_storyboard != NULL) {
    size_t njourney = 0;
    pv_screen **journey = ccb_preview_storyboard_parse(proj,
      a->preview_storyboard, &njourney);
    int rc = 0;
    if (journey == NULL) {
      pv_project_free(proj);
      ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
        watch_styles, watch_flow);
      return 2;
    }
    rc = ccb_preview_render_storyboard(a, proj, journey, njourney,
      errors);
    free(journey);
    pv_project_free(proj);
    ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
      watch_styles, watch_flow);
    return rc;
  }
  // Screen selection: --screen overrides the positional argument.
  selected = a->preview_screen != NULL ? a->preview_screen : a->positional;
  if (selected != NULL) {
    for (i = 0; i < proj->nscreens; i++) {
      if (proj->screens[i]->name != NULL &&
          strcmp(proj->screens[i]->name, selected) == 0) {
        selected_screen = proj->screens[i];
        break;
      }
    }
    if (selected_screen == NULL) {
      fprintf(stderr, "error: no screen named '%s'\n", selected);
      fprintf(stderr, "available screens:");
      for (i = 0; i < proj->nscreens; i++) {
        fprintf(stderr, " %s",
          proj->screens[i]->name != NULL ? proj->screens[i]->name : "?");
      }
      fprintf(stderr, "\nusage: ccb preview [--screen NAME] [--walk] [screen]\n");
      pv_project_free(proj);
      ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
        watch_styles, watch_flow);
      return 1;
    }
  }
  if (a->preview_walk) {
    int wrc = 0;
    int color = 0;
    const char *term = NULL;
    if (!a->preview_ascii && getenv("NO_COLOR") == NULL) {
      term = getenv("TERM");
      color = term == NULL || strcmp(term, "dumb") != 0;
    }
    wrc = pv_walk_run(proj,
      selected_screen != NULL ? selected_screen->name : NULL,
      proj->frame_width, a->preview_ascii, color, a->preview_record);
    pv_project_free(proj);
    return wrc;
  }
  if (a->preview_watch) {
    // The loop takes proj; the caller's path copies are freed here.
    int wrc = ccb_preview_watch_loop(a, watch_toml, watch_config_dir,
      watch_styles, watch_flow, eff_width, proj, errors, selected);
    ccb_preview_watch_paths_free(watch_toml, watch_config_dir,
      watch_styles, watch_flow);
    return wrc;
  }
  if (a->preview_update_goldens) {
    char *golden_dir = ccb_preview_golden_dir(cwd);
    int rc = 0;
    if (golden_dir == NULL) {
      pv_project_free(proj);
      return 2;
    }
    rc = ccb_preview_update_goldens(proj, a->preview_ascii, golden_dir);
    free(golden_dir);
    pv_project_free(proj);
    // A write failure (2) dominates; otherwise preview errors (1)
    // still surface, matching a plain preview run.
    return rc != 0 ? rc : (errors > 0 ? 1 : 0);
  }
  // Single render, shared with the watch loop's initial render.
  {
    int rc = ccb_preview_render_once(a, proj, selected_screen, errors);
    pv_project_free(proj);
    return rc;
  }
}

// ---------- dispatch ----------

static int ccb_parse_args(int argc, char **argv, ccb_args *a) {
  memset(a, 0, sizeof(*a));
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const char *value = NULL;
    char flag[64];
    if (strncmp(arg, "--", 2) == 0) {
      const char *eq = strchr(arg, '=');
      size_t flen = eq == NULL ? strlen(arg) : (size_t)(eq - arg);
      if (flen >= sizeof(flag)) {
        return -1;
      }
      memcpy(flag, arg, flen);
      flag[flen] = '\0';
      value = eq == NULL ? NULL : eq + 1;
      if (strcmp(flag, "--json") == 0) {
        a->json = 1;
      } else if (strcmp(flag, "--verbose") == 0) {
        a->verbose = 1;
      } else if (strcmp(flag, "--dry-run") == 0) {
        a->dry_run = 1;
      } else if (strcmp(flag, "--explain") == 0) {
        a->explain = 1;
      } else if (strcmp(flag, "--yes") == 0) {
        a->yes = 1;
      } else if (strcmp(flag, "--upgrade") == 0) {
        a->upgrade = 1;
      } else if (strcmp(flag, "--print-env") == 0) {
        a->print_env = 1;
      } else if (strcmp(flag, "--aab") == 0) {
        a->aab = 1;
      } else if (strcmp(flag, "--config") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->config_path = value;
      } else if (strcmp(flag, "--variant") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->variant = value;
      } else if (strcmp(flag, "--package") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->init_package = value;
      } else if (strcmp(flag, "--name") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->init_name = value;
      } else if (strcmp(flag, "--ascii") == 0) {
        a->preview_ascii = 1;
      } else if (strcmp(flag, "--walk") == 0) {
        a->preview_walk = 1;
      } else if (strcmp(flag, "--watch") == 0) {
        a->preview_watch = 1;
      } else if (strcmp(flag, "--tree-live") == 0) {
        a->preview_tree_live = 1;
      } else if (strcmp(flag, "--strict") == 0) {
        a->preview_strict = 1;
      } else if (strcmp(flag, "--width") == 0) {
        char *end = NULL;
        long v = 0;
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        v = strtol(value, &end, 10);
        if (end == value || *end != '\0') {
          return -1;
        }
        a->preview_width = v;
        a->preview_width_set = 1;
      } else if (strcmp(flag, "--device") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        if (strcmp(value, "phone") != 0 && strcmp(value, "tablet") != 0) {
          return -1;
        }
        a->preview_device = value;
      } else if (strcmp(flag, "--output") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_output = value;
      } else if (strcmp(flag, "--update-goldens") == 0) {
        a->preview_update_goldens = 1;
      } else if (strcmp(flag, "--screen") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_screen = value;
      } else if (strcmp(flag, "--diff") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_diff = value;
      } else if (strcmp(flag, "--storyboard") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_storyboard = value;
      } else if (strcmp(flag, "--scenario") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_scenario = value;
      } else if (strcmp(flag, "--trace") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_trace = value;
      } else if (strcmp(flag, "--record") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_record = value;
      } else if (strcmp(flag, "--preview-gate") == 0) {
        a->preview_gate_on = 1;
      } else if (strcmp(flag, "--no-preview-gate") == 0) {
        a->preview_gate_off = 1;
      } else {
        return -1;
      }
    } else if (a->command == NULL) {
      a->command = arg;
    } else if (a->positional == NULL) {
      a->positional = arg;
    } else {
      return -1;
    }
  }
  return 0;
}

// ---------- lsp ----------

// `ccb lsp [--scenario NAME] [--trace FILE]`: run the preview LSP
// server on stdio for the project found by upward search. --scenario
// and --trace reuse the preview flags (parsed globally). Exit 0 on
// shutdown+exit, 1 on exit/EOF without shutdown, 2 on usage/config
// errors (unknown scenario, unreadable trace, no ccb.toml, preview
// not applicable). Other preview flags are rejected: they belong to
// the render modes, not to the server.
static int ccb_cmd_lsp(ccb_args *a) {
  char cwd[4096];
  char *path = NULL;
  char *err = NULL;
  pv_lsp_server *s = NULL;
  int rc = 0;
  if (a->preview_walk || a->preview_watch || a->preview_diff != NULL ||
      a->preview_storyboard != NULL || a->preview_screen != NULL ||
      a->preview_output != NULL || a->preview_update_goldens ||
      a->preview_record != NULL || a->preview_tree_live ||
      a->positional != NULL) {
    fprintf(stderr, "error: lsp takes only --scenario and --trace\n");
    return 2;
  }
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    fprintf(stderr, "error: cannot determine working directory\n");
    return 2;
  }
  if (ccb_config_discover(cwd, a->config_path, &path) != CCB_OK) {
    fprintf(stderr,
      "error: no ccb.toml found in this directory or any parent\n");
    return 2;
  }
  s = pv_lsp_open(path, a->preview_scenario, a->preview_trace, &err);
  free(path);
  if (s == NULL) {
    fprintf(stderr, "error: %s\n",
      err != NULL ? err : "cannot start lsp server");
    free(err);
    return 2;
  }
  rc = pv_lsp_run(s, STDIN_FILENO, STDOUT_FILENO);
  pv_lsp_close(s);
  return rc;
}

int ccb_cli_main(int argc, char **argv) {
  ccb_args a;
  if (ccb_parse_args(argc, argv, &a) != 0) {
    fputs(CCB_USAGE, stderr);
    return 2;
  }
  if (a.command == NULL) {
    fputs(CCB_USAGE, stderr);
    return 2;
  }
  if (strcmp(a.command, "init") == 0) {
    return ccb_cmd_init(&a);
  }
  if (strcmp(a.command, "setup") == 0) {
    return ccb_cmd_setup(&a);
  }
  if (strcmp(a.command, "check") == 0) {
    return ccb_cmd_check(&a);
  }
  if (strcmp(a.command, "build") == 0) {
    return ccb_cmd_build(&a);
  }
  if (strcmp(a.command, "clean") == 0) {
    return ccb_cmd_clean(&a);
  }
  if (strcmp(a.command, "doctor") == 0) {
    return ccb_cmd_doctor(&a);
  }
  if (strcmp(a.command, "install") == 0) {
    return ccb_cmd_install(&a);
  }
  if (strcmp(a.command, "run") == 0) {
    return ccb_cmd_run(&a);
  }
  if (strcmp(a.command, "logcat") == 0) {
    return ccb_cmd_logcat(&a);
  }
  if (strcmp(a.command, "preview") == 0) {
    return ccb_cmd_preview(&a);
  }
  if (strcmp(a.command, "lsp") == 0) {
    return ccb_cmd_lsp(&a);
  }
  if (strcmp(a.command, "version") == 0) {
    if (a.json) {
      printf("{\"event\":\"version\",\"version\":\"%s\"}\n", CCB_VERSION);
    } else {
      printf("ccb %s\n", CCB_VERSION);
    }
    return 0;
  }
  if (strcmp(a.command, "help") == 0) {
    fputs(CCB_USAGE, stdout);
    return 0;
  }
  fprintf(stderr, "error: unknown command '%s'\n", a.command);
  fputs(CCB_USAGE, stderr);
  return 2;
}
