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
      if (i + 2 < len && s[i + 1] == '"' && s[i + 2] == '"') {
        size_t j = i + 3;
        int closed = 0;
        while (j + 2 < len) {
          if (s[j] == '"' && s[j + 1] == '"' && s[j + 2] == '"') {
            closed = 1;
            break;
          }
          j++;
        }
        if (!closed) {
          return (size_t)-1;
        }
        i = j + 3;
        continue;
      }
      i++;
      while (i < len && s[i] != '"' && s[i] != '\n') {
        if (s[i] == '\\' && i + 1 < len) {
          i++;
        }
        i++;
      }
      if (i < len) {
        i++;
      }
      continue;
    }
    if (c == '\'') {
      i++;
      while (i < len && s[i] != '\'' && s[i] != '\n') {
        if (s[i] == '\\' && i + 1 < len) {
          i++;
        }
        i++;
      }
      if (i < len) {
        i++;
      }
      continue;
    }
    if (c == '/' && i + 1 < len && s[i + 1] == '/') {
      while (i < len && s[i] != '\n') {
        i++;
      }
      continue;
    }
    if (c == '/' && i + 1 < len && s[i + 1] == '*') {
      size_t j = i + 2;
      int closed = 0;
      while (j + 1 < len) {
        if (s[j] == '*' && s[j + 1] == '/') {
          closed = 1;
          break;
        }
        j++;
      }
      if (!closed) {
        return (size_t)-1;
      }
      i = j + 2;
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
static char *rv_xml_attr(const char *text, size_t start, size_t end,
    const char *attr) {
  size_t i = start;
  size_t alen = strlen(attr);
  while (i < end) {
    size_t ns = 0;
    size_t ne = 0;
    while (i < end && (text[i] == ' ' || text[i] == '\t' ||
        text[i] == '\n' || text[i] == '\r')) {
      i++;
    }
    if (i >= end || text[i] == '/') {
      return NULL;
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
    if (i < end && text[i] == '=') {
      char quote = '\0';
      size_t vs = 0;
      size_t ve = 0;
      i++;
      while (i < end && (text[i] == ' ' || text[i] == '\t' ||
          text[i] == '\n' || text[i] == '\r')) {
        i++;
      }
      if (i < end && (text[i] == '"' || text[i] == '\'')) {
        quote = text[i];
        vs = i + 1;
        ve = vs;
        while (ve < end && text[ve] != quote) {
          ve++;
        }
        if (ve < end) {
          if (ne - ns == alen && memcmp(text + ns, attr, alen) == 0) {
            char *out = ccb_xmalloc(ve - vs + 1);
            memcpy(out, text + vs, ve - vs);
            out[ve - vs] = '\0';
            return out;
          }
          i = ve + 1;
          continue;
        }
      }
      return NULL;
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
    char quote = '\0';
    int found = 0;
    size_t k = 0;
    if (i + 4 <= len && memcmp(text + i, "<!--", 4) == 0) {
      size_t j = 0;
      for (j = i + 4; j + 3 <= len; j++) {
        if (memcmp(text + j, "-->", 3) == 0) {
          i = j + 3;
          break;
        }
      }
      if (j + 3 > len) {
        break;
      }
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
    for (k = i + 1; k < len; k++) {
      if (quote != '\0') {
        if (text[k] == quote) {
          quote = '\0';
        }
        continue;
      }
      if (text[k] == '"' || text[k] == '\'') {
        quote = text[k];
      } else if (text[k] == '>') {
        gt = k;
        found = 1;
        break;
      }
    }
    if (!found) {
      break;
    }
    ns = i + 1;
    ne = ns;
    while (ne < gt && text[ne] != ' ' && text[ne] != '\t' &&
        text[ne] != '\n' && text[ne] != '\r' && text[ne] != '/' &&
        text[ne] != '>') {
      ne++;
    }
    local = text + ns;
    local_len = ne - ns;
    {
      size_t d = 0;
      for (d = 0; d < local_len; d++) {
        if (local[d] == '.') {
          local = text + ns + d + 1;
          local_len = ne - (ns + d + 1);
        }
      }
    }
    if (local_len >= 6 &&
        memcmp(local + local_len - 6, "Button", 6) == 0) {
      char *id = rv_xml_attr(text, ns, gt, "android:id");
      char *raw_text = rv_xml_attr(text, ns, gt, "android:text");
      char *method = rv_xml_attr(text, ns, gt, "android:onClick");
      if (id != NULL) {
        const char *slash = strrchr(id, '/');
        const char *entry = slash != NULL ? slash + 1 : id;
        char *label = NULL;
        if (raw_text != NULL && strncmp(raw_text, "@string/", 8) == 0) {
          const char *resolved = rv_strings_find(strings,
            raw_text + 8);
          label = ccb_xstrdup(resolved != NULL ? resolved : raw_text);
        } else if (raw_text != NULL) {
          label = ccb_xstrdup(raw_text);
        } else {
          label = ccb_xstrdup(entry);
        }
        if (layout->nbuttons == layout->capbuttons) {
          size_t ncap = layout->capbuttons == 0 ? 8 :
            layout->capbuttons * 2;
          layout->buttons = ccb_xrealloc(layout->buttons,
            ncap * sizeof(*layout->buttons));
          layout->capbuttons = ncap;
        }
        layout->buttons[layout->nbuttons].id =
          ccb_xstrdup(entry);
        layout->buttons[layout->nbuttons].label = label;
        layout->buttons[layout->nbuttons].method = method;
        layout->nbuttons++;
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

static void rv_wire_skip_junk(rv_wire_cursor *c) {
  for (;;) {
    while (c->pos < c->len && (c->text[c->pos] == ' ' ||
        c->text[c->pos] == '\t' || c->text[c->pos] == '\n' ||
        c->text[c->pos] == '\r')) {
      c->pos++;
    }
    if (c->pos + 4 <= c->len &&
        memcmp(c->text + c->pos, "<!--", 4) == 0) {
      size_t j = 0;
      for (j = c->pos + 4; j + 3 <= c->len; j++) {
        if (memcmp(c->text + j, "-->", 3) == 0) {
          break;
        }
      }
      if (j + 3 > c->len) {
        c->pos = c->len;
        return;
      }
      c->pos = j + 3;
      continue;
    }
    if (c->pos + 2 <= c->len &&
        memcmp(c->text + c->pos, "<?", 2) == 0) {
      size_t j = c->pos + 2;
      while (j + 1 < c->len &&
          !(c->text[j] == '?' && c->text[j + 1] == '>')) {
        j++;
      }
      c->pos = j + 1 < c->len ? j + 2 : c->len;
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

// Strip one trailing "Activity" or "Fragment" (a bare "Activity" is
// a name, not a suffix). Malloc'd.
static char *rv_strip_suffix(const char *name) {
  size_t n = strlen(name);
  if (n > 8 && strcmp(name + n - 8, "Activity") == 0) {
    return ccb_xstrndup(name, n - 8);
  }
  if (n > 8 && strcmp(name + n - 8, "Fragment") == 0) {
    return ccb_xstrndup(name, n - 8);
  }
  return ccb_xstrdup(name);
}

// Body transforms, applied to an extracted lambda/member-fun body
// before it is embedded in the synthetic flow text:
//   1. findViewById<...>(R.id.x) / findViewById(R.id.x) -> x
//   2. `val v = Intent(this, C::class.java)` + startActivity(v)
//      -> startActivity(Intent(this, C::class.java))
//   3. CActivity::class.java / CFragment::class.java -> C::class.java
// The result is malloc'd.
static char *rv_transform_body(const char *body, size_t len) {
  ccb_str out;
  size_t i = 0;
  // Pass 1+2 are fused: scan left to right, rewriting findViewById
  // spans and startActivity(var) spans as they are found. Intent
  // val bindings are collected first (a val precedes its use).
  typedef struct {
    char *var;
    char *cls;
  } rv_intent_var;
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
    p = use + 3;
    {
      size_t q = rv_skip_ws(body, len, p);
      size_t ve = 0;
      char *var = NULL;
      size_t e = 0;
      if (q >= len || !rv_is_id_start(body[q])) {
        continue;
      }
      ve = rv_ident_end(body, len, q);
      var = ccb_xstrndup(body + q, ve - q);
      e = rv_skip_ws(body, len, ve);
      if (e >= len || body[e] != '=') {
        free(var);
        continue;
      }
      e = rv_skip_ws(body, len, e + 1);
      if (!rv_at_word(body, len, e, "Intent")) {
        free(var);
        continue;
      }
      e = rv_skip_ws(body, len, e + 6);
      if (e >= len || body[e] != '(') {
        free(var);
        continue;
      }
      {
        size_t cls_start = 0;
        size_t cls_end = 0;
        e = rv_skip_ws(body, len, e + 1);
        if (!rv_at_word(body, len, e, "this")) {
          free(var);
          continue;
        }
        e = rv_skip_ws(body, len, e + 4);
        if (e >= len || body[e] != ',') {
          free(var);
          continue;
        }
        e = rv_skip_ws(body, len, e + 1);
        if (e >= len || !rv_is_id_start(body[e])) {
          free(var);
          continue;
        }
        cls_start = e;
        cls_end = rv_ident_end(body, len, e);
        if (cls_end + 12 > len ||
            memcmp(body + cls_end, "::class.java", 12) != 0) {
          free(var);
          continue;
        }
        if (nvars == capvars) {
          size_t ncap = capvars == 0 ? 4 : capvars * 2;
          vars = ccb_xrealloc(vars, ncap * sizeof(*vars));
          capvars = ncap;
        }
        vars[nvars].var = var;
        vars[nvars].cls = ccb_xstrndup(body + cls_start,
          cls_end - cls_start);
        nvars++;
        p = cls_end + 12;
      }
    }
  }
  ccb_str_init(&out);
  while (i < len) {
    // findViewById<...>(R.id.x) or findViewById(R.id.x) -> x.
    if (rv_at_word(body, len, i, "findViewById")) {
      size_t j = rv_skip_ws(body, len, i + 12);
      size_t close = (size_t)-1;
      size_t id_start = 0;
      size_t id_end = 0;
      if (j < len && body[j] == '<') {
        size_t gt = rv_match(body, len, j, '<', '>');
        if (gt != (size_t)-1) {
          j = rv_skip_ws(body, len, gt + 1);
        } else {
          j = len;
        }
      }
      if (j < len && body[j] == '(') {
        size_t k = rv_skip_ws(body, len, j + 1);
        if (k + 5 < len && memcmp(body + k, "R.id.", 5) == 0 &&
            rv_is_id_start(body[k + 5])) {
          id_start = k + 5;
          id_end = rv_ident_end(body, len, id_start);
          close = rv_match(body, len, j, '(', ')');
        }
      }
      if (close != (size_t)-1) {
        ccb_str_append_n(&out, body + id_start, id_end - id_start);
        i = close + 1;
        continue;
      }
    }
    // startActivity(var) where var held Intent(this, C::class.java).
    if (rv_at_word(body, len, i, "startActivity")) {
      size_t j = rv_skip_ws(body, len, i + 13);
      size_t v = 0;
      size_t k = 0;
      if (j < len && body[j] == '(') {
        v = rv_skip_ws(body, len, j + 1);
        if (v < len && rv_is_id_start(body[v])) {
          size_t ve = rv_ident_end(body, len, v);
          size_t after = rv_skip_ws(body, len, ve);
          if (after < len && body[after] == ')') {
            for (k = 0; k < nvars; k++) {
              if (strlen(vars[k].var) == ve - v &&
                  memcmp(vars[k].var, body + v, ve - v) == 0) {
                // Strip the Activity/Fragment suffix here too, so
                // the rewritten Intent names the same screen as
                // the direct form below (Detail, not
                // DetailActivity) and dedups against sketch edges.
                char *stripped = rv_strip_suffix(vars[k].cls);
                ccb_str_append(&out,
                  "startActivity(Intent(this, ");
                ccb_str_append(&out, stripped);
                ccb_str_append(&out, "::class.java))");
                free(stripped);
                i = after + 1;
                break;
              }
            }
            if (k < nvars) {
              continue;
            }
          }
        }
      }
    }
    // CActivity::class.java / CFragment::class.java -> C::class.java.
    if (rv_is_id_start(body[i])) {
      size_t e = rv_ident_end(body, len, i);
      if (e + 12 <= len && memcmp(body + e, "::class.java", 12) == 0) {
        size_t n = e - i;
        size_t keep = n;
        if (n > 8 && memcmp(body + i + n - 8, "Activity", 8) == 0) {
          keep = n - 8;
        } else if (n > 8 &&
            memcmp(body + i + n - 8, "Fragment", 8) == 0) {
          keep = n - 8;
        }
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
      size_t j = 0;
      int bare = 1;
      if (hit > start && s[hit - 1] == '.') {
        bare = 0;
      }
      if (bare) {
        j = rv_skip_ws(s, len, hit + strlen(a->funs[i].name));
        if (j < end && s[j] == '(' &&
            rv_body_has_nav_direct(a, a->funs[i].brace + 1,
              a->funs[i].close)) {
          return 1;
        }
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
      size_t m = 0;
      if (i + 2 < a->text_len && a->text[i + 1] == '"' &&
          a->text[i + 2] == '"') {
        m = i + 3;
        while (m + 2 < a->text_len && !(a->text[m] == '"' &&
            a->text[m + 1] == '"' && a->text[m + 2] == '"')) {
          m++;
        }
        i = m + 3 < a->text_len ? m + 3 : a->text_len;
        continue;
      }
      i++;
      while (i < a->text_len && a->text[i] != '"' &&
          a->text[i] != '\n') {
        if (a->text[i] == '\\' && i + 1 < a->text_len) {
          i++;
        }
        i++;
      }
      if (i < a->text_len) {
        i++;
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
      size_t m = i + 2;
      while (m + 1 < a->text_len && !(a->text[m] == '*' &&
          a->text[m + 1] == '/')) {
        m++;
      }
      i = m + 2 < a->text_len ? m + 2 : a->text_len;
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
    if (j < a->text_len && a->text[j] == '{') {
      size_t close = rv_match(a->text, a->text_len, j, '{', '}');
      if (close == (size_t)-1) {
        return 0;
      }
      rv_listener_add(a, id, NULL, j + 1, close, line, 0);
      rv_consumed_add(a, recv_start, close + 1);
      return 1;
    }
    if (j < a->text_len && a->text[j] == '(') {
      size_t close = rv_match(a->text, a->text_len, j, '(', ')');
      size_t k = 0;
      if (close == (size_t)-1) {
        return 0;
      }
      k = rv_skip_ws(a->text, a->text_len, j + 1);
      if (k < a->text_len && a->text[k] == '{') {
        size_t bc = rv_match(a->text, a->text_len, k, '{', '}');
        if (bc == (size_t)-1 || bc + 1 != close) {
          return 0;
        }
        rv_listener_add(a, id, NULL, k + 1, bc, line, 0);
        rv_consumed_add(a, recv_start, close + 1);
        return 1;
      }
      if (k + 1 < a->text_len && a->text[k] == ':' &&
          a->text[k + 1] == ':' &&
          rv_is_id_start(a->text[k + 2])) {
        size_t ne = rv_ident_end(a->text, a->text_len, k + 2);
        char *name = ccb_xstrndup(a->text + k + 2, ne - (k + 2));
        rv_listener_add(a, id, name, 0, 0, line, 0);
        free(name);
        rv_consumed_add(a, recv_start, close + 1);
        return 1;
      }
      if (k < a->text_len && rv_is_id_start(a->text[k])) {
        size_t ne = rv_ident_end(a->text, a->text_len, k);
        size_t f = 0;
        int is_fun = 0;
        for (f = 0; f < a->nfuns; f++) {
          if (strlen(a->funs[f].name) == ne - k &&
              memcmp(a->funs[f].name, a->text + k, ne - k) == 0) {
            is_fun = 1;
            break;
          }
        }
        if (is_fun) {
          char *name = ccb_xstrndup(a->text + k, ne - k);
          rv_listener_add(a, id, name, 0, 0, line, 0);
          free(name);
          rv_consumed_add(a, recv_start, close + 1);
          return 1;
        }
      }
      return 0;
    }
    return 0;
  }
  if (rv_at_word(a->text, a->text_len, i, "onClick")) {
    size_t j = rv_skip_ws(a->text, a->text_len, i + 7);
    if (j < a->text_len && a->text[j] == '=') {
      size_t k = 0;
      j = rv_skip_ws(a->text, a->text_len, j + 1);
      if (j < a->text_len && a->text[j] == '{') {
        size_t close = rv_match(a->text, a->text_len, j, '{', '}');
        if (close == (size_t)-1) {
          return 0;
        }
        rv_listener_add(a, id, NULL, j + 1, close, line, 1);
        rv_consumed_add(a, recv_start, close + 1);
        return 1;
      }
      if (j + 1 < a->text_len && a->text[j] == ':' &&
          a->text[j + 1] == ':' &&
          rv_is_id_start(a->text[j + 2])) {
        size_t ne = rv_ident_end(a->text, a->text_len, j + 2);
        char *name = ccb_xstrndup(a->text + j + 2, ne - (j + 2));
        rv_listener_add(a, id, name, 0, 0, line, 1);
        free(name);
        rv_consumed_add(a, recv_start, ne);
        return 1;
      }
      k = j;
      if (k < a->text_len && rv_is_id_start(a->text[k])) {
        size_t ne = rv_ident_end(a->text, a->text_len, k);
        size_t f = 0;
        int is_fun = 0;
        for (f = 0; f < a->nfuns; f++) {
          if (strlen(a->funs[f].name) == ne - k &&
              memcmp(a->funs[f].name, a->text + k, ne - k) == 0) {
            is_fun = 1;
            break;
          }
        }
        if (is_fun) {
          char *name = ccb_xstrndup(a->text + k, ne - k);
          rv_listener_add(a, id, name, 0, 0, line, 1);
          free(name);
          rv_consumed_add(a, recv_start, ne);
          return 1;
        }
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
      size_t close = rv_match(a->text, a->text_len, j, '{', '}');
      if (close != (size_t)-1 && close < end &&
          rv_body_has_nav(a, j + 1, close)) {
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
        ccb_str_append(&msg, "unattributed navigation: click "
          "handler for '");
        if (re - rs > 48) {
          ccb_str_append_n(&msg, a->text + rs, 48);
          ccb_str_append(&msg, "...");
        } else {
          ccb_str_append_n(&msg, a->text + rs, re - rs);
        }
        ccb_str_append(&msg, "' has no literal view id, so its "
          "navigation is not recorded as an edge from any button; "
          "it is reported here so it is not silently dropped");
        rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "",
          a->file, rv_line_of(a->text, hit));
        ccb_str_free(&msg);
      }
      continue;
    }
    if (j < a->text_len && a->text[j] == '(') {
      size_t close = rv_match(a->text, a->text_len, j, '(', ')');
      size_t k = 0;
      if (close == (size_t)-1) {
        continue;
      }
      k = rv_skip_ws(a->text, a->text_len, j + 1);
      if (k < a->text_len && a->text[k] == '{') {
        size_t bc = rv_match(a->text, a->text_len, k, '{', '}');
        if (bc != (size_t)-1 && bc < end &&
            rv_body_has_nav(a, k + 1, bc)) {
          ccb_str msg;
          ccb_str_init(&msg);
          ccb_str_append(&msg, "unattributed navigation: click "
            "handler with no literal view id has navigation that "
            "is not recorded as an edge from any button; it is "
            "reported here so it is not silently dropped");
          rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "",
            a->file, rv_line_of(a->text, hit));
          ccb_str_free(&msg);
        }
      }
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
      if (n == cap) {
        size_t ncap = cap == 0 ? 8 : cap * 2;
        names = ccb_xrealloc(names, ncap * sizeof(*names));
        cap = ncap;
      }
      names[n++] = ccb_xstrdup(a->listeners[li].method);
    }
    if (a->listeners[li].method == NULL) {
      size_t f = 0;
      for (f = 0; f < a->nfuns; f++) {
        if (!rv_name_in(names, n, a->funs[f].name) &&
            rv_body_refers(a, a->listeners[li].body_start,
              a->listeners[li].body_end, a->funs[f].name)) {
          if (n == cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            names = ccb_xrealloc(names, ncap * sizeof(*names));
            cap = ncap;
          }
          names[n++] = ccb_xstrdup(a->funs[f].name);
        }
      }
    }
  }
  for (k = 0; k < nonclick; k++) {
    if (!rv_name_in(names, n, onclick_methods[k])) {
      if (n == cap) {
        size_t ncap = cap == 0 ? 8 : cap * 2;
        names = ccb_xrealloc(names, ncap * sizeof(*names));
        cap = ncap;
      }
      names[n++] = ccb_xstrdup(onclick_methods[k]);
    }
  }
  changed = 1;
  while (changed) {
    changed = 0;
    for (k = 0; k < n; k++) {
      size_t f = 0;
      for (f = 0; f < a->nfuns; f++) {
        if (strcmp(a->funs[f].name, names[k]) != 0) {
          continue;
        }
        {
          size_t g = 0;
          for (g = 0; g < a->nfuns; g++) {
            if (!rv_name_in(names, n, a->funs[g].name) &&
                rv_body_refers(a, a->funs[f].brace + 1,
                  a->funs[f].close, a->funs[g].name)) {
              if (n == cap) {
                size_t ncap = cap == 0 ? 8 : cap * 2;
                names = ccb_xrealloc(names,
                  ncap * sizeof(*names));
                cap = ncap;
              }
              names[n++] = ccb_xstrdup(a->funs[g].name);
              changed = 1;
            }
          }
        }
      }
    }
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
    brace = ne;
    while (brace < len && text[brace] != '{') {
      if (text[brace] == '(') {
        size_t m = rv_match(text, len, brace, '(', ')');
        if (m == (size_t)-1) {
          break;
        }
        brace = m + 1;
        continue;
      }
      if (text[brace] == ';' || text[brace] == '\n') {
        break;
      }
      brace++;
    }
    if (brace >= len || text[brace] != '{') {
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
      {
        size_t p = rv_skip_ws(text, len, fe);
        if (p < len && text[p] == '(') {
          size_t m = rv_match(text, len, p, '(', ')');
          if (m != (size_t)-1) {
            oc_brace = rv_skip_ws(text, len, m + 1);
            if (oc_brace < len && text[oc_brace] == '{') {
              oc_close = rv_match(text, len, oc_brace, '{', '}');
              if (oc_close != (size_t)-1) {
                a.oncreate_brace = oc_brace;
                a.oncreate_close = oc_close;
              }
            }
          }
        }
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
      size_t p = rv_skip_ws(text, len, sv + 14);
      if (p < len && text[p] == '(') {
        size_t k = rv_skip_ws(text, len, p + 1);
        if (k + 9 < len && memcmp(text + k, "R.layout.", 9) == 0 &&
            rv_is_id_start(text[k + 9])) {
          size_t ls = k + 9;
          size_t le = rv_ident_end(text, len, ls);
          a.layout = ccb_xstrndup(text + ls, le - ls);
        }
      }
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
    if (strstr(p, "android.intent.action.MAIN") != NULL &&
        strstr(p, "android.intent.category.LAUNCHER") != NULL) {
      const char *n = strstr(p, "android:name=\"");
      if (n != NULL && n < end) {
        const char *q = NULL;
        char *norm = NULL;
        n += 14;
        q = strchr(n, '"');
        if (q != NULL && q < end) {
          name = ccb_xstrndup(n, (size_t)(q - n));
          norm = pv_normalize_class(package, name);
          free(name);
          free(text);
          if (norm == NULL) {
            return NULL;
          }
          name = rv_strip_suffix(norm);
          free(norm);
          return name;
        }
      }
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
  {
    // @string table: [preview] resources, else <res>/values.
    char *res_dir = NULL;
    char *spath = NULL;
    if (cfg->preview_resources != NULL) {
      res_dir = ccb_xstrdup(cfg->preview_resources);
    } else if (cfg->res != NULL) {
      res_dir = ccb_path_join(cfg->res, "values");
    }
    if (res_dir != NULL) {
      spath = ccb_path_join(res_dir, "strings.xml");
      rv_strings_parse_file(&strings, spath);
      free(spath);
      free(res_dir);
    }
  }
  {
    // Layout buttons, from the same layout inputs Slice 3 uses.
    char *layout_path = pv_preview_layout_path(cfg);
    char **layout_files = NULL;
    size_t nlayout_files = 0;
    size_t li = 0;
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
    for (li = 0; li < nlayout_files; li++) {
      char *text = NULL;
      size_t len = 0;
      char *base = ccb_basename(layout_files[li]);
      size_t bn = strlen(base);
      char *lname = NULL;
      if (bn <= 4 || strcmp(base + bn - 4, ".xml") != 0) {
        free(base);
        continue;
      }
      lname = ccb_xstrndup(base, bn - 4);
      free(base);
      if (ccb_read_file(layout_files[li], &text, &len) == CCB_OK) {
        rv_scan_layout_file(text, len, lname, layout_files[li],
          &layouts, &nlayouts, &strings);
        free(text);
      }
      free(lname);
    }
    ccb_free_str_array(layout_files, nlayout_files);
  }
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
      rv_activity *a = &acts[ai];
      char **emitted = NULL;
      size_t nemitted = 0;
      size_t ei = 0;
      rv_layout *layout = NULL;
      rv_warn_unresolved_listeners(a, diags);
      emitted = rv_emitted_funs(a, onclick_methods, nonclick, diags,
        &nemitted);
      if (a->layout != NULL) {
        layout = rv_layout_find(layouts, nlayouts, a->layout);
      }
      if (nsyn == capsyn) {
        size_t ncap = capsyn == 0 ? 4 : capsyn * 2;
        syn_labels = ccb_xrealloc(syn_labels,
          ncap * sizeof(*syn_labels));
        syn_texts = ccb_xrealloc(syn_texts,
          ncap * sizeof(*syn_texts));
        syn_layouts = ccb_xrealloc(syn_layouts,
          ncap * sizeof(*syn_layouts));
        capsyn = ncap;
      }
      syn_labels[nsyn] = kt_files[i];
      syn_texts[nsyn] = rv_synth_activity(a, layout, emitted,
        nemitted);
      syn_layouts[nsyn] = a->layout;
      nsyn++;
      for (ei = 0; ei < nemitted; ei++) {
        if (nemitted_all == capemitted_all) {
          size_t ncap = capemitted_all == 0 ? 8 : capemitted_all * 2;
          all_emitted = ccb_xrealloc(all_emitted,
            ncap * sizeof(*all_emitted));
          capemitted_all = ncap;
        }
        all_emitted[nemitted_all++] = ccb_xstrdup(emitted[ei]);
        free(emitted[ei]);
      }
      free(emitted);
    }
    free(text);
  }
  // A listener method reference with no definition in any scanned
  // Activity (a top-level fun, or a typo) records no edge: warn
  // once per method, so its navigation is visible, not silent.
  // (An android:onClick method with no matching fun stays silent,
  // like Slice 3: the layout, not a button, names it.)
  {
    size_t ai = 0;
    for (ai = 0; ai < nacts; ai++) {
      size_t li = 0;
      for (li = 0; li < acts[ai].nlisteners; li++) {
        const char *method = acts[ai].listeners[li].method;
        size_t k = 0;
        int found = 0;
        if (method == NULL) {
          continue;
        }
        for (k = 0; k < nemitted_all; k++) {
          if (strcmp(all_emitted[k], method) == 0) {
            found = 1;
            break;
          }
        }
        if (!found) {
          ccb_str msg;
          ccb_str_init(&msg);
          ccb_str_append(&msg, "unattributed navigation: click "
            "handler '");
          ccb_str_append(&msg, method);
          ccb_str_append(&msg, "' has no definition in the scanned "
            "Activities, so its navigation is not recorded as an "
            "edge; it is reported here so it is not silently "
            "dropped");
          rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "",
            acts[ai].file, acts[ai].listeners[li].line);
          ccb_str_free(&msg);
          if (nemitted_all == capemitted_all) {
            size_t ncap = capemitted_all == 0 ? 8 :
              capemitted_all * 2;
            all_emitted = ccb_xrealloc(all_emitted,
              ncap * sizeof(*all_emitted));
            capemitted_all = ncap;
          }
          all_emitted[nemitted_all++] = ccb_xstrdup(method);
        }
      }
    }
  }
  if (nsyn > 0) {
    // Bindings: the config's [preview.bindings] first (explicit
    // wins), then one real binding per layout view id (first wins
    // across layouts, in sorted file order).
    char **bind_keys = NULL;
    char **bind_vals = NULL;
    size_t nbind = 0;
    size_t li = 0;
    size_t bi = 0;
    pv_fun_index *index = NULL;
    for (bi = 0; bi < cfg->preview_binding_count; bi++) {
      bind_keys = ccb_xrealloc(bind_keys,
        (nbind + 1) * sizeof(*bind_keys));
      bind_vals = ccb_xrealloc(bind_vals,
        (nbind + 1) * sizeof(*bind_vals));
      bind_keys[nbind] = cfg->preview_binding_keys[bi];
      bind_vals[nbind] = cfg->preview_binding_values[bi];
      nbind++;
    }
    for (li = 0; li < nlayouts; li++) {
      for (bi = 0; bi < layouts[li]->nbuttons; bi++) {
        const char *id = layouts[li]->buttons[bi].id;
        const char *label = layouts[li]->buttons[bi].label;
        int dup = 0;
        size_t k = 0;
        for (k = 0; k < nbind; k++) {
          if (strcmp(bind_keys[k], id) == 0) {
            dup = 1;
            break;
          }
        }
        if (dup) {
          continue;
        }
        bind_keys = ccb_xrealloc(bind_keys,
          (nbind + 1) * sizeof(*bind_keys));
        bind_vals = ccb_xrealloc(bind_vals,
          (nbind + 1) * sizeof(*bind_vals));
        bind_keys[nbind] = ccb_arena_strdup(arena, id);
        bind_vals[nbind] = ccb_arena_strdup(arena, label);
        nbind++;
      }
    }
    index = pv_fun_index_build(arena, syn_labels, syn_texts, nsyn);
    for (i = 0; i < nsyn; i++) {
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
