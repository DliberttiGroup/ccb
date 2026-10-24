// ccb_toml implementation: TOML subset parser.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_slice.h"
#include "ccb_toml.h"
#include <ctype.h>
#include <stdarg.h>

typedef struct {
  const char *text;
  size_t len;
  size_t pos;
  long line;
  ccb_toml_doc *doc;
  ccb_toml_error *errors;
  size_t ecount;
  size_t ecap;
  char *cur_table;
} ccb_toml_parser;

static void ccb_toml_err(ccb_toml_parser *p, long line, const char *fmt, ...) {
  if (p->ecount == p->ecap) {
    size_t ncap = p->ecap == 0 ? 8 : p->ecap * 2;
    p->errors = ccb_xrealloc(p->errors, ncap * sizeof(*p->errors));
    p->ecap = ncap;
  }
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  p->errors[p->ecount].line = line;
  p->errors[p->ecount].message = ccb_xstrdup(buf);
  p->ecount++;
}

static int ccb_is_bare_key_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static ccb_toml_table *ccb_find_table_mut(ccb_toml_doc *doc, const char *name) {
  for (size_t i = 0; i < doc->count; i++) {
    if (strcmp(doc->tables[i].name, name) == 0) {
      return &doc->tables[i];
    }
  }
  return NULL;
}

static ccb_toml_table *ccb_add_table(ccb_toml_doc *doc, const char *name, long line) {
  if (doc->count == doc->cap) {
    size_t ncap = doc->cap == 0 ? 8 : doc->cap * 2;
    doc->tables = ccb_xrealloc(doc->tables, ncap * sizeof(*doc->tables));
    doc->cap = ncap;
  }
  ccb_toml_table *t = &doc->tables[doc->count++];
  memset(t, 0, sizeof(*t));
  t->name = ccb_xstrdup(name);
  t->line = line;
  return t;
}

static void ccb_free_value(ccb_toml_value *v) {
  free(v->str);
  if (v->items != NULL) {
    for (size_t i = 0; i < v->count; i++) {
      free(v->items[i]);
    }
    free(v->items);
  }
  v->str = NULL;
  v->items = NULL;
  v->count = 0;
}

void ccb_toml_free_doc(ccb_toml_doc *doc) {
  if (doc == NULL) {
    return;
  }
  for (size_t i = 0; i < doc->count; i++) {
    ccb_toml_table *t = &doc->tables[i];
    free(t->name);
    for (size_t j = 0; j < t->count; j++) {
      free(t->kvs[j].key);
      ccb_free_value(&t->kvs[j].value);
    }
    free(t->kvs);
  }
  free(doc->tables);
  free(doc);
}

void ccb_toml_free_errors(ccb_toml_error *errors, size_t count) {
  if (errors == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(errors[i].message);
  }
  free(errors);
}

const ccb_toml_table *ccb_toml_find_table(const ccb_toml_doc *doc, const char *name) {
  for (size_t i = 0; i < doc->count; i++) {
    if (strcmp(doc->tables[i].name, name) == 0) {
      return &doc->tables[i];
    }
  }
  return NULL;
}

const ccb_toml_value *ccb_toml_find_value(const ccb_toml_table *t, const char *key) {
  if (t == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < t->count; i++) {
    if (strcmp(t->kvs[i].key, key) == 0) {
      return &t->kvs[i].value;
    }
  }
  return NULL;
}

// ---------- low-level scanners (operate on the parser cursor) ----------

static char ccb_peek(ccb_toml_parser *p) {
  return p->pos < p->len ? p->text[p->pos] : '\0';
}

static void ccb_skip_inline_ws(ccb_toml_parser *p) {
  while (p->pos < p->len && (p->text[p->pos] == ' ' || p->text[p->pos] == '\t' || p->text[p->pos] == '\r')) {
    p->pos++;
  }
}

// Skip blank lines and full-line comments. Returns 0 at EOF.
static int ccb_skip_to_content(ccb_toml_parser *p) {
  while (p->pos < p->len) {
    char c = p->text[p->pos];
    if (c == ' ' || c == '\t' || c == '\r') {
      p->pos++;
      continue;
    }
    if (c == '\n') {
      p->pos++;
      p->line++;
      continue;
    }
    if (c == '#') {
      while (p->pos < p->len && p->text[p->pos] != '\n') {
        p->pos++;
      }
      continue;
    }
    return 1;
  }
  return 0;
}

// Skip to the end of the current line (used for error recovery).
static void ccb_skip_line(ccb_toml_parser *p) {
  while (p->pos < p->len && p->text[p->pos] != '\n') {
    p->pos++;
  }
}

// After a value or header: only whitespace, a comment, newline, or EOF
// may follow on the line.
static int ccb_expect_eol(ccb_toml_parser *p, long line, const char *what) {
  ccb_skip_inline_ws(p);
  if (p->pos >= p->len) {
    return 0;
  }
  char c = p->text[p->pos];
  if (c == '\n') {
    return 0;
  }
  if (c == '#') {
    ccb_skip_line(p);
    return 0;
  }
  ccb_toml_err(p, line, "trailing characters after %s", what);
  ccb_skip_line(p);
  return -1;
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

static int ccb_hex_val(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Parse a basic string starting at the opening quote. On success *out
// receives the decoded string and the cursor sits after the closing
// quote. Returns 0 or -1 (error already recorded).
static int ccb_parse_basic(ccb_toml_parser *p, long line, char **out) {
  p->pos++;
  ccb_str s;
  ccb_str_init(&s);
  while (p->pos < p->len) {
    char c = p->text[p->pos];
    if (c == '"') {
      p->pos++;
      *out = ccb_str_steal(&s);
      return 0;
    }
    if (c == '\n') {
      ccb_str_free(&s);
      ccb_toml_err(p, line, "unterminated string");
      return -1;
    }
    if (c == '\\') {
      p->pos++;
      if (p->pos >= p->len) {
        break;
      }
      char e = p->text[p->pos++];
      switch (e) {
        case 'b': ccb_str_append_c(&s, '\b'); break;
        case 't': ccb_str_append_c(&s, '\t'); break;
        case 'n': ccb_str_append_c(&s, '\n'); break;
        case 'f': ccb_str_append_c(&s, '\f'); break;
        case 'r': ccb_str_append_c(&s, '\r'); break;
        case '"': ccb_str_append_c(&s, '"'); break;
        case '\\': ccb_str_append_c(&s, '\\'); break;
        case 'u':
        case 'U': {
          int digits = e == 'u' ? 4 : 8;
          unsigned cp = 0;
          int ok = 1;
          for (int i = 0; i < digits; i++) {
            if (p->pos >= p->len) {
              ok = 0;
              break;
            }
            int hv = ccb_hex_val(p->text[p->pos]);
            if (hv < 0) {
              ok = 0;
              break;
            }
            cp = (cp << 4) | (unsigned)hv;
            p->pos++;
          }
          if (!ok || cp > 0x10FFFF) {
            ccb_str_free(&s);
            ccb_toml_err(p, line, "bad unicode escape in string");
            return -1;
          }
          ccb_utf8_encode(&s, cp);
          break;
        }
        default:
          ccb_str_free(&s);
          ccb_toml_err(p, line, "unknown escape sequence '\\%c'", e);
          return -1;
      }
      continue;
    }
    ccb_str_append_c(&s, c);
    p->pos++;
  }
  ccb_str_free(&s);
  ccb_toml_err(p, line, "unterminated string");
  return -1;
}

// Parse a literal string starting at the opening quote.
static int ccb_parse_literal(ccb_toml_parser *p, long line, char **out) {
  p->pos++;
  size_t start = p->pos;
  while (p->pos < p->len) {
    char c = p->text[p->pos];
    if (c == '\'') {
      *out = ccb_xstrndup(p->text + start, p->pos - start);
      p->pos++;
      return 0;
    }
    if (c == '\n') {
      ccb_toml_err(p, line, "unterminated string");
      return -1;
    }
    p->pos++;
  }
  ccb_toml_err(p, line, "unterminated string");
  return -1;
}

// Parse one key segment (bare or quoted). Returns a malloc'd segment or
// NULL on error (already recorded).
static char *ccb_parse_key_segment(ccb_toml_parser *p, long line) {
  ccb_skip_inline_ws(p);
  char c = ccb_peek(p);
  if (c == '"') {
    char *s = NULL;
    if (ccb_parse_basic(p, line, &s) != 0) {
      return NULL;
    }
    return s;
  }
  if (c == '\'') {
    char *s = NULL;
    if (ccb_parse_literal(p, line, &s) != 0) {
      return NULL;
    }
    return s;
  }
  size_t start = p->pos;
  while (p->pos < p->len && ccb_is_bare_key_char(p->text[p->pos])) {
    p->pos++;
  }
  if (p->pos == start) {
    ccb_toml_err(p, line, "expected a key");
    return NULL;
  }
  return ccb_xstrndup(p->text + start, p->pos - start);
}

// Strip comments from array text, respecting strings. Returns a malloc'd
// copy. Newlines are kept so element errors stay roughly line-accurate.
static char *ccb_strip_array_comments(const char *text, size_t len) {
  ccb_str out;
  ccb_str_init(&out);
  size_t i = 0;
  while (i < len) {
    char c = text[i];
    if (c == '"') {
      ccb_str_append_c(&out, c);
      i++;
      while (i < len && text[i] != '"' && text[i] != '\n') {
        if (text[i] == '\\' && i + 1 < len) {
          ccb_str_append_c(&out, text[i]);
          ccb_str_append_c(&out, text[i + 1]);
          i += 2;
        } else {
          ccb_str_append_c(&out, text[i]);
          i++;
        }
      }
      if (i < len && text[i] == '"') {
        ccb_str_append_c(&out, text[i]);
        i++;
      }
      continue;
    }
    if (c == '\'') {
      ccb_str_append_c(&out, c);
      i++;
      while (i < len && text[i] != '\'' && text[i] != '\n') {
        ccb_str_append_c(&out, text[i]);
        i++;
      }
      if (i < len && text[i] == '\'') {
        ccb_str_append_c(&out, text[i]);
        i++;
      }
      continue;
    }
    if (c == '#') {
      while (i < len && text[i] != '\n') {
        i++;
      }
      continue;
    }
    ccb_str_append_c(&out, c);
    i++;
  }
  return ccb_str_steal(&out);
}

// Parse a string array value starting at '['. Handles multiline input.
// On success fills value and leaves the cursor after ']'.
static int ccb_parse_array(ccb_toml_parser *p, long line, ccb_toml_value *value) {
  size_t start = p->pos;
  long array_line = line;
  int depth = 0;
  int in_basic = 0;
  int in_literal = 0;
  while (p->pos < p->len) {
    char c = p->text[p->pos];
    if (c == '\n') {
      p->line++;
    }
    if (in_basic) {
      if (c == '\\') {
        p->pos += 2;
        continue;
      }
      if (c == '"') {
        in_basic = 0;
      }
      p->pos++;
      continue;
    }
    if (in_literal) {
      if (c == '\'') {
        in_literal = 0;
      }
      p->pos++;
      continue;
    }
    // Comments must not affect bracket depth.
    if (c == '#') {
      while (p->pos < p->len && p->text[p->pos] != '\n') {
        p->pos++;
      }
      continue;
    }
    if (c == '"') {
      in_basic = 1;
    } else if (c == '\'') {
      in_literal = 1;
    } else if (c == '[') {
      depth++;
    } else if (c == ']') {
      depth--;
      if (depth == 0) {
        p->pos++;
        break;
      }
    }
    p->pos++;
  }
  if (depth != 0) {
    ccb_toml_err(p, array_line, "unterminated array");
    return -1;
  }
  // Inner text between the brackets.
  size_t inner_start = start + 1;
  size_t inner_end = p->pos - 1;
  char *clean = ccb_strip_array_comments(p->text + inner_start, inner_end - inner_start);
  value->type = CCB_TOML_STR_ARRAY;
  value->items = NULL;
  value->count = 0;
  size_t cap = 0;
  // Split top-level commas, respecting strings.
  size_t i = 0;
  size_t clen = strlen(clean);
  int failed = 0;
  while (i < clen) {
    while (i < clen && (clean[i] == ' ' || clean[i] == '\t' || clean[i] == '\n' || clean[i] == '\r' || clean[i] == ',')) {
      i++;
    }
    if (i >= clen) {
      break;
    }
    size_t elem_start = i;
    int b = 0;
    int l = 0;
    while (i < clen) {
      char c = clean[i];
      if (b) {
        if (c == '\\') {
          i += 2;
          continue;
        }
        if (c == '"') {
          b = 0;
        }
        i++;
        continue;
      }
      if (l) {
        if (c == '\'') {
          l = 0;
        }
        i++;
        continue;
      }
      if (c == '"') {
        b = 1;
      } else if (c == '\'') {
        l = 1;
      } else if (c == ',') {
        break;
      }
      i++;
    }
    size_t elem_end = i;
    while (elem_end > elem_start && (clean[elem_end - 1] == ' ' || clean[elem_end - 1] == '\t' ||
        clean[elem_end - 1] == '\n' || clean[elem_end - 1] == '\r')) {
      elem_end--;
    }
    if (elem_end == elem_start) {
      continue;
    }
    // Parse the element as a string using a throwaway parser.
    ccb_toml_parser ep;
    memset(&ep, 0, sizeof(ep));
    ep.text = clean + elem_start;
    ep.len = elem_end - elem_start;
    ep.pos = 0;
    ep.line = array_line;
    ep.doc = NULL;
    char *s = NULL;
    int rc = -1;
    if (clean[elem_start] == '"') {
      rc = ccb_parse_basic(&ep, array_line, &s);
    } else if (clean[elem_start] == '\'') {
      rc = ccb_parse_literal(&ep, array_line, &s);
    } else {
      ccb_toml_err(p, array_line, "only string arrays are supported");
      failed = 1;
      break;
    }
    if (rc != 0) {
      // Adopt the element parser's errors.
      for (size_t k = 0; k < ep.ecount; k++) {
        ccb_toml_err(p, ep.errors[k].line, "%s", ep.errors[k].message);
      }
      ccb_toml_free_errors(ep.errors, ep.ecount);
      failed = 1;
      break;
    }
    ccb_toml_free_errors(ep.errors, ep.ecount);
    if (value->count == cap) {
      size_t ncap = cap == 0 ? 8 : cap * 2;
      value->items = ccb_xrealloc(value->items, ncap * sizeof(*value->items));
      cap = ncap;
    }
    value->items[value->count++] = s;
  }
  free(clean);
  if (failed) {
    return -1;
  }
  return 0;
}

static int ccb_parse_value(ccb_toml_parser *p, long line, ccb_toml_value *value) {
  memset(value, 0, sizeof(*value));
  char c = ccb_peek(p);
  if (c == '"') {
    char *s = NULL;
    if (ccb_parse_basic(p, line, &s) != 0) {
      return -1;
    }
    value->type = CCB_TOML_STRING;
    value->str = s;
    return 0;
  }
  if (c == '\'') {
    char *s = NULL;
    if (ccb_parse_literal(p, line, &s) != 0) {
      return -1;
    }
    value->type = CCB_TOML_STRING;
    value->str = s;
    return 0;
  }
  if (c == '[') {
    return ccb_parse_array(p, line, value);
  }
  if (c == 't' || c == 'f') {
    size_t save = p->pos;
    if (strncmp(p->text + p->pos, "true", 4) == 0) {
      p->pos += 4;
      value->type = CCB_TOML_BOOL;
      value->boolean = 1;
      return 0;
    }
    if (strncmp(p->text + p->pos, "false", 5) == 0) {
      p->pos += 5;
      value->type = CCB_TOML_BOOL;
      value->boolean = 0;
      return 0;
    }
    p->pos = save;
    ccb_toml_err(p, line, "invalid value");
    return -1;
  }
  // Integer: optional sign, digits, underscores between digits.
  {
    size_t start = p->pos;
    if (c == '+' || c == '-') {
      p->pos++;
    }
    size_t digits = 0;
    while (p->pos < p->len) {
      char d = p->text[p->pos];
      if (isdigit((unsigned char)d)) {
        digits++;
        p->pos++;
      } else if (d == '_' && digits > 0) {
        p->pos++;
      } else {
        break;
      }
    }
    if (digits == 0) {
      ccb_toml_err(p, line, "unsupported value: only strings, integers, booleans, and string arrays are supported");
      return -1;
    }
    // Reject trailing junk that would make this a float or date.
    char next = ccb_peek(p);
    if (next == '.' || next == '-' || next == ':' || next == 'T') {
      ccb_toml_err(p, line, "unsupported value: only strings, integers, booleans, and string arrays are supported");
      return -1;
    }
    size_t raw_len = p->pos - start;
    char tmp[64];
    if (raw_len >= sizeof(tmp)) {
      ccb_toml_err(p, line, "integer too long");
      return -1;
    }
    size_t w = 0;
    for (size_t i = 0; i < raw_len; i++) {
      char ch = p->text[start + i];
      if (ch != '_') {
        tmp[w++] = ch;
      }
    }
    tmp[w] = '\0';
    char *end = NULL;
    long v = strtol(tmp, &end, 10);
    if (end == NULL || *end != '\0') {
      ccb_toml_err(p, line, "invalid integer");
      return -1;
    }
    value->type = CCB_TOML_INT;
    value->integer = v;
    return 0;
  }
}

static int ccb_parse_table_header(ccb_toml_parser *p, long line) {
  p->pos++;
  ccb_str name;
  ccb_str_init(&name);
  int first = 1;
  while (1) {
    ccb_skip_inline_ws(p);
    char *seg = ccb_parse_key_segment(p, line);
    if (seg == NULL) {
      ccb_str_free(&name);
      ccb_skip_line(p);
      return -1;
    }
    if (!first) {
      ccb_str_append_c(&name, '.');
    }
    ccb_str_append(&name, seg);
    free(seg);
    first = 0;
    ccb_skip_inline_ws(p);
    if (ccb_peek(p) == '.') {
      p->pos++;
      continue;
    }
    break;
  }
  if (ccb_peek(p) != ']') {
    ccb_toml_err(p, line, "expected ']' in table header");
    ccb_str_free(&name);
    ccb_skip_line(p);
    return -1;
  }
  p->pos++;
  char *tname = ccb_str_steal(&name);
  if (ccb_find_table_mut(p->doc, tname) != NULL) {
    ccb_toml_err(p, line, "duplicate table [%s]", tname);
    free(tname);
    ccb_expect_eol(p, line, "table header");
    return -1;
  }
  ccb_add_table(p->doc, tname, line);
  free(p->cur_table);
  p->cur_table = tname;
  ccb_expect_eol(p, line, "table header");
  return 0;
}

static int ccb_parse_key_value(ccb_toml_parser *p, long line) {
  char *key = ccb_parse_key_segment(p, line);
  if (key == NULL) {
    ccb_skip_line(p);
    return -1;
  }
  ccb_skip_inline_ws(p);
  if (ccb_peek(p) == '.') {
    ccb_toml_err(p, line, "dotted keys are not supported");
    free(key);
    ccb_skip_line(p);
    return -1;
  }
  if (ccb_peek(p) != '=') {
    ccb_toml_err(p, line, "expected '=' after key");
    free(key);
    ccb_skip_line(p);
    return -1;
  }
  p->pos++;
  ccb_skip_inline_ws(p);
  ccb_toml_value value;
  if (ccb_parse_value(p, line, &value) != 0) {
    free(key);
    ccb_skip_line(p);
    return -1;
  }
  ccb_toml_table *t = ccb_find_table_mut(p->doc, p->cur_table);
  if (t == NULL) {
    t = ccb_add_table(p->doc, p->cur_table, line);
  }
  if (ccb_toml_find_value(t, key) != NULL) {
    ccb_toml_err(p, line, "duplicate key '%s' in table [%s]", key, p->cur_table);
    ccb_free_value(&value);
    free(key);
    ccb_expect_eol(p, line, "value");
    return -1;
  }
  if (t->count == t->cap) {
    size_t ncap = t->cap == 0 ? 8 : t->cap * 2;
    t->kvs = ccb_xrealloc(t->kvs, ncap * sizeof(*t->kvs));
    t->cap = ncap;
  }
  t->kvs[t->count].key = key;
  t->kvs[t->count].value = value;
  t->kvs[t->count].line = line;
  t->count++;
  ccb_expect_eol(p, line, "value");
  return 0;
}

ccb_toml_doc *ccb_toml_parse(const char *text, ccb_toml_error **errors_out, size_t *error_count_out) {
  ccb_toml_parser p;
  memset(&p, 0, sizeof(p));
  p.text = text;
  p.len = strlen(text);
  p.pos = 0;
  p.line = 1;
  p.doc = ccb_xcalloc(1, sizeof(*p.doc));
  p.cur_table = ccb_xstrdup("");
  ccb_add_table(p.doc, "", 0);
  while (ccb_skip_to_content(&p)) {
    long line = p.line;
    if (ccb_peek(&p) == '[') {
      ccb_parse_table_header(&p, line);
    } else {
      ccb_parse_key_value(&p, line);
    }
    // Advance past the newline we stopped at, if any.
    if (p.pos < p.len && p.text[p.pos] == '\n') {
      p.pos++;
      p.line++;
    }
  }
  free(p.cur_table);
  if (p.ecount > 0) {
    ccb_toml_free_doc(p.doc);
    if (errors_out != NULL) {
      *errors_out = p.errors;
    } else {
      ccb_toml_free_errors(p.errors, p.ecount);
    }
    if (error_count_out != NULL) {
      *error_count_out = p.ecount;
    }
    return NULL;
  }
  free(p.errors);
  if (errors_out != NULL) {
    *errors_out = NULL;
  }
  if (error_count_out != NULL) {
    *error_count_out = 0;
  }
  return p.doc;
}
