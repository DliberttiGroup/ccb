// ccb_cli implementation, part 1: arg parsing, diag output, init, setup.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_json.h"
#include "ccb_diag.h"
#include "ccb_log.h"
#include "ccb_proc.h"
#include "ccb_config.h"
#include "ccb_sdk.h"
#include "ccb_pipeline.h"
#include "ccb_cli.h"
#include <unistd.h>

#define CCB_VERSION "0.1.0"

// SHA-256 of the official cmdline-tools Linux zip used by `ccb setup`
// to bootstrap an SDK. Verified against the complete download
// (commandlinetools-linux-11076708_latest.zip, 153607504 bytes).
#define CCB_CMDLINE_SHA256 "2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258"

static const char *CCB_USAGE =
  "usage: ccb [--json] [--verbose] [--config PATH] [--variant debug|release]\n"
  "           [--dry-run] [--explain] <command> [args]\n"
  "\n"
  "commands:\n"
  "  init [dir] [--package PKG] [--name NAME]  scaffold a new project\n"
  "  setup                                     install missing SDK components\n"
  "  check                                     validate config + toolchain\n"
  "  build                                     build the APK\n"
  "  clean                                     remove the build directory\n"
  "  doctor                                    report toolchain status\n"
  "  install                                   adb install the built APK\n"
  "  run                                       install + launch the app\n"
  "  logcat                                    stream device logs\n"
  "  preview                                   (M5) render a UI preview\n"
  "  version                                   print the version\n";

typedef struct {
  int json;
  int verbose;
  int dry_run;
  int explain;
  const char *config_path;
  const char *variant;
  const char *command;
  const char *positional;
  const char *init_package;
  const char *init_name;
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
// ccb_proc_run uses execve (no PATH lookup), so resolve curl first.
static int ccb_download(const char *url, const char *dest) {
  char *curl = ccb_sdk_find_on_path("curl");
  if (curl == NULL) {
    return -1;
  }
  char *argv[] = { curl, "-sL", "--fail", "-o", (char *)dest, (char *)url, NULL };
  ccb_proc_result r;
  ccb_status rc = ccb_proc_run(argv, NULL, NULL, &r);
  int ok = rc == CCB_OK && r.exit_code == 0;
  ccb_proc_result_free(&r);
  free(curl);
  return ok ? 0 : -1;
}

static int ccb_cmd_setup(ccb_args *a) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  // Config is optional for setup: defaults apply without one.
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
    if (ccb_download(url, tmp) != 0) {
      fprintf(stderr, "error: download failed: %s\n", url);
      free(tmp);
      free(default_home);
      ccb_config_free(cfg);
      return 3;
    }
    char hex[65];
    if (ccb_sha256_file(tmp, hex) != CCB_OK || strcmp(hex, CCB_CMDLINE_SHA256) != 0) {
      fprintf(stderr, "error: downloaded cmdline-tools archive failed checksum verification\n");
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
  // Accept licenses non-interactively.
  {
    // Run via the sdkmanager's own directory: replace argv[0] command.
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
    if (sh != NULL) {
      char *sh_argv[] = { sh, "-c", lic_cmd, NULL };
      ccb_proc_result lr;
      ccb_proc_run(sh_argv, NULL, NULL, &lr);
      ccb_proc_result_free(&lr);
      free(sh);
    }
    free(lic_cmd);
  }
  printf("SDK ready at %s (platform android-%ld, build-tools %s)\n", sdk_home, compile_sdk, build_tools);
  ccb_str_free(&platform);
  ccb_str_free(&bt);
  free(sdkmanager);
  free(default_home);
  ccb_config_free(cfg);
  return 0;
}

// ---------- check / build / clean / doctor ----------

static int ccb_cmd_check(ccb_args *a) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
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
    ccb_str_append(&s, ",\"kotlin\":");
    ccb_json_escape(&s, sdk->kotlin_version);
    ccb_str_appendf(&s, ",\"java\":%d}", sdk->java_major);
    printf("%s\n", s.buf);
    ccb_str_free(&s);
  } else {
    printf("ok: %s (%s, variant %s)\n", cfg->name, cfg->package, cfg->variant);
    printf("  sdk: %s\n  build-tools: %s\n  kotlinc: %s\n  java: %d\n",
      sdk->sdk_home, sdk->build_tools_version, sdk->kotlin_version, sdk->java_major);
    if (cfg->has_ndk) {
      printf("  ndk: %s\n", sdk->ndk_home);
    }
  }
  ccb_sdk_free(sdk);
  ccb_config_free(cfg);
  ccb_diag_list_free(&diags);
  return 0;
}

static int ccb_cmd_build(ccb_args *a) {
  ccb_build_opts opts;
  memset(&opts, 0, sizeof(opts));
  opts.json = a->json;
  opts.verbose = a->verbose;
  opts.dry_run = a->dry_run;
  opts.explain = a->explain;
  opts.variant_override = a->variant;
  opts.config_path = a->config_path;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
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
      ccb_str_append(&s, "{\"event\":\"build_done\",\"apk\":");
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
static char *ccb_find_adb(ccb_config *cfg) {
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
    if (access(adb, X_OK) == 0) {
      return adb;
    }
    free(adb);
  }
  return ccb_sdk_find_on_path("adb");
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
  char *adb = ccb_find_adb(cfg);
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
    ccb_diag d = ccb_diag_make("CCB-E2009", "install", "adb not found");
    d.suggestion = ccb_xstrdup("run `ccb setup` to install platform-tools, or put adb on PATH");
    ccb_diag_list_push(&diags, &d);
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
  char *adb = ccb_find_adb(cfg);
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
    ccb_diag d = ccb_diag_make("CCB-E2009", "install", "adb not found");
    d.suggestion = ccb_xstrdup("run `ccb setup` to install platform-tools, or put adb on PATH");
    ccb_diag_list_push(&diags, &d);
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
  char *adb = ccb_find_adb(cfg);
  if (cfg != NULL) {
    ccb_config_free(cfg);
  }
  if (adb == NULL) {
    fprintf(stderr, "error: adb not found (install platform-tools or put adb on PATH)\n");
    return 3;
  }
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
    fprintf(stderr, "preview is not implemented yet (M5 stretch goal)\n");
    return 1;
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
