// Minimal test harness for CCB unit tests (no external framework).
// Each test file has its own main() and ends with TEST_MAIN_END().
#ifndef CCB_TEST_H
#define CCB_TEST_H

// First: feature-test macros for mkdtemp, before any system header.
#include "ccb_common.h"

#include <stdio.h>
#include <string.h>

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

static int ccb_test_failures = 0;
static int ccb_test_checks = 0;

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
