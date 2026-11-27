// ccb_cli_setup_run: `ccb setup` orchestration (the setup driver).
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

// Shared state for the `setup` steps. The orchestrator owns the
// cleanup: every step frees only its own locals and reports an exit
// code, so a step never frees ctx fields. sdk_home and build_tools
// are borrowed (config, env, or default_home); default_home and
// sdkmanager are owned.

// Whether to install OpenJDK 17: 1 installs, 0 declines (the caller
// reports the failure). Prints the prompt or the non-interactive
// error itself.
static int ccb_setup_java_decide(ccb_args *a, const ccb_jdkinst_info *inst) {
  if (inst->kind == CCB_JDKINST_NONE) {
    fprintf(stderr,
      "error: java not found: `ccb setup` runs sdkmanager, which needs a JDK 17+.\n"
      "No supported package installer found (Termux pkg, apt-get, dnf, yum, pacman, zypper, apk).\n"
      "Install a JDK 17+ manually and set JAVA_HOME, or put java 17+ on PATH.\n");
    return 0;
  }
  if (a->yes || a->upgrade) {
    return 1;
  }
  if (isatty(STDIN_FILENO)) {
    ccb_str q;
    int go = 0;
    ccb_str_init(&q);
    ccb_str_append(&q, "Java 17+ not found. Install OpenJDK 17 via ");
    ccb_str_append(&q, inst->display);
    ccb_str_append(&q, "?");
    go = ccb_prompt_yes(q.buf);
    ccb_str_free(&q);
    if (!go) {
      fprintf(stderr, "setup aborted: Java 17+ is required.\n");
    }
    return go;
  }
  fprintf(stderr,
    "error: java not found: `ccb setup` runs sdkmanager, which needs a JDK 17+.\n"
    "Run `ccb setup` in a terminal to install OpenJDK 17 via %s, or pass --yes to install automatically.\n",
    inst->display);
  return 0;
}

// A JDK 17+ must exist before sdkmanager can run: sdkmanager is a
// Java program. When no usable JDK is present, offer to install one
// (Termux pkg or the distro package manager) instead of failing at
// the sdkmanager step with a generic error. Returns 0 when a usable
// JDK is configured, 3 otherwise.
static int ccb_setup_ensure_java(ccb_args *a) {
  char *java = NULL;
  int java_major = 0;
  if (ccb_sdk_find_java(&java, &java_major) == CCB_OK) {
    ccb_configure_java_home(java);
    free(java);
    return 0;
  }
  {
    ccb_jdkinst_info inst;
    ccb_detect_jdkinst(&inst);
    if (!ccb_setup_java_decide(a, &inst)) {
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
    ccb_configure_java_home(java);
    free(java);
    return 0;
  }
}

// Loads the project config. Defaults apply when no ccb.toml exists.
// But a config that was found (or explicitly requested) and failed
// to load is an error: silently falling back to the default SDK home
// would provision the wrong SDK. Returns 0 with *cfg_out set
// (possibly NULL), 2 on a load error.
static int ccb_setup_load_config(ccb_args *a, ccb_config **cfg_out) {
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  ccb_diag_list_init(&diags);
  cfg = ccb_load_config_or_die(a, &diags);
  if (cfg != NULL) {
    ccb_diag_list_free(&diags);
    *cfg_out = cfg;
    return 0;
  }
  {
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
    if (!found) {
      // No config anywhere: defaults apply.
      ccb_diag_list_free(&diags);
      *cfg_out = NULL;
      return 0;
    }
  }
  ccb_print_diags(a, &diags);
  ccb_diag_list_free(&diags);
  return 2;
}

// Resolves the SDK home: config, then ANDROID_HOME /
// ANDROID_SDK_ROOT, then ~/Android/Sdk. Returns 0, or 3 when no
// location can be determined.
static int ccb_setup_resolve_sdk_home(ccb_setup_ctx *ctx) {
  const char *sdk_home = ctx->cfg != NULL ? ctx->cfg->sdk_home : NULL;
  if (sdk_home == NULL) {
    sdk_home = getenv("ANDROID_HOME");
  }
  if (sdk_home == NULL) {
    sdk_home = getenv("ANDROID_SDK_ROOT");
  }
  if (sdk_home == NULL) {
    const char *home = getenv("HOME");
    if (home == NULL) {
      fprintf(stderr, "error: cannot determine an SDK location (no HOME, no ANDROID_HOME)\n");
      return 3;
    }
    ctx->default_home = ccb_path_join(home, "Android/Sdk");
    sdk_home = ctx->default_home;
  }
  ctx->sdk_home = sdk_home;
  return 0;
}

// Downloads and installs cmdline-tools when sdkmanager is missing.
// Sets ctx->sdkmanager. Returns 0, or 3 on failure.
static int ccb_setup_bootstrap_sdkmanager(ccb_setup_ctx *ctx) {
  const char *sdk_home = ctx->sdk_home;
  char *tmp = NULL;
  char *unzip_dir = NULL;
  char *unzip = NULL;
  // Bootstrap: download the official cmdline-tools archive.
  fprintf(stderr, "cmdline-tools not found, downloading the official archive...\n");
  if (ccb_mkdir_p(sdk_home) != CCB_OK) {
    fprintf(stderr, "error: cannot create SDK directory %s\n", sdk_home);
    return 3;
  }
  tmp = ccb_path_join(sdk_home, "cmdline-tools.zip");
  {
    const char *url = "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip";
    int drc = ccb_download_cached(url, tmp, CCB_CMDLINE_SHA256);
    if (drc != 0) {
      fprintf(stderr, "error: %s: %s\n",
        drc == -2 ? "downloaded cmdline-tools archive failed checksum verification" : "download failed",
        url);
      remove(tmp);
      free(tmp);
      return 3;
    }
  }
  unzip_dir = ccb_path_join(sdk_home, "cmdline-tools-tmp");
  ccb_remove_recursive(unzip_dir);
  if (ccb_mkdir_p(unzip_dir) != CCB_OK) {
    free(unzip_dir);
    free(tmp);
    return 3;
  }
  unzip = ccb_sdk_find_on_path("unzip");
  {
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
      return 3;
    }
  }
  {
    char *src = ccb_path_join(unzip_dir, "cmdline-tools");
    char *dst = ccb_path_join(sdk_home, "cmdline-tools/latest");
    char *dst_parent = ccb_dirname(dst);
    if (ccb_mkdir_p(dst_parent) != CCB_OK) {
      free(dst_parent);
      free(src);
      free(dst);
      free(unzip_dir);
      return 3;
    }
    free(dst_parent);
    if (rename(src, dst) != 0) {
      fprintf(stderr, "error: cannot install cmdline-tools into %s\n", dst);
      free(src);
      free(dst);
      free(unzip_dir);
      return 3;
    }
    free(src);
    free(dst);
  }
  ccb_remove_recursive(unzip_dir);
  free(unzip_dir);
  ctx->sdkmanager = ccb_find_sdkmanager(sdk_home);
  if (ctx->sdkmanager == NULL) {
    fprintf(stderr, "error: cmdline-tools installed but sdkmanager not found\n");
    return 3;
  }
  return 0;
}

// Accepts the SDK licenses before installing. A licenses failure is
// only a warning here: the post-install verification is the arbiter
// of success.
static void ccb_setup_accept_licenses(ccb_setup_ctx *ctx) {
  char *lic_cmd = NULL;
  char *sh = NULL;
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "yes | \"");
    ccb_str_append(&s, ctx->sdkmanager);
    ccb_str_append(&s, "\" --licenses >/dev/null");
    lic_cmd = ccb_str_steal(&s);
  }
  // execve does no PATH lookup, so resolve sh explicitly.
  sh = ccb_sdk_find_on_path("sh");
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

// Installs the components this project needs via sdkmanager, then
// verifies they actually landed: an install that silently installed
// nothing must not print "SDK ready". Returns 0, or 3.
static int ccb_setup_install_components(ccb_setup_ctx *ctx) {
  ccb_str_append(&ctx->platform, "platforms;android-");
  ccb_str_appendf(&ctx->platform, "%ld", ctx->compile_sdk);
  ccb_str_append(&ctx->bt, "build-tools;");
  ccb_str_append(&ctx->bt, ctx->build_tools);
  {
    char *components[] = { "platform-tools", ctx->platform.buf, ctx->bt.buf, NULL };
    char *argv[] = { ctx->sdkmanager, components[0], components[1], components[2], NULL };
    ccb_proc_result r;
    ccb_status prc = ccb_proc_run(argv, NULL, NULL, &r);
    if (prc != CCB_OK || r.exit_code != 0) {
      fprintf(stderr, "error: sdkmanager failed to install components\n%s\n", r.err);
      fprintf(stderr,
        "note: sdkmanager needs network access to Google's repository. It has no flags for\n"
        "authenticated proxies, so behind an authenticated egress proxy it cannot fetch;\n"
        "in that case provision the SDK by direct download instead (see docs/setup.md).\n");
      ccb_proc_result_free(&r);
      return 3;
    }
    ccb_proc_result_free(&r);
  }
  {
    const char *missing[3];
    int nmissing = ccb_setup_missing_components(ctx->sdk_home, ctx->compile_sdk, ctx->build_tools, missing);
    if (nmissing > 0) {
      fprintf(stderr, "error: ccb setup finished but the following components are still missing:");
      for (int i = 0; i < nmissing; i++) {
        fprintf(stderr, " %s", missing[i]);
      }
      fprintf(stderr, "\nCheck the sdkmanager output above, then run `ccb setup` again.\n");
      return 3;
    }
  }
  return 0;
}

// Upgrades the setup-managed SDK-home kotlinc when --upgrade is set
// and it is not the pinned version. Sets *upgraded. Returns 0, or 3
// when the reinstall fails.
static int ccb_setup_maybe_upgrade_kotlinc(ccb_setup_ctx *ctx, int *upgraded) {
  char *home_bin = ccb_path_join3(ctx->sdk_home, "kotlinc", "bin/kotlinc");
  char *ver = NULL;
  *upgraded = 0;
  if (!ctx->a->upgrade) {
    free(home_bin);
    return 0;
  }
  // Upgrade only the setup-managed copy: when the SDK-home
  // kotlinc exists and is not the pinned version, reinstall it.
  // A user-managed kotlinc (config, KOTLIN_HOME, PATH) is never
  // touched, and SDK component versions stay pinned by config.
  if (access(home_bin, X_OK) != 0) {
    free(home_bin);
    return 0;
  }
  {
    char *argv[] = { home_bin, "-version", NULL };
    ccb_proc_result vr;
    if (ccb_proc_run(argv, NULL, NULL, &vr) == CCB_OK) {
      ver = ccb_sdk_parse_kotlin_version(vr.err);
    }
    ccb_proc_result_free(&vr);
  }
  if (ver != NULL && strcmp(ver, CCB_KOTLIN_VERSION) == 0) {
    free(ver);
    free(home_bin);
    return 0;
  }
  fprintf(stderr, "upgrading SDK-home kotlinc %s -> %s\n",
    ver != NULL ? ver : "unversioned", CCB_KOTLIN_VERSION);
  if (ccb_setup_install_kotlinc(ctx->sdk_home) != 0) {
    free(ver);
    free(home_bin);
    return 3;
  }
  free(ver);
  free(home_bin);
  *upgraded = 1;
  return 0;
}

// Kotlin: when no usable kotlinc 2.x resolves, install the official
// compiler into the SDK home, where the resolver picks it up as a
// last resort (after [sdk] kotlin_home, KOTLIN_HOME, and PATH).
// Automatic, like the SDK component install: the destination is
// CCB's own directory, not a system package. Returns 0, or 3.
static int ccb_setup_ensure_kotlinc(ccb_setup_ctx *ctx) {
  int upgraded = 0;
  if (ccb_setup_maybe_upgrade_kotlinc(ctx, &upgraded) != 0) {
    return 3;
  }
  if (upgraded) {
    return 0;
  }
  {
    char *kpath = NULL;
    char *kver = NULL;
    int kprobe = ccb_setup_probe_kotlinc(ctx->cfg, ctx->sdk_home, &kpath, &kver);
    if (kprobe == 1) {
      fprintf(stderr, "kotlinc %s found at %s\n", kver, kpath);
    } else {
      if (kprobe == -1) {
        fprintf(stderr,
          "warning: found kotlinc %s at %s, which is not 2.x; installing %s into the SDK home.\n"
          "Builds will keep picking the old one until it is removed or [sdk] kotlin_home / KOTLIN_HOME points at %s/kotlinc.\n",
          kver != NULL ? kver : "unversioned", kpath, CCB_KOTLIN_VERSION, ctx->sdk_home);
      }
      if (ccb_setup_install_kotlinc(ctx->sdk_home) != 0) {
        free(kpath);
        free(kver);
        return 3;
      }
    }
    free(kpath);
    free(kver);
  }
  return 0;
}

// NDK: a project declaring [ndk] needs its revision installed, or
// check fails after setup says ready. Returns 0, or 3.
static int ccb_setup_ensure_ndk(ccb_setup_ctx *ctx) {
  if (ctx->cfg == NULL || !ctx->cfg->has_ndk || ctx->cfg->ndk_version == NULL) {
    return 0;
  }
  {
    char *ndk_dir = ccb_path_join3(ctx->sdk_home, "ndk", ctx->cfg->ndk_version);
    if (!ccb_path_is_dir(ndk_dir)) {
      fprintf(stderr, "ndk %s not found, installing via sdkmanager...\n", ctx->cfg->ndk_version);
      if (ccb_setup_install_ndk(ctx->sdk_home, ctx->sdkmanager, ctx->cfg->ndk_version) != 0) {
        free(ndk_dir);
        return 3;
      }
    } else {
      fprintf(stderr, "ndk %s found at %s\n", ctx->cfg->ndk_version, ndk_dir);
    }
    free(ndk_dir);
  }
  return 0;
}

// r8: only for minify projects without an explicit jar. The
// setup-managed copy lives at <sdk-home>/r8/r8.jar, where the
// build picks it up. Returns 0, or 3.
static int ccb_setup_ensure_r8(ccb_setup_ctx *ctx) {
  if (ctx->cfg == NULL || !ctx->cfg->minify || ctx->cfg->r8_jar != NULL) {
    return 0;
  }
  {
    char *r8_path = ccb_path_join3(ctx->sdk_home, "r8", "r8.jar");
    int need = !ccb_path_exists(r8_path);
    if (!need && ctx->a->upgrade && getenv("CCB_R8_URL") == NULL && getenv("CCB_R8_SHA256") == NULL) {
      char hex[65];
      need = ccb_sha256_file(r8_path, hex) != CCB_OK || strcmp(hex, CCB_R8_SHA256) != 0;
    }
    if (need) {
      if (ccb_setup_install_r8(ctx->sdk_home) != 0) {
        free(r8_path);
        return 3;
      }
    } else {
      fprintf(stderr, "r8 found at %s\n", r8_path);
    }
    free(r8_path);
  }
  return 0;
}

// One host tool that may need installing from the platform package
// manager (aapt2, zipalign, or adb).

// Works out which host tools still need installing, honouring
// overrides (config or CCB_* env) and the runnable checks.
static void ccb_setup_host_tools_needed(ccb_setup_ctx *ctx, ccb_host_tool tools[3]) {
  int ov_aapt2 = (ctx->cfg != NULL && ctx->cfg->aapt2_override != NULL) || ccb_env_nonempty("CCB_AAPT2");
  int ov_zipalign = (ctx->cfg != NULL && ctx->cfg->zipalign_override != NULL) || ccb_env_nonempty("CCB_ZIPALIGN");
  int ov_adb = (ctx->cfg != NULL && ctx->cfg->adb_override != NULL) || ccb_env_nonempty("CCB_ADB");
  char *bt_dir = ccb_path_join3(ctx->sdk_home, "build-tools", ctx->build_tools);
  char *sdk_aapt2 = ccb_path_join(bt_dir, "aapt2");
  char *sdk_zipalign = ccb_path_join(bt_dir, "zipalign");
  char *sdk_adb = ccb_path_join(ctx->sdk_home, "platform-tools/adb");
  int need_aapt2 = 0;
  int need_zipalign = 0;
  int need_adb = 0;
  free(bt_dir);
  need_aapt2 = !ov_aapt2 && !ccb_tool_runs(sdk_aapt2, "version") && !ccb_path_tool_runs("aapt2", "version");
  need_zipalign = !ov_zipalign && !ccb_tool_runs(sdk_zipalign, NULL) && !ccb_path_tool_runs("zipalign", NULL);
  need_adb = !ov_adb && !ccb_tool_runs(sdk_adb, "version") && !ccb_path_tool_runs("adb", "version");
  free(sdk_aapt2);
  free(sdk_zipalign);
  free(sdk_adb);
  tools[0] = (ccb_host_tool){ "aapt2", "aapt2", "version", 0, need_aapt2 };
  tools[1] = (ccb_host_tool){ "zipalign", "zipalign", NULL, 1, need_zipalign };
  tools[2] = (ccb_host_tool){ "adb", "android-tools", "version", 0, need_adb };
}

// Asks whether to install the missing host tools: 1 installs, 0
// skips. --yes / --upgrade installs without asking; without a
// terminal it only notes the option.
static int ccb_setup_host_tools_decide(ccb_args *a, const ccb_jdkinst_info *inst, const ccb_host_tool tools[3]) {
  if (a->yes || a->upgrade) {
    return 1;
  }
  if (!isatty(STDIN_FILENO)) {
    fprintf(stderr, "note: run `ccb setup` in a terminal or pass --yes to install the missing host tools.\n");
    return 0;
  }
  {
    ccb_str q;
    int go = 0;
    int first = 1;
    ccb_str_init(&q);
    ccb_str_append(&q, "Install missing host tools via ");
    ccb_str_append(&q, inst->display);
    ccb_str_append(&q, " (");
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
    if (inst->kind == CCB_JDKINST_TERMUX && tools[1].needed && !ccb_termux_repo_present()) {
      ccb_str_append(&q, " zipalign is not in the official Termux repos, so this adds the rendiix repository (https://rendiix.github.io) for it.");
    }
    go = ccb_prompt_yes(q.buf);
    ccb_str_free(&q);
    if (!go) {
      fprintf(stderr, "skipping host tool installs; the final check below lists what is still missing.\n");
    }
    return go;
  }
}

// Installs one missing host tool and verifies it runs. Returns 0,
// or 3 on failure.
static int ccb_setup_install_host_tool(const ccb_jdkinst_info *inst, const ccb_host_tool *tool) {
  fprintf(stderr, "Installing %s via %s...\n", tool->tool, inst->display);
  if (ccb_setup_install_system_tool(inst, tool->pkg, tool->needs_repo) != 0) {
    fprintf(stderr, "error: failed to install %s via %s.\n", tool->tool, inst->display);
    return 3;
  }
  if (!ccb_path_tool_runs(tool->tool, tool->arg)) {
    fprintf(stderr, "error: installed %s via %s but no runnable %s found on PATH.\n", tool->tool, inst->display, tool->tool);
    return 3;
  }
  return 0;
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
// Returns 0, or 3.
static int ccb_setup_install_host_tools(ccb_setup_ctx *ctx) {
  ccb_host_tool tools[3];
  ccb_jdkinst_info inst;
  int any = 0;
  int go = 0;
  ccb_setup_host_tools_needed(ctx, tools);
  ccb_detect_jdkinst(&inst);
  if (inst.kind != CCB_JDKINST_TERMUX) {
    // Linux: only adb has a package fix.
    tools[0].needed = 0;
    tools[1].needed = 0;
    if (inst.kind == CCB_JDKINST_APT) {
      tools[2].pkg = "adb";
    }
  }
  any = tools[0].needed || tools[1].needed || tools[2].needed;
  if (!any || inst.kind == CCB_JDKINST_NONE) {
    free(inst.path);
    free(inst.sudo_path);
    return 0;
  }
  go = ccb_setup_host_tools_decide(ctx->a, &inst, tools);
  if (go) {
    for (int i = 0; i < 3; i++) {
      if (!tools[i].needed) {
        continue;
      }
      if (ccb_setup_install_host_tool(&inst, &tools[i]) != 0) {
        free(inst.path);
        free(inst.sudo_path);
        return 3;
      }
    }
  }
  free(inst.path);
  free(inst.sudo_path);
  return 0;
}

// Maven prefetch: the one network step setup owns for dependencies.
// Builds never download; a project that declares [deps] maven gets
// its artifacts into the local cache here, and a failure stops setup
// instead of leaving a half-prefetched cache behind as "ready".
// Returns 0, or 3.
static int ccb_setup_prefetch_maven(ccb_args *a, ccb_config *cfg) {
  ccb_diag_list diags;
  ccb_maven_result mv;
  if (cfg == NULL || cfg->maven_count <= 0) {
    return 0;
  }
  ccb_diag_list_init(&diags);
  if (ccb_maven_resolve(cfg, 1, a->verbose, 0, &mv, &diags) != CCB_OK) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
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
    return 3;
  }
  fprintf(stderr, "maven: lockfile written to ccb.lock\n");
  ccb_maven_result_free(&mv);
  return 0;
}

// Compiler-plugin prefetch: the same network step setup owns for
// [build] kotlin_plugins. Plugin jars are not in ccb.lock (the exact
// coordinate in ccb.toml pins them); a failure stops setup like a
// Maven prefetch failure. Returns 0, or 3.
static int ccb_setup_prefetch_plugins(ccb_args *a, ccb_config *cfg) {
  ccb_diag_list diags;
  char **jars = NULL;
  size_t count = 0;
  if (cfg == NULL || cfg->kotlin_plugins_count <= 0) {
    return 0;
  }
  ccb_diag_list_init(&diags);
  if (ccb_maven_resolve_plugins(cfg, 1, a->verbose, 0, &jars, &count,
      &diags) != CCB_OK) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 3;
  }
  fprintf(stderr, "maven: prefetched %zu compiler plugin jars\n", count);
  ccb_free_str_array(jars, count);
  ccb_diag_list_free(&diags);
  return 0;
}

// Prints the "SDK ready" line, plus the env exports with
// --print-env.
static void ccb_setup_print_java_home(void) {
  const char *jh = getenv("JAVA_HOME");
  char *java = NULL;
  int major = 0;
  char *derived = NULL;
  if (jh != NULL) {
    printf("export JAVA_HOME=\"%s\"\n", jh);
    return;
  }
  if (ccb_sdk_find_java(&java, &major) != CCB_OK) {
    return;
  }
  derived = ccb_derive_java_home(java);
  if (derived != NULL) {
    printf("export JAVA_HOME=\"%s\"\n", derived);
    free(derived);
  }
  free(java);
}

static void ccb_setup_print_ready(ccb_args *a, ccb_setup_ctx *ctx) {
  if (a->print_env) {
    printf("export ANDROID_HOME=\"%s\"\n", ctx->sdk_home);
    ccb_setup_print_java_home();
    {
      char *kbin = ccb_path_join3(ctx->sdk_home, "kotlinc", "bin");
      if (ccb_path_is_dir(kbin)) {
        printf("export PATH=\"%s:$PATH\"\n", kbin);
      }
      free(kbin);
    }
    fprintf(stderr, "SDK ready at %s (platform android-%ld, build-tools %s)\n", ctx->sdk_home, ctx->compile_sdk, ctx->build_tools);
  } else {
    printf("SDK ready at %s (platform android-%ld, build-tools %s)\n", ctx->sdk_home, ctx->compile_sdk, ctx->build_tools);
  }
}

int ccb_cmd_setup(ccb_args *a) {
  ccb_setup_ctx ctx;
  int rc = 0;
  int missing = 0;
  memset(&ctx, 0, sizeof(ctx));
  ctx.a = a;
  ctx.compile_sdk = 35;
  ctx.build_tools = "35.0.0";
  ccb_str_init(&ctx.platform);
  ccb_str_init(&ctx.bt);
  if (ccb_setup_ensure_java(a) != 0) {
    rc = 3;
    goto out;
  }
  rc = ccb_setup_load_config(a, &ctx.cfg);
  if (rc != 0) {
    goto out;
  }
  if (ctx.cfg != NULL) {
    ctx.compile_sdk = ctx.cfg->compile_sdk;
    ctx.build_tools = ctx.cfg->build_tools;
  }
  if (ccb_setup_resolve_sdk_home(&ctx) != 0) {
    rc = 3;
    goto out;
  }
  ctx.sdkmanager = ccb_find_sdkmanager(ctx.sdk_home);
  if (ctx.sdkmanager == NULL && ccb_setup_bootstrap_sdkmanager(&ctx) != 0) {
    rc = 3;
    goto out;
  }
  // Skip sdkmanager entirely when the components are already
  // present (bug-setup): behind an authenticated egress proxy
  // sdkmanager cannot fetch at all, so running it can only fail.
  // The licenses step is skipped with it — there is nothing to
  // install, so there is nothing to license.
  missing = ccb_setup_missing_components(ctx.sdk_home, ctx.compile_sdk, ctx.build_tools, NULL);
  if (missing == 0) {
    fprintf(stderr, "SDK components already present, skipping sdkmanager\n");
  }
  if (missing > 0) {
    // Accept licenses BEFORE installing: on a fresh SDK an install
    // attempted before license acceptance installs nothing, and the
    // old order then reported success anyway (see the verification
    // below). A licenses failure is only a warning here: the
    // post-install verification is the arbiter of success.
    ccb_setup_accept_licenses(&ctx);
    // Install the components this project needs.
    if (ccb_setup_install_components(&ctx) != 0) {
      rc = 3;
      goto out;
    }
  }
  if (ccb_setup_ensure_kotlinc(&ctx) != 0) {
    rc = 3;
    goto out;
  }
  if (ccb_setup_ensure_ndk(&ctx) != 0) {
    rc = 3;
    goto out;
  }
  if (ccb_setup_ensure_r8(&ctx) != 0) {
    rc = 3;
    goto out;
  }
  if (ccb_setup_install_host_tools(&ctx) != 0) {
    rc = 3;
    goto out;
  }
  if (ccb_setup_prefetch_maven(a, ctx.cfg) != 0) {
    rc = 3;
    goto out;
  }
  if (ccb_setup_prefetch_plugins(a, ctx.cfg) != 0) {
    rc = 3;
    goto out;
  }
  ccb_setup_final_pass(ctx.cfg, ctx.compile_sdk, ctx.build_tools);
  ccb_setup_print_ready(a, &ctx);
out:
  ccb_str_free(&ctx.platform);
  ccb_str_free(&ctx.bt);
  free(ctx.sdkmanager);
  free(ctx.default_home);
  ccb_config_free(ctx.cfg);
  return rc;
}
