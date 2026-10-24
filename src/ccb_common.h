// ccb_common: shared status codes and helpers.
// Every .c file includes this header FIRST so the feature-test macros
// below apply before any system header is pulled in.
#ifndef CCB_COMMON_H
#define CCB_COMMON_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Status values. The first six map 1:1 onto the spec §6.4 exit codes;
// the rest are internal and map to exit code 1 (see ccb_exit_code).
typedef enum {
  CCB_OK = 0,
  CCB_ERR_BUILD = 1,
  CCB_ERR_CONFIG = 2,
  CCB_ERR_TOOLCHAIN = 3,
  CCB_ERR_SIGNING = 4,
  CCB_ERR_DEVICE = 5,
  CCB_ERR_IO = 6,
  CCB_ERR_USAGE = 7
} ccb_status;

static inline int ccb_exit_code(ccb_status s) {
  switch (s) {
    case CCB_OK: return 0;
    case CCB_ERR_CONFIG: return 2;
    case CCB_ERR_TOOLCHAIN: return 3;
    case CCB_ERR_SIGNING: return 4;
    case CCB_ERR_DEVICE: return 5;
    case CCB_ERR_USAGE: return 2;
    default: return 1;
  }
}

#define CCB_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CCB_UNUSED(x) ((void)(x))

#endif
