// ccb_json: JSON string escaping, JSONL event helpers, and a minimal
// JSON parser (objects, arrays, strings, numbers, booleans, null) used
// for the cache manifest. The parser has a depth limit so hostile input
// cannot blow the stack.
#ifndef CCB_JSON_H
#define CCB_JSON_H

#include "ccb_common.h"
#include "ccb_str.h"
#include "ccb_slice.h"

// Append s as a quoted, escaped JSON string.
void ccb_json_escape(ccb_str *out, const char *s);

typedef enum {
  CCB_JSON_NULL,
  CCB_JSON_BOOL,
  CCB_JSON_NUM,
  CCB_JSON_STR,
  CCB_JSON_ARR,
  CCB_JSON_OBJ
} ccb_json_type;

typedef struct ccb_json_value ccb_json_value;

struct ccb_json_value {
  ccb_json_type type;
  bool boolean;
  double num;
  char *str;
  ccb_json_value **items;
  size_t count;
  size_t cap;
  char **keys;
  ccb_json_value **vals;
  size_t kcount;
  size_t kcap;
};

// Parse a whole document. Returns NULL on error and sets *err to a
// malloc'd message (caller frees) when err is non-NULL.
ccb_json_value *ccb_json_parse(ccb_slice input, char **err);
const ccb_json_value *ccb_json_obj_get(const ccb_json_value *obj, const char *key);
void ccb_json_free(ccb_json_value *v);

#endif
