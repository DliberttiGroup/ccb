// ccb_toml: a TOML subset parser for ccb.toml files.
// Supported: comments, [table] and [a.b.c] headers, bare or quoted keys,
// basic strings ("..." with escapes), literal strings ('...'), integers,
// booleans, and multiline arrays of strings. Everything else (floats,
// dates, inline tables, arrays of tables, dotted keys) is a parse error
// with a clear message: ccb.toml never needs those.
// The parser accumulates errors and keeps going where possible, so one
// run can report every problem in a file.
#ifndef CCB_TOML_H
#define CCB_TOML_H

#include "ccb_common.h"

typedef enum {
  CCB_TOML_STRING,
  CCB_TOML_INT,
  CCB_TOML_BOOL,
  CCB_TOML_STR_ARRAY
} ccb_toml_type;

typedef struct {
  ccb_toml_type type;
  char *str;
  long integer;
  int boolean;
  char **items;
  size_t count;
} ccb_toml_value;

typedef struct {
  char *key;
  ccb_toml_value value;
  long line;
} ccb_toml_kv;

typedef struct {
  char *name;
  ccb_toml_kv *kvs;
  size_t count;
  size_t cap;
  long line;
} ccb_toml_table;

typedef struct {
  ccb_toml_table *tables;
  size_t count;
  size_t cap;
} ccb_toml_doc;

typedef struct {
  long line;
  char *message;
} ccb_toml_error;

// Parse text. Returns NULL on any parse error; when errors_out and
// error_count_out are non-NULL they receive the error list (caller frees
// with ccb_toml_free_errors). On success the caller frees the doc with
// ccb_toml_free_doc.
ccb_toml_doc *ccb_toml_parse(const char *text, ccb_toml_error **errors_out, size_t *error_count_out);
const ccb_toml_table *ccb_toml_find_table(const ccb_toml_doc *doc, const char *name);
const ccb_toml_value *ccb_toml_find_value(const ccb_toml_table *t, const char *key);
void ccb_toml_free_doc(ccb_toml_doc *doc);
void ccb_toml_free_errors(ccb_toml_error *errors, size_t count);

#endif
