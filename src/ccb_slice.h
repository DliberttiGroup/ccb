// ccb_slice: pointer + length for parsing. Parsers never rely on NUL
// termination and never read past the slice end.
#ifndef CCB_SLICE_H
#define CCB_SLICE_H

#include "ccb_common.h"

typedef struct {
  const char *ptr;
  size_t len;
} ccb_slice;

static inline ccb_slice ccb_slice_from(const char *s) {
  ccb_slice v;
  v.ptr = s;
  v.len = strlen(s);
  return v;
}

bool ccb_slice_eq(ccb_slice a, const char *b);
bool ccb_slice_eq_slice(ccb_slice a, ccb_slice b);
bool ccb_slice_starts_with(ccb_slice a, const char *prefix);
ccb_slice ccb_slice_trim(ccb_slice s);
char *ccb_slice_to_str(ccb_slice s);
int ccb_slice_to_long(ccb_slice s, long *out);

#endif
