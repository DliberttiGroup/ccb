// ccb_cli_daemon: `ccb daemon` / `ccb clean` / `ccb doctor` handlers.
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

// ---------- daemon ----------

// Stop the project's kotlinc daemon. Returns 1 when a daemon was
// stopped, 0 when none was running, -1 when the stop could not even
// run (missing jars, spawn failure, daemon unreachable). The stop
// client itself exits 0 in both the stopped and the none-running
// cases, so the outcome comes from its output, not its exit code.
static int ccb_daemon_stop(const ccb_config *cfg, const ccb_sdk *sdk) {
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  char *run_dir = ccb_daemon_run_dir(ar, cfg->build_dir);
  char **argv = ccb_daemon_stop_argv(ar, sdk->java, sdk->kotlin_home,
    run_dir);
  if (argv == NULL) {
    return -1;
  }
  ccb_proc_result r;
  if (ccb_proc_run(argv, NULL, NULL, &r) != CCB_OK) {
    return -1;
  }
  int stopped = strstr(r.out, "Daemon shut down successfully") != NULL ||
    strstr(r.err, "Daemon shut down successfully") != NULL;
  int none = strstr(r.out, "No daemon found to shut down") != NULL ||
    strstr(r.err, "No daemon found to shut down") != NULL;
  ccb_proc_result_free(&r);
  if (stopped) {
    return 1;
  }
  if (none) {
    return 0;
  }
  return -1;
}

// `ccb daemon stop`: stop the kotlinc daemon for this project.
// Exit 0 when a daemon was stopped or none was running, 1 when the
// stop itself failed, 2 on config errors, 3 on toolchain errors.
int ccb_cmd_daemon(ccb_args *a) {
  if (a->positional == NULL || strcmp(a->positional, "stop") != 0) {
    fprintf(stderr, "error: unknown daemon command '%s'\n",
      a->positional != NULL ? a->positional : "");
    fputs(CCB_USAGE, stderr);
    return 2;
  }
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
  int rc = ccb_daemon_stop(cfg, sdk);
  ccb_sdk_free(sdk);
  ccb_config_free(cfg);
  ccb_diag_list_free(&diags);
  if (rc < 0) {
    fprintf(stderr, "error: could not stop the kotlinc daemon\n");
    return 1;
  }
  if (a->json) {
    printf("{\"event\":\"daemon_stop\",\"stopped\":%s}\n",
      rc == 1 ? "true" : "false");
  } else if (rc == 1) {
    printf("kotlinc daemon stopped\n");
  } else {
    printf("no kotlinc daemon running\n");
  }
  return 0;
}

int ccb_cmd_clean(ccb_args *a) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  char *build_dir = NULL;
  if (cfg != NULL) {
    build_dir = ccb_xstrdup(cfg->build_dir);
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
    if (cfg != NULL) {
      ccb_config_free(cfg);
    }
    return 0;
  }
  // A daemon-enabled project owns a daemon in its build dir: stop it
  // first, so `clean` leaves no background process behind. Best
  // effort only — a daemon that cannot be reached still shuts itself
  // down on idle, and clean must never fail because of it. Skipped
  // entirely when the daemon was never enabled.
  if (cfg != NULL && cfg->kotlinc_daemon) {
    ccb_diag_list sdiags;
    ccb_diag_list_init(&sdiags);
    ccb_sdk *sdk = NULL;
    if (ccb_sdk_resolve(cfg, &sdk, &sdiags) == CCB_OK) {
      ccb_daemon_stop(cfg, sdk);
      ccb_sdk_free(sdk);
    }
    ccb_diag_list_free(&sdiags);
  }
  if (cfg != NULL) {
    ccb_config_free(cfg);
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

int ccb_cmd_doctor(ccb_args *a) {
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
      if (sdk->kotlin_version[0] == '\0') {
        ccb_str_append(&s, ",\"kotlin\":null");
      } else {
        ccb_str_append(&s, ",\"kotlin\":");
        ccb_json_escape(&s, sdk->kotlin_version);
      }
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
      printf("kotlinc: %s (%s)\n",
        sdk->kotlin_version[0] == '\0' ? "none" : sdk->kotlin_version,
        sdk->kotlinc == NULL ? "none" : sdk->kotlinc);
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
