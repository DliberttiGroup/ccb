// ccb_slice implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_slice.h"
#include <ctype.h>

bool ccb_slice_eq(ccb_slice a, const char *b) {
  size_t n = strlen(b);
  return a.len == n && (n == 0 || memcmp(a.ptr, b, n) == 0);
}

bool ccb_slice_eq_slice(ccb_slice a, ccb_slice b) {
  return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

bool ccb_slice_starts_with(ccb_slice a, const char *prefix) {
  size_t n = strlen(prefix);
  return a.len >= n && (n == 0 || memcmp(a.ptr, prefix, n) == 0);
}

ccb_slice ccb_slice_trim(ccb_slice s) {
  while (s.len > 0 && isspace((unsigned char)s.ptr[0])) {
    s.ptr++;
    s.len--;
  }
  while (s.len > 0 && isspace((unsigned char)s.ptr[s.len - 1])) {
    s.len--;
  }
  return s;
}

char *ccb_slice_to_str(ccb_slice s) {
  return ccb_xstrndup(s.ptr, s.len);
}

int ccb_slice_to_long(ccb_slice s, long *out) {
  if (s.len == 0 || s.len >= 32) {
    return -1;
  }
  char tmp[32];
  memcpy(tmp, s.ptr, s.len);
  tmp[s.len] = '\0';
  char *end = NULL;
  long v = strtol(tmp, &end, 10);
  if (end == NULL || *end != '\0') {
    return -1;
  }
  *out = v;
  return 0;
}
