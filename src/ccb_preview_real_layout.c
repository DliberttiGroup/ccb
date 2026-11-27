// ccb_preview_real_layout: layout XML parsing + layout-derived wireframe.
// Split from ccb_preview_real.c (Chunk 2).

#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include <ctype.h>
#include "ccb_preview_real_internal.h"

// ---------- layout XML ----------

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

rv_layout *rv_layout_find(rv_layout **layouts, size_t nlayouts,
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

void rv_scan_layout_file(const char *text, size_t len,
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
pv_box *rv_layout_wireframe(ccb_arena *arena,
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
