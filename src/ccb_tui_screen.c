// ccb_tui_screen: the frame — a grid of cells, pure data.
#include "ccb_tui_screen.h"

struct ccb_tui_screen {
  int w;
  int h;
  ccb_tui_cell *cells; // row-major, w * h
  ccb_arena *arena;    // owns cells (and nothing else)
};

static ccb_tui_cell *tui_cell_at(ccb_tui_screen *s, int x, int y) {
  return &s->cells[(size_t)y * (size_t)s->w + (size_t)x];
}

static const ccb_tui_cell *tui_cell_at_const(
    const ccb_tui_screen *s, int x, int y) {
  return &s->cells[(size_t)y * (size_t)s->w + (size_t)x];
}

ccb_tui_screen *ccb_tui_screen_new(int w, int h) {
  ccb_tui_screen *s = NULL;
  size_t n = 0;
  size_t i = 0;
  if (w < 1 || h < 1) {
    return NULL;
  }
  s = ccb_xmalloc(sizeof(*s));
  s->w = w;
  s->h = h;
  s->arena = ccb_arena_create();
  n = (size_t)w * (size_t)h;
  s->cells = ccb_arena_alloc(s->arena, n * sizeof(ccb_tui_cell));
  for (i = 0; i < n; i++) {
    s->cells[i] = ccb_tui_cell_blank();
  }
  return s;
}

void ccb_tui_screen_free(ccb_tui_screen *s) {
  if (s == NULL) {
    return;
  }
  ccb_arena_destroy(s->arena);
  free(s);
}

int ccb_tui_screen_cols(const ccb_tui_screen *s) {
  return s == NULL ? 0 : s->w;
}

int ccb_tui_screen_rows(const ccb_tui_screen *s) {
  return s == NULL ? 0 : s->h;
}

ccb_status ccb_tui_screen_resize(ccb_tui_screen *s, int w, int h) {
  ccb_arena *na = NULL;
  ccb_tui_cell *nc = NULL;
  size_t n = 0;
  size_t i = 0;
  int cw = 0;
  int ch = 0;
  int y = 0;
  int x = 0;
  if (s == NULL || w < 1 || h < 1) {
    return CCB_ERR_USAGE;
  }
  if (w == s->w && h == s->h) {
    return CCB_OK;
  }
  // New arena first, copy the overlap, then drop the old arena:
  // the arena cannot free the old grid on its own.
  na = ccb_arena_create();
  n = (size_t)w * (size_t)h;
  nc = ccb_arena_alloc(na, n * sizeof(ccb_tui_cell));
  for (i = 0; i < n; i++) {
    nc[i] = ccb_tui_cell_blank();
  }
  cw = w < s->w ? w : s->w;
  ch = h < s->h ? h : s->h;
  for (y = 0; y < ch; y++) {
    for (x = 0; x < cw; x++) {
      nc[(size_t)y * (size_t)w + (size_t)x] =
        *tui_cell_at_const(s, x, y);
    }
  }
  ccb_arena_destroy(s->arena);
  s->arena = na;
  s->cells = nc;
  s->w = w;
  s->h = h;
  return CCB_OK;
}

void ccb_tui_screen_clear(ccb_tui_screen *s) {
  size_t n = 0;
  size_t i = 0;
  if (s == NULL) {
    return;
  }
  n = (size_t)s->w * (size_t)s->h;
  for (i = 0; i < n; i++) {
    s->cells[i] = ccb_tui_cell_blank();
  }
}

ccb_status ccb_tui_screen_copy(ccb_tui_screen *dst,
    const ccb_tui_screen *src) {
  size_t n = 0;
  size_t i = 0;
  ccb_status st = CCB_OK;
  if (dst == NULL || src == NULL) {
    return CCB_ERR_USAGE;
  }
  if (dst->w != src->w || dst->h != src->h) {
    st = ccb_tui_screen_resize(dst, src->w, src->h);
    if (st != CCB_OK) {
      return st;
    }
  }
  n = (size_t)dst->w * (size_t)dst->h;
  for (i = 0; i < n; i++) {
    dst->cells[i] = src->cells[i];
  }
  return CCB_OK;
}

const ccb_tui_cell *ccb_tui_screen_get(const ccb_tui_screen *s,
    int x, int y) {
  if (s == NULL || x < 0 || x >= s->w || y < 0 || y >= s->h) {
    return NULL;
  }
  return tui_cell_at_const(s, x, y);
}

// Write one cell's fields (text already encoded).
static void tui_set_cell(ccb_tui_screen *s, int x, int y,
    const char *text, size_t text_len, uint8_t width,
    ccb_tui_style style) {
  ccb_tui_cell *c = tui_cell_at(s, x, y);
  memset(c->text, 0, sizeof(c->text));
  memcpy(c->text, text, text_len);
  c->width = width;
  c->mods = style.mods;
  c->fg = style.fg;
  c->bg = style.bg;
}

int ccb_tui_screen_put(ccb_tui_screen *s, int x, int y, uint32_t cp,
    ccb_tui_style style) {
  int width = 0;
  char encoded[4];
  size_t n = 0;
  if (s == NULL || x < 0 || x >= s->w || y < 0 || y >= s->h) {
    return 0;
  }
  width = ccb_tui_char_width(cp);
  if (width == 0) {
    return 0;
  }
  if (width == 2 && x == s->w - 1) {
    // Wide glyph in the last column: a space (tcell's rule).
    tui_set_cell(s, x, y, " ", 1, 1, style);
    return 1;
  }
  // Clear stale wide-glyph state around the target cell.
  if (tui_cell_at(s, x, y)->width == 0 && x > 0) {
    // x is a continuation: the glyph to its left is gone.
    *tui_cell_at(s, x - 1, y) = ccb_tui_cell_blank();
  }
  if (tui_cell_at(s, x, y)->width == 2) {
    // Overwriting a wide glyph: clear its continuation.
    *tui_cell_at(s, x + 1, y) = ccb_tui_cell_blank();
  }
  if (x + 1 < s->w && tui_cell_at(s, x + 1, y)->width == 0) {
    // The continuation at x+1 belonged to the glyph at x.
    *tui_cell_at(s, x + 1, y) = ccb_tui_cell_blank();
  }
  n = ccb_tui_utf8_encode(cp, encoded);
  if (n == 0) {
    return 0;
  }
  tui_set_cell(s, x, y, encoded, n, (uint8_t)width, style);
  if (width == 2) {
    // Continuation cell: empty text, width 0, default style.
    *tui_cell_at(s, x + 1, y) = ccb_tui_cell_blank();
    tui_cell_at(s, x + 1, y)->text[0] = '\0';
    tui_cell_at(s, x + 1, y)->width = 0;
  }
  return width;
}

int ccb_tui_screen_put_text(ccb_tui_screen *s, int x, int y,
    const char *text, ccb_tui_style style) {
  int start_x = x;
  int placed = 0;
  size_t i = 0;
  size_t n = 0;
  if (s == NULL || text == NULL) {
    return 0;
  }
  n = strlen(text);
  while (i < n) {
    uint32_t cp = 0;
    size_t used = 0;
    if (text[i] == '\n') {
      y++;
      x = start_x;
      i++;
      if (y >= s->h) {
        break;
      }
      continue;
    }
    if (text[i] == '\t') {
      int next = (x / 8 + 1) * 8;
      x = next >= s->w ? s->w : next;
      i++;
      continue;
    }
    used = ccb_tui_utf8_decode(text + i, n - i, &cp);
    if (used == 0) {
      break; // truncated sequence at the end of the string
    }
    if (used == (size_t)-1) {
      if (x < s->w && y < s->h) {
        x += ccb_tui_screen_put(s, x, y, '?', style);
        placed++;
      }
      i++;
      continue;
    }
    if (x >= s->w || y >= s->h) {
      break;
    }
    x += ccb_tui_screen_put(s, x, y, cp, style);
    placed++;
    i += used;
  }
  return placed;
}

void ccb_tui_screen_box(ccb_tui_screen *s, int x, int y, int w,
    int h, ccb_tui_style style) {
  int i = 0;
  if (s == NULL || w < 1 || h < 1) {
    return;
  }
  ccb_tui_screen_put(s, x, y, 0x250C, style); // ┌
  if (w >= 2) {
    ccb_tui_screen_put(s, x + w - 1, y, 0x2510, style); // ┐
  }
  if (h >= 2) {
    ccb_tui_screen_put(s, x, y + h - 1, 0x2514, style); // └
  }
  if (w >= 2 && h >= 2) {
    ccb_tui_screen_put(s, x + w - 1, y + h - 1, 0x2518, style); // ┘
  }
  for (i = 1; i < w - 1; i++) {
    ccb_tui_screen_put(s, x + i, y, 0x2500, style); // ─
    if (h >= 2) {
      ccb_tui_screen_put(s, x + i, y + h - 1, 0x2500, style);
    }
  }
  for (i = 1; i < h - 1; i++) {
    ccb_tui_screen_put(s, x, y + i, 0x2502, style); // │
    if (w >= 2) {
      ccb_tui_screen_put(s, x + w - 1, y + i, 0x2502, style);
    }
  }
}

void ccb_tui_screen_hline(ccb_tui_screen *s, int x, int y, int len,
    ccb_tui_style style) {
  int i = 0;
  if (s == NULL) {
    return;
  }
  for (i = 0; i < len; i++) {
    ccb_tui_screen_put(s, x + i, y, 0x2500, style);
  }
}

void ccb_tui_screen_vline(ccb_tui_screen *s, int x, int y, int len,
    ccb_tui_style style) {
  int i = 0;
  if (s == NULL) {
    return;
  }
  for (i = 0; i < len; i++) {
    ccb_tui_screen_put(s, x, y + i, 0x2502, style);
  }
}

void ccb_tui_screen_fill(ccb_tui_screen *s, int x, int y, int w,
    int h, uint32_t cp, ccb_tui_style style) {
  int yy = 0;
  int xx = 0;
  if (s == NULL) {
    return;
  }
  for (yy = y; yy < y + h; yy++) {
    for (xx = x; xx < x + w; xx++) {
      ccb_tui_screen_put(s, xx, yy, cp, style);
    }
  }
}

void ccb_tui_screen_half(ccb_tui_screen *s, int x, int y,
    ccb_tui_color top, ccb_tui_color bottom) {
  ccb_tui_style style;
  if (s == NULL || x < 0 || x >= s->w || y < 0 || y >= s->h) {
    return;
  }
  style = ccb_tui_style_default();
  style.fg = top;
  style.bg = bottom;
  // '▀' U+2580, three UTF-8 bytes, width 1.
  tui_set_cell(s, x, y, "\xE2\x96\x80", 3, 1, style);
}

void ccb_tui_screen_dump(const ccb_tui_screen *s, ccb_str *out) {
  int y = 0;
  int x = 0;
  if (s == NULL || out == NULL) {
    return;
  }
  for (y = 0; y < s->h; y++) {
    ccb_str row;
    ccb_str_init(&row);
    for (x = 0; x < s->w; x++) {
      const ccb_tui_cell *c = tui_cell_at_const(s, x, y);
      if (c->width == 0) {
        continue; // continuation cell: part of the wide glyph
      }
      ccb_str_append(&row, c->text);
    }
    while (row.len > 0 && row.buf[row.len - 1] == ' ') {
      row.len--;
      row.buf[row.len] = '\0';
    }
    if (y > 0) {
      ccb_str_append_c(out, '\n');
    }
    ccb_str_append(out, row.buf);
    ccb_str_free(&row);
  }
}

int ccb_tui_screen_equal(const ccb_tui_screen *a,
    const ccb_tui_screen *b) {
  size_t n = 0;
  size_t i = 0;
  if (a == NULL || b == NULL) {
    return a == b;
  }
  if (a->w != b->w || a->h != b->h) {
    return 0;
  }
  n = (size_t)a->w * (size_t)a->h;
  for (i = 0; i < n; i++) {
    if (!ccb_tui_cell_equal(&a->cells[i], &b->cells[i])) {
      return 0;
    }
  }
  return 1;
}
