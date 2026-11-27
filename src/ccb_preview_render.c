// ccb_preview_render: wireframe renderer for one laid-out screen.
// Implements pv_render_screen from ccb_preview.h. The renderer is dumb
// by design: it draws the box tree it is given, clipped to the frame,
// and adds no layout intelligence of its own.
//
// The frame is built in a ccb_tui_screen: the interior grid plus the
// outer phone frame, all as cells. The string form (pv_render_screen)
// dumps that frame as plain text, or emits per-cell SGR for the color
// form; the walk terminal loop presents the same frame through the
// tui diff (pv_render_frame_tui). One builder feeds both, so the
// string and the terminal can never drift apart.
//
// Colors live in the cells as RGB (truecolour); the string form
// quantises them to the 256-color palette at emit time, the terminal
// form to the terminal's detected level. Backgrounds are kept in a
// side map (see pv_painter), never written into cells, so a fill can
// never erase a glyph — the same order the old grid painter had.
#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_tui_cell.h"
#include "ccb_tui_screen.h"
#include "ccb_tui_style.h"

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
// Kept for display_width (layout measurements); painting itself uses
// the library's UAX #11 table, which is what the screen cells use.
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

// CSS color string -> tui RGB color. Returns 1 on success, 0 when
// the color is unresolvable (then the caller keeps the default).
static int rule_rgb(const char *s, ccb_tui_color *out) {
  long r = 0;
  long g = 0;
  long b = 0;
  if (!pv_color_rgb(s, &r, &g, &b)) {
    return 0;
  }
  *out = ccb_tui_color_rgb((unsigned)r, (unsigned)g, (unsigned)b);
  return 1;
}

// Paint state for one frame build: the tui screen, the interior size,
// and the background map (interior coords) that paint_bg fills before
// any text is painted. Backgrounds live in the map, never in cells,
// so a fill can never erase a glyph; paint_str/paint_border read the
// map for each cell they touch. The map is zero-filled, which is the
// default color (kind 0), so no fill loop is needed.
typedef struct {
  ccb_tui_screen *s;
  long cols, rows; // interior size (the frame is cols+2 by rows+2)
  ccb_tui_color *bgmap; // cols*rows, interior coords
  int ascii;
  int color;
} pv_painter;

// Background color of one interior cell, or default when out of
// bounds or color is off.
static ccb_tui_color painter_bg(pv_painter *p, long x, long y) {
  if (p == NULL || p->bgmap == NULL || !p->color || x < 0 || y < 0 ||
      x >= p->cols || y >= p->rows) {
    return ccb_tui_color_default();
  }
  return p->bgmap[(size_t)(y * p->cols + x)];
}

// Style for one interior cell: the caller's foreground over the
// background the map holds for that cell. Default everywhere when
// color is off.
static ccb_tui_style painter_style(pv_painter *p, long x, long y,
    ccb_tui_color fg) {
  ccb_tui_style st = ccb_tui_style_default();
  if (p != NULL && p->color) {
    st.fg = fg;
    st.bg = painter_bg(p, x, y);
  }
  return st;
}

// Paint one string starting at interior (x, y), at most max_cells
// terminal cells. Column accounting mirrors the old grid painter: a
// wide char occupies 2 cells; a wide char that would straddle the
// cell limit is not painted at all; cells outside the frame still
// consume columns (so a box starting off-grid paints its on-grid
// tail); an invalid UTF-8 byte or a control byte is painted as one
// raw byte, exactly as before. Widths come from the library's UAX
// #11 table, which is what the screen cells use, so accounting and
// placement can never disagree. A wide glyph in the screen's last
// column becomes a space (the library's rule); the old painter put
// the glyph there, but that case needs a box overflowing the frame
// edge, which layout never produces.
static void paint_str(pv_painter *p, long x, long y, const char *s,
    long max_cells, ccb_tui_color fg) {
  size_t len = 0;
  size_t i = 0;
  long painted = 0;
  if (p == NULL || s == NULL || max_cells <= 0) {
    return;
  }
  len = strlen(s);
  while (i < len && painted < max_cells) {
    long cp = 0;
    int w = 1;
    size_t sl = utf8_seq_len((const unsigned char *)s + i, len - i);
    long gx = 0;
    if (sl == 0) {
      break;
    }
    if (!utf8_decode((const unsigned char *)s + i, sl, &cp)) {
      // Invalid byte: one raw byte, one cell (the width test pins
      // this: "\xFF\xFEabc" survives verbatim).
      gx = x + painted;
      if (gx >= 0 && gx < p->cols && y >= 0 && y < p->rows) {
        ccb_tui_screen_put_byte(p->s, (int)(gx + 1), (int)(y + 1),
          (unsigned char)s[i], painter_style(p, gx, y, fg));
      }
      i += 1;
      painted += 1;
      continue;
    }
    w = ccb_tui_char_width((uint32_t)cp);
    if (w == 0) {
      // Width-0 codepoint: a control byte paints as one raw byte, as
      // before; a combining mark is dropped (the library's v1 rule)
      // but still consumes its column, so alignment never shifts.
      gx = x + painted;
      if (cp < 0x20 || cp == 0x7f) {
        if (gx >= 0 && gx < p->cols && y >= 0 && y < p->rows) {
          ccb_tui_screen_put_byte(p->s, (int)(gx + 1), (int)(y + 1),
            (unsigned char)s[i], painter_style(p, gx, y, fg));
        }
      }
      i += sl;
      painted += 1;
      continue;
    }
    if (painted + w > max_cells) {
      break;
    }
    gx = x + painted;
    if (gx >= 0 && gx < p->cols && y >= 0 && y < p->rows) {
      ccb_tui_screen_put(p->s, (int)(gx + 1), (int)(y + 1),
        (uint32_t)cp, painter_style(p, gx, y, fg));
    }
    i += sl;
    painted += w;
  }
}

// Background fill, parents before children so a child's fill wins
// where rects overlap. Branch boxes are transparent: no fill.
// Writes into the painter's bgmap, never into cells.
static void paint_bg(pv_painter *p, pv_box *box) {
  size_t i = 0;
  if (p == NULL || box == NULL || !p->color) {
    return;
  }
  if (!render_is_branch(box) && box->rule != NULL &&
      box->rule->bg != NULL) {
    ccb_tui_color rgb = ccb_tui_color_default();
    long yy = 0;
    long xx = 0;
    if (rule_rgb(box->rule->bg, &rgb)) {
      for (yy = box->y; yy < box->y + box->h; yy++) {
        for (xx = box->x; xx < box->x + box->w; xx++) {
          if (xx >= 0 && xx < p->cols && yy >= 0 && yy < p->rows) {
            p->bgmap[(size_t)(yy * p->cols + xx)] = rgb;
          }
        }
      }
    }
  }
  for (i = 0; i < box->nchildren; i++) {
    paint_bg(p, box->children[i]);
  }
}

// Foreground color for a box's border: its border-color, else its
// color, else default. Only when color output is enabled.
static ccb_tui_color border_color(const pv_box *box, int color) {
  ccb_tui_color rgb = ccb_tui_color_default();
  if (!color || box == NULL || box->rule == NULL) {
    return rgb;
  }
  if (box->rule->border_color != NULL) {
    if (rule_rgb(box->rule->border_color, &rgb)) {
      return rgb;
    }
    return ccb_tui_color_default();
  }
  if (box->rule->fg != NULL) {
    if (rule_rgb(box->rule->fg, &rgb)) {
      return rgb;
    }
  }
  return ccb_tui_color_default();
}

// Draw a 1-cell outline around a bordered box (Unicode frame chars,
// or ASCII under --ascii). The background comes from the map per
// cell, so the outline never erases a fill. Degenerate boxes (under
// 2x2) get none. Cells outside the interior are skipped, so a box
// overflowing the frame edge can never overwrite the outer frame.
static void paint_border(pv_painter *p, pv_box *box) {
  uint32_t tl = 0;
  uint32_t tr = 0;
  uint32_t bl = 0;
  uint32_t br = 0;
  uint32_t hch = 0;
  uint32_t vch = 0;
  long x0 = 0;
  long y0 = 0;
  long x1 = 0;
  long y1 = 0;
  long x = 0;
  long y = 0;
  ccb_tui_color fg = ccb_tui_color_default();
  if (p == NULL || box == NULL || !box->has_border || box->w < 2 ||
      box->h < 2) {
    return;
  }
  if (p->ascii) {
    tl = '+';
    tr = '+';
    bl = '+';
    br = '+';
    hch = '-';
    vch = '|';
  } else {
    tl = 0x250C;
    tr = 0x2510;
    bl = 0x2514;
    br = 0x2518;
    hch = 0x2500;
    vch = 0x2502;
  }
  fg = border_color(box, p->color);
  x0 = box->x;
  y0 = box->y;
  x1 = box->x + box->w - 1;
  y1 = box->y + box->h - 1;
  // Top and bottom edges, corners first so the horizontals between
  // them never overwrite a corner.
  for (x = x0; x <= x1; x++) {
    uint32_t top = x == x0 ? tl : (x == x1 ? tr : hch);
    uint32_t bot = x == x0 ? bl : (x == x1 ? br : hch);
    if (x >= 0 && x < p->cols && y0 >= 0 && y0 < p->rows) {
      ccb_tui_screen_put(p->s, (int)(x + 1), (int)(y0 + 1), top,
        painter_style(p, x, y0, fg));
    }
    if (x >= 0 && x < p->cols && y1 >= 0 && y1 < p->rows) {
      ccb_tui_screen_put(p->s, (int)(x + 1), (int)(y1 + 1), bot,
        painter_style(p, x, y1, fg));
    }
  }
  for (y = y0 + 1; y < y1; y++) {
    if (x0 >= 0 && x0 < p->cols && y >= 0 && y < p->rows) {
      ccb_tui_screen_put(p->s, (int)(x0 + 1), (int)(y + 1), vch,
        painter_style(p, x0, y, fg));
    }
    if (x1 >= 0 && x1 < p->cols && y >= 0 && y < p->rows) {
      ccb_tui_screen_put(p->s, (int)(x1 + 1), (int)(y + 1), vch,
        painter_style(p, x1, y, fg));
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

static void paint_box(pv_painter *p, pv_box *box) {
  size_t i = 0;
  long cx = 0;
  long cy = 0;
  long cw = 0;
  ccb_tui_color fg = ccb_tui_color_default();
  if (p == NULL || box == NULL) {
    return;
  }
  if (render_is_branch(box) || render_is_container(box)) {
    // Transparent / chrome-less, except a border outline around the
    // container box: children render inside the inset content box,
    // so they never overwrite the outline.
    if (box->has_border) {
      paint_border(p, box);
    }
    for (i = 0; i < box->nchildren; i++) {
      paint_box(p, box->children[i]);
    }
    return;
  }
  // A leaf paints inset by its own scaled padding (set by layout),
  // clipped to the content width: the box origin is the border, not
  // the content start. A bordered leaf additionally insets by its
  // 1-cell outline, drawn first.
  if (box->has_border) {
    paint_border(p, box);
  }
  cx = box->x + box->pad_left_cols + box->has_border;
  cy = box->y + box->pad_top_rows + box->has_border;
  cw = box->w - box->pad_left_cols - box->pad_right_cols -
    2 * box->has_border;
  if (cw < 0) {
    cw = 0;
  }
  if (p->color && box->rule != NULL && box->rule->fg != NULL) {
    if (!rule_rgb(box->rule->fg, &fg)) {
      fg = ccb_tui_color_default();
    }
  }
  if (render_type_eq(box, "text")) {
    long off = align_offset(box->rule, cw,
      pv_scaled_text_width(display_width(box->label), box->rule));
    paint_str(p, cx + off, cy, box->label, cw - off, fg);
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
    paint_str(p, cx + off, cy, s.buf != NULL ? s.buf : "", cw - off, fg);
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
    paint_str(p, cx + off, cy, s.buf != NULL ? s.buf : "", cw - off, fg);
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
        ccb_str_append(&row, p->ascii ? "* " : "\xE2\x80\xA2 ");
        if (child != NULL && child->label != NULL) {
          ccb_str_append(&row, child->label);
        } else {
          ccb_str_append(&row, "item");
        }
        off = align_offset(box->rule, cw,
          display_width(row.buf != NULL ? row.buf : ""));
        paint_str(p, cx + off, ry, row.buf != NULL ? row.buf : "",
          cw - off, fg);
        ccb_str_free(&row);
      }
    } else {
      // Unknown count (a bare List("notes") with no children): the
      // long-standing 4 placeholder rows.
      long r = 0;
      const char *row = p->ascii ? "* item" : "\xE2\x80\xA2 item";
      long off = align_offset(box->rule, cw, display_width(row));
      for (r = 0; r < 4; r++) {
        long ry = cy + r;
        if (ry >= box->y + box->h) {
          break;
        }
        paint_str(p, cx + off, ry, row, cw - off, fg);
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
        paint_str(p, sx, my, s, rem, fg);
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
    paint_str(p, cx, cy, s.buf != NULL ? s.buf : "", cw, fg);
    ccb_str_free(&s);
  }
}

// Frame geometry: clamped frame width and frame height (interior
// rows + 2 for the outer border), shared by the string form and the
// walk frame builder so both size from the same numbers.
void pv_render_frame_geom(pv_screen *screen, long frame_width,
    long *width_out, long *height_out) {
  long fw = frame_width;
  long rows = 0;
  pv_box *root = screen != NULL ? screen->root : NULL;
  if (fw <= 0) {
    fw = 40;
  }
  if (fw < 4) {
    fw = 4;
  }
  if (fw > 1024) {
    fw = 1024;
  }
  rows = root != NULL ? root->h : 0;
  if (rows < 0) {
    rows = 0;
  }
  if (rows > 10000) {
    rows = 10000;
  }
  if (fw - 2 > 0 && rows > 2000000 / (fw - 2)) {
    rows = 2000000 / (fw - 2);
  }
  if (width_out != NULL) {
    *width_out = fw;
  }
  if (height_out != NULL) {
    *height_out = rows + 2;
  }
}

// Paint the screen name into the top frame row, centered with one
// space on each side, or leave the plain border when the title does
// not fit. Paints raw bytes for invalid sequences, exactly as the
// old string form did (the name was appended to the output, never
// put through the grid).
static void paint_title(ccb_tui_screen *s, const char *name, long fw,
    long interior, int ascii) {
  long title_len = 0;
  long left = 0;
  long x = 0;
  size_t len = 0;
  size_t i = 0;
  uint32_t hch = 0;
  ccb_tui_style st = ccb_tui_style_default();
  if (name == NULL || name[0] == '\0') {
    return;
  }
  title_len = display_width(name) + 2;
  if (title_len > interior) {
    return;
  }
  left = (interior - title_len) / 2;
  hch = ascii ? (uint32_t)'-' : 0x2500;
  // The title span replaces dashes: left dashes, space, name,
  // space, right dashes. The dashes are painted first so the name
  // cells overwrite them.
  for (x = 1; x < fw - 1; x++) {
    ccb_tui_screen_put(s, (int)x, 0, hch, st);
  }
  x = 1 + left;
  ccb_tui_screen_put(s, (int)x, 0, ' ', st);
  x++;
  len = strlen(name);
  while (i < len) {
    long cp = 0;
    size_t sl = utf8_seq_len((const unsigned char *)name + i, len - i);
    int w = 1;
    if (sl == 0) {
      break;
    }
    if (!utf8_decode((const unsigned char *)name + i, sl, &cp)) {
      ccb_tui_screen_put_byte(s, (int)x, 0, (unsigned char)name[i],
        st);
      i += 1;
      x += 1;
      continue;
    }
    w = ccb_tui_char_width((uint32_t)cp);
    if (w == 0) {
      if (cp < 0x20 || cp == 0x7f) {
        ccb_tui_screen_put_byte(s, (int)x, 0,
          (unsigned char)name[i], st);
      }
      i += sl;
      x += 1;
      continue;
    }
    ccb_tui_screen_put(s, (int)x, 0, (uint32_t)cp, st);
    i += sl;
    x += w;
  }
  ccb_tui_screen_put(s, (int)x, 0, ' ', st);
}

// Outer phone frame: border + title. The border cells are the screen
// edges; interior painting is clipped to the interior and can never
// overwrite them.
static void paint_frame(ccb_tui_screen *s, const char *name, long fw,
    long fh, long interior, int ascii) {
  uint32_t tl = 0;
  uint32_t tr = 0;
  uint32_t bl = 0;
  uint32_t br = 0;
  uint32_t hch = 0;
  uint32_t vch = 0;
  long x = 0;
  long y = 0;
  ccb_tui_style st = ccb_tui_style_default();
  if (ascii) {
    tl = '+';
    tr = '+';
    bl = '+';
    br = '+';
    hch = '-';
    vch = '|';
  } else {
    tl = 0x250C;
    tr = 0x2510;
    bl = 0x2514;
    br = 0x2518;
    hch = 0x2500;
    vch = 0x2502;
  }
  for (x = 1; x < fw - 1; x++) {
    ccb_tui_screen_put(s, (int)x, 0, hch, st);
    ccb_tui_screen_put(s, (int)x, (int)(fh - 1), hch, st);
  }
  for (y = 1; y < fh - 1; y++) {
    ccb_tui_screen_put(s, 0, (int)y, vch, st);
    ccb_tui_screen_put(s, (int)(fw - 1), (int)y, vch, st);
  }
  ccb_tui_screen_put(s, 0, 0, tl, st);
  ccb_tui_screen_put(s, (int)(fw - 1), 0, tr, st);
  ccb_tui_screen_put(s, 0, (int)(fh - 1), bl, st);
  ccb_tui_screen_put(s, (int)(fw - 1), (int)(fh - 1), br, st);
  paint_title(s, name, fw, interior, ascii);
}

// Build the wireframe for one laid-out screen into a tui screen at
// (0,0): outer frame + title + interior grid. The screen should be
// sized from pv_render_frame_geom; anything outside it clips.
void pv_render_frame_tui(ccb_tui_screen *s, pv_screen *screen,
    long frame_width, int ascii, int color) {
  long fw = 0;
  long fh = 0;
  long interior = 0;
  long rows = 0;
  const char *name = NULL;
  pv_box *root = NULL;
  pv_painter p;
  if (s == NULL) {
    return;
  }
  pv_render_frame_geom(screen, frame_width, &fw, &fh);
  interior = fw - 2;
  rows = fh - 2;
  name = screen != NULL ? screen->name : NULL;
  root = screen != NULL ? screen->root : NULL;
  memset(&p, 0, sizeof(p));
  p.s = s;
  p.cols = interior;
  p.rows = rows;
  p.ascii = ascii;
  p.color = color;
  if (interior > 0 && rows > 0) {
    // Zero-filled: the default color is kind 0, so the map starts
    // as all-default with no fill loop.
    p.bgmap = ccb_xcalloc((size_t)(interior * rows),
      sizeof(*p.bgmap));
  }
  if (root != NULL) {
    paint_bg(&p, root);
    paint_box(&p, root);
  }
  paint_frame(s, name, fw, fh, interior, ascii);
  free(p.bgmap);
}

// Emit the frame as a string with per-cell SGR, the old color form:
// a colored cell is ESC[<bg>;<fg>m + text + ESC[0m, at the 256-color
// level. One deliberate difference from the old hand-rolled
// quantiser: near-grey colors land on the xterm grey ramp instead of
// the cube (the library's quantiser); no test asserts a grey code,
// and the two pinned colors (#ffffff -> 231, red -> 196) match.
static void render_color_string(ccb_str *out, const ccb_tui_screen *s) {
  int y = 0;
  int x = 0;
  ccb_tui_style def = ccb_tui_style_default();
  for (y = 0; y < ccb_tui_screen_rows(s); y++) {
    for (x = 0; x < ccb_tui_screen_cols(s); x++) {
      const ccb_tui_cell *c = ccb_tui_screen_get(s, x, y);
      ccb_tui_style st = ccb_tui_style_default();
      if (c == NULL || c->width == 0) {
        continue;
      }
      st.mods = c->mods;
      st.fg = c->fg;
      st.bg = c->bg;
      if (ccb_tui_style_equal(st, def)) {
        ccb_str_append(out, c->text);
        continue;
      }
      ccb_tui_sgr_diff(out, CCB_TUI_LEVEL_256, &def, &st);
      ccb_str_append(out, c->text);
      ccb_tui_sgr_reset(out);
    }
    ccb_str_append_c(out, '\n');
  }
}

void pv_render_screen(ccb_str *out, pv_screen *screen, long frame_width,
    int ascii, int color) {
  long fw = 0;
  long fh = 0;
  ccb_tui_screen *s = NULL;
  if (out == NULL) {
    return;
  }
  pv_render_frame_geom(screen, frame_width, &fw, &fh);
  s = ccb_tui_screen_new((int)fw, (int)fh);
  if (s == NULL) {
    return;
  }
  pv_render_frame_tui(s, screen, frame_width, ascii, color);
  if (!color) {
    // Plain-text dump: every row ends with the frame border, so the
    // dump's trailing-space trim changes nothing; the dump has no
    // trailing newline, so one is appended, as before.
    ccb_tui_screen_dump(s, out);
    ccb_str_append(out, "\n");
  } else {
    render_color_string(out, s);
  }
  ccb_tui_screen_free(s);
}
