// Tests for the opt-in preview gate (worker W-GATE): the error
// classification helper, the enabled() switch with its flag_off >
// flag_on > config precedence, and gate_run() over temp copies of
// the fixtures under tests/fixtures/preview/. Fixture trees are
// COPIED to a mkdtemp scratch dir and loaded by absolute path, so
// the real fixtures (and the real tests/golden/ tree, which this
// test never touches) are only ever read, never written. No CLI is
// exercised here: the CLI wiring belongs to the coordinator, and
// these in-process checks need no SDK/toolchain, so there is nothing
// to skip on a toolchain-less machine.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_preview_gate.h"
#include "ccb_config.h"
#include "ccb_str.h"
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- small file helpers ----------

static const char *g_scratch = NULL;

static void test_dir(ccb_str *out) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  if (slash != NULL && slash != file) {
    ccb_str_append_n(out, file, (size_t)(slash - file));
  } else {
    ccb_str_append(out, ".");
  }
}

static int write_all(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return -1;
  }
  if (len > 0 && fwrite(data, 1, len, f) != len) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

static char *join2(const char *a, const char *b) {
  ccb_str s;
  char *out = NULL;
  ccb_str_init(&s);
  ccb_str_append(&s, a);
  ccb_str_append(&s, "/");
  ccb_str_append(&s, b);
  out = ccb_xstrdup(s.buf);
  ccb_str_free(&s);
  return out;
}

// First candidate directory (from __FILE__-relative suffixes) that
// contains the given probe file, or NULL.
static char *find_dir(const char *suffixes[], size_t n,
    const char *probe) {
  size_t i = 0;
  ccb_str dir;
  ccb_str_init(&dir);
  test_dir(&dir);
  for (i = 0; i < n; i++) {
    ccb_str path;
    FILE *f = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, probe);
    f = fopen(path.buf != NULL ? path.buf : "", "rb");
    ccb_str_free(&path);
    if (f != NULL) {
      char *out = NULL;
      fclose(f);
      ccb_str_init(&path);
      ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
      ccb_str_append(&path, suffixes[i]);
      out = ccb_xstrdup(path.buf);
      ccb_str_free(&path);
      ccb_str_free(&dir);
      return out;
    }
  }
  ccb_str_free(&dir);
  return NULL;
}

// Directory holding fixture <name> (with its trailing slash kept, as
// join2 tolerates it), or NULL when not found.
static char *fixture_src(const char *name) {
  static const char *suffixes[] = {
    "/fixtures/preview/",
    "/../tests/fixtures/preview/",
  };
  static const char *rel[] = {
    "tests/fixtures/preview/",
    "../tests/fixtures/preview/",
  };
  ccb_str probe;
  char *out = NULL;
  size_t i = 0;
  ccb_str_init(&probe);
  ccb_str_append(&probe, name);
  ccb_str_append(&probe, "/ccb.toml");
  out = find_dir(suffixes, 2, probe.buf);
  ccb_str_free(&probe);
  if (out != NULL) {
    // find_dir returns the directory CONTAINING the probe, which is
    // the fixtures parent (probe already carried the fixture name),
    // so re-attach the fixture name here.
    char *full = join2(out, name);
    free(out);
    return full;
  }
  for (i = 0; i < 2; i++) {
    ccb_str path;
    FILE *f = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, rel[i]);
    ccb_str_append(&path, name);
    ccb_str_append(&path, "/ccb.toml");
    f = fopen(path.buf != NULL ? path.buf : "", "rb");
    ccb_str_free(&path);
    if (f != NULL) {
      ccb_str base;
      fclose(f);
      ccb_str_init(&base);
      ccb_str_append(&base, rel[i]);
      ccb_str_append(&base, name);
      out = ccb_xstrdup(base.buf);
      ccb_str_free(&base);
      return out;
    }
  }
  return NULL;
}

// Recursively copy a directory tree (fixture -> scratch).
static int copy_tree(const char *src, const char *dst) {
  DIR *d = opendir(src);
  struct dirent *e = NULL;
  if (d == NULL) {
    return -1;
  }
  mkdir(dst, 0777);
  while ((e = readdir(d)) != NULL) {
    char *sp = NULL;
    char *dp = NULL;
    struct stat st;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    sp = join2(src, e->d_name);
    dp = join2(dst, e->d_name);
    if (stat(sp, &st) == 0 && S_ISDIR(st.st_mode)) {
      int rc = copy_tree(sp, dp);
      free(sp);
      free(dp);
      if (rc != 0) {
        closedir(d);
        return -1;
      }
    } else {
      char *data = ccb_test_try_read(sp);
      int rc = data == NULL ? -1 :
        write_all(dp, data, strlen(data));
      free(data);
      free(sp);
      free(dp);
      if (rc != 0) {
        closedir(d);
        return -1;
      }
    }
  }
  closedir(d);
  return 0;
}

// Copy fixture <name> to a fresh scratch subdir <dst_name>; returns
// the scratch copy's path (caller frees), or NULL on failure.
static char *copy_fixture(const char *name, const char *dst_name) {
  char *src = fixture_src(name);
  char *dst = NULL;
  int rc = -1;
  if (src == NULL) {
    return NULL;
  }
  dst = join2(g_scratch, dst_name);
  rc = copy_tree(src, dst);
  free(src);
  if (rc != 0) {
    free(dst);
    return NULL;
  }
  return dst;
}

// Insert one line (without its newline) right after the "[preview]"
// line of the copy's ccb.toml. Returns 0 on success.
static int insert_preview_line(const char *dir, const char *line) {
  char *path = join2(dir, "ccb.toml");
  char *text = ccb_test_try_read(path);
  char *hit = NULL;
  ccb_str nw;
  int rc = -1;
  if (text == NULL) {
    free(path);
    return -1;
  }
  hit = strstr(text, "[preview]\n");
  if (hit == NULL) {
    free(text);
    free(path);
    return -1;
  }
  hit += strlen("[preview]\n");
  ccb_str_init(&nw);
  ccb_str_append_n(&nw, text, (size_t)(hit - text));
  ccb_str_append(&nw, line);
  ccb_str_append(&nw, "\n");
  ccb_str_append(&nw, hit);
  rc = write_all(path, nw.buf, strlen(nw.buf));
  ccb_str_free(&nw);
  free(text);
  free(path);
  return rc;
}

// chdir into a scratch copy (saved cwd restored by leave_dir), so
// the gate runs with its working directory inside the temp copy,
// exactly like the flags tests' chdir-into-copy pattern. Loading
// still uses the absolute toml path, so project_root resolution is
// unaffected by the chdir.
static int enter_dir(const char *dir, char *saved, size_t cap) {
  if (getcwd(saved, cap) == NULL) {
    return -1;
  }
  return chdir(dir);
}

static void leave_dir(const char *saved) {
  int rc = chdir(saved);
  (void)rc;
}

static ccb_config *load_cfg(const char *dir) {
  char *toml = join2(dir, "ccb.toml");
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  ccb_diag_list_init(&diags);
  if (ccb_config_load(toml, NULL, &cfg, &diags) != CCB_OK) {
    ccb_diag_list_free(&diags);
    free(toml);
    return NULL;
  }
  ccb_diag_list_free(&diags);
  free(toml);
  return cfg;
}

static long count_lines(const char *s) {
  long n = 0;
  const char *p = NULL;
  if (s == NULL) {
    return 0;
  }
  for (p = s; *p != '\0'; p++) {
    if (*p == '\n') {
      n++;
    }
  }
  return n;
}

int main(void) {
  char scratch[64];
  ccb_test_tmpdir(scratch, sizeof scratch, "gate");
  g_scratch = scratch;

  // --- pv_diag_is_error: the shared error rule, both strict modes.
  CHECK(pv_diag_is_error("CCB-P1005", 0) == 1);
  CHECK(pv_diag_is_error("CCB-P1005", 1) == 1);
  CHECK(pv_diag_is_error("CCB-P1008", 0) == 1);
  CHECK(pv_diag_is_error("CCB-P1008", 1) == 1);
  CHECK(pv_diag_is_error("CCB-P1001", 0) == 0);
  CHECK(pv_diag_is_error("CCB-P1001", 1) == 1);
  CHECK(pv_diag_is_error("CCB-P1014", 0) == 0);
  CHECK(pv_diag_is_error("CCB-P1014", 1) == 1);
  CHECK(pv_diag_is_error("CCB-P1006", 0) == 0);
  CHECK(pv_diag_is_error("CCB-P1006", 1) == 0);
  CHECK(pv_diag_is_error("CCB-P1003", 1) == 0);
  CHECK(pv_diag_is_error("CCB-P1011", 1) == 0);
  CHECK(pv_diag_is_error(NULL, 1) == 0);
  CHECK(pv_diag_is_error("CCB-P9999", 1) == 0);

  // --- Gate on + error diagnostic: missing-target fails, naming
  // the code, with exactly one concise line. Loading with
  // gate = true also proves "gate" is an accepted [preview] key
  // (an unknown key would fail the config load).
  {
    char *dir = copy_fixture("missing-target", "gate-on");
    char saved[PATH_MAX];
    ccb_config *cfg = NULL;
    ccb_str failed;
    int rc = -1;
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(insert_preview_line(dir, "gate = true") == 0);
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        CHECK(cfg->preview_gate == 1);
        CHECK(pv_gate_enabled(cfg, 0, 0) == 1);
        ccb_str_init(&failed);
        rc = pv_gate_run(cfg, 0, 0, 0, &failed);
        CHECK(rc == 1);
        CHECK(failed.buf != NULL);
        if (failed.buf != NULL) {
          CHECK_STR_EQ(failed.buf,
            "CCB-P1005: navigation to unknown screen 'Ghost'\n");
          CHECK(count_lines(failed.buf) == 1);
        }
        ccb_str_free(&failed);
        // A NULL failed_out is allowed: the code is still computed.
        CHECK(pv_gate_run(cfg, 0, 0, 0, NULL) == 1);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
  }

  // --- Gate on + warnings only: unreachable passes, no lines.
  {
    char *dir = copy_fixture("unreachable", "gate-warn");
    char saved[PATH_MAX];
    ccb_config *cfg = NULL;
    ccb_str failed;
    int rc = -1;
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(insert_preview_line(dir, "gate = true") == 0);
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        CHECK(pv_gate_enabled(cfg, 0, 0) == 1);
        ccb_str_init(&failed);
        rc = pv_gate_run(cfg, 0, 0, 0, &failed);
        CHECK(rc == 0);
        CHECK(failed.len == 0);
        ccb_str_free(&failed);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
  }

  // --- Gate off + errors: enabled() is false, so the gate would
  // not run. gate_run() itself is NOT the decider: called directly
  // it still reports the error, which is why the CLI must consult
  // enabled() first. Also proves the default: no gate key means 0.
  {
    char *dir = copy_fixture("missing-target", "gate-off");
    char saved[PATH_MAX];
    ccb_config *cfg = NULL;
    ccb_str failed;
    int rc = -1;
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        CHECK(cfg->preview_gate == 0);
        CHECK(pv_gate_enabled(cfg, 0, 0) == 0);
        ccb_str_init(&failed);
        rc = pv_gate_run(cfg, 0, 0, 0, &failed);
        CHECK(rc == 1);
        CHECK(failed.buf != NULL);
        if (failed.buf != NULL) {
          CHECK(strstr(failed.buf, "CCB-P1005") != NULL);
        }
        ccb_str_free(&failed);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
  }

  // --- Not applicable: no [preview] section passes silently.
  {
    char *dir = copy_fixture("no-preview", "gate-na");
    char saved[PATH_MAX];
    ccb_config *cfg = NULL;
    ccb_str failed;
    int rc = -1;
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        CHECK(cfg->has_preview == 0);
        CHECK(pv_gate_enabled(cfg, 0, 0) == 0);
        ccb_str_init(&failed);
        rc = pv_gate_run(cfg, 0, 0, 0, &failed);
        CHECK(rc == 0);
        CHECK(failed.len == 0);
        ccb_str_free(&failed);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
  }

  // --- Strict promotion: the strict fixture's CCB-P1001 (unknown
  // CSS property) passes without strict and fails under strict,
  // via the CLI flag argument and via the config strict key (OR).
  {
    char *dir = copy_fixture("strict", "gate-strict");
    char saved[PATH_MAX];
    ccb_config *cfg = NULL;
    ccb_str failed;
    int rc = -1;
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        ccb_str_init(&failed);
        rc = pv_gate_run(cfg, 0, 0, 0, &failed);
        CHECK(rc == 0);
        CHECK(failed.len == 0);
        ccb_str_free(&failed);
        ccb_str_init(&failed);
        rc = pv_gate_run(cfg, 0, 0, 1, &failed);
        CHECK(rc == 1);
        CHECK(failed.buf != NULL);
        if (failed.buf != NULL) {
          CHECK(strstr(failed.buf, "CCB-P1001") != NULL);
        }
        ccb_str_free(&failed);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
    // Same fixture with strict = true in config: the strict
    // argument is 0, but config strict alone must promote.
    dir = copy_fixture("strict", "gate-strict-cfg");
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(insert_preview_line(dir, "strict = true") == 0);
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        CHECK(cfg->preview_strict == 1);
        ccb_str_init(&failed);
        rc = pv_gate_run(cfg, 0, 0, 0, &failed);
        CHECK(rc == 1);
        CHECK(failed.buf != NULL);
        if (failed.buf != NULL) {
          CHECK(strstr(failed.buf, "CCB-P1001") != NULL);
        }
        ccb_str_free(&failed);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
  }

  // --- Duplicate screen is an always-error (CCB-P1008).
  {
    char *dir = copy_fixture("duplicate-screen", "gate-dup");
    char saved[PATH_MAX];
    ccb_config *cfg = NULL;
    ccb_str failed;
    int rc = -1;
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(insert_preview_line(dir, "gate = true") == 0);
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        ccb_str_init(&failed);
        rc = pv_gate_run(cfg, 0, 0, 0, &failed);
        CHECK(rc == 1);
        CHECK(failed.buf != NULL);
        if (failed.buf != NULL) {
          CHECK(strstr(failed.buf, "CCB-P1008") != NULL);
          CHECK(strstr(failed.buf, "duplicate screen 'Home'") != NULL);
        }
        ccb_str_free(&failed);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
  }

  // --- Flag/config precedence: flag_off first, then flag_on, then
  // config. Both flags set resolves to off (flag_off wins); the CLI
  // should still reject that combination as a usage error.
  {
    char *dir = copy_fixture("missing-target", "gate-prec-on");
    char saved[PATH_MAX];
    ccb_config *cfg = NULL;
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(insert_preview_line(dir, "gate = true") == 0);
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        CHECK(pv_gate_enabled(cfg, 0, 0) == 1);
        CHECK(pv_gate_enabled(cfg, 1, 0) == 1);
        CHECK(pv_gate_enabled(cfg, 0, 1) == 0);
        CHECK(pv_gate_enabled(cfg, 1, 1) == 0);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
    dir = copy_fixture("missing-target", "gate-prec-off");
    CHECK(dir != NULL);
    if (dir != NULL) {
      CHECK(enter_dir(dir, saved, sizeof(saved)) == 0);
      cfg = load_cfg(dir);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        CHECK(pv_gate_enabled(cfg, 0, 0) == 0);
        CHECK(pv_gate_enabled(cfg, 1, 0) == 1);
        CHECK(pv_gate_enabled(cfg, 0, 1) == 0);
        CHECK(pv_gate_enabled(cfg, 1, 1) == 0);
        ccb_config_free(cfg);
      }
      leave_dir(saved);
      free(dir);
    }
    // No config at all: flags still resolve, bare default is off,
    // and gate_run on NULL config is a genuine failure (2).
    CHECK(pv_gate_enabled(NULL, 0, 0) == 0);
    CHECK(pv_gate_enabled(NULL, 1, 0) == 1);
    CHECK(pv_gate_enabled(NULL, 0, 1) == 0);
    CHECK(pv_gate_enabled(NULL, 1, 1) == 0);
    CHECK(pv_gate_run(NULL, 0, 0, 0, NULL) == 2);
  }

  TEST_MAIN_END();
}
