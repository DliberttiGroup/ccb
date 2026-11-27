// ccb_cli_setup_host: `ccb setup` host-tool checks (Termux + distro pkgs).
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

// ---------- setup: host tools (Termux + distro packages) ----------

int ccb_env_nonempty(const char *name) {
  const char *v = getenv(name);
  return v != NULL && v[0] != '\0';
}

// 1 when the tool at path exists, is executable, and runs.
int ccb_tool_runs(const char *path, const char *arg) {
  return access(path, X_OK) == 0 && ccb_sdk_tool_runnable(path, arg);
}

// 1 when a runnable copy of the tool is on PATH.
int ccb_path_tool_runs(const char *name, const char *arg) {
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

int ccb_termux_repo_present(void) {
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
int ccb_setup_install_system_tool(const ccb_jdkinst_info *inst, const char *pkg_name, int needs_repo) {
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

// After every install step, resolve the toolchain and report what
// still needs attention (e.g. an old kotlinc on PATH shadowing the
// SDK-home install). Informational only: setup already did what it
// can; the remaining items need user action. adb is outside the
// toolchain resolve, so it is checked separately.
