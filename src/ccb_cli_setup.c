// ccb_cli_setup: `ccb setup` downloads + JDK detection/install.
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

// ---------- setup ----------

char *ccb_find_sdkmanager(const char *sdk_home) {
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

// Count the SDK components setup would install that are missing
// from disk (platform-tools, the platform, build-tools). When
// names is not NULL it is filled with the missing component names.
// Shared by the pre-install skip and the post-install verification.
int ccb_setup_missing_components(const char *sdk_home, long compile_sdk,
    const char *build_tools, const char **names) {
  int n = 0;
  char *pt = ccb_path_join(sdk_home, "platform-tools");
  if (!ccb_path_exists(pt)) {
    if (names != NULL) {
      names[n] = "platform-tools";
    }
    n++;
  }
  free(pt);
  ccb_str ps;
  ccb_str_init(&ps);
  ccb_str_append(&ps, "platforms/android-");
  ccb_str_appendf(&ps, "%ld", compile_sdk);
  char *plat = ccb_path_join(sdk_home, ps.buf);
  ccb_str_free(&ps);
  if (!ccb_path_exists(plat)) {
    if (names != NULL) {
      names[n] = "platform";
    }
    n++;
  }
  free(plat);
  char *btd = ccb_path_join3(sdk_home, "build-tools", build_tools);
  if (!ccb_path_exists(btd)) {
    if (names != NULL) {
      names[n] = "build-tools";
    }
    n++;
  }
  free(btd);
  return n;
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
int ccb_download_cached(const char *url, const char *dest, const char *expected_sha) {
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

void ccb_detect_jdkinst(ccb_jdkinst_info *out) {
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
int ccb_run_jdkinst_cmd(const ccb_jdkinst_info *inst, char *const cmd[]) {
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

int ccb_install_jdk(const ccb_jdkinst_info *inst) {
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
int ccb_prompt_yes(const char *question) {
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
char *ccb_derive_java_home(const char *java_path) {
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
void ccb_configure_java_home(const char *java_path) {
  if (getenv("JAVA_HOME") != NULL) {
    return;
  }
  char *home = ccb_derive_java_home(java_path);
  if (home != NULL) {
    setenv("JAVA_HOME", home, 1);
    free(home);
  }
}
