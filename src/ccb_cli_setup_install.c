// ccb_cli_setup_install: `ccb setup` kotlinc / R8 / NDK installers.
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
int ccb_setup_probe_kotlinc(const ccb_config *cfg, const char *sdk_home,
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
int ccb_setup_install_kotlinc(const char *sdk_home) {
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
int ccb_setup_install_r8(const char *sdk_home) {
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
int ccb_setup_install_ndk(const char *sdk_home, const char *sdkmanager, const char *version) {
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
