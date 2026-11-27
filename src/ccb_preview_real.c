#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include <ctype.h>
#include "ccb_preview_real_internal.h"

// ccb_preview_real: Phase 2 real-code front end.
// Parses the app's own Kotlin Activities + layout XML and derives
// screens/edges from them, with the flow sketch + CSS supplying only
// the wireframe. The sketch is optional: a project with no flow/
// still previews from real code alone.
//
// How it works, in one paragraph: for each Activity class it
// synthesizes a small flow text (screen fun + Button components from
// the layout + listener attachments + referenced member funs) and
// feeds it to the existing flow parser (pv_flow_parse_indexed), so
// handler shapes, one-level helpers, branch annotations, args,
// android:onClick, navigate(), and the CCB-P1025 sweep are all
// reused, not reimplemented. The synthesis normalizes only what the
// flow parser cannot see: findViewById chains become bare view ids,
// `startActivity(intentVar)` becomes the direct Intent form, and
// `XActivity::class.java` becomes `X::class.java`, so real edges
// dedup against sketch edges to the same screen.
//
// Recognized shapes only; everything else warns and falls back,
// never crashes, never guesses (PROJECT-FIRST: manifest/graph/layout
// > flow sketch > derived).

// ---------- tiny text helpers ----------

int rv_is_id_start(char c) {
  return isalpha((unsigned char)c) || c == '_';
}

int rv_is_id_char(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

// Line number (1-based) of offset off in NUL-terminated text.
long rv_line_of(const char *text, size_t off) {
  long line = 1;
  size_t i = 0;
  for (i = 0; i < off && text[i] != '\0'; i++) {
    if (text[i] == '\n') {
      line++;
    }
  }
  return line;
}

// Skip whitespace starting at i.
size_t rv_skip_ws(const char *s, size_t len, size_t i) {
  while (i < len && isspace((unsigned char)s[i])) {
    i++;
  }
  return i;
}

// Skip a `"""` raw string starting at i. Returns the position past
// its closer, or (size_t)-1 when unterminated.
size_t rv_skip_raw_string(const char *s, size_t len, size_t i) {
  size_t j = i + 3;
  while (j + 2 < len) {
    if (s[j] == '"' && s[j + 1] == '"' && s[j + 2] == '"') {
      return j + 3;
    }
    j++;
  }
  return (size_t)-1;
}

// Skip a "..." string or 'x' char literal starting at i (i is the
// opening quote). Returns the position past its closer, or len when
// unterminated (a newline ends it too).
size_t rv_skip_quoted(const char *s, size_t len, size_t i) {
  char quote = s[i];
  i++;
  while (i < len && s[i] != quote && s[i] != '\n') {
    if (s[i] == '\\' && i + 1 < len) {
      i++;
    }
    i++;
  }
  if (i < len) {
    i++;
  }
  return i;
}

// Skip a `/* */` comment starting at i. Returns the position past
// its closer, or (size_t)-1 when unterminated.
size_t rv_skip_block_comment(const char *s, size_t len,
    size_t i) {
  size_t j = i + 2;
  while (j + 1 < len) {
    if (s[j] == '*' && s[j + 1] == '/') {
      return j + 2;
    }
    j++;
  }
  return (size_t)-1;
}

// Index of the closer matching the opener at open (oc/cc are e.g.
// '{'/'}' or '('/'), '<'/'>'), skipping "..." strings (with \ escapes
// and """ raw strings), 'x' char literals, // and /* */ comments; or
// (size_t)-1 when unbalanced.
size_t rv_match(const char *s, size_t len, size_t open, char oc,
    char cc) {
  long depth = 0;
  size_t i = open;
  while (i < len) {
    char c = s[i];
    if (c == '"') {
      size_t after = 0;
      if (i + 2 < len && s[i + 1] == '"' && s[i + 2] == '"') {
        after = rv_skip_raw_string(s, len, i);
      } else {
        after = rv_skip_quoted(s, len, i);
      }
      if (after == (size_t)-1) {
        return (size_t)-1;
      }
      i = after;
      continue;
    }
    if (c == '\'') {
      i = rv_skip_quoted(s, len, i);
      continue;
    }
    if (c == '/' && i + 1 < len && s[i + 1] == '/') {
      while (i < len && s[i] != '\n') {
        i++;
      }
      continue;
    }
    if (c == '/' && i + 1 < len && s[i + 1] == '*') {
      size_t after = rv_skip_block_comment(s, len, i);
      if (after == (size_t)-1) {
        return (size_t)-1;
      }
      i = after;
      continue;
    }
    if (c == oc) {
      depth++;
    } else if (c == cc) {
      depth--;
      if (depth == 0) {
        return i;
      }
    }
    i++;
  }
  return (size_t)-1;
}

// True when word occurs at s + off with word boundaries.
int rv_at_word(const char *s, size_t len, size_t off,
    const char *word) {
  size_t n = strlen(word);
  if (off + n > len || memcmp(s + off, word, n) != 0) {
    return 0;
  }
  if (off > 0 && rv_is_id_char(s[off - 1])) {
    return 0;
  }
  if (off + n < len && rv_is_id_char(s[off + n])) {
    return 0;
  }
  return 1;
}

// Next occurrence of word (word-bounded) at or after from, or
// (size_t)-1. A plain scan: callers only use it on code ranges, and
// string/comment false positives are acceptable for the warning
// heuristics that use it (never for edge creation).
size_t rv_find_word(const char *s, size_t len, size_t from,
    const char *word) {
  size_t n = strlen(word);
  size_t i = from;
  if (n == 0 || len < n) {
    return (size_t)-1;
  }
  for (i = from; i + n <= len; i++) {
    if (rv_at_word(s, len, i, word)) {
      return i;
    }
  }
  return (size_t)-1;
}

// Identifier starting at i (i must be an ident start); end in *end_out.
size_t rv_ident_end(const char *s, size_t len, size_t i) {
  while (i < len && rv_is_id_char(s[i])) {
    i++;
  }
  return i;
}

void rv_diag(ccb_diag_list *diags, const char *code,
    const char *message, const char *file, long line) {
  ccb_diag d;
  if (diags == NULL) {
    return;
  }
  ccb_diag_init(&d);
  ccb_diag_set_full(&d, code, "preview", message, file, line, 1,
    NULL, NULL, NULL);
  ccb_diag_list_push(diags, &d);
}

// ---------- @string table ----------

const char *rv_strings_find(rv_strings *t, const char *name) {
  size_t i = 0;
  if (t == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0; i < t->n; i++) {
    if (strcmp(t->items[i].name, name) == 0) {
      return t->items[i].value;
    }
  }
  return NULL;
}

// Decode the five XML entities; anything else copies literally.
static char *rv_decode_entities(const char *s, size_t n) {
  ccb_str out;
  size_t i = 0;
  ccb_str_init(&out);
  while (i < n) {
    if (s[i] == '&') {
      if (i + 5 <= n && memcmp(s + i, "&amp;", 5) == 0) {
        ccb_str_append_c(&out, '&');
        i += 5;
        continue;
      }
      if (i + 4 <= n && memcmp(s + i, "&lt;", 4) == 0) {
        ccb_str_append_c(&out, '<');
        i += 4;
        continue;
      }
      if (i + 4 <= n && memcmp(s + i, "&gt;", 4) == 0) {
        ccb_str_append_c(&out, '>');
        i += 4;
        continue;
      }
      if (i + 6 <= n && memcmp(s + i, "&quot;", 6) == 0) {
        ccb_str_append_c(&out, '"');
        i += 6;
        continue;
      }
      if (i + 6 <= n && memcmp(s + i, "&apos;", 6) == 0) {
        ccb_str_append_c(&out, '\'');
        i += 6;
        continue;
      }
    }
    ccb_str_append_c(&out, s[i]);
    i++;
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

static void rv_strings_put(rv_strings *t, char *name, char *value) {
  size_t i = 0;
  for (i = 0; i < t->n; i++) {
    if (strcmp(t->items[i].name, name) == 0) {
      free(t->items[i].value);
      t->items[i].value = value;
      free(name);
      return;
    }
  }
  if (t->n == t->cap) {
    size_t ncap = t->cap == 0 ? 8 : t->cap * 2;
    t->items = ccb_xrealloc(t->items, ncap * sizeof(*t->items));
    t->cap = ncap;
  }
  t->items[t->n].name = name;
  t->items[t->n].value = value;
  t->n++;
}

// Parse <string name="n">v</string> entries from a values file.
// A missing file is an empty table, never an error.
void rv_strings_parse_file(rv_strings *t, const char *path) {
  char *text = NULL;
  size_t len = 0;
  const char *p = NULL;
  if (path == NULL || ccb_read_file(path, &text, &len) != CCB_OK) {
    return;
  }
  p = text;
  while ((p = strstr(p, "<string")) != NULL) {
    const char *q = strstr(p, "name=\"");
    const char *gt = NULL;
    const char *close = NULL;
    char *name = NULL;
    char *value = NULL;
    if (q == NULL) {
      break;
    }
    q += 6;
    {
      const char *qe = strchr(q, '"');
      if (qe == NULL) {
        break;
      }
      name = ccb_xstrndup(q, (size_t)(qe - q));
    }
    gt = strchr(p, '>');
    if (gt == NULL) {
      free(name);
      break;
    }
    close = strstr(gt + 1, "</string>");
    if (close == NULL) {
      free(name);
      break;
    }
    value = rv_decode_entities(gt + 1, (size_t)(close - (gt + 1)));
    rv_strings_put(t, name, value);
    p = close + 9;
  }
  free(text);
}
