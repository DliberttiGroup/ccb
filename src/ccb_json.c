// ccb_json implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_json.h"
#include <ctype.h>

void ccb_json_escape(ccb_str *out, const char *s) {
  ccb_str_append_c(out, '"');
  for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
    switch (*p) {
      case '"': ccb_str_append(out, "\\\""); break;
      case '\\': ccb_str_append(out, "\\\\"); break;
      case '\b': ccb_str_append(out, "\\b"); break;
      case '\f': ccb_str_append(out, "\\f"); break;
      case '\n': ccb_str_append(out, "\\n"); break;
      case '\r': ccb_str_append(out, "\\r"); break;
      case '\t': ccb_str_append(out, "\\t"); break;
      default:
        if (*p < 0x20) {
          ccb_str_appendf(out, "\\u%04x", *p);
        } else {
          ccb_str_append_c(out, (char)*p);
        }
        break;
    }
  }
  ccb_str_append_c(out, '"');
}

// ---------- minimal parser ----------

typedef struct {
  const char *p;
  const char *end;
  char *err;
} ccb_parser;

static void ccb_parse_err(ccb_parser *ps, const char *msg) {
  if (ps->err == NULL) {
    ps->err = ccb_xstrdup(msg);
  }
}

static void ccb_skip_ws(ccb_parser *ps) {
  while (ps->p < ps->end && isspace((unsigned char)*ps->p)) {
    ps->p++;
  }
}

static ccb_json_value *ccb_value_new(ccb_json_type t) {
  ccb_json_value *v = ccb_xcalloc(1, sizeof(*v));
  v->type = t;
  return v;
}

static void ccb_arr_push(ccb_json_value *arr, ccb_json_value *item) {
  if (arr->count == arr->cap) {
    size_t ncap = arr->cap == 0 ? 8 : arr->cap * 2;
    arr->items = ccb_xrealloc(arr->items, ncap * sizeof(*arr->items));
    arr->cap = ncap;
  }
  arr->items[arr->count++] = item;
}

static void ccb_obj_put(ccb_json_value *obj, char *key, ccb_json_value *val) {
  if (obj->kcount == obj->kcap) {
    size_t ncap = obj->kcap == 0 ? 8 : obj->kcap * 2;
    obj->keys = ccb_xrealloc(obj->keys, ncap * sizeof(*obj->keys));
    obj->vals = ccb_xrealloc(obj->vals, ncap * sizeof(*obj->vals));
    obj->kcap = ncap;
  }
  obj->keys[obj->kcount] = key;
  obj->vals[obj->kcount] = val;
  obj->kcount++;
}

static void ccb_utf8_encode(ccb_str *out, unsigned cp) {
  if (cp < 0x80) {
    ccb_str_append_c(out, (char)cp);
  } else if (cp < 0x800) {
    ccb_str_append_c(out, (char)(0xC0 | (cp >> 6)));
    ccb_str_append_c(out, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    ccb_str_append_c(out, (char)(0xE0 | (cp >> 12)));
    ccb_str_append_c(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
    ccb_str_append_c(out, (char)(0x80 | (cp & 0x3F)));
  } else {
    ccb_str_append_c(out, (char)(0xF0 | (cp >> 18)));
    ccb_str_append_c(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
    ccb_str_append_c(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
    ccb_str_append_c(out, (char)(0x80 | (cp & 0x3F)));
  }
}

static int ccb_hex4(ccb_parser *ps, unsigned *out) {
  if (ps->end - ps->p < 4) {
    return -1;
  }
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = ps->p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= (unsigned)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= (unsigned)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v |= (unsigned)(c - 'A' + 10);
    } else {
      return -1;
    }
  }
  ps->p += 4;
  *out = v;
  return 0;
}

static ccb_json_value *ccb_parse_value(ccb_parser *ps, int depth);

static char *ccb_parse_string_raw(ccb_parser *ps) {
  // Assumes *ps->p == '"'. Returns a malloc'd decoded string or NULL.
  ps->p++;
  ccb_str out;
  ccb_str_init(&out);
  while (ps->p < ps->end) {
    unsigned char c = (unsigned char)*ps->p;
    if (c == '"') {
      ps->p++;
      return ccb_str_steal(&out);
    }
    if (c == '\\') {
      ps->p++;
      if (ps->p >= ps->end) {
        break;
      }
      char e = *ps->p++;
      switch (e) {
        case '"': ccb_str_append_c(&out, '"'); break;
        case '\\': ccb_str_append_c(&out, '\\'); break;
        case '/': ccb_str_append_c(&out, '/'); break;
        case 'b': ccb_str_append_c(&out, '\b'); break;
        case 'f': ccb_str_append_c(&out, '\f'); break;
        case 'n': ccb_str_append_c(&out, '\n'); break;
        case 'r': ccb_str_append_c(&out, '\r'); break;
        case 't': ccb_str_append_c(&out, '\t'); break;
        case 'u': {
          unsigned cp = 0;
          if (ccb_hex4(ps, &cp) != 0) {
            ccb_str_free(&out);
            ccb_parse_err(ps, "bad \\u escape");
            return NULL;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 &&
              ps->p[0] == '\\' && ps->p[1] == 'u') {
            ps->p += 2;
            unsigned lo = 0;
            if (ccb_hex4(ps, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              ccb_str_free(&out);
              ccb_parse_err(ps, "bad surrogate pair");
              return NULL;
            }
          }
          ccb_utf8_encode(&out, cp);
          break;
        }
        default:
          ccb_str_free(&out);
          ccb_parse_err(ps, "bad escape");
          return NULL;
      }
      continue;
    }
    if (c < 0x20) {
      ccb_str_free(&out);
      ccb_parse_err(ps, "control character in string");
      return NULL;
    }
    ccb_str_append_c(&out, (char)c);
    ps->p++;
  }
  ccb_str_free(&out);
  ccb_parse_err(ps, "unterminated string");
  return NULL;
}

static int ccb_expect_lit(ccb_parser *ps, const char *lit) {
  size_t n = strlen(lit);
  if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) {
    return -1;
  }
  ps->p += n;
  return 0;
}

static ccb_json_value *ccb_parse_value(ccb_parser *ps, int depth) {
  if (depth > 64) {
    ccb_parse_err(ps, "nesting too deep");
    return NULL;
  }
  ccb_skip_ws(ps);
  if (ps->p >= ps->end) {
    ccb_parse_err(ps, "unexpected end of input");
    return NULL;
  }
  char c = *ps->p;
  if (c == '{') {
    ps->p++;
    ccb_json_value *obj = ccb_value_new(CCB_JSON_OBJ);
    ccb_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
      ps->p++;
      return obj;
    }
    while (1) {
      ccb_skip_ws(ps);
      if (ps->p >= ps->end || *ps->p != '"') {
        ccb_json_free(obj);
        ccb_parse_err(ps, "expected object key");
        return NULL;
      }
      char *key = ccb_parse_string_raw(ps);
      if (key == NULL) {
        ccb_json_free(obj);
        return NULL;
      }
      ccb_skip_ws(ps);
      if (ps->p >= ps->end || *ps->p != ':') {
        free(key);
        ccb_json_free(obj);
        ccb_parse_err(ps, "expected ':'");
        return NULL;
      }
      ps->p++;
      ccb_json_value *val = ccb_parse_value(ps, depth + 1);
      if (val == NULL) {
        free(key);
        ccb_json_free(obj);
        return NULL;
      }
      ccb_obj_put(obj, key, val);
      ccb_skip_ws(ps);
      if (ps->p < ps->end && *ps->p == ',') {
        ps->p++;
        continue;
      }
      if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return obj;
      }
      ccb_json_free(obj);
      ccb_parse_err(ps, "expected ',' or '}'");
      return NULL;
    }
  }
  if (c == '[') {
    ps->p++;
    ccb_json_value *arr = ccb_value_new(CCB_JSON_ARR);
    ccb_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
      ps->p++;
      return arr;
    }
    while (1) {
      ccb_json_value *val = ccb_parse_value(ps, depth + 1);
      if (val == NULL) {
        ccb_json_free(arr);
        return NULL;
      }
      ccb_arr_push(arr, val);
      ccb_skip_ws(ps);
      if (ps->p < ps->end && *ps->p == ',') {
        ps->p++;
        continue;
      }
      if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return arr;
      }
      ccb_json_free(arr);
      ccb_parse_err(ps, "expected ',' or ']'");
      return NULL;
    }
  }
  if (c == '"') {
    char *s = ccb_parse_string_raw(ps);
    if (s == NULL) {
      return NULL;
    }
    ccb_json_value *v = ccb_value_new(CCB_JSON_STR);
    v->str = s;
    return v;
  }
  if (c == 't') {
    if (ccb_expect_lit(ps, "true") != 0) {
      ccb_parse_err(ps, "bad literal");
      return NULL;
    }
    ccb_json_value *v = ccb_value_new(CCB_JSON_BOOL);
    v->boolean = true;
    return v;
  }
  if (c == 'f') {
    if (ccb_expect_lit(ps, "false") != 0) {
      ccb_parse_err(ps, "bad literal");
      return NULL;
    }
    ccb_json_value *v = ccb_value_new(CCB_JSON_BOOL);
    v->boolean = false;
    return v;
  }
  if (c == 'n') {
    if (ccb_expect_lit(ps, "null") != 0) {
      ccb_parse_err(ps, "bad literal");
      return NULL;
    }
    return ccb_value_new(CCB_JSON_NULL);
  }
  // Number: use strtod over a bounded copy.
  {
    const char *start = ps->p;
    while (ps->p < ps->end && strchr("-+0123456789.eE", *ps->p) != NULL) {
      ps->p++;
    }
    if (ps->p == start) {
      ccb_parse_err(ps, "unexpected character");
      return NULL;
    }
    size_t n = (size_t)(ps->p - start);
    char tmp[64];
    if (n >= sizeof(tmp)) {
      ccb_parse_err(ps, "number too long");
      return NULL;
    }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    char *end = NULL;
    double v = strtod(tmp, &end);
    if (end == NULL || *end != '\0') {
      ccb_parse_err(ps, "bad number");
      return NULL;
    }
    ccb_json_value *val = ccb_value_new(CCB_JSON_NUM);
    val->num = v;
    return val;
  }
}

ccb_json_value *ccb_json_parse(ccb_slice input, char **err) {
  ccb_parser ps;
  ps.p = input.ptr;
  ps.end = input.ptr + input.len;
  ps.err = NULL;
  ccb_json_value *v = ccb_parse_value(&ps, 0);
  if (v != NULL) {
    ccb_skip_ws(&ps);
    if (ps.p != ps.end) {
      ccb_json_free(v);
      ccb_parse_err(&ps, "trailing characters");
      v = NULL;
    }
  }
  if (err != NULL) {
    *err = ps.err;
  } else {
    free(ps.err);
  }
  return v;
}

const ccb_json_value *ccb_json_obj_get(const ccb_json_value *obj, const char *key) {
  if (obj == NULL || obj->type != CCB_JSON_OBJ) {
    return NULL;
  }
  for (size_t i = 0; i < obj->kcount; i++) {
    if (strcmp(obj->keys[i], key) == 0) {
      return obj->vals[i];
    }
  }
  return NULL;
}

void ccb_json_free(ccb_json_value *v) {
  if (v == NULL) {
    return;
  }
  free(v->str);
  for (size_t i = 0; i < v->count; i++) {
    ccb_json_free(v->items[i]);
  }
  free(v->items);
  for (size_t i = 0; i < v->kcount; i++) {
    free(v->keys[i]);
    ccb_json_free(v->vals[i]);
  }
  free(v->keys);
  free(v->vals);
  free(v);
}
