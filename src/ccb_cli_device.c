// ccb_cli_device: `ccb install` / `ccb run` / `ccb logcat` handlers.
// Split from ccb_cli.c.

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
#include "ccb_daemon.h"
#include "ccb_sdk.h"
#include "ccb_pipeline.h"
#include "ccb_zip.h"
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
#include "ccb_cli_internal.h"

// ---------- install / run / logcat ----------

// adb lives under the SDK's platform-tools; no full toolchain resolve is
// needed just to find it (that would waste a kotlinc -version probe).
// Precedence: [sdk] adb, then CCB_ADB, then the SDK's platform-tools
// adb, then adb on PATH. An explicit override that is missing or
// cannot run pushes a diagnostic (when diags is given) and returns
// NULL instead of silently falling back. SDK and PATH candidates must
// actually run: an SDK adb can exist yet be unrunnable (x86-64 binary
// on Termux aarch64), in which case the PATH copy wins.
char *ccb_find_adb(ccb_config *cfg, ccb_diag_list *diags) {
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

int ccb_cmd_install(ccb_args *a) {
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

int ccb_cmd_run(ccb_args *a) {
  // Step 1: build. `run` always builds first so install + launch act on
  // the current sources. In dry-run mode the build prints its commands
  // and nothing is executed.
  ccb_build_opts opts;
  memset(&opts, 0, sizeof(opts));
  opts.json = a->json;
  opts.verbose = a->verbose;
  opts.dry_run = a->dry_run || a->explain;
  opts.variant_override = a->variant;
  opts.kotlinc_daemon_on = a->kotlinc_daemon_on;
  opts.kotlinc_daemon_off = a->kotlinc_daemon_off;
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

int ccb_cmd_logcat(ccb_args *a) {
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
