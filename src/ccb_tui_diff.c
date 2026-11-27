// ccb_tui_diff: the pure renderer.
#include "ccb_tui_diff.h"

// Software cursor + emitted-style tracking for one frame.
typedef struct {
  ccb_str *out;
  ccb_tui_color_level level;
  ccb_tui_style cur; // style currently in effect on the terminal
  int cur_x;         // cursor column, -1 when unknown
  int cur_y;         // cursor row, -1 when unknown
  int w;
} tui_emit;

static void tui_emit_move(tui_emit *e, int x, int y) {
  if (e->cur_x == x && e->cur_y == y) {
    return;
  }
  if (x == 0 && y == 0) {
    ccb_str_append(e->out, "\x1b[H"); // Home beats CUP for 0,0
  } else if (e->cur_x >= 0 && e->cur_y == y && x > e->cur_x &&
      x - e->cur_x < 6) {
    // Short hop right: CUF beats CUP (ESC[<n>C, or ESC[C for 1).
    if (x - e->cur_x == 1) {
      ccb_str_append(e->out, "\x1b[C");
    } else {
      ccb_str_appendf(e->out, "\x1b[%dC", x - e->cur_x);
    }
  } else if (e->cur_x >= 0 && e->cur_y == y && x < e->cur_x &&
      e->cur_x - x < 6) {
    if (e->cur_x - x == 1) {
      ccb_str_append(e->out, "\x1b[D");
    } else {
      ccb_str_appendf(e->out, "\x1b[%dD", e->cur_x - x);
    }
  } else if (e->cur_x >= 0 && y == e->cur_y + 1 && x == 0) {
    ccb_str_append(e->out, "\r\n");
  } else {
    ccb_str_appendf(e->out, "\x1b[%d;%dH", y + 1, x + 1);
  }
  e->cur_x = x;
  e->cur_y = y;
}

static void tui_emit_style(tui_emit *e, ccb_tui_style style) {
  ccb_tui_style emitted = style;
  if (e->level == CCB_TUI_LEVEL_NONE) {
    // Colours are not emitted at this level, so the terminal
    // keeps its default colours: track the emitted style, not
    // the logical one, or the frame ends with a stray reset.
    emitted.fg = ccb_tui_color_default();
    emitted.bg = ccb_tui_color_default();
  }
  ccb_tui_sgr_diff(e->out, e->level, &e->cur, &emitted);
  e->cur = emitted;
}

static ccb_tui_style tui_style_of(const ccb_tui_cell *c) {
  ccb_tui_style s;
  s.fg = c->fg;
  s.bg = c->bg;
  s.mods = c->mods;
  return s;
}

// Emit one cell's text and advance the software cursor. A wide
// glyph or a print in the last column leaves the cursor unknown:
// the terminal may disagree about the width, and the last column
// leaves it in wrap-pending state.
static void tui_emit_cell(tui_emit *e, const ccb_tui_cell *c) {
  ccb_str_append(e->out, c->text);
  if (c->width == 2 || e->cur_x + 1 >= e->w) {
    e->cur_x = -1;
    e->cur_y = -1;
  } else {
    e->cur_x += c->width;
  }
}

void ccb_tui_diff(const ccb_tui_screen *old_s,
    const ccb_tui_screen *new_s, ccb_tui_color_level level,
    ccb_str *out) {
  tui_emit e;
  int y = 0;
  if (old_s == NULL || new_s == NULL || out == NULL) {
    return;
  }
  if (ccb_tui_screen_cols(old_s) != ccb_tui_screen_cols(new_s) ||
      ccb_tui_screen_rows(old_s) != ccb_tui_screen_rows(new_s)) {
    return;
  }
  e.out = out;
  e.level = level;
  e.cur = ccb_tui_style_default();
  e.cur_x = -1;
  e.cur_y = -1;
  e.w = ccb_tui_screen_cols(new_s);
  for (y = 0; y < ccb_tui_screen_rows(new_s); y++) {
    int first = -1;
    int last = -1;
    int el_at = -1;
    int x = 0;
    int i = 0;
    for (x = 0; x < e.w; x++) {
      const ccb_tui_cell *o = ccb_tui_screen_get(old_s, x, y);
      const ccb_tui_cell *n = ccb_tui_screen_get(new_s, x, y);
      if (n->width == 0) {
        continue; // continuation cell: never emitted on its own
      }
      if (!ccb_tui_cell_equal(o, n)) {
        if (first < 0) {
          first = x;
        }
        last = x;
      }
    }
    if (first < 0) {
      continue;
    }
    // Trailing blank run: if the last changed cell is blank and
    // every cell after it to the end of the line is blank too,
    // clear with EL instead of printing spaces — but only when
    // the run is at least 3 cells (EL is 3 bytes) and the current
    // background is default (EL clears with the current bg).
    if (ccb_tui_cell_is_blank(ccb_tui_screen_get(new_s, last, y))) {
      int k = last;
      int tail_blank = 1;
      ccb_tui_style at = e.cur;
      for (x = last + 1; x < e.w; x++) {
        if (!ccb_tui_cell_is_blank(
            ccb_tui_screen_get(new_s, x, y))) {
          tail_blank = 0;
          break;
        }
      }
      while (k > first &&
          ccb_tui_cell_is_blank(ccb_tui_screen_get(new_s, k - 1, y)) &&
          !ccb_tui_cell_equal(ccb_tui_screen_get(old_s, k - 1, y),
            ccb_tui_screen_get(new_s, k - 1, y))) {
        k--;
      }
      if (k > first) {
        at = tui_style_of(ccb_tui_screen_get(new_s, k - 1, y));
      }
      if (tail_blank && last - k + 1 >= 3 &&
          at.bg.kind == CCB_TUI_COLOR_DEFAULT) {
        el_at = k;
      }
    }
    for (i = first; i <= last; i++) {
      const ccb_tui_cell *n = NULL;
      const ccb_tui_cell *o = NULL;
      if (el_at >= 0 && i >= el_at) {
        break;
      }
      n = ccb_tui_screen_get(new_s, i, y);
      o = ccb_tui_screen_get(old_s, i, y);
      if (n->width == 0) {
        continue;
      }
      if (ccb_tui_cell_equal(o, n)) {
        continue; // unchanged cell inside the span: skip it,
        // the next changed cell gets a move instead
      }
      tui_emit_move(&e, i, y);
      tui_emit_style(&e, tui_style_of(n));
      tui_emit_cell(&e, n);
    }
    if (el_at >= 0) {
      tui_emit_move(&e, el_at, y);
      ccb_str_append(out, "\x1b[K");
      // EL does not move the cursor.
      e.cur_x = el_at;
      e.cur_y = y;
    }
  }
  if (!ccb_tui_style_equal(e.cur, ccb_tui_style_default())) {
    ccb_tui_sgr_reset(out);
  }
}
