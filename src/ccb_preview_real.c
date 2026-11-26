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
#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"

#include <ctype.h>

// ---------- tiny text helpers ----------

static int rv_is_id_start(char c) {
  return isalpha((unsigned char)c) || c == '_';
}

static int rv_is_id_char(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

// Line number (1-based) of offset off in NUL-terminated text.
static long rv_line_of(const char *text, size_t off) {
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
static size_t rv_skip_ws(const char *s, size_t len, size_t i) {
  while (i < len && isspace((unsigned char)s[i])) {
    i++;
  }
  return i;
}

// Skip a `"""` raw string starting at i. Returns the position past
// its closer, or (size_t)-1 when unterminated.
static size_t rv_skip_raw_string(const char *s, size_t len, size_t i) {
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
static size_t rv_skip_quoted(const char *s, size_t len, size_t i) {
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
static size_t rv_skip_block_comment(const char *s, size_t len,
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
static size_t rv_match(const char *s, size_t len, size_t open, char oc,
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
static int rv_at_word(const char *s, size_t len, size_t off,
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
static size_t rv_find_word(const char *s, size_t len, size_t from,
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
static size_t rv_ident_end(const char *s, size_t len, size_t i) {
  while (i < len && rv_is_id_char(s[i])) {
    i++;
  }
  return i;
}

static void rv_diag(ccb_diag_list *diags, const char *code,
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

typedef struct {
  char *name;
  char *value;
} rv_str_entry;

typedef struct {
  rv_str_entry *items;
  size_t n, cap;
} rv_strings;

static const char *rv_strings_find(rv_strings *t, const char *name) {
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
static void rv_strings_parse_file(rv_strings *t, const char *path) {
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

// ---------- layout XML ----------

typedef struct {
  char *id; // view id, e.g. note1_button
  char *label; // resolved label (or the id when no android:text)
  char *method; // android:onClick method, or NULL
} rv_button;

typedef struct {
  char *name; // layout name, e.g. activity_main
  char *path; // first file found with that name (malloc'd)
  rv_button *buttons;
  size_t nbuttons, capbuttons;
} rv_layout;

// Attribute value of attr inside the tag text [start, end): walks
// real attributes, so a name inside another attribute's quoted
// value never matches. Malloc'd, or NULL.
// The quoted value at i (i is the opening quote): 1 with
// *vs_out/*ve_out delimiting the value, or 0 when unterminated.
static int rv_quoted_value(const char *text, size_t end, size_t i,
    size_t *vs_out, size_t *ve_out) {
  char quote = text[i];
  size_t vs = i + 1;
  size_t ve = vs;
  while (ve < end && text[ve] != quote) {
    ve++;
  }
  if (ve >= end) {
    return 0;
  }
  *vs_out = vs;
  *ve_out = ve;
  return 1;
}

// The value of attr at [ns, ne) with quoted value [vs, ve):
// malloc'd when the name matches, else NULL.
static char *rv_attr_match(const char *text, size_t ns, size_t ne,
    size_t vs, size_t ve, const char *attr, size_t alen) {
  if (ne - ns != alen || memcmp(text + ns, attr, alen) != 0) {
    return NULL;
  }
  char *out = ccb_xmalloc(ve - vs + 1);
  memcpy(out, text + vs, ve - vs);
  out[ve - vs] = '\0';
  return out;
}

// Scan one attribute starting at *i_io (past leading whitespace).
// Returns 1 with *value_out set (malloc'd) when it is `attr`,
// 0 when it is another well-formed attribute (with *i_io past
// it), or -1 when malformed or the tag ends.
static int rv_xml_attr_one(const char *text, size_t end, size_t *i_io,
    const char *attr, size_t alen, char **value_out) {
  size_t i = *i_io;
  size_t ns = 0;
  size_t ne = 0;
  size_t vs = 0;
  size_t ve = 0;
  char *out = NULL;
  while (i < end && (text[i] == ' ' || text[i] == '\t' ||
      text[i] == '\n' || text[i] == '\r')) {
    i++;
  }
  if (i >= end || text[i] == '/') {
    return -1;
  }
  ns = i;
  while (i < end && text[i] != ' ' && text[i] != '\t' &&
      text[i] != '\n' && text[i] != '\r' && text[i] != '=' &&
      text[i] != '/' && text[i] != '>') {
    i++;
  }
  ne = i;
  while (i < end && (text[i] == ' ' || text[i] == '\t' ||
      text[i] == '\n' || text[i] == '\r')) {
    i++;
  }
  if (i >= end || text[i] != '=') {
    *i_io = i;
    return 0;
  }
  i++;
  while (i < end && (text[i] == ' ' || text[i] == '\t' ||
      text[i] == '\n' || text[i] == '\r')) {
    i++;
  }
  if (i >= end || (text[i] != '"' && text[i] != '\'')) {
    return -1;
  }
  if (!rv_quoted_value(text, end, i, &vs, &ve)) {
    return -1;
  }
  out = rv_attr_match(text, ns, ne, vs, ve, attr, alen);
  if (out != NULL) {
    *value_out = out;
    return 1;
  }
  *i_io = ve + 1;
  return 0;
}

static char *rv_xml_attr(const char *text, size_t start, size_t end,
    const char *attr) {
  size_t i = start;
  size_t alen = strlen(attr);
  while (i < end) {
    char *value = NULL;
    int rc = rv_xml_attr_one(text, end, &i, attr, alen, &value);
    if (rc != 0) {
      return rc > 0 ? value : NULL;
    }
  }
  return NULL;
}

static rv_layout *rv_layout_find(rv_layout **layouts, size_t nlayouts,
    const char *name) {
  size_t i = 0;
  for (i = 0; i < nlayouts; i++) {
    if (strcmp(layouts[i]->name, name) == 0) {
      return layouts[i];
    }
  }
  return NULL;
}

// Scan one layout file's tags for Button elements. Only the three
// attributes preview needs are read; anything else is skipped,
// never guessed.
// Skip an XML comment starting at i ("<!--...-->"). Returns the
// position past it, or (size_t)-1 when the comment is unterminated.
static size_t rv_skip_xml_comment(const char *text, size_t len,
    size_t i) {
  size_t j = 0;
  for (j = i + 4; j + 3 <= len; j++) {
    if (memcmp(text + j, "-->", 3) == 0) {
      return j + 3;
    }
  }
  return (size_t)-1;
}

// Find the '>' closing the tag starting at i (skipping quoted
// sections). Returns 1 with *gt_out set, or 0 when unterminated.
static int rv_tag_end(const char *text, size_t len, size_t i,
    size_t *gt_out) {
  char quote = '\0';
  for (size_t k = i + 1; k < len; k++) {
    if (quote != '\0') {
      if (text[k] == quote) {
        quote = '\0';
      }
      continue;
    }
    if (text[k] == '"' || text[k] == '\'') {
      quote = text[k];
    } else if (text[k] == '>') {
      *gt_out = k;
      return 1;
    }
  }
  return 0;
}

// The local tag name: strip the package prefix up to the last '.'
// (android.widget.Button -> Button).
static void rv_local_name(const char *text, size_t ns, size_t ne,
    const char **local_io, size_t *local_len_io) {
  const char *local = text + ns;
  size_t local_len = ne - ns;
  for (size_t d = 0; d < local_len; d++) {
    if (local[d] == '.') {
      local = text + ns + d + 1;
      local_len = ne - (ns + d + 1);
    }
  }
  *local_io = local;
  *local_len_io = local_len;
}

// The button label: @string/<name> resolved via the strings
// table (falling back to the raw reference), else the raw text,
// else the view id entry.
static char *rv_button_label(rv_strings *strings,
    const char *raw_text, const char *entry) {
  if (raw_text != NULL && strncmp(raw_text, "@string/", 8) == 0) {
    const char *resolved = rv_strings_find(strings, raw_text + 8);
    return ccb_xstrdup(resolved != NULL ? resolved : raw_text);
  }
  if (raw_text != NULL) {
    return ccb_xstrdup(raw_text);
  }
  return ccb_xstrdup(entry);
}

// Append one button (id entry, label, onClick method) to the layout.
static void rv_layout_add_button(rv_layout *layout, const char *entry,
    char *label, char *method) {
  if (layout->nbuttons == layout->capbuttons) {
    size_t ncap = layout->capbuttons == 0 ? 8 :
      layout->capbuttons * 2;
    layout->buttons = ccb_xrealloc(layout->buttons,
      ncap * sizeof(*layout->buttons));
    layout->capbuttons = ncap;
  }
  layout->buttons[layout->nbuttons].id = ccb_xstrdup(entry);
  layout->buttons[layout->nbuttons].label = label;
  layout->buttons[layout->nbuttons].method = method;
  layout->nbuttons++;
}

static void rv_scan_layout_file(const char *text, size_t len,
    const char *layout_name, const char *layout_path,
    rv_layout ***layouts_io, size_t *nlayouts_io,
    rv_strings *strings) {
  size_t i = 0;
  rv_layout *layout = rv_layout_find(*layouts_io, *nlayouts_io,
    layout_name);
  if (layout == NULL) {
    layout = ccb_xcalloc(1, sizeof(*layout));
    layout->name = ccb_xstrdup(layout_name);
    layout->path = ccb_xstrdup(layout_path);
    *layouts_io = ccb_xrealloc(*layouts_io,
      (*nlayouts_io + 1) * sizeof(**layouts_io));
    (*layouts_io)[*nlayouts_io] = layout;
    (*nlayouts_io)++;
  }
  while (i < len) {
    size_t gt = 0;
    size_t ns = 0;
    size_t ne = 0;
    const char *local = NULL;
    size_t local_len = 0;
    if (i + 4 <= len && memcmp(text + i, "<!--", 4) == 0) {
      size_t after = rv_skip_xml_comment(text, len, i);
      if (after == (size_t)-1) {
        break;
      }
      i = after;
      continue;
    }
    if (text[i] != '<') {
      i++;
      continue;
    }
    if (i + 1 >= len || text[i + 1] == '/' || text[i + 1] == '?' ||
        text[i + 1] == '!') {
      while (i < len && text[i] != '>') {
        i++;
      }
      if (i < len) {
        i++;
      }
      continue;
    }
    if (!rv_tag_end(text, len, i, &gt)) {
      break;
    }
    ns = i + 1;
    ne = ns;
    while (ne < gt && text[ne] != ' ' && text[ne] != '\t' &&
        text[ne] != '\n' && text[ne] != '\r' && text[ne] != '/' &&
        text[ne] != '>') {
      ne++;
    }
    rv_local_name(text, ns, ne, &local, &local_len);
    if (local_len >= 6 &&
        memcmp(local + local_len - 6, "Button", 6) == 0) {
      char *id = rv_xml_attr(text, ns, gt, "android:id");
      char *raw_text = rv_xml_attr(text, ns, gt, "android:text");
      char *method = rv_xml_attr(text, ns, gt, "android:onClick");
      if (id != NULL) {
        const char *slash = strrchr(id, '/');
        const char *entry = slash != NULL ? slash + 1 : id;
        char *label = rv_button_label(strings, raw_text, entry);
        rv_layout_add_button(layout, entry, label, method);
        free(id);
        free(raw_text);
      } else {
        free(raw_text);
        free(method);
      }
    }
    i = gt + 1;
  }
}

// ---------- layout-derived wireframe (Chunk 2) ----------

// Map one layout element to its wireframe box type. Only the
// shapes the task names get a specific type; everything else is a
// generic box — never a guess about what it renders as. Suffix
// matching, so fully-qualified and AppCompat names map the same
// way (AppCompatButton ends in Button, like Button itself).
static const char *rv_wire_type(const char *local, size_t local_len,
    const char *text, size_t tag_start, size_t tag_end) {
  if (local_len >= 12 &&
      memcmp(local + local_len - 12, "LinearLayout", 12) == 0) {
    char *orient = rv_xml_attr(text, tag_start, tag_end,
      "android:orientation");
    int vertical = orient != NULL && strcmp(orient, "vertical") == 0;
    free(orient);
    // Android's default LinearLayout orientation is horizontal.
    return vertical ? "column" : "row";
  }
  if (local_len >= 8 &&
      memcmp(local + local_len - 8, "TextView", 8) == 0) {
    return "text";
  }
  if (local_len >= 6 &&
      memcmp(local + local_len - 6, "Button", 6) == 0) {
    return "button";
  }
  if (local_len >= 8 &&
      memcmp(local + local_len - 8, "EditText", 8) == 0) {
    return "textfield";
  }
  if (local_len >= 9 &&
      memcmp(local + local_len - 9, "ImageView", 9) == 0) {
    return "image";
  }
  if (local_len == 5 && memcmp(local, "merge", 5) == 0) {
    // A <merge> root has no parent to merge into: stack its
    // children vertically, like a vertical LinearLayout.
    return "column";
  }
  return "box";
}

#define RV_WIRE_MAX_DEPTH 32

typedef struct {
  const char *text;
  size_t len;
  size_t pos;
  ccb_arena *arena;
} rv_wire_cursor;

// Skip an XML comment at the cursor. Returns 1 when a comment was
// skipped (cursor past "-->", or at len on an unterminated one),
// 0 when the cursor is not at a comment.
static int rv_wire_skip_comment(rv_wire_cursor *c) {
  if (c->pos + 4 > c->len || memcmp(c->text + c->pos, "<!--", 4) != 0) {
    return 0;
  }
  size_t j = c->pos + 4;
  while (j + 3 <= c->len && memcmp(c->text + j, "-->", 3) != 0) {
    j++;
  }
  c->pos = j + 3 <= c->len ? j + 3 : c->len;
  return 1;
}

// Skip a processing instruction at the cursor. Returns 1 when one
// was skipped (cursor past "?>" or at len), 0 otherwise.
static int rv_wire_skip_pi(rv_wire_cursor *c) {
  if (c->pos + 2 > c->len || memcmp(c->text + c->pos, "<?", 2) != 0) {
    return 0;
  }
  size_t j = c->pos + 2;
  while (j + 1 < c->len &&
      !(c->text[j] == '?' && c->text[j + 1] == '>')) {
    j++;
  }
  c->pos = j + 1 < c->len ? j + 2 : c->len;
  return 1;
}

static void rv_wire_skip_junk(rv_wire_cursor *c) {
  for (;;) {
    while (c->pos < c->len && (c->text[c->pos] == ' ' ||
        c->text[c->pos] == '\t' || c->text[c->pos] == '\n' ||
        c->text[c->pos] == '\r')) {
      c->pos++;
    }
    if (rv_wire_skip_comment(c)) {
      continue;
    }
    if (rv_wire_skip_pi(c)) {
      continue;
    }
    return;
  }
}

// Parse one element at the cursor (which must be at '<').
// Returns the box, or NULL on malformed input. On return the
// cursor is past the element's close tag (or at len). A mismatched
// close tag ends the current element; malformed input stops the
// subtree, never crashes and never loops: every path either
// returns or advances past at least the open tag.
static pv_box *rv_wire_element(rv_wire_cursor *c, int depth) {
  size_t gt = 0;
  size_t ns = 0;
  size_t ne = 0;
  const char *local = NULL;
  size_t local_len = 0;
  char quote = '\0';
  int found = 0;
  size_t k = 0;
  int self_close = 0;
  const char *type = NULL;
  char *label = NULL;
  pv_box *box = NULL;
  if (c->pos >= c->len || c->text[c->pos] != '<' ||
      c->pos + 1 >= c->len || c->text[c->pos + 1] == '/' ||
      c->text[c->pos + 1] == '?' || c->text[c->pos + 1] == '!') {
    return NULL;
  }
  for (k = c->pos + 1; k < c->len; k++) {
    if (quote != '\0') {
      if (c->text[k] == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c->text[k] == '"' || c->text[k] == '\'') {
      quote = c->text[k];
    } else if (c->text[k] == '>') {
      gt = k;
      found = 1;
      break;
    }
  }
  if (!found) {
    c->pos = c->len;
    return NULL;
  }
  self_close = gt > c->pos + 1 && c->text[gt - 1] == '/';
  ns = c->pos + 1;
  ne = ns;
  while (ne < gt && c->text[ne] != ' ' && c->text[ne] != '\t' &&
      c->text[ne] != '\n' && c->text[ne] != '\r' &&
      c->text[ne] != '/' && c->text[ne] != '>') {
    ne++;
  }
  local = c->text + ns;
  local_len = ne - ns;
  {
    size_t d = 0;
    for (d = 0; d < local_len; d++) {
      if (local[d] == '.') {
        local = c->text + ns + d + 1;
        local_len = ne - (ns + d + 1);
      }
    }
  }
  if (local_len == 0) {
    c->pos = gt + 1;
    return NULL;
  }
  type = rv_wire_type(local, local_len, c->text, ns, gt);
  // android:text is the label for every element that has one,
  // kept raw here — an "@string/..." reference resolves with the
  // other component labels later in the load, exactly like the
  // synthetic Button labels Chunk 1 already produces.
  label = rv_xml_attr(c->text, ns, gt, "android:text");
  box = pv_box_new(c->arena, ccb_arena_strdup(c->arena, type),
    label != NULL ? ccb_arena_strdup(c->arena, label) : NULL);
  free(label);
  c->pos = gt + 1;
  if (self_close || depth >= RV_WIRE_MAX_DEPTH) {
    return box;
  }
  for (;;) {
    rv_wire_skip_junk(c);
    if (c->pos >= c->len) {
      return box;
    }
    if (c->text[c->pos] == '<' && c->pos + 1 < c->len &&
        c->text[c->pos + 1] == '/') {
      size_t close_gt = c->pos;
      while (close_gt < c->len && c->text[close_gt] != '>') {
        close_gt++;
      }
      c->pos = close_gt < c->len ? close_gt + 1 : c->len;
      return box;
    }
    if (c->text[c->pos] != '<') {
      while (c->pos < c->len && c->text[c->pos] != '<') {
        c->pos++;
      }
      continue;
    }
    {
      pv_box *child = rv_wire_element(c, depth + 1);
      if (child != NULL) {
        pv_box_add_child(c->arena, box, child);
      } else if (c->pos >= c->len) {
        return box;
      }
    }
  }
}

// Build the wireframe box tree for one layout file: a "screen"
// root whose single child is the layout's root element. Returns
// NULL when the file is unreadable or holds no element (the caller
// keeps the synthetic Button root).
static pv_box *rv_layout_wireframe(ccb_arena *arena,
    const char *layout_path) {
  char *text = NULL;
  size_t len = 0;
  rv_wire_cursor c;
  pv_box *root = NULL;
  pv_box *child = NULL;
  if (layout_path == NULL ||
      ccb_read_file(layout_path, &text, &len) != CCB_OK) {
    return NULL;
  }
  c.text = text;
  c.len = len;
  c.pos = 0;
  c.arena = arena;
  rv_wire_skip_junk(&c);
  child = c.pos < len ? rv_wire_element(&c, 0) : NULL;
  free(text);
  if (child == NULL) {
    return NULL;
  }
  root = pv_box_new(arena, ccb_arena_strdup(arena, "screen"), NULL);
  pv_box_add_child(arena, root, child);
  return root;
}

// ---------- Activity extraction ----------

typedef struct {
  char *name; // member fun name
  char *params; // raw parameter list text (may be "")
  size_t brace, close; // body brace range in the file text
  long line;
} rv_fun;

typedef struct {
  char *id; // literal view id
  char *method; // ::method name, or NULL for a lambda
  size_t body_start, body_end; // lambda body range, or 0 when a method ref
  long line;
  int is_onclick_prop; // `.onClick = ...` instead of setOnClickListener
} rv_listener;

typedef struct {
  char *cls; // class name as written
  char *screen; // stripped screen name
  char *layout; // layout name, or NULL
  const char *file; // borrowed .kt path
  const char *text; // borrowed file text
  size_t text_len;
  size_t class_brace, class_close;
  size_t oncreate_brace, oncreate_close; // (size_t)-1 when absent
  rv_fun *funs;
  size_t nfuns, capfuns;
  rv_listener *listeners;
  size_t nlisteners, caplisteners;
  // [start, end) ranges of consumed findViewById attachments, so a
  // later scan can tell an unrecognized setOnClickListener apart.
  size_t *consumed_starts, *consumed_ends;
  size_t nconsumed, capconsumed;
} rv_activity;

// Length of name after stripping one trailing "Activity"/"Fragment"
// (a bare "Activity" is a name, not a suffix).
static size_t rv_stripped_len(const char *name, size_t n) {
  if (n > 8 && memcmp(name + n - 8, "Activity", 8) == 0) {
    return n - 8;
  }
  if (n > 8 && memcmp(name + n - 8, "Fragment", 8) == 0) {
    return n - 8;
  }
  return n;
}

// Strip one trailing "Activity" or "Fragment" (a bare "Activity" is
// a name, not a suffix). Malloc'd.
static char *rv_strip_suffix(const char *name) {
  size_t n = strlen(name);
  return ccb_xstrndup(name, rv_stripped_len(name, n));
}

// Body transforms, applied to an extracted lambda/member-fun body
// before it is embedded in the synthetic flow text:
//   1. findViewById<...>(R.id.x) / findViewById(R.id.x) -> x
//   2. `val v = Intent(this, C::class.java)` + startActivity(v)
//      -> startActivity(Intent(this, C::class.java))
//   3. CActivity::class.java / CFragment::class.java -> C::class.java
// The result is malloc'd.
typedef struct {
  char *var;
  char *cls;
} rv_intent_var;

// Parse `val|var <name> = Intent(this, <Cls>::class.java)` at use
// (use points at the val/var keyword). Returns 1 on success with
// *var_out / *cls_out owned by the caller and *end_out past
// "::class.java"; 0 otherwise (nothing allocated).
static int rv_parse_intent_var(const char *body, size_t len, size_t use,
    char **var_out, char **cls_out, size_t *end_out) {
  size_t p = use + 3;
  size_t q = rv_skip_ws(body, len, p);
  if (q >= len || !rv_is_id_start(body[q])) {
    return 0;
  }
  size_t ve = rv_ident_end(body, len, q);
  size_t e = rv_skip_ws(body, len, ve);
  if (e >= len || body[e] != '=') {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 1);
  if (!rv_at_word(body, len, e, "Intent")) {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 6);
  if (e >= len || body[e] != '(') {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 1);
  if (!rv_at_word(body, len, e, "this")) {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 4);
  if (e >= len || body[e] != ',') {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 1);
  if (e >= len || !rv_is_id_start(body[e])) {
    return 0;
  }
  size_t cls_start = e;
  size_t cls_end = rv_ident_end(body, len, e);
  if (cls_end + 12 > len || memcmp(body + cls_end, "::class.java", 12) != 0) {
    return 0;
  }
  *var_out = ccb_xstrndup(body + q, ve - q);
  *cls_out = ccb_xstrndup(body + cls_start, cls_end - cls_start);
  *end_out = cls_end + 12;
  return 1;
}

// findViewById<...>(R.id.x) or findViewById(R.id.x) -> x. Returns
// the index past the call, or 0 when this is not such a call.
static size_t rv_rewrite_find_view(ccb_str *out, const char *body,
    size_t len, size_t i) {
  size_t j = rv_skip_ws(body, len, i + 12);
  if (j < len && body[j] == '<') {
    size_t gt = rv_match(body, len, j, '<', '>');
    if (gt == (size_t)-1) {
      return 0;
    }
    j = rv_skip_ws(body, len, gt + 1);
  }
  if (j >= len || body[j] != '(') {
    return 0;
  }
  size_t k = rv_skip_ws(body, len, j + 1);
  if (k + 5 >= len || memcmp(body + k, "R.id.", 5) != 0 ||
      !rv_is_id_start(body[k + 5])) {
    return 0;
  }
  size_t id_start = k + 5;
  size_t id_end = rv_ident_end(body, len, id_start);
  size_t close = rv_match(body, len, j, '(', ')');
  if (close == (size_t)-1) {
    return 0;
  }
  ccb_str_append_n(out, body + id_start, id_end - id_start);
  return close + 1;
}

// startActivity(var) where var held Intent(this, C::class.java):
// rewrite to startActivity(Intent(this, C::class.java)). Returns
// the index past the call, or 0 when this is not such a call.
static size_t rv_rewrite_start_activity(ccb_str *out, const char *body,
    size_t len, size_t i, rv_intent_var *vars, size_t nvars) {
  size_t j = rv_skip_ws(body, len, i + 13);
  if (j >= len || body[j] != '(') {
    return 0;
  }
  size_t v = rv_skip_ws(body, len, j + 1);
  if (v >= len || !rv_is_id_start(body[v])) {
    return 0;
  }
  size_t ve = rv_ident_end(body, len, v);
  size_t after = rv_skip_ws(body, len, ve);
  if (after >= len || body[after] != ')') {
    return 0;
  }
  for (size_t k = 0; k < nvars; k++) {
    if (strlen(vars[k].var) != ve - v ||
        memcmp(vars[k].var, body + v, ve - v) != 0) {
      continue;
    }
    // Strip the Activity/Fragment suffix here too, so the
    // rewritten Intent names the same screen as the direct form
    // below (Detail, not DetailActivity) and dedups against
    // sketch edges.
    char *stripped = rv_strip_suffix(vars[k].cls);
    ccb_str_append(out, "startActivity(Intent(this, ");
    ccb_str_append(out, stripped);
    ccb_str_append(out, "::class.java))");
    free(stripped);
    return after + 1;
  }
  return 0;
}

static char *rv_transform_body(const char *body, size_t len) {
  ccb_str out;
  size_t i = 0;
  // Pass 1+2 are fused: scan left to right, rewriting findViewById
  // spans and startActivity(var) spans as they are found. Intent
  // val bindings are collected first (a val precedes its use).
  rv_intent_var *vars = NULL;
  size_t nvars = 0;
  size_t capvars = 0;
  size_t p = 0;
  while (p < len) {
    size_t v = rv_find_word(body, len, p, "val");
    size_t w = rv_find_word(body, len, p, "var");
    size_t use = (size_t)-1;
    if (v != (size_t)-1 && (w == (size_t)-1 || v < w)) {
      use = v;
    } else if (w != (size_t)-1) {
      use = w;
    }
    if (use == (size_t)-1) {
      break;
    }
    char *var = NULL;
    char *cls = NULL;
    size_t end = 0;
    if (rv_parse_intent_var(body, len, use, &var, &cls, &end)) {
      if (nvars == capvars) {
        size_t ncap = capvars == 0 ? 4 : capvars * 2;
        vars = ccb_xrealloc(vars, ncap * sizeof(*vars));
        capvars = ncap;
      }
      vars[nvars].var = var;
      vars[nvars].cls = cls;
      nvars++;
      p = end;
    } else {
      p = use + 3;
    }
  }
  ccb_str_init(&out);
  while (i < len) {
    // findViewById<...>(R.id.x) or findViewById(R.id.x) -> x.
    if (rv_at_word(body, len, i, "findViewById")) {
      size_t after = rv_rewrite_find_view(&out, body, len, i);
      if (after != 0) {
        i = after;
        continue;
      }
    }
    // startActivity(var) where var held Intent(this, C::class.java).
    if (rv_at_word(body, len, i, "startActivity")) {
      size_t after = rv_rewrite_start_activity(&out, body, len, i,
        vars, nvars);
      if (after != 0) {
        i = after;
        continue;
      }
    }
    // CActivity::class.java / CFragment::class.java -> C::class.java.
    if (rv_is_id_start(body[i])) {
      size_t e = rv_ident_end(body, len, i);
      if (e + 12 <= len && memcmp(body + e, "::class.java", 12) == 0) {
        size_t n = e - i;
        size_t keep = rv_stripped_len(body + i, n);
        ccb_str_append_n(&out, body + i, keep);
        ccb_str_append(&out, "::class.java");
        i = e + 12;
        continue;
      }
      ccb_str_append_n(&out, body + i, e - i);
      i = e;
      continue;
    }
    ccb_str_append_c(&out, body[i]);
    i++;
  }
  {
    size_t k = 0;
    for (k = 0; k < nvars; k++) {
      free(vars[k].var);
      free(vars[k].cls);
    }
    free(vars);
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

// True when the body range contains a direct nav-ish call
// (nav/navigateTo/startActivity/navigate followed by '(').
// A warning heuristic only — it never creates an edge.
static int rv_body_has_nav_direct(rv_activity *a, size_t start,
    size_t end) {
  static const char *navs[] = {
    "nav", "navigateTo", "startActivity", "navigate",
  };
  size_t n = 0;
  const char *s = a->text;
  size_t len = a->text_len;
  for (n = 0; n < 4; n++) {
    size_t hit = rv_find_word(s, len, start, navs[n]);
    while (hit != (size_t)-1 && hit < end) {
      size_t j = rv_skip_ws(s, len, hit + strlen(navs[n]));
      if (j < end && s[j] == '(') {
        return 1;
      }
      hit = rv_find_word(s, len, hit + 1, navs[n]);
    }
  }
  return 0;
}

// True when the fun named at `hit` is called bare (not `x.name(`)
// and its body contains a direct nav.
static int rv_hit_calls_nav(rv_activity *a, size_t start, size_t end,
    size_t hit, size_t f) {
  size_t j = 0;
  if (hit > start && a->text[hit - 1] == '.') {
    return 0;
  }
  j = rv_skip_ws(a->text, a->text_len,
    hit + strlen(a->funs[f].name));
  if (j >= end || a->text[j] != '(') {
    return 0;
  }
  return rv_body_has_nav_direct(a, a->funs[f].brace + 1,
    a->funs[f].close);
}

// The direct check, plus one level of helper calls: a bare call to
// one of the activity's member funs whose body contains a direct
// nav. One level only, so helper cycles terminate.
static int rv_body_has_nav(rv_activity *a, size_t start, size_t end) {
  size_t i = 0;
  const char *s = a->text;
  size_t len = a->text_len;
  if (rv_body_has_nav_direct(a, start, end)) {
    return 1;
  }
  for (i = 0; i < a->nfuns; i++) {
    size_t hit = rv_find_word(s, len, start, a->funs[i].name);
    while (hit != (size_t)-1 && hit < end) {
      if (rv_hit_calls_nav(a, start, end, hit, i)) {
        return 1;
      }
      hit = rv_find_word(s, len, hit + 1, a->funs[i].name);
    }
  }
  return 0;
}

// Collect member funs at class-body depth 1 (direct members only;
// local funs inside onCreate sit deeper). onCreate itself is
// skipped: its attachments are extracted separately.
static void rv_collect_funs(rv_activity *a) {
  size_t i = a->class_brace + 1;
  long depth = 1;
  while (i < a->class_close) {
    char c = a->text[i];
    if (c == '"') {
      size_t after = 0;
      if (i + 2 < a->text_len && a->text[i + 1] == '"' &&
          a->text[i + 2] == '"') {
        after = rv_skip_raw_string(a->text, a->text_len, i);
        i = after == (size_t)-1 ? a->text_len : after;
      } else {
        i = rv_skip_quoted(a->text, a->text_len, i);
      }
      continue;
    }
    if (c == '/' && i + 1 < a->text_len && a->text[i + 1] == '/') {
      while (i < a->text_len && a->text[i] != '\n') {
        i++;
      }
      continue;
    }
    if (c == '/' && i + 1 < a->text_len && a->text[i + 1] == '*') {
      size_t after = rv_skip_block_comment(a->text, a->text_len, i);
      i = after == (size_t)-1 ? a->text_len : after;
      continue;
    }
    if (c == '{') {
      depth++;
      i++;
      continue;
    }
    if (c == '}') {
      depth--;
      i++;
      continue;
    }
    if (depth == 1 && rv_at_word(a->text, a->text_len, i, "fun")) {
      size_t j = rv_skip_ws(a->text, a->text_len, i + 3);
      size_t ne = 0;
      char *name = NULL;
      size_t pc = 0;
      size_t open = 0;
      size_t brace = 0;
      size_t close = 0;
      if (j >= a->text_len || !rv_is_id_start(a->text[j])) {
        i++;
        continue;
      }
      ne = rv_ident_end(a->text, a->text_len, j);
      name = ccb_xstrndup(a->text + j, ne - j);
      if (strcmp(name, "onCreate") == 0) {
        free(name);
        i = ne;
        continue;
      }
      open = rv_skip_ws(a->text, a->text_len, ne);
      if (open >= a->text_len || a->text[open] != '(') {
        free(name);
        i = ne;
        continue;
      }
      pc = rv_match(a->text, a->text_len, open, '(', ')');
      if (pc == (size_t)-1) {
        free(name);
        i = ne;
        continue;
      }
      brace = rv_skip_ws(a->text, a->text_len, pc + 1);
      if (brace >= a->text_len || a->text[brace] != '{') {
        free(name);
        i = ne;
        continue;
      }
      close = rv_match(a->text, a->text_len, brace, '{', '}');
      if (close == (size_t)-1) {
        free(name);
        i = ne;
        continue;
      }
      if (a->nfuns == a->capfuns) {
        size_t ncap = a->capfuns == 0 ? 4 : a->capfuns * 2;
        a->funs = ccb_xrealloc(a->funs, ncap * sizeof(*a->funs));
        a->capfuns = ncap;
      }
      a->funs[a->nfuns].name = name;
      a->funs[a->nfuns].params = ccb_xstrndup(a->text + open + 1,
        pc - (open + 1));
      a->funs[a->nfuns].brace = brace;
      a->funs[a->nfuns].close = close;
      a->funs[a->nfuns].line = rv_line_of(a->text, j);
      a->nfuns++;
      i = close + 1;
      continue;
    }
    i++;
  }
}

// ---------- listener extraction ----------

static void rv_listener_add(rv_activity *a, const char *id,
    const char *method, size_t body_start, size_t body_end, long line,
    int is_onclick_prop) {
  if (a->nlisteners == a->caplisteners) {
    size_t ncap = a->caplisteners == 0 ? 4 : a->caplisteners * 2;
    a->listeners = ccb_xrealloc(a->listeners,
      ncap * sizeof(*a->listeners));
    a->caplisteners = ncap;
  }
  a->listeners[a->nlisteners].id = ccb_xstrdup(id);
  a->listeners[a->nlisteners].method =
    method != NULL ? ccb_xstrdup(method) : NULL;
  a->listeners[a->nlisteners].body_start = body_start;
  a->listeners[a->nlisteners].body_end = body_end;
  a->listeners[a->nlisteners].line = line;
  a->listeners[a->nlisteners].is_onclick_prop = is_onclick_prop;
  a->nlisteners++;
}

static void rv_consumed_add(rv_activity *a, size_t start, size_t end) {
  if (a->nconsumed == a->capconsumed) {
    size_t ncap = a->capconsumed == 0 ? 8 : a->capconsumed * 2;
    a->consumed_starts = ccb_xrealloc(a->consumed_starts,
      ncap * sizeof(*a->consumed_starts));
    a->consumed_ends = ccb_xrealloc(a->consumed_ends,
      ncap * sizeof(*a->consumed_ends));
    a->capconsumed = ncap;
  }
  a->consumed_starts[a->nconsumed] = start;
  a->consumed_ends[a->nconsumed] = end;
  a->nconsumed++;
}

static int rv_in_consumed(rv_activity *a, size_t off) {
  size_t i = 0;
  for (i = 0; i < a->nconsumed; i++) {
    if (off >= a->consumed_starts[i] && off < a->consumed_ends[i]) {
      return 1;
    }
  }
  return 0;
}

// Parse the attachment after a literal findViewById receiver.
// recv_end is the index just past the findViewById(...) call.
// Records a listener and its consumed range; returns 1 on success.
// True when text[k..ne) names one of the activity's member funs.
static int rv_is_member_fun(rv_activity *a, size_t k, size_t ne) {
  for (size_t f = 0; f < a->nfuns; f++) {
    if (strlen(a->funs[f].name) == ne - k &&
        memcmp(a->funs[f].name, a->text + k, ne - k) == 0) {
      return 1;
    }
  }
  return 0;
}

// A `{ ... }` lambda block at j: 1 with *close_out at its '}'.
static int rv_lambda_block(rv_activity *a, size_t j, size_t *close_out) {
  if (j >= a->text_len || a->text[j] != '{') {
    return 0;
  }
  size_t close = rv_match(a->text, a->text_len, j, '{', '}');
  if (close == (size_t)-1) {
    return 0;
  }
  *close_out = close;
  return 1;
}

// A `::name` method reference at k: the name (malloc'd, NULL when
// there is none) and *end_out past the name.
static char *rv_method_ref(rv_activity *a, size_t k, size_t *end_out) {
  if (k + 1 >= a->text_len || a->text[k] != ':' ||
      a->text[k + 1] != ':' || k + 2 >= a->text_len ||
      !rv_is_id_start(a->text[k + 2])) {
    return NULL;
  }
  size_t ne = rv_ident_end(a->text, a->text_len, k + 2);
  *end_out = ne;
  return ccb_xstrndup(a->text + k + 2, ne - (k + 2));
}

// A bare member-fun name at k: record the attachment. Returns the
// ident end, or 0 when there is no bare member-fun name here. The
// caller records the consumed range.
static size_t rv_add_bare_fun(rv_activity *a, size_t k, const char *id,
    long line, int flag) {
  if (k >= a->text_len || !rv_is_id_start(a->text[k])) {
    return 0;
  }
  size_t fe = rv_ident_end(a->text, a->text_len, k);
  if (!rv_is_member_fun(a, k, fe)) {
    return 0;
  }
  char *fname = ccb_xstrndup(a->text + k, fe - k);
  rv_listener_add(a, id, fname, 0, 0, line, flag);
  free(fname);
  return fe;
}

// setOnClickListener(<form>) at j: a lambda, a ::ref, or a bare
// member-fun name. Returns 1 when an attachment was recorded.
static int rv_parse_listener_paren(rv_activity *a, size_t j,
    const char *id, size_t recv_start, long line) {
  size_t close = rv_match(a->text, a->text_len, j, '(', ')');
  if (close == (size_t)-1) {
    return 0;
  }
  size_t k = rv_skip_ws(a->text, a->text_len, j + 1);
  if (k < a->text_len && a->text[k] == '{') {
    size_t bc = 0;
    if (!rv_lambda_block(a, k, &bc) || bc + 1 != close) {
      return 0;
    }
    rv_listener_add(a, id, NULL, k + 1, bc, line, 0);
    rv_consumed_add(a, recv_start, close + 1);
    return 1;
  }
  size_t ne = 0;
  char *name = rv_method_ref(a, k, &ne);
  if (name != NULL) {
    rv_listener_add(a, id, name, 0, 0, line, 0);
    free(name);
    rv_consumed_add(a, recv_start, close + 1);
    return 1;
  }
  if (rv_add_bare_fun(a, k, id, line, 0) != 0) {
    rv_consumed_add(a, recv_start, close + 1);
    return 1;
  }
  return 0;
}

static int rv_parse_attachment(rv_activity *a, size_t recv_start,
    const char *id, size_t recv_end) {
  size_t i = rv_skip_ws(a->text, a->text_len, recv_end);
  long line = rv_line_of(a->text, recv_start);
  if (i >= a->text_len || a->text[i] != '.') {
    return 0;
  }
  i++;
  if (rv_at_word(a->text, a->text_len, i, "setOnClickListener")) {
    size_t j = rv_skip_ws(a->text, a->text_len, i + 18);
    size_t close = 0;
    if (rv_lambda_block(a, j, &close)) {
      rv_listener_add(a, id, NULL, j + 1, close, line, 0);
      rv_consumed_add(a, recv_start, close + 1);
      return 1;
    }
    if (j < a->text_len && a->text[j] == '(') {
      return rv_parse_listener_paren(a, j, id, recv_start, line);
    }
    return 0;
  }
  if (rv_at_word(a->text, a->text_len, i, "onClick")) {
    size_t j = rv_skip_ws(a->text, a->text_len, i + 7);
    if (j < a->text_len && a->text[j] == '=') {
      size_t close = 0;
      size_t ne = 0;
      char *name = NULL;
      j = rv_skip_ws(a->text, a->text_len, j + 1);
      if (rv_lambda_block(a, j, &close)) {
        rv_listener_add(a, id, NULL, j + 1, close, line, 1);
        rv_consumed_add(a, recv_start, close + 1);
        return 1;
      }
      name = rv_method_ref(a, j, &ne);
      if (name != NULL) {
        rv_listener_add(a, id, name, 0, 0, line, 1);
        free(name);
        rv_consumed_add(a, recv_start, ne);
        return 1;
      }
      size_t fe = rv_add_bare_fun(a, j, id, line, 1);
      if (fe != 0) {
        rv_consumed_add(a, recv_start, fe);
        return 1;
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

// Scan the onCreate body for literal findViewById attachments.
static void rv_extract_listeners(rv_activity *a) {
  size_t start = a->oncreate_brace + 1;
  size_t end = a->oncreate_close;
  size_t pos = start;
  if (a->oncreate_brace == (size_t)-1) {
    return;
  }
  while (pos < end) {
    size_t hit = rv_find_word(a->text, a->text_len, pos,
      "findViewById");
    size_t j = 0;
    size_t id_start = 0;
    size_t id_end = 0;
    size_t close = 0;
    char *id = NULL;
    if (hit == (size_t)-1 || hit >= end) {
      break;
    }
    pos = hit + 12;
    j = rv_skip_ws(a->text, a->text_len, pos);
    if (j < a->text_len && a->text[j] == '<') {
      size_t gt = rv_match(a->text, a->text_len, j, '<', '>');
      if (gt == (size_t)-1) {
        continue;
      }
      j = rv_skip_ws(a->text, a->text_len, gt + 1);
    }
    if (j >= a->text_len || a->text[j] != '(') {
      continue;
    }
    {
      size_t k = rv_skip_ws(a->text, a->text_len, j + 1);
      if (k + 5 >= a->text_len || memcmp(a->text + k, "R.id.", 5) != 0 ||
          !rv_is_id_start(a->text[k + 5])) {
        continue;
      }
      id_start = k + 5;
      id_end = rv_ident_end(a->text, a->text_len, id_start);
      close = rv_match(a->text, a->text_len, j, '(', ')');
      if (close == (size_t)-1) {
        continue;
      }
    }
    id = ccb_xstrndup(a->text + id_start, id_end - id_start);
    (void)rv_parse_attachment(a, hit, id, close + 1);
    free(id);
    pos = close + 1;
  }
}

// Warn CCB-P1025 for listener attachments the extractor did not
// consume (a variable receiver, a val-held view, a loop): the nav
// inside is real but has no button to attribute it to, so it is
// reported here instead of silently dropped.
// Warn for a `{ ... }` listener whose body navigates but whose
// receiver has no literal view id. `start` is the onCreate body
// start, `hit` the setOnClickListener position, `j` the '{'.
static void rv_warn_brace_listener(rv_activity *a, ccb_diag_list *diags,
    size_t start, size_t hit, size_t j, size_t end) {
  size_t close = rv_match(a->text, a->text_len, j, '{', '}');
  if (close == (size_t)-1 || close >= end ||
      !rv_body_has_nav(a, j + 1, close)) {
    return;
  }
  ccb_str msg;
  size_t rs = hit;
  size_t re = hit;
  ccb_str_init(&msg);
  while (rs > start && a->text[rs - 1] != '\n' &&
      a->text[rs - 1] != ';' && a->text[rs - 1] != '{') {
    rs--;
  }
  while (re > rs && (a->text[re - 1] == ' ' ||
      a->text[re - 1] == '\t')) {
    re--;
  }
  ccb_str_append(&msg, "unattributed navigation: click handler for '");
  if (re - rs > 48) {
    ccb_str_append_n(&msg, a->text + rs, 48);
    ccb_str_append(&msg, "...");
  } else {
    ccb_str_append_n(&msg, a->text + rs, re - rs);
  }
  ccb_str_append(&msg, "' has no literal view id, so its navigation "
    "is not recorded as an edge from any button; it is reported "
    "here so it is not silently dropped");
  rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "", a->file,
    rv_line_of(a->text, hit));
  ccb_str_free(&msg);
}

// Warn for a setOnClickListener(<lambda>) whose body navigates but
// whose receiver has no literal view id. `j` is the '('.
static void rv_warn_paren_listener(rv_activity *a, ccb_diag_list *diags,
    size_t hit, size_t j, size_t end) {
  size_t close = rv_match(a->text, a->text_len, j, '(', ')');
  size_t k = 0;
  size_t bc = 0;
  if (close == (size_t)-1) {
    return;
  }
  k = rv_skip_ws(a->text, a->text_len, j + 1);
  if (k >= a->text_len || a->text[k] != '{') {
    return;
  }
  bc = rv_match(a->text, a->text_len, k, '{', '}');
  if (bc == (size_t)-1 || bc >= end ||
      !rv_body_has_nav(a, k + 1, bc)) {
    return;
  }
  ccb_str msg;
  ccb_str_init(&msg);
  ccb_str_append(&msg, "unattributed navigation: click handler with "
    "no literal view id has navigation that is not recorded as an "
    "edge from any button; it is reported here so it is not "
    "silently dropped");
  rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "", a->file,
    rv_line_of(a->text, hit));
  ccb_str_free(&msg);
}

static void rv_warn_unresolved_listeners(rv_activity *a,
    ccb_diag_list *diags) {
  size_t start = a->oncreate_brace + 1;
  size_t end = a->oncreate_close;
  size_t pos = start;
  if (a->oncreate_brace == (size_t)-1) {
    return;
  }
  while (pos < end) {
    size_t hit = rv_find_word(a->text, a->text_len, pos,
      "setOnClickListener");
    size_t j = 0;
    if (hit == (size_t)-1 || hit >= end) {
      break;
    }
    pos = hit + 18;
    if (rv_in_consumed(a, hit)) {
      continue;
    }
    j = rv_skip_ws(a->text, a->text_len, hit + 18);
    if (j < a->text_len && a->text[j] == '{') {
      rv_warn_brace_listener(a, diags, start, hit, j, end);
      continue;
    }
    if (j < a->text_len && a->text[j] == '(') {
      rv_warn_paren_listener(a, diags, hit, j, end);
      continue;
    }
  }
}

// True when body range [start, end) references member fun `name`,
// via ::name or a bare name( call.
static int rv_body_refers(rv_activity *a, size_t start, size_t end,
    const char *name) {
  size_t n = strlen(name);
  size_t i = start;
  const char *s = a->text;
  size_t len = a->text_len;
  while (i < end) {
    size_t hit = rv_find_word(s, len, i, name);
    if (hit == (size_t)-1 || hit >= end) {
      return 0;
    }
    if (hit >= start + 2 && s[hit - 1] == ':' &&
        s[hit - 2] == ':') {
      return 1;
    }
    if (!(hit > start && s[hit - 1] == '.')) {
      size_t j = rv_skip_ws(s, len, hit + n);
      if (j < end && s[j] == '(') {
        return 1;
      }
    }
    i = hit + 1;
  }
  return 0;
}

static int rv_name_in(char **names, size_t n, const char *name) {
  size_t i = 0;
  for (i = 0; i < n; i++) {
    if (strcmp(names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

// Append `name` (copied) to the emitted list, growing it as needed.
static void rv_emitted_push(char ***names_io, size_t *n_io,
    size_t *cap_io, const char *name) {
  char **names = *names_io;
  size_t n = *n_io;
  size_t cap = *cap_io;
  if (n == cap) {
    size_t ncap = cap == 0 ? 8 : cap * 2;
    names = ccb_xrealloc(names, ncap * sizeof(*names));
    cap = ncap;
  }
  names[n++] = ccb_xstrdup(name);
  *names_io = names;
  *n_io = n;
  *cap_io = cap;
}

// Add the member funs referenced from one listener lambda body
// (body_start..body_end) to the emitted list.
static void rv_emitted_scan_body(rv_activity *a, char ***names_io,
    size_t *n_io, size_t *cap_io, size_t body_start, size_t body_end) {
  for (size_t f = 0; f < a->nfuns; f++) {
    if (!rv_name_in(*names_io, *n_io, a->funs[f].name) &&
        rv_body_refers(a, body_start, body_end, a->funs[f].name)) {
      rv_emitted_push(names_io, n_io, cap_io, a->funs[f].name);
    }
  }
}

// Add the member funs referenced from fun `f`'s body to the emitted
// list. Returns 1 when anything was added.
static int rv_emitted_scan_fun(rv_activity *a, char ***names_io,
    size_t *n_io, size_t *cap_io, size_t f) {
  int added = 0;
  for (size_t g = 0; g < a->nfuns; g++) {
    if (!rv_name_in(*names_io, *n_io, a->funs[g].name) &&
        rv_body_refers(a, a->funs[f].brace + 1, a->funs[f].close,
          a->funs[g].name)) {
      rv_emitted_push(names_io, n_io, cap_io, a->funs[g].name);
      added = 1;
    }
  }
  return added;
}

// One fixed-point pass over the emitted names: add the funs
// referenced from each emitted fun's body. Returns 1 when anything
// was added.
static int rv_emitted_close_once(rv_activity *a, char ***names_io,
    size_t *n_io, size_t *cap_io) {
  int added = 0;
  for (size_t k = 0; k < *n_io; k++) {
    for (size_t f = 0; f < a->nfuns; f++) {
      if (strcmp(a->funs[f].name, (*names_io)[k]) == 0) {
        added |= rv_emitted_scan_fun(a, names_io, n_io, cap_io, f);
      }
    }
  }
  return added;
}

// The member funs to emit as top-level funs: those named by a layout
// android:onClick method, referenced from a listener (method ref or
// a call inside a lambda body), or referenced from another emitted
// fun (closed to a fixed point, so helper chains count). The rest
// are dropped from the synthetic text; when a dropped fun contains
// a nav, one CCB-P1025 names it, so its navigation is visible
// instead of silent. Returns the emitted name list (malloc'd).
static char **rv_emitted_funs(rv_activity *a, char **onclick_methods,
    size_t nonclick, ccb_diag_list *diags, size_t *nemitted_io) {
  char **names = NULL;
  size_t n = 0;
  size_t cap = 0;
  size_t li = 0;
  size_t k = 0;
  int changed = 0;
  for (li = 0; li < a->nlisteners; li++) {
    if (a->listeners[li].method != NULL &&
        !rv_name_in(names, n, a->listeners[li].method)) {
      rv_emitted_push(&names, &n, &cap, a->listeners[li].method);
    }
    if (a->listeners[li].method == NULL) {
      rv_emitted_scan_body(a, &names, &n, &cap,
        a->listeners[li].body_start, a->listeners[li].body_end);
    }
  }
  for (k = 0; k < nonclick; k++) {
    if (!rv_name_in(names, n, onclick_methods[k])) {
      rv_emitted_push(&names, &n, &cap, onclick_methods[k]);
    }
  }
  changed = 1;
  while (changed) {
    changed = rv_emitted_close_once(a, &names, &n, &cap);
  }
  {
    size_t f = 0;
    for (f = 0; f < a->nfuns; f++) {
      if (!rv_name_in(names, n, a->funs[f].name) &&
          rv_body_has_nav_direct(a, a->funs[f].brace + 1,
            a->funs[f].close)) {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg, "unattributed navigation: nav call "
          "in '");
        ccb_str_append(&msg, a->funs[f].name);
        ccb_str_append(&msg, "' is never referenced from a button, "
          "so it is not recorded as an edge; it is reported here "
          "so it is not silently dropped");
        rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "",
          a->file, a->funs[f].line);
        ccb_str_free(&msg);
      }
    }
  }
  *nemitted_io = n;
  return names;
}

// Escape a layout label as a Kotlin string literal body.
static char *rv_escape_label(const char *s) {
  ccb_str out;
  size_t i = 0;
  ccb_str_init(&out);
  for (i = 0; s[i] != '\0'; i++) {
    if (s[i] == '\\' || s[i] == '"') {
      ccb_str_append_c(&out, '\\');
      ccb_str_append_c(&out, s[i]);
    } else if (s[i] == '\n') {
      ccb_str_append(&out, "\\n");
    } else if (s[i] == '\r') {
      ccb_str_append(&out, "\\r");
    } else if (s[i] == '\t') {
      ccb_str_append(&out, "\\t");
    } else {
      ccb_str_append_c(&out, s[i]);
    }
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

// Build the synthetic flow text for one activity. Buttons come from
// its layout (or none when the layout is unknown); listeners become
// attachment statements; emitted member funs become top-level funs.
static char *rv_synth_activity(rv_activity *a, rv_layout *layout,
    char **emitted, size_t nemitted) {
  ccb_str out;
  size_t i = 0;
  ccb_str_init(&out);
  ccb_str_append(&out, "fun ");
  ccb_str_append(&out, a->screen);
  ccb_str_append(&out, "() {\n");
  if (layout != NULL) {
    for (i = 0; i < layout->nbuttons; i++) {
      char *esc = rv_escape_label(layout->buttons[i].label);
      ccb_str_append(&out, "  Button(\"");
      ccb_str_append(&out, esc);
      ccb_str_append(&out, "\")\n");
      free(esc);
    }
  }
  for (i = 0; i < a->nlisteners; i++) {
    rv_listener *l = &a->listeners[i];
    ccb_str_append(&out, "  ");
    ccb_str_append(&out, l->id);
    if (l->method != NULL) {
      ccb_str_append(&out, ".setOnClickListener(::");
      ccb_str_append(&out, l->method);
      ccb_str_append(&out, ")\n");
    } else {
      char *body = rv_transform_body(a->text + l->body_start,
        l->body_end - l->body_start);
      ccb_str_append(&out, ".setOnClickListener { ");
      ccb_str_append(&out, body);
      ccb_str_append(&out, " }\n");
      free(body);
    }
  }
  ccb_str_append(&out, "}\n");
  for (i = 0; i < a->nfuns; i++) {
    char *body = NULL;
    if (!rv_name_in(emitted, nemitted, a->funs[i].name)) {
      continue;
    }
    body = rv_transform_body(a->text + a->funs[i].brace + 1,
      a->funs[i].close - (a->funs[i].brace + 1));
    ccb_str_append(&out, "fun ");
    ccb_str_append(&out, a->funs[i].name);
    ccb_str_append(&out, "(");
    ccb_str_append(&out, a->funs[i].params);
    ccb_str_append(&out, ") { ");
    ccb_str_append(&out, body);
    ccb_str_append(&out, " }\n");
    free(body);
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

static void rv_activity_free(rv_activity *a) {
  size_t i = 0;
  if (a == NULL) {
    return;
  }
  free(a->cls);
  free(a->screen);
  free(a->layout);
  for (i = 0; i < a->nfuns; i++) {
    free(a->funs[i].name);
    free(a->funs[i].params);
  }
  free(a->funs);
  for (i = 0; i < a->nlisteners; i++) {
    free(a->listeners[i].id);
    free(a->listeners[i].method);
  }
  free(a->listeners);
  free(a->consumed_starts);
  free(a->consumed_ends);
}

// Extract every Activity class in one .kt file: a class with an
// onCreate that calls setContentView. A class without one is not a
// screen (a Compose setContent or a helper class is out of scope,
// and a nav to it stays valid through the manifest exemption).
// Skip the class header (extends/implements/ctor parens) to its
// '{'. Returns the brace position, or (size_t)-1 when there is
// none (a ';' or newline ends the header, an unmatched '(' too).
static size_t rv_class_brace(const char *text, size_t len, size_t ne) {
  size_t brace = ne;
  while (brace < len && text[brace] != '{') {
    if (text[brace] == '(') {
      size_t m = rv_match(text, len, brace, '(', ')');
      if (m == (size_t)-1) {
        return (size_t)-1;
      }
      brace = m + 1;
      continue;
    }
    if (text[brace] == ';' || text[brace] == '\n') {
      return (size_t)-1;
    }
    brace++;
  }
  if (brace >= len || text[brace] != '{') {
    return (size_t)-1;
  }
  return brace;
}

// The onCreate body braces after `onCreate(...)` at fe: 1 with
// *brace_out/*close_out set, or 0 when there is no body.
static int rv_oncreate_braces(const char *text, size_t len, size_t fe,
    size_t *brace_out, size_t *close_out) {
  size_t p = rv_skip_ws(text, len, fe);
  size_t m = 0;
  size_t brace = 0;
  size_t close = 0;
  if (p >= len || text[p] != '(') {
    return 0;
  }
  m = rv_match(text, len, p, '(', ')');
  if (m == (size_t)-1) {
    return 0;
  }
  brace = rv_skip_ws(text, len, m + 1);
  if (brace >= len || text[brace] != '{') {
    return 0;
  }
  close = rv_match(text, len, brace, '{', '}');
  if (close == (size_t)-1) {
    return 0;
  }
  *brace_out = brace;
  *close_out = close;
  return 1;
}

// The layout id of `setContentView(R.layout.<id>)` at sv: malloc'd,
// or NULL when the call is not that form.
static char *rv_content_layout(const char *text, size_t len, size_t sv) {
  size_t p = rv_skip_ws(text, len, sv + 14);
  size_t k = 0;
  if (p >= len || text[p] != '(') {
    return NULL;
  }
  k = rv_skip_ws(text, len, p + 1);
  if (k + 9 >= len || memcmp(text + k, "R.layout.", 9) != 0 ||
      !rv_is_id_start(text[k + 9])) {
    return NULL;
  }
  size_t ls = k + 9;
  size_t le = rv_ident_end(text, len, ls);
  return ccb_xstrndup(text + ls, le - ls);
}

static void rv_extract_activities(const char *file, const char *text,
    rv_activity **acts_io, size_t *nacts_io) {
  size_t len = strlen(text);
  size_t pos = 0;
  while (pos < len) {
    size_t hit = rv_find_word(text, len, pos, "class");
    size_t j = 0;
    size_t ne = 0;
    char *cls = NULL;
    size_t brace = 0;
    size_t close = 0;
    rv_activity a;
    size_t oc = 0;
    size_t oc_brace = 0;
    size_t oc_close = 0;
    size_t sv = 0;
    if (hit == (size_t)-1) {
      break;
    }
    pos = hit + 5;
    j = rv_skip_ws(text, len, pos);
    if (j >= len || !rv_is_id_start(text[j])) {
      continue;
    }
    ne = rv_ident_end(text, len, j);
    cls = ccb_xstrndup(text + j, ne - j);
    // Class names are UpperCamelCase by convention; this also keeps
    // `::class.java` (lowercase "class" + "java") from ever parsing
    // as a class named "java".
    if (!isupper((unsigned char)cls[0])) {
      free(cls);
      continue;
    }
    brace = rv_class_brace(text, len, ne);
    if (brace == (size_t)-1) {
      free(cls);
      continue;
    }
    close = rv_match(text, len, brace, '{', '}');
    if (close == (size_t)-1) {
      free(cls);
      break;
    }
    memset(&a, 0, sizeof(a));
    a.cls = cls;
    a.screen = rv_strip_suffix(cls);
    a.file = file;
    a.text = text;
    a.text_len = len;
    a.class_brace = brace;
    a.class_close = close;
    a.oncreate_brace = (size_t)-1;
    a.oncreate_close = (size_t)-1;
    oc = brace + 1;
    while (oc < close) {
      size_t fhit = rv_find_word(text, len, oc, "fun");
      size_t fn = 0;
      size_t fe = 0;
      if (fhit == (size_t)-1 || fhit >= close) {
        break;
      }
      oc = fhit + 3;
      fn = rv_skip_ws(text, len, oc);
      if (fn >= len || !rv_is_id_start(text[fn])) {
        continue;
      }
      fe = rv_ident_end(text, len, fn);
      if (fe - fn != 8 || memcmp(text + fn, "onCreate", 8) != 0) {
        continue;
      }
      if (rv_oncreate_braces(text, len, fe, &oc_brace, &oc_close)) {
        a.oncreate_brace = oc_brace;
        a.oncreate_close = oc_close;
      }
      break;
    }
    if (a.oncreate_brace == (size_t)-1) {
      rv_activity_free(&a);
      pos = close + 1;
      continue;
    }
    sv = rv_find_word(text, len, a.oncreate_brace + 1,
      "setContentView");
    if (sv != (size_t)-1 && sv < a.oncreate_close) {
      a.layout = rv_content_layout(text, len, sv);
    }
    if (a.layout == NULL) {
      rv_activity_free(&a);
      pos = close + 1;
      continue;
    }
    rv_collect_funs(&a);
    rv_extract_listeners(&a);
    *acts_io = ccb_xrealloc(*acts_io,
      (*nacts_io + 1) * sizeof(**acts_io));
    (*acts_io)[*nacts_io] = a;
    (*nacts_io)++;
    pos = close + 1;
  }
}

// ---------- public entry points ----------

// Kotlin source dir for a config: [paths] kotlin, else [paths]
// java, else the kotlin/java dir next to the manifest. Malloc'd,
// or NULL when no candidate exists on disk.
char *pv_real_kotlin_dir(const ccb_config *cfg) {
  char *dir = NULL;
  char *cand = NULL;
  if (cfg == NULL) {
    return NULL;
  }
  if (cfg->kotlin_dir != NULL &&
      ccb_path_is_dir(cfg->kotlin_dir)) {
    return ccb_xstrdup(cfg->kotlin_dir);
  }
  if (cfg->java_dir != NULL && ccb_path_is_dir(cfg->java_dir)) {
    return ccb_xstrdup(cfg->java_dir);
  }
  if (cfg->manifest == NULL) {
    return NULL;
  }
  dir = ccb_dirname(cfg->manifest);
  cand = ccb_path_join(dir, "kotlin");
  if (ccb_path_is_dir(cand)) {
    free(dir);
    return cand;
  }
  free(cand);
  cand = ccb_path_join(dir, "java");
  if (ccb_path_is_dir(cand)) {
    free(dir);
    return cand;
  }
  free(cand);
  free(dir);
  return NULL;
}

// Manifest launcher activity, as a stripped screen name ("Main" for
// MainActivity): the <activity> whose intent-filter carries both
// MAIN and LAUNCHER. Malloc'd, or NULL when there is none.
// The android:name value of the launcher <activity> block starting
// at p and ending at end: malloc'd, or NULL when this block is not
// the launcher or has no name. (The MAIN/LAUNCHER check is
// unbounded, as in the caller: a later block's strings can match.)
static char *rv_activity_name(const char *p, const char *end) {
  const char *n = NULL;
  const char *q = NULL;
  if (strstr(p, "android.intent.action.MAIN") == NULL ||
      strstr(p, "android.intent.category.LAUNCHER") == NULL) {
    return NULL;
  }
  n = strstr(p, "android:name=\"");
  if (n == NULL || n >= end) {
    return NULL;
  }
  n += 14;
  q = strchr(n, '"');
  if (q == NULL || q >= end) {
    return NULL;
  }
  return ccb_xstrndup(n, (size_t)(q - n));
}

static char *rv_manifest_launcher(const char *manifest_path,
    const char *package) {
  char *text = NULL;
  size_t len = 0;
  const char *p = NULL;
  if (manifest_path == NULL ||
      ccb_read_file(manifest_path, &text, &len) != CCB_OK) {
    return NULL;
  }
  p = text;
  while ((p = strstr(p, "<activity")) != NULL) {
    const char *after = p + 9;
    const char *block_end = NULL;
    const char *next_activity = NULL;
    const char *end = NULL;
    char *name = NULL;
    if (*after != '\0' && (rv_is_id_char(*after) || *after == '-')) {
      p = after;
      continue;
    }
    block_end = strstr(p, "</activity>");
    next_activity = strstr(p + 1, "<activity");
    end = block_end;
    if (end == NULL ||
        (next_activity != NULL && next_activity < end)) {
      end = next_activity == NULL ? p + strlen(p) : next_activity;
    } else {
      end = block_end + strlen("</activity>");
    }
    name = rv_activity_name(p, end);
    if (name != NULL) {
      char *norm = pv_normalize_class(package, name);
      free(name);
      free(text);
      if (norm == NULL) {
        return NULL;
      }
      name = rv_strip_suffix(norm);
      free(norm);
      return name;
    }
    p = end;
  }
  free(text);
  return NULL;
}

// Phase 2 load (see ccb_preview.h): parse the app's own Activities
// into screens, appended to *screens_io (with pending onClick edges
// appended to *pending_io), reusing the flow parser over synthesized
// flow text. The launcher screen name is returned in *launcher_io
// (malloc'd, or NULL; the caller frees it). Returns CCB_OK always;
// an absent Kotlin dir or unreadable sources simply yield no
// screens, never an error.
// @string table: [preview] resources, else <res>/values.
static void rv_load_strings(const ccb_config *cfg, rv_strings *strings) {
  char *res_dir = NULL;
  char *spath = NULL;
  if (cfg->preview_resources != NULL) {
    res_dir = ccb_xstrdup(cfg->preview_resources);
  } else if (cfg->res != NULL) {
    res_dir = ccb_path_join(cfg->res, "values");
  }
  if (res_dir != NULL) {
    spath = ccb_path_join(res_dir, "strings.xml");
    rv_strings_parse_file(strings, spath);
    free(spath);
    free(res_dir);
  }
}

// One layout file: scan its buttons into the layouts list.
static void rv_load_one_layout(const char *path, rv_layout ***layouts,
    size_t *nlayouts, rv_strings *strings) {
  char *base = ccb_basename(path);
  size_t bn = strlen(base);
  if (bn <= 4 || strcmp(base + bn - 4, ".xml") != 0) {
    free(base);
    return;
  }
  char *lname = ccb_xstrndup(base, bn - 4);
  free(base);
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &text, &len) == CCB_OK) {
    rv_scan_layout_file(text, len, lname, path, layouts, nlayouts, strings);
    free(text);
  }
  free(lname);
}

// Layout buttons, from the same layout inputs Slice 3 uses.
static void rv_load_layouts(const ccb_config *cfg, rv_layout ***layouts,
    size_t *nlayouts, rv_strings *strings) {
  char *layout_path = pv_preview_layout_path(cfg);
  char **layout_files = NULL;
  size_t nlayout_files = 0;
  if (layout_path != NULL) {
    if (ccb_path_is_dir(layout_path)) {
      (void)ccb_collect_files(layout_path, ".xml", &layout_files,
        &nlayout_files);
    } else if (ccb_path_exists(layout_path)) {
      layout_files = ccb_xmalloc(sizeof(*layout_files));
      layout_files[0] = ccb_xstrdup(layout_path);
      nlayout_files = 1;
    }
    free(layout_path);
  }
  for (size_t li = 0; li < nlayout_files; li++) {
    rv_load_one_layout(layout_files[li], layouts, nlayouts, strings);
  }
  ccb_free_str_array(layout_files, nlayout_files);
}

// Append one emitted fun name (owned copy) to the growing list.
static void rv_emitted_add(char ***all, size_t *n, size_t *cap,
    const char *name) {
  if (*n == *cap) {
    size_t ncap = *cap == 0 ? 8 : *cap * 2;
    *all = ccb_xrealloc(*all, ncap * sizeof(**all));
    *cap = ncap;
  }
  (*all)[(*n)++] = ccb_xstrdup(name);
}

// True when method is already in the emitted list.
static int rv_emitted_has(char **all_emitted, size_t nemitted_all,
    const char *method) {
  for (size_t k = 0; k < nemitted_all; k++) {
    if (strcmp(all_emitted[k], method) == 0) {
      return 1;
    }
  }
  return 0;
}

// Synthesize one flow text for one activity and record it, plus
// its emitted fun names.
static void rv_synth_one_activity(rv_activity *a, char *file,
    rv_layout **layouts, size_t nlayouts, char **onclick_methods,
    size_t nonclick, ccb_diag_list *diags, char ***syn_labels,
    char ***syn_texts, const char ***syn_layouts, size_t *nsyn,
    size_t *capsyn, char ***all_emitted, size_t *nemitted_all,
    size_t *capemitted_all) {
  size_t nemitted = 0;
  char **emitted = NULL;
  rv_layout *layout = NULL;
  rv_warn_unresolved_listeners(a, diags);
  emitted = rv_emitted_funs(a, onclick_methods, nonclick, diags,
    &nemitted);
  if (a->layout != NULL) {
    layout = rv_layout_find(layouts, nlayouts, a->layout);
  }
  if (*nsyn == *capsyn) {
    size_t ncap = *capsyn == 0 ? 4 : *capsyn * 2;
    *syn_labels = ccb_xrealloc(*syn_labels, ncap * sizeof(**syn_labels));
    *syn_texts = ccb_xrealloc(*syn_texts, ncap * sizeof(**syn_texts));
    *syn_layouts = ccb_xrealloc(*syn_layouts, ncap * sizeof(**syn_layouts));
    *capsyn = ncap;
  }
  (*syn_labels)[*nsyn] = file;
  (*syn_texts)[*nsyn] = rv_synth_activity(a, layout, emitted, nemitted);
  (*syn_layouts)[*nsyn] = a->layout;
  (*nsyn)++;
  for (size_t ei = 0; ei < nemitted; ei++) {
    rv_emitted_add(all_emitted, nemitted_all, capemitted_all, emitted[ei]);
    free(emitted[ei]);
  }
  free(emitted);
}

// A listener method reference with no definition in any scanned
// Activity (a top-level fun, or a typo) records no edge: warn
// once per method, so its navigation is visible, not silent.
// (An android:onClick method with no matching fun stays silent,
// like Slice 3: the layout, not a button, names it.)
static void rv_warn_unattributed_listeners(rv_activity *acts, size_t nacts,
    char ***all_emitted, size_t *nemitted_all, size_t *capemitted_all,
    ccb_diag_list *diags) {
  for (size_t ai = 0; ai < nacts; ai++) {
    for (size_t li = 0; li < acts[ai].nlisteners; li++) {
      const char *method = acts[ai].listeners[li].method;
      if (method == NULL) {
        continue;
      }
      if (rv_emitted_has(*all_emitted, *nemitted_all, method)) {
        continue;
      }
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "unattributed navigation: click handler '");
      ccb_str_append(&msg, method);
      ccb_str_append(&msg, "' has no definition in the scanned "
        "Activities, so its navigation is not recorded as an "
        "edge; it is reported here so it is not silently dropped");
      rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "",
        acts[ai].file, acts[ai].listeners[li].line);
      ccb_str_free(&msg);
      rv_emitted_add(all_emitted, nemitted_all, capemitted_all, method);
    }
  }
}

// True when id already has a binding (first wins).
static int rv_bind_has(char **bind_keys, size_t nbind, const char *id) {
  for (size_t k = 0; k < nbind; k++) {
    if (strcmp(bind_keys[k], id) == 0) {
      return 1;
    }
  }
  return 0;
}

// Parse the synthesized flow texts into screens, with bindings:
// the config's [preview.bindings] first (explicit wins), then one
// real binding per layout view id (first wins across layouts, in
// sorted file order).
static void rv_parse_synth(const ccb_config *cfg, ccb_arena *arena,
    ccb_diag_list *diags, char **syn_labels, char **syn_texts,
    const char **syn_layouts, size_t nsyn, rv_layout **layouts,
    size_t nlayouts, char **graph_keys, char **graph_vals, size_t ngraph,
    char **onclick_methods, char **onclick_labels, size_t nonclick,
    pv_screen ***screens_io, size_t *nscreens_io,
    pv_pending_edge ***pending_io, size_t *npending_io) {
  char **bind_keys = NULL;
  char **bind_vals = NULL;
  size_t nbind = 0;
  for (size_t bi = 0; bi < cfg->preview_binding_count; bi++) {
    bind_keys = ccb_xrealloc(bind_keys, (nbind + 1) * sizeof(*bind_keys));
    bind_vals = ccb_xrealloc(bind_vals, (nbind + 1) * sizeof(*bind_vals));
    bind_keys[nbind] = cfg->preview_binding_keys[bi];
    bind_vals[nbind] = cfg->preview_binding_values[bi];
    nbind++;
  }
  for (size_t li = 0; li < nlayouts; li++) {
    for (size_t bi = 0; bi < layouts[li]->nbuttons; bi++) {
      const char *id = layouts[li]->buttons[bi].id;
      const char *label = layouts[li]->buttons[bi].label;
      if (rv_bind_has(bind_keys, nbind, id)) {
        continue;
      }
      bind_keys = ccb_xrealloc(bind_keys, (nbind + 1) * sizeof(*bind_keys));
      bind_vals = ccb_xrealloc(bind_vals, (nbind + 1) * sizeof(*bind_vals));
      bind_keys[nbind] = ccb_arena_strdup(arena, id);
      bind_vals[nbind] = ccb_arena_strdup(arena, label);
      nbind++;
    }
  }
  pv_fun_index *index = pv_fun_index_build(arena, syn_labels, syn_texts, nsyn);
  for (size_t i = 0; i < nsyn; i++) {
    size_t before = *nscreens_io;
    pv_flow_parse_indexed(arena, syn_texts[i], syn_labels[i], diags,
      screens_io, nscreens_io, bind_keys, bind_vals, nbind,
      onclick_methods, onclick_labels, nonclick,
      graph_keys, graph_vals, ngraph, pending_io, npending_io,
      index);
    // Chunk 2: replace the synthetic Button root with the
    // layout-derived wireframe when the Activity's layout file
    // exists; otherwise keep the synthetic root. The parse
    // appends the Activity's screen first (its fun is first in
    // the synthetic text), so index `before` is the fresh
    // screen. The wireframe carries the same button labels, so
    // onClick distribution by label is unaffected.
    if (*nscreens_io > before && syn_layouts[i] != NULL) {
      rv_layout *layout = rv_layout_find(layouts, nlayouts,
        syn_layouts[i]);
      pv_box *wire = NULL;
      if (layout != NULL && layout->path != NULL) {
        wire = rv_layout_wireframe(arena, layout->path);
      }
      if (wire != NULL) {
        (*screens_io)[before]->root = wire;
      }
    }
  }
  // Chunk C, same rule as flow files: a nav no parse attributed
  // warns CCB-P1025 (with the real file path; the line is the
  // synthesized line — see the as-built note).
  pv_sweep_unattributed_navs(index, diags);
  pv_fun_index_free(index);
  free(bind_keys);
  free(bind_vals);
}

ccb_status pv_real_load(const ccb_config *cfg, ccb_arena *arena,
    ccb_diag_list *diags, char **graph_keys, char **graph_vals,
    size_t ngraph, char **onclick_methods, char **onclick_labels,
    size_t nonclick, pv_screen ***screens_io, size_t *nscreens_io,
    pv_pending_edge ***pending_io, size_t *npending_io,
    char **launcher_io) {
  char *kdir = NULL;
  char **kt_files = NULL;
  size_t nkt = 0;
  rv_strings strings;
  rv_layout **layouts = NULL;
  size_t nlayouts = 0;
  rv_activity *acts = NULL;
  size_t nacts = 0;
  char **syn_labels = NULL;
  char **syn_texts = NULL;
  // Layout name per synthetic text (borrowed from the activity;
  // Chunk 2 wireframe lookup). Parallel to syn_labels/syn_texts.
  const char **syn_layouts = NULL;
  size_t nsyn = 0;
  size_t capsyn = 0;
  char **all_emitted = NULL;
  size_t nemitted_all = 0;
  size_t capemitted_all = 0;
  size_t i = 0;
  if (launcher_io != NULL) {
    *launcher_io = NULL;
  }
  if (cfg == NULL || arena == NULL || screens_io == NULL ||
      nscreens_io == NULL) {
    return CCB_OK;
  }
  kdir = pv_real_kotlin_dir(cfg);
  if (kdir == NULL) {
    return CCB_OK;
  }
  memset(&strings, 0, sizeof(strings));
  // @string table: [preview] resources, else <res>/values.
  rv_load_strings(cfg, &strings);
  // Layout buttons, from the same layout inputs Slice 3 uses.
  rv_load_layouts(cfg, &layouts, &nlayouts, &strings);
  if (ccb_collect_files(kdir, ".kt", &kt_files, &nkt) != CCB_OK) {
    nkt = 0;
  }
  free(kdir);
  // Extract activities, warn for unresolved listeners and dropped
  // member funs, and synthesize one flow text per activity.
  for (i = 0; i < nkt; i++) {
    char *text = NULL;
    size_t len = 0;
    size_t before = nacts;
    size_t ai = 0;
    if (ccb_read_file(kt_files[i], &text, &len) != CCB_OK) {
      continue;
    }
    rv_extract_activities(kt_files[i], text, &acts, &nacts);
    for (ai = before; ai < nacts; ai++) {
      rv_synth_one_activity(&acts[ai], kt_files[i], layouts, nlayouts,
        onclick_methods, nonclick, diags, &syn_labels, &syn_texts,
        &syn_layouts, &nsyn, &capsyn, &all_emitted, &nemitted_all,
        &capemitted_all);
    }
    free(text);
  }
  rv_warn_unattributed_listeners(acts, nacts, &all_emitted, &nemitted_all,
    &capemitted_all, diags);
  if (nsyn > 0) {
    rv_parse_synth(cfg, arena, diags, syn_labels, syn_texts, syn_layouts,
      nsyn, layouts, nlayouts, graph_keys, graph_vals, ngraph,
      onclick_methods, onclick_labels, nonclick, screens_io, nscreens_io,
      pending_io, npending_io);
  }
  for (i = 0; i < nsyn; i++) {
    free(syn_texts[i]);
  }
  free(syn_labels);
  free(syn_texts);
  free(syn_layouts);
  for (i = 0; i < nemitted_all; i++) {
    free(all_emitted[i]);
  }
  free(all_emitted);
  ccb_free_str_array(kt_files, nkt);
  for (i = 0; i < nacts; i++) {
    rv_activity_free(&acts[i]);
  }
  free(acts);
  for (i = 0; i < nlayouts; i++) {
    size_t b = 0;
    free(layouts[i]->name);
    free(layouts[i]->path);
    for (b = 0; b < layouts[i]->nbuttons; b++) {
      free(layouts[i]->buttons[b].id);
      free(layouts[i]->buttons[b].label);
      free(layouts[i]->buttons[b].method);
    }
    free(layouts[i]->buttons);
    free(layouts[i]);
  }
  free(layouts);
  for (i = 0; i < strings.n; i++) {
    free(strings.items[i].name);
    free(strings.items[i].value);
  }
  free(strings.items);
  if (launcher_io != NULL) {
    *launcher_io = rv_manifest_launcher(cfg->manifest, cfg->package);
  }
  return CCB_OK;
}
