// ccb_str implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include <stdarg.h>

void ccb_str_init(ccb_str *s) {
  s->buf = NULL;
  s->len = 0;
  s->cap = 0;
}

void ccb_str_free(ccb_str *s) {
  free(s->buf);
  s->buf = NULL;
  s->len = 0;
  s->cap = 0;
}

static void ccb_str_reserve(ccb_str *s, size_t extra) {
  size_t need = 0;
  if (__builtin_add_overflow(s->len, extra, &need) ||
      __builtin_add_overflow(need, 1, &need)) {
    fprintf(stderr, "ccb: string size overflow\n");
    abort();
  }
  if (need <= s->cap) {
    return;
  }
  size_t ncap = s->cap == 0 ? 64 : s->cap;
  while (ncap < need) {
    size_t doubled = 0;
    if (__builtin_mul_overflow(ncap, 2, &doubled)) {
      ncap = need;
      break;
    }
    ncap = doubled;
  }
  s->buf = ccb_xrealloc(s->buf, ncap);
  s->cap = ncap;
}

void ccb_str_append_n(ccb_str *s, const char *text, size_t n) {
  ccb_str_reserve(s, n);
  memcpy(s->buf + s->len, text, n);
  s->len += n;
  s->buf[s->len] = '\0';
}

void ccb_str_append(ccb_str *s, const char *text) {
  ccb_str_append_n(s, text, strlen(text));
}

void ccb_str_append_c(ccb_str *s, char c) {
  ccb_str_append_n(s, &c, 1);
}

void ccb_str_appendf(ccb_str *s, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return;
  }
  ccb_str_reserve(s, (size_t)n);
  vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap2);
  va_end(ap2);
  s->len += (size_t)n;
}

char *ccb_str_steal(ccb_str *s) {
  char *p = s->buf;
  if (p == NULL) {
    p = ccb_xstrdup("");
  }
  s->buf = NULL;
  s->len = 0;
  s->cap = 0;
  return p;
}
