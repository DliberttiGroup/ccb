// Minimal test harness for CCB unit tests (no external framework).
// Each test file has its own main() and ends with TEST_MAIN_END().
#ifndef CCB_TEST_H
#define CCB_TEST_H

// First: feature-test macros for mkdtemp, before any system header.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Unique per-process scratch dir, so parallel ctest runs (-j) and two
// concurrent ctest invocations never share a path. Each test binary
// creates its own dir at the start of main() and removes it at the
// end; buf must hold at least 64 bytes. Exits on failure: without a
// scratch dir nothing meaningful can run.
static inline void ccb_test_tmpdir(char *buf, size_t n, const char *tag) {
  snprintf(buf, n, "/tmp/ccb_test_%s_XXXXXX", tag);
  if (mkdtemp(buf) == NULL) {
    fprintf(stderr, "ccb_test_tmpdir: mkdtemp failed for %s\n", tag);
    exit(1);
  }
}

// Join base + rel into one of 8 rotating static buffers, so paths can
// be built inline in expressions: B("proj/ccb.toml"). At most 8
// results are live at once in a single statement; every use in these
// tests needs 3 or fewer. Single-threaded test binaries only; the
// buffers are never freed, so no leak for LeakSanitizer to report.
static inline char *ccb_test_path(const char *base, const char *rel) {
  static char bufs[8][1024];
  static unsigned next = 0;
  char *buf = bufs[next++ & 7];
  snprintf(buf, sizeof bufs[0], "%s/%s", base, rel);
  return buf;
}

// Read a whole file, or NULL when it cannot be read. Shared by the
// preview tests; was twelve copies in three spellings (an fseek/ftell
// one, an fread-loop one, and this stat-guarded one). This is the
// stat-guarded spelling: only regular files are read. ftell on a
// directory stream returns LONG_MAX on some filesystems, and
// malloc(size + 1) then wraps and aborts, so a directory must fail
// cleanly here instead of trusting ftell. (The plain fseek spelling
// passed locally and aborted in CI on exactly this.)
static inline char *ccb_test_try_read(const char *path) {
  struct stat st;
  FILE *f = NULL;
  long size = 0;
  char *buf = NULL;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return NULL;
  }
  f = fopen(path, "rb");
  if (f == NULL) {
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  buf = ccb_xmalloc((size_t)size + 1);
  if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[size] = '\0';
  fclose(f);
  return buf;
}

// Substring check, NULL-safe. Shared by the preview tests; was
// fifteen copies in three spellings (bare strstr, hay-only NULL
// check, both NULL-checked). This is the both-NULL-checked spelling.
static inline int ccb_test_contains(const char *hay,
    const char *needle) {
  return hay != NULL && needle != NULL && strstr(hay, needle) != NULL;
}

static int ccb_test_failures = 0;
static int ccb_test_checks = 0;

// Highest build-tools version under sdk_home that has an aapt2, or
// NULL. The version string (e.g. "36.0.0") is malloc'd. Shared by the
// slow pipeline tests that need a real build-tools to build a hello
// project; was five identical copies before.
static inline char *ccb_test_find_build_tools(const char *sdk_home) {
  char *bt_dir = ccb_path_join(sdk_home, "build-tools");
  DIR *d = opendir(bt_dir);
  if (d == NULL) {
    free(bt_dir);
    return NULL;
  }
  char *best = NULL;
  struct dirent *e = NULL;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') {
      continue;
    }
    // Only build-tools CCB accepts (major 34..36): the runner image
    // ships newer ones (e.g. 37) with no matching platform, and
    // deriving compile_sdk from them fails config load (CCB-E1005).
    int major = 0;
    const char *mp = e->d_name;
    if (*mp < '0' || *mp > '9') {
      continue;
    }
    while (*mp >= '0' && *mp <= '9') {
      major = major * 10 + (*mp - '0');
      mp++;
    }
    if (major < 34 || major > 36) {
      continue;
    }
    char *cand = ccb_path_join3(bt_dir, e->d_name, "aapt2");
    int ok = access(cand, X_OK) == 0;
    free(cand);
    if (ok && (best == NULL || strcmp(e->d_name, best) > 0)) {
      free(best);
      best = ccb_xstrdup(e->d_name);
    }
  }
  closedir(d);
  free(bt_dir);
  return best;
}

#define CHECK(cond) do { \
    ++ccb_test_checks; \
    if (!(cond)) { \
      ++ccb_test_failures; \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
  } while (0)

#define CHECK_STR_EQ(a, b) do { \
    ++ccb_test_checks; \
    const char *_a = (a); \
    const char *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
      ++ccb_test_failures; \
      fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); \
    } \
  } while (0)

#define TEST_MAIN_END() do { \
    fprintf(stderr, "%d checks, %d failures\n", ccb_test_checks, ccb_test_failures); \
    return ccb_test_failures ? 1 : 0; \
  } while (0)

#endif
