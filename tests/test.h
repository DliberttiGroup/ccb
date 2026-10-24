// Minimal test harness for CCB unit tests (no external framework).
// Each test file has its own main() and ends with TEST_MAIN_END().
#ifndef CCB_TEST_H
#define CCB_TEST_H

#include <stdio.h>
#include <string.h>

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
