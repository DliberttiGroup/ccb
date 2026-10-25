// ccb_preview_render: wireframe renderer for one laid-out screen.
// Implements pv_render_screen from ccb_preview.h. The renderer is dumb
// by design: it draws the box tree it is given, clipped to the frame,
// and adds no layout intelligence of its own.
#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"

// One terminal cell: one Unicode codepoint (width-1 assumption, per
// the header) plus optional ANSI 256-color attributes (-1 = none).
typedef struct {
  char ch[5];
  int bg;
  int fg;
} pv_cell;

typedef struct {
  pv_cell *cells;
  long rows;
  long cols;
} pv_grid;

static pv_cell *grid_cell(pv_grid *g, long x, long y) {
  if (g == NULL || g->cells == NULL || x < 0 || y < 0 || x >= g->cols ||
      y >= g->rows) {
    return NULL;
  }
  return &g->cells[(size_t)(y * g->cols + x)];
}

static int render_type_eq(const pv_box *box, const char *type) {
  return box != NULL && box->type != NULL && strcmp(box->type, type) == 0;
}

static int render_is_leaf_type(const pv_box *box) {
  return render_type_eq(box, "text") || render_type_eq(box, "button") ||
    render_type_eq(box, "textfield") || render_type_eq(box, "list") ||
    render_type_eq(box, "image") || render_type_eq(box, "spacer");
}

static int render_is_branch(const pv_box *box) {
  return render_type_eq(box, "branch") || render_type_eq(box, "branch-entry") ||
    (box != NULL && box->is_branch);
}

static int render_is_container(const pv_box *box) {
  if (box == NULL) {
    return 0;
  }
  if (render_is_leaf_type(box)) {
    return 0;
  }
  if (render_type_eq(box, "screen") || render_type_eq(box, "column") ||
      render_type_eq(box, "row") || render_type_eq(box, "box") ||
      render_is_branch(box)) {
    return 1;
  }
  return box->nchildren > 0;
}

// Length in bytes of the UTF-8 sequence starting at s (bounded).
static size_t utf8_seq_len(const unsigned char *s, size_t remaining) {
  unsigned char c = 0;
  if (remaining == 0) {
    return 0;
  }
  c = s[0];
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    return remaining >= 2 ? 2 : 1;
  }
  if ((c & 0xF0) == 0xE0) {
    return remaining >= 3 ? 3 : 1;
  }
  if ((c & 0xF8) == 0xF0) {
    return remaining >= 4 ? 4 : 1;
  }
  return 1;
}

// Decode one UTF-8 sequence (its length from utf8_seq_len) to a
// codepoint. Returns 0 when the bytes are not valid UTF-8 (overlong,
// surrogate, out of range, or a bad continuation); the caller still
// paints such bytes as one width-1 cell, exactly as before.
static int utf8_decode(const unsigned char *s, size_t len, long *cp) {
  unsigned char c0 = 0;
  unsigned char c1 = 0;
  long v = 0;
  if (len == 0 || cp == NULL) {
    return 0;
  }
  c0 = s[0];
  if (len == 1) {
    if (c0 < 0x80) {
      *cp = c0;
      return 1;
    }
    return 0;
  }
  c1 = s[1];
  if (c1 < 0x80 || c1 > 0xBF) {
    return 0;
  }
  if (len == 2) {
    if (c0 < 0xC2 || c0 > 0xDF) {
      return 0;
    }
    *cp = ((long)(c0 & 0x1F) << 6) | (c1 & 0x3F);
    return 1;
  }
  if (len == 3) {
    unsigned char c2 = s[2];
    if (c2 < 0x80 || c2 > 0xBF) {
      return 0;
    }
    // E0 must not be overlong, ED must not encode a surrogate.
    if (c0 == 0xE0 && c1 < 0xA0) {
      return 0;
    }
    if (c0 == 0xED && c1 > 0x9F) {
      return 0;
    }
    if (c0 < 0xE0 || c0 > 0xEF) {
      return 0;
    }
    v = ((long)(c0 & 0x0F) << 12) | ((long)(c1 & 0x3F) << 6) |
      (c2 & 0x3F);
    *cp = v;
    return 1;
  }
  if (len == 4) {
    unsigned char c2 = s[2];
    unsigned char c3 = s[3];
    if (c2 < 0x80 || c2 > 0xBF || c3 < 0x80 || c3 > 0xBF) {
      return 0;
    }
    if (c0 == 0xF0 && c1 < 0x90) {
      return 0;
    }
    if (c0 == 0xF4 && c1 > 0x8F) {
      return 0;
    }
    if (c0 < 0xF0 || c0 > 0xF4) {
      return 0;
    }
    v = ((long)(c0 & 0x07) << 18) | ((long)(c1 & 0x3F) << 12) |
      ((long)(c2 & 0x3F) << 6) | (c3 & 0x3F);
    *cp = v;
    return 1;
  }
  return 0;
}

// wcwidth-style cell width: 2 for wide CJK/emoji ranges, 1 otherwise.
// This is an approximation (ambiguous-width symbols stay 1), matching
// the documented contract: wide = 2, ASCII = 1, invalid bytes = 1.
static int char_cell_width(long cp) {
  if ((cp >= 0x1100 && cp <= 0x115F) ||
      (cp >= 0x2329 && cp <= 0x232A) ||
      (cp >= 0x2E80 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) ||
      (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE10 && cp <= 0xFE19) ||
      (cp >= 0xFE30 && cp <= 0xFE6F) ||
      (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||
      (cp >= 0x1F000 && cp <= 0x1FAFF) ||
      (cp >= 0x20000 && cp <= 0x2FFFD) ||
      (cp >= 0x30000 && cp <= 0x3FFFD)) {
    return 2;
  }
  return 1;
}

// Display width of a string in terminal cells.
static long display_width(const char *s) {
  long w = 0;
  size_t len = 0;
  size_t i = 0;
  if (s == NULL) {
    return 0;
  }
  len = strlen(s);
  while (i < len) {
    size_t sl = utf8_seq_len((const unsigned char *)s + i, len - i);
    long cp = 0;
    if (sl == 0) {
      break;
    }
    if (utf8_decode((const unsigned char *)s + i, sl, &cp)) {
      w += char_cell_width(cp);
    } else {
      w += 1;
    }
    i += sl;
  }
  return w;
}

static int hex_val(char c) {
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

static int color_component(long v) {
  // round(v * 5 / 255)
  return (int)((v * 5 + 127) / 255);
}

typedef struct {
  const char *name;
  long r, g, b;
} named_color;

static const named_color kNamedColors[] = {
  { "black", 0, 0, 0 },
  { "white", 255, 255, 255 },
  { "red", 255, 0, 0 },
  { "green", 0, 128, 0 },
  { "blue", 0, 0, 255 },
  { "yellow", 255, 255, 0 },
  { "magenta", 255, 0, 255 },
  { "cyan", 0, 255, 255 },
  { "gray", 128, 128, 128 },
  { "grey", 128, 128, 128 },
  { "orange", 255, 165, 0 },
  { "purple", 128, 0, 128 },
  { "pink", 255, 192, 203 },
  { "brown", 165, 42, 42 },
  { "lime", 0, 255, 0 },
  { "navy", 0, 0, 128 },
  { "teal", 0, 128, 128 },
  { "silver", 192, 192, 192 },
  { "maroon", 128, 0, 0 },
  { "olive", 128, 128, 0 },
};

// Shared CSS color -> RGB resolution (see ccb_preview.h): hex
// #rgb/#rrggbb or the named-color table, case-insensitive names,
// surrounding spaces ignored. Anything else (including an unresolved
// @color/ literal) is unresolvable: 0, outputs untouched.
int pv_color_rgb(const char *s, long *r, long *g, long *b) {
  size_t i = 0;
  size_t n = 0;
  if (s == NULL || r == NULL || g == NULL || b == NULL) {
    return 0;
  }
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
    n--;
  }
  if (n == 0) {
    return 0;
  }
  if (s[0] == '#') {
    int d0 = 0;
    int d1 = 0;
    if (n == 4) {
      d0 = hex_val(s[1]);
      d1 = hex_val(s[2]);
      if (d0 < 0 || d1 < 0 || hex_val(s[3]) < 0) {
        return 0;
      }
      *r = d0 * 17;
      *g = d1 * 17;
      *b = hex_val(s[3]) * 17;
    } else if (n == 7) {
      int d[6];
      for (i = 0; i < 6; i++) {
        d[i] = hex_val(s[1 + i]);
        if (d[i] < 0) {
          return 0;
        }
      }
      *r = d[0] * 16 + d[1];
      *g = d[2] * 16 + d[3];
      *b = d[4] * 16 + d[5];
    } else {
      return 0;
    }
    return 1;
  }
  for (i = 0; i < CCB_ARRAY_LEN(kNamedColors); i++) {
    size_t m = strlen(kNamedColors[i].name);
    size_t k = 0;
    int eq = 1;
    if (m != n) {
      continue;
    }
    for (k = 0; k < n; k++) {
      char a = s[k];
      char bch = kNamedColors[i].name[k];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (a != bch) {
        eq = 0;
        break;
      }
    }
    if (eq) {
      *r = kNamedColors[i].r;
      *g = kNamedColors[i].g;
      *b = kNamedColors[i].b;
      return 1;
    }
  }
  return 0;
}

// CSS color -> ANSI 256-color code, or -1 when unknown. Unknown names
// produce no color and no diagnostic (per the task's renderer rules).
static int color_to_256(const char *s) {
  long r = 0;
  long g = 0;
  long b = 0;
  if (!pv_color_rgb(s, &r, &g, &b)) {
    return -1;
  }
  return 16 + 36 * color_component(r) + 6 * color_component(g) +
    color_component(b);
}

// Paint one string starting at (x, y), at most max_cells terminal
// cells, clipped to the grid. A wide char occupies 2 cells: its glyph
// sits in the first cell and the second cell is a continuation (an
// empty string, so the terminal's own wide glyph spans both cells).
// A wide char that would straddle the cell limit is not painted at
// all (never a partial wide char). Cells outside the grid still
// consume columns (so a box starting off-grid paints its on-grid
// tail correctly); a wide char straddling the left edge leaves a
// space in its on-grid continuation cell.
static void paint_str(pv_grid *g, long x, long y, const char *s,
    long max_cells, int fg) {
  size_t len = 0;
  size_t i = 0;
  long painted = 0;
  pv_cell *cell = NULL;
  size_t sl = 0;
  if (s == NULL || max_cells <= 0) {
    return;
  }
  len = strlen(s);
  while (i < len && painted < max_cells) {
    long cp = 0;
    int w = 1;
    sl = utf8_seq_len((const unsigned char *)s + i, len - i);
    if (sl == 0) {
      break;
    }
    if (utf8_decode((const unsigned char *)s + i, sl, &cp)) {
      w = char_cell_width(cp);
    }
    if (painted + w > max_cells) {
      break;
    }
    cell = grid_cell(g, x + painted, y);
    if (cell != NULL) {
      memcpy(cell->ch, s + i, sl);
      cell->ch[sl] = '\0';
      cell->fg = fg;
    }
    if (w == 2) {
      pv_cell *cont = grid_cell(g, x + painted + 1, y);
      if (cont != NULL) {
        if (cell == NULL) {
          cont->ch[0] = ' ';
          cont->ch[1] = '\0';
        } else {
          cont->ch[0] = '\0';
        }
      }
    }
    i += sl;
    painted += w;
  }
}

// Background fill, parents before children so a child's fill wins
// where rects overlap. Branch boxes are transparent: no fill.
static void paint_bg(pv_grid *g, pv_box *box, int color) {
  size_t i = 0;
  if (box == NULL || !color) {
    if (box != NULL) {
      for (i = 0; i < box->nchildren; i++) {
        paint_bg(g, box->children[i], color);
      }
    }
    return;
  }
  if (!render_is_branch(box) && box->rule != NULL && box->rule->bg != NULL) {
    int code = color_to_256(box->rule->bg);
    long yy = 0;
    long xx = 0;
    if (code >= 0) {
      for (yy = box->y; yy < box->y + box->h; yy++) {
        for (xx = box->x; xx < box->x + box->w; xx++) {
          pv_cell *cell = grid_cell(g, xx, yy);
          if (cell != NULL) {
            cell->bg = code;
          }
        }
      }
    }
  }
  for (i = 0; i < box->nchildren; i++) {
    paint_bg(g, box->children[i], color);
  }
}

// Foreground code for a box's border: its border-color, else its
// color, else none (-1). Only when color output is enabled.
static int border_fg(const pv_box *box, int color) {
  if (!color || box == NULL || box->rule == NULL) {
    return -1;
  }
  if (box->rule->border_color != NULL) {
    return color_to_256(box->rule->border_color);
  }
  if (box->rule->fg != NULL) {
    return color_to_256(box->rule->fg);
  }
  return -1;
}

// Draw a 1-cell outline around a bordered box (Unicode frame chars,
// or ASCII under --ascii). The background fill (paint_bg) already
// ran, so only the glyph and foreground are set here; the cell's
// background is preserved. Degenerate boxes (under 2x2) get none.
static void paint_border(pv_grid *g, pv_box *box, int ascii, int fg) {
  const char *tl = NULL;
  const char *tr = NULL;
  const char *bl = NULL;
  const char *br = NULL;
  const char *hch = NULL;
  const char *vch = NULL;
  long x0 = 0;
  long y0 = 0;
  long x1 = 0;
  long y1 = 0;
  long x = 0;
  long y = 0;
  pv_cell *cell = NULL;
  if (g == NULL || box == NULL || !box->has_border || box->w < 2 ||
      box->h < 2) {
    return;
  }
  if (ascii) {
    tl = "+";
    tr = "+";
    bl = "+";
    br = "+";
    hch = "-";
    vch = "|";
  } else {
    tl = "\xE2\x94\x8C";
    tr = "\xE2\x94\x90";
    bl = "\xE2\x94\x94";
    br = "\xE2\x94\x98";
    hch = "\xE2\x94\x80";
    vch = "\xE2\x94\x82";
  }
  x0 = box->x;
  y0 = box->y;
  x1 = box->x + box->w - 1;
  y1 = box->y + box->h - 1;
  // Top and bottom edges, corners first so the horizontals between
  // them never overwrite a corner.
  for (x = x0; x <= x1; x++) {
    const char *top = x == x0 ? tl : (x == x1 ? tr : hch);
    const char *bot = x == x0 ? bl : (x == x1 ? br : hch);
    cell = grid_cell(g, x, y0);
    if (cell != NULL) {
      memcpy(cell->ch, top, strlen(top) + 1);
      cell->fg = fg;
    }
    cell = grid_cell(g, x, y1);
    if (cell != NULL) {
      memcpy(cell->ch, bot, strlen(bot) + 1);
      cell->fg = fg;
    }
  }
  for (y = y0 + 1; y < y1; y++) {
    cell = grid_cell(g, x0, y);
    if (cell != NULL) {
      memcpy(cell->ch, vch, strlen(vch) + 1);
      cell->fg = fg;
    }
    cell = grid_cell(g, x1, y);
    if (cell != NULL) {
      memcpy(cell->ch, vch, strlen(vch) + 1);
      cell->fg = fg;
    }
  }
}

// Offset of a painted string within its content width under the
// box rule's text-align: left = 0, center = (cw - sw)/2,
// right = cw - sw, clamped at 0 (a string wider than the box starts
// at the content edge and clips, as before).
static long align_offset(const pv_rule *rule, long cw, long sw) {
  long off = 0;
  if (rule == NULL || !rule->has_text_align) {
    return 0;
  }
  if (rule->text_align == 1) {
    off = (cw - sw) / 2;
  } else if (rule->text_align == 2) {
    off = cw - sw;
  } else {
    return 0;
  }
  return off < 0 ? 0 : off;
}

// F1: the label's display width scaled by the rule's font-size
// bucket (see pv_scaled_text_width in the header), rounded up.
long pv_scaled_text_width(long display_width, const pv_rule *rule) {
  long num = 1;
  long den = 1;
  if (display_width <= 0) {
    return 0;
  }
  if (rule != NULL && rule->has_font_size) {
    if (rule->font_size < 14) {
      num = 4;
      den = 5;
    } else if (rule->font_size >= 24) {
      num = 5;
      den = 4;
    }
  }
  return (display_width * num + den - 1) / den;
}

static void paint_box(pv_grid *g, pv_box *box, int ascii, int color) {
  size_t i = 0;
  long cx = 0;
  long cy = 0;
  long cw = 0;
  int fg = -1;
  if (box == NULL) {
    return;
  }
  if (render_is_branch(box) || render_is_container(box)) {
    // Transparent / chrome-less, except a border outline around the
    // container box: children render inside the inset content box,
    // so they never overwrite the outline.
    if (box->has_border) {
      paint_border(g, box, ascii, border_fg(box, color));
    }
    for (i = 0; i < box->nchildren; i++) {
      paint_box(g, box->children[i], ascii, color);
    }
    return;
  }
  // A leaf paints inset by its own scaled padding (set by layout),
  // clipped to the content width: the box origin is the border, not
  // the content start. A bordered leaf additionally insets by its
  // 1-cell outline, drawn first.
  if (box->has_border) {
    paint_border(g, box, ascii, border_fg(box, color));
  }
  cx = box->x + box->pad_left_cols + box->has_border;
  cy = box->y + box->pad_top_rows + box->has_border;
  cw = box->w - box->pad_left_cols - box->pad_right_cols -
    2 * box->has_border;
  if (cw < 0) {
    cw = 0;
  }
  if (color && box->rule != NULL && box->rule->fg != NULL) {
    fg = color_to_256(box->rule->fg);
  }
  if (render_type_eq(box, "text")) {
    long off = align_offset(box->rule, cw,
      pv_scaled_text_width(display_width(box->label), box->rule));
    paint_str(g, cx + off, cy, box->label, cw - off, fg);
  } else if (render_type_eq(box, "button")) {
    ccb_str s;
    long off = 0;
    long sw = 0;
    ccb_str_init(&s);
    if (box->label != NULL) {
      ccb_str_append(&s, "[ ");
      ccb_str_append(&s, box->label);
      ccb_str_append(&s, " ]");
      // F1: only the label text scales; the "[ ]" chrome does not.
      sw = pv_scaled_text_width(display_width(box->label),
        box->rule) + 4;
    } else {
      ccb_str_append(&s, "[ ]");
      sw = 3;
    }
    off = align_offset(box->rule, cw, sw);
    paint_str(g, cx + off, cy, s.buf != NULL ? s.buf : "", cw - off, fg);
    ccb_str_free(&s);
  } else if (render_type_eq(box, "textfield")) {
    ccb_str s;
    long off = 0;
    long sw = 0;
    ccb_str_init(&s);
    ccb_str_append(&s, "[");
    if (box->label != NULL) {
      ccb_str_append(&s, box->label);
      // F1: only the label text scales; the "[]" chrome does not.
      sw = pv_scaled_text_width(display_width(box->label),
        box->rule) + 2;
      // Chunk 1: scenario content is additional, never a
      // replacement — the static label stays and the content joins
      // it as "[label: text]".
      if (box->scenario_text != NULL) {
        ccb_str_append(&s, ": ");
        ccb_str_append(&s, box->scenario_text);
        sw += 2 + pv_scaled_text_width(
          display_width(box->scenario_text), box->rule);
      }
    } else if (box->scenario_text != NULL) {
      ccb_str_append(&s, box->scenario_text);
      sw = pv_scaled_text_width(display_width(box->scenario_text),
        box->rule) + 2;
    } else {
      sw = 2;
    }
    ccb_str_append(&s, "]");
    off = align_offset(box->rule, cw, sw);
    paint_str(g, cx + off, cy, s.buf != NULL ? s.buf : "", cw - off, fg);
    ccb_str_free(&s);
  } else if (render_type_eq(box, "list")) {
    if (box->nchildren > 0) {
      // Real rows: one per child, the child's label or the "item"
      // placeholder when a child has no label. Layout sized this box
      // to exactly one row per child, so the rows fit by construction.
      size_t k = 0;
      for (k = 0; k < box->nchildren; k++) {
        pv_box *child = box->children[k];
        long ry = cy + (long)k;
        ccb_str row;
        if (ry >= box->y + box->h) {
          break;
        }
        long off = 0;
        ccb_str_init(&row);
        ccb_str_append(&row, ascii ? "* " : "\xE2\x80\xA2 ");
        if (child != NULL && child->label != NULL) {
          ccb_str_append(&row, child->label);
        } else {
          ccb_str_append(&row, "item");
        }
        off = align_offset(box->rule, cw,
          display_width(row.buf != NULL ? row.buf : ""));
        paint_str(g, cx + off, ry, row.buf != NULL ? row.buf : "",
          cw - off, fg);
        ccb_str_free(&row);
      }
    } else {
      // Unknown count (a bare List("notes") with no children): the
      // long-standing 4 placeholder rows.
      long r = 0;
      const char *row = ascii ? "* item" : "\xE2\x80\xA2 item";
      long off = align_offset(box->rule, cw, display_width(row));
      for (r = 0; r < 4; r++) {
        long ry = cy + r;
        if (ry >= box->y + box->h) {
          break;
        }
        paint_str(g, cx + off, ry, row, cw - off, fg);
      }
    }
  } else if (render_type_eq(box, "image")) {
    long my = cy + 1;
    const char *s = "[ image ]";
    long sw = 9;
    long sx = cx;
    long rem = 0;
    if (my < box->y + box->h) {
      if (cw > sw) {
        sx = cx + (cw - sw) / 2;
      }
      rem = cw - (sx - cx);
      if (rem > 0) {
        paint_str(g, sx, my, s, rem, fg);
      }
    }
  } else if (render_type_eq(box, "spacer")) {
    // Nothing to paint.
  } else {
    // Generic/unknown leaf: "[Type]", type capitalized first letter.
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "[");
    if (box->type != NULL) {
      ccb_str_append(&s, box->type);
      if (s.len >= 2 && s.buf[1] >= 'a' && s.buf[1] <= 'z') {
        s.buf[1] = (char)(s.buf[1] - 'a' + 'A');
      }
    } else {
      ccb_str_append(&s, "?");
    }
    ccb_str_append(&s, "]");
    paint_str(g, cx, cy, s.buf != NULL ? s.buf : "", cw, fg);
    ccb_str_free(&s);
  }
}

void pv_render_screen(ccb_str *out, pv_screen *screen, long frame_width,
    int ascii, int color) {
  long fw = frame_width;
  long interior = 0;
  const char *name = NULL;
  pv_box *root = NULL;
  long rows = 0;
  pv_grid g;
  long i = 0;
  long y = 0;
  long x = 0;
  const char *tl = NULL;
  const char *tr = NULL;
  const char *bl = NULL;
  const char *br = NULL;
  const char *hch = NULL;
  const char *vch = NULL;
  if (out == NULL) {
    return;
  }
  if (fw <= 0) {
    fw = 40;
  }
  if (fw < 4) {
    fw = 4;
  }
  if (fw > 1024) {
    fw = 1024;
  }
  interior = fw - 2;
  name = screen != NULL ? screen->name : NULL;
  root = screen != NULL ? screen->root : NULL;
  rows = root != NULL ? root->h : 0;
  if (rows < 0) {
    rows = 0;
  }
  if (rows > 10000) {
    rows = 10000;
  }
  if (interior > 0 && rows > 2000000 / interior) {
    rows = 2000000 / interior;
  }
  memset(&g, 0, sizeof(g));
  g.rows = rows;
  g.cols = interior;
  if (rows > 0 && interior > 0) {
    g.cells = ccb_xcalloc((size_t)(rows * interior), sizeof(pv_cell));
    for (i = 0; i < rows * interior; i++) {
      g.cells[i].ch[0] = ' ';
      g.cells[i].ch[1] = '\0';
      g.cells[i].bg = -1;
      g.cells[i].fg = -1;
    }
  }
  if (root != NULL) {
    paint_bg(&g, root, color);
    paint_box(&g, root, ascii, color);
  }
  if (ascii) {
    tl = "+";
    tr = "+";
    bl = "+";
    br = "+";
    hch = "-";
    vch = "|";
  } else {
    tl = "\xE2\x94\x8C";
    tr = "\xE2\x94\x90";
    bl = "\xE2\x94\x94";
    br = "\xE2\x94\x98";
    hch = "\xE2\x94\x80";
    vch = "\xE2\x94\x82";
  }
  // Top border, with the screen name centered when it fits.
  ccb_str_append(out, tl);
  if (name != NULL && name[0] != '\0' &&
      display_width(name) + 2 <= interior) {
    long title_len = display_width(name) + 2;
    long left = (interior - title_len) / 2;
    long right = interior - title_len - left;
    for (i = 0; i < left; i++) {
      ccb_str_append(out, hch);
    }
    ccb_str_append(out, " ");
    ccb_str_append(out, name);
    ccb_str_append(out, " ");
    for (i = 0; i < right; i++) {
      ccb_str_append(out, hch);
    }
  } else {
    for (i = 0; i < interior; i++) {
      ccb_str_append(out, hch);
    }
  }
  ccb_str_append(out, tr);
  ccb_str_append(out, "\n");
  for (y = 0; y < rows; y++) {
    ccb_str_append(out, vch);
    for (x = 0; x < interior; x++) {
      pv_cell *cell = grid_cell(&g, x, y);
      if (cell != NULL && color && (cell->bg >= 0 || cell->fg >= 0)) {
        ccb_str_append(out, "\x1b[");
        if (cell->bg >= 0) {
          ccb_str_appendf(out, "48;5;%d", cell->bg);
        }
        if (cell->fg >= 0) {
          if (cell->bg >= 0) {
            ccb_str_append(out, ";");
          }
          ccb_str_appendf(out, "38;5;%d", cell->fg);
        }
        ccb_str_append(out, "m");
        ccb_str_append(out, cell->ch);
        ccb_str_append(out, "\x1b[0m");
      } else if (cell != NULL) {
        ccb_str_append(out, cell->ch);
      } else {
        ccb_str_append(out, " ");
      }
    }
    ccb_str_append(out, vch);
    ccb_str_append(out, "\n");
  }
  ccb_str_append(out, bl);
  for (i = 0; i < interior; i++) {
    ccb_str_append(out, hch);
  }
  ccb_str_append(out, br);
  ccb_str_append(out, "\n");
  free(g.cells);
}
