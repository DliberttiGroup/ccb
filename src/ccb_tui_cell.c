// ccb_tui_cell: the cell, UTF-8 codec, and the width table.
#include "ccb_tui_cell.h"

ccb_tui_color ccb_tui_color_default(void) {
  ccb_tui_color c;
  c.kind = CCB_TUI_COLOR_DEFAULT;
  c.idx = 0;
  c.r = 0;
  c.g = 0;
  c.b = 0;
  return c;
}

ccb_tui_color ccb_tui_color_ansi(unsigned idx) {
  ccb_tui_color c = ccb_tui_color_default();
  c.kind = CCB_TUI_COLOR_ANSI;
  c.idx = idx > 15 ? 15 : (uint8_t)idx;
  return c;
}

ccb_tui_color ccb_tui_color_256(unsigned idx) {
  ccb_tui_color c = ccb_tui_color_default();
  c.kind = CCB_TUI_COLOR_256;
  c.idx = idx > 255 ? 255 : (uint8_t)idx;
  return c;
}

ccb_tui_color ccb_tui_color_rgb(unsigned r, unsigned g, unsigned b) {
  ccb_tui_color c = ccb_tui_color_default();
  c.kind = CCB_TUI_COLOR_RGB;
  c.r = r > 255 ? 255 : (uint8_t)r;
  c.g = g > 255 ? 255 : (uint8_t)g;
  c.b = b > 255 ? 255 : (uint8_t)b;
  return c;
}

int ccb_tui_color_equal(ccb_tui_color a, ccb_tui_color b) {
  return a.kind == b.kind && a.idx == b.idx && a.r == b.r &&
    a.g == b.g && a.b == b.b;
}

ccb_tui_cell ccb_tui_cell_blank(void) {
  ccb_tui_cell c;
  memset(&c, 0, sizeof(c));
  c.text[0] = ' ';
  c.width = 1;
  c.fg = ccb_tui_color_default();
  c.bg = ccb_tui_color_default();
  return c;
}

int ccb_tui_cell_equal(const ccb_tui_cell *a, const ccb_tui_cell *b) {
  if (a == NULL || b == NULL) {
    return a == b;
  }
  return a->width == b->width && a->mods == b->mods &&
    memcmp(a->text, b->text, sizeof(a->text)) == 0 &&
    ccb_tui_color_equal(a->fg, b->fg) &&
    ccb_tui_color_equal(a->bg, b->bg);
}

int ccb_tui_cell_is_blank(const ccb_tui_cell *c) {
  if (c == NULL || c->width != 1 || c->mods != 0) {
    return 0;
  }
  return c->text[0] == ' ' && c->text[1] == '\0' &&
    c->fg.kind == CCB_TUI_COLOR_DEFAULT &&
    c->bg.kind == CCB_TUI_COLOR_DEFAULT;
}

size_t ccb_tui_utf8_decode(const char *s, size_t n, uint32_t *cp) {
  unsigned char c = 0;
  size_t len = 0;
  uint32_t min = 0;
  uint32_t v = 0;
  size_t i = 0;
  if (s == NULL || cp == NULL || n == 0) {
    return 0;
  }
  c = (unsigned char)s[0];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    len = 2;
    v = c & 0x1F;
    min = 0x80;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    v = c & 0x0F;
    min = 0x800;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
    v = c & 0x07;
    min = 0x10000;
  } else {
    return (size_t)-1;
  }
  if (n < len) {
    return 0;
  }
  for (i = 1; i < len; i++) {
    unsigned char cc = (unsigned char)s[i];
    if ((cc & 0xC0) != 0x80) {
      return (size_t)-1;
    }
    v = (v << 6) | (cc & 0x3F);
  }
  if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
    return (size_t)-1;
  }
  *cp = v;
  return len;
}

size_t ccb_tui_utf8_encode(uint32_t cp, char out[4]) {
  if (out == NULL) {
    return 0;
  }
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      return 0;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp > 0x10FFFF) {
    return 0;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// Width ranges, sorted by first codepoint for binary search.
// Width 0: controls, combining marks, format chars, jamo.
// Width 2: East Asian Wide + Fullwidth (CJK, fullwidth forms,
// most emoji). Everything not listed is width 1, including
// East Asian Ambiguous (treated as narrow).
typedef struct {
  uint32_t first;
  uint32_t last;
  uint8_t width;
} tui_width_range;

static const tui_width_range tui_width_table[] = {
  { 0x0000, 0x001F, 0 }, { 0x007F, 0x009F, 0 },
  { 0x0300, 0x036F, 0 }, { 0x0483, 0x0489, 0 },
  { 0x0591, 0x05BD, 0 }, { 0x0610, 0x061A, 0 },
  { 0x064B, 0x065F, 0 }, { 0x0670, 0x0670, 0 },
  { 0x06D6, 0x06DC, 0 }, { 0x0E31, 0x0E31, 0 },
  { 0x0E34, 0x0E3A, 0 }, { 0x0E47, 0x0E4E, 0 },
  { 0x1100, 0x115F, 2 }, { 0x1160, 0x11FF, 0 },
  { 0x1AB0, 0x1AFF, 0 }, { 0x1DC0, 0x1DFF, 0 },
  { 0x20D0, 0x20FF, 0 }, { 0x2E80, 0x303E, 2 },
  { 0x3041, 0x33FF, 2 }, { 0x3400, 0x4DBF, 2 },
  { 0x4E00, 0x9FFF, 2 }, { 0xA000, 0xA4CF, 2 },
  { 0xAC00, 0xD7A3, 2 }, { 0xF900, 0xFAFF, 2 },
  { 0xFE00, 0xFE0F, 0 }, { 0xFE20, 0xFE2F, 0 },
  { 0xFE30, 0xFE4F, 2 }, { 0xFF00, 0xFF60, 2 },
  { 0xFFE0, 0xFFE6, 2 }, { 0x1F300, 0x1F64F, 2 },
  { 0x1F900, 0x1F9FF, 2 }, { 0x20000, 0x2FFFD, 2 },
  { 0x30000, 0x3FFFD, 2 }, { 0xE0100, 0xE01EF, 0 },
};

int ccb_tui_char_width(uint32_t cp) {
  size_t lo = 0;
  size_t hi = CCB_ARRAY_LEN(tui_width_table);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const tui_width_range *r = &tui_width_table[mid];
    if (cp < r->first) {
      hi = mid;
    } else if (cp > r->last) {
      lo = mid + 1;
    } else {
      return r->width;
    }
  }
  return 1;
}
