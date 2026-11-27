// ccb_tui_style: semantic styles, SGR emission, colour quantising.
#include "ccb_tui_style.h"

ccb_tui_color_level ccb_tui_detect_color_level(void) {
  const char *no_color = getenv("NO_COLOR");
  const char *colorterm = getenv("COLORTERM");
  const char *term = getenv("TERM");
  if (no_color != NULL) {
    return CCB_TUI_LEVEL_NONE;
  }
  if (colorterm != NULL &&
      (strstr(colorterm, "truecolor") != NULL ||
       strstr(colorterm, "24bit") != NULL)) {
    return CCB_TUI_LEVEL_RGB;
  }
  if (term != NULL && strstr(term, "256color") != NULL) {
    return CCB_TUI_LEVEL_256;
  }
  if (term == NULL || strcmp(term, "dumb") == 0) {
    return CCB_TUI_LEVEL_NONE;
  }
  return CCB_TUI_LEVEL_16;
}

ccb_tui_style ccb_tui_style_default(void) {
  ccb_tui_style s;
  s.fg = ccb_tui_color_default();
  s.bg = ccb_tui_color_default();
  s.mods = 0;
  return s;
}

int ccb_tui_style_equal(ccb_tui_style a, ccb_tui_style b) {
  return a.mods == b.mods && ccb_tui_color_equal(a.fg, b.fg) &&
    ccb_tui_color_equal(a.bg, b.bg);
}

ccb_tui_style ccb_tui_style_resolve(ccb_tui_style_id id) {
  ccb_tui_style s = ccb_tui_style_default();
  switch (id) {
    case CCB_TUI_STYLE_BORDER:
      break; // plain default: borders are shape, not colour
    case CCB_TUI_STYLE_TITLE:
      s.mods = CCB_TUI_BOLD;
      break;
    case CCB_TUI_STYLE_SELECTED:
      s.mods = CCB_TUI_REVERSE;
      break;
    case CCB_TUI_STYLE_ERROR:
      s.fg = ccb_tui_color_ansi(1); // red
      break;
    case CCB_TUI_STYLE_DIMMED:
      s.mods = CCB_TUI_DIM;
      break;
    case CCB_TUI_STYLE_DEFAULT:
    case CCB_TUI_STYLE_COUNT:
    default:
      break;
  }
  return s;
}

// The standard xterm 16-colour palette, for RGB->16 quantising.
static const uint8_t tui_ansi_palette[16][3] = {
  { 0, 0, 0 }, { 205, 0, 0 }, { 0, 205, 0 }, { 205, 205, 0 },
  { 0, 0, 238 }, { 205, 0, 205 }, { 0, 205, 205 }, { 229, 229, 229 },
  { 127, 127, 127 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 },
  { 92, 92, 255 }, { 255, 0, 255 }, { 0, 255, 255 },
  { 255, 255, 255 },
};

uint8_t ccb_tui_rgb_to_16(uint8_t r, uint8_t g, uint8_t b) {
  unsigned best = 0;
  unsigned best_d = 0;
  unsigned i = 0;
  for (i = 0; i < 16; i++) {
    int dr = (int)r - tui_ansi_palette[i][0];
    int dg = (int)g - tui_ansi_palette[i][1];
    int db = (int)b - tui_ansi_palette[i][2];
    unsigned d = (unsigned)(dr * dr + dg * dg + db * db);
    if (i == 0 || d < best_d) {
      best = i;
      best_d = d;
    }
  }
  return (uint8_t)best;
}

static uint8_t tui_cube_step(uint8_t v) {
  // Round v (0..255) to a 0..5 cube step.
  return (uint8_t)((v * 5 + 127) / 255);
}

uint8_t ccb_tui_rgb_to_256(uint8_t r, uint8_t g, uint8_t b) {
  unsigned mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  unsigned mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  if (mx - mn <= 8) {
    // Near-grey: use the grey ramp (232..255).
    unsigned v = (r + g + b) / 3;
    if (v < 8) {
      return 16;
    }
    if (v > 248) {
      return 231;
    }
    return (uint8_t)(232 + (v - 8) / 10);
  }
  return (uint8_t)(16 + 36 * tui_cube_step(r) +
    6 * tui_cube_step(g) + tui_cube_step(b));
}

// Map a 256-palette index back to RGB (for the 256 -> 16 path).
static void tui_256_to_rgb(uint8_t idx, uint8_t *r, uint8_t *g,
    uint8_t *b) {
  static const uint8_t steps[6] = { 0, 95, 135, 175, 215, 255 };
  uint8_t v = 0;
  if (idx < 16) {
    *r = tui_ansi_palette[idx][0];
    *g = tui_ansi_palette[idx][1];
    *b = tui_ansi_palette[idx][2];
    return;
  }
  if (idx >= 232) {
    v = (uint8_t)(8 + (idx - 232) * 10);
    *r = v;
    *g = v;
    *b = v;
    return;
  }
  idx -= 16;
  *r = steps[idx / 36];
  *g = steps[(idx / 6) % 6];
  *b = steps[idx % 6];
}

// Append one colour as SGR parameters (no ESC[ or m yet): fg uses
// 38, bg uses 48. level NONE emits nothing. `first` tracks whether
// a ';' separator is needed before the next parameter.
static void tui_append_color(ccb_str *out, ccb_tui_color_level level,
    ccb_tui_color c, int is_bg, int *first) {
  int base = is_bg ? 48 : 38;
  uint8_t idx = 0;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  if (level == CCB_TUI_LEVEL_NONE) {
    return;
  }
  if (c.kind == CCB_TUI_COLOR_DEFAULT) {
    ccb_str_appendf(out, "%s%d", *first ? "" : ";", base + 1);
    *first = 0;
    return;
  }
  if (c.kind == CCB_TUI_COLOR_ANSI) {
    int code = 0;
    if (c.idx < 8) {
      code = (is_bg ? 40 : 30) + c.idx;
    } else {
      code = (is_bg ? 100 : 90) + (c.idx - 8);
    }
    ccb_str_appendf(out, "%s%d", *first ? "" : ";", code);
    *first = 0;
    return;
  }
  if (level == CCB_TUI_LEVEL_RGB && c.kind == CCB_TUI_COLOR_RGB) {
    ccb_str_appendf(out, "%s%d;2;%d;%d;%d", *first ? "" : ";", base,
      c.r, c.g, c.b);
    *first = 0;
    return;
  }
  if (c.kind == CCB_TUI_COLOR_RGB) {
    if (level == CCB_TUI_LEVEL_256) {
      idx = ccb_tui_rgb_to_256(c.r, c.g, c.b);
    } else {
      idx = ccb_tui_rgb_to_16(c.r, c.g, c.b);
      if (idx < 8) {
        ccb_str_appendf(out, "%s%d", *first ? "" : ";",
          (is_bg ? 40 : 30) + idx);
      } else {
        ccb_str_appendf(out, "%s%d", *first ? "" : ";",
          (is_bg ? 100 : 90) + (idx - 8));
      }
      *first = 0;
      return;
    }
  } else if (c.kind == CCB_TUI_COLOR_256) {
    if (level == CCB_TUI_LEVEL_16) {
      tui_256_to_rgb(c.idx, &r, &g, &b);
      idx = ccb_tui_rgb_to_16(r, g, b);
      if (idx < 8) {
        ccb_str_appendf(out, "%s%d", *first ? "" : ";",
          (is_bg ? 40 : 30) + idx);
      } else {
        ccb_str_appendf(out, "%s%d", *first ? "" : ";",
          (is_bg ? 100 : 90) + (idx - 8));
      }
      *first = 0;
      return;
    }
    idx = c.idx;
  } else {
    idx = c.idx;
  }
  ccb_str_appendf(out, "%s%d;5;%d", *first ? "" : ";", base, idx);
  *first = 0;
}

// Append one modifier on/off code, with separator handling.
static void tui_append_code(ccb_str *out, int code, int *first) {
  ccb_str_appendf(out, "%s%d", *first ? "" : ";", code);
  *first = 0;
}

void ccb_tui_sgr_diff(ccb_str *out, ccb_tui_color_level level,
    const ccb_tui_style *from, const ccb_tui_style *to) {
  int first = 1;
  uint16_t removed = 0;
  uint16_t added = 0;
  uint16_t kept = 0;
  if (out == NULL || from == NULL || to == NULL) {
    return;
  }
  if (ccb_tui_style_equal(*from, *to)) {
    return;
  }
  removed = (uint16_t)(from->mods & ~to->mods);
  added = (uint16_t)(to->mods & ~from->mods);
  kept = (uint16_t)(from->mods & to->mods);
  ccb_str_append(out, "\x1b[");
  if ((removed & (CCB_TUI_BOLD | CCB_TUI_DIM)) != 0) {
    // 22 turns off bold and dim together; re-apply the ones
    // that were already on and are still wanted.
    tui_append_code(out, 22, &first);
    if ((kept & CCB_TUI_BOLD) != 0) {
      tui_append_code(out, 1, &first);
    }
    if ((kept & CCB_TUI_DIM) != 0) {
      tui_append_code(out, 2, &first);
    }
  }
  if ((removed & CCB_TUI_ITALIC) != 0) {
    tui_append_code(out, 23, &first);
  }
  if ((removed & CCB_TUI_UNDERLINE) != 0) {
    tui_append_code(out, 24, &first);
  }
  if ((removed & CCB_TUI_REVERSE) != 0) {
    tui_append_code(out, 27, &first);
  }
  if ((removed & CCB_TUI_STRIKE) != 0) {
    tui_append_code(out, 29, &first);
  }
  if ((added & CCB_TUI_BOLD) != 0) {
    tui_append_code(out, 1, &first);
  }
  if ((added & CCB_TUI_DIM) != 0) {
    tui_append_code(out, 2, &first);
  }
  if ((added & CCB_TUI_ITALIC) != 0) {
    tui_append_code(out, 3, &first);
  }
  if ((added & CCB_TUI_UNDERLINE) != 0) {
    tui_append_code(out, 4, &first);
  }
  if ((added & CCB_TUI_REVERSE) != 0) {
    tui_append_code(out, 7, &first);
  }
  if ((added & CCB_TUI_STRIKE) != 0) {
    tui_append_code(out, 9, &first);
  }
  if (!ccb_tui_color_equal(from->fg, to->fg)) {
    tui_append_color(out, level, to->fg, 0, &first);
  }
  if (!ccb_tui_color_equal(from->bg, to->bg)) {
    tui_append_color(out, level, to->bg, 1, &first);
  }
  if (first) {
    // Nothing was emitted (e.g. level NONE dropped the colours
    // and only colours differed): remove the bare ESC[ again.
    out->len -= 2;
    out->buf[out->len] = '\0';
    return;
  }
  ccb_str_append_c(out, 'm');
}

void ccb_tui_sgr_reset(ccb_str *out) {
  if (out != NULL) {
    ccb_str_append(out, "\x1b[0m");
  }
}
