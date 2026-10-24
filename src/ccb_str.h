// ccb_str: growable string buffer. Always NUL-terminated.
// Ownership: the buffer is owned by the struct; free with ccb_str_free,
// or take ownership with ccb_str_steal (caller must free the result).
#ifndef CCB_STR_H
#define CCB_STR_H

#include "ccb_common.h"

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} ccb_str;

void ccb_str_init(ccb_str *s);
void ccb_str_free(ccb_str *s);
void ccb_str_append(ccb_str *s, const char *text);
void ccb_str_append_n(ccb_str *s, const char *text, size_t n);
void ccb_str_append_c(ccb_str *s, char c);
void ccb_str_appendf(ccb_str *s, const char *fmt, ...);
char *ccb_str_steal(ccb_str *s);

#endif
