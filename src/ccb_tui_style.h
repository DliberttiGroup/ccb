// ccb_tui_style: semantic styles, SGR emission, colour levels.
// A style is a resolved fg/bg/modifier triple — flat, comparable
// values, which is what makes the diff a cheap equality check
// (ratatui does the same: the buffer holds resolved styles, not
// incremental merges). Callers use semantic ids (border, title,
// selected, error); no raw ANSI codes ever leak into callers.
//
// What this module does NOT do: it does not know about screens or
// terminals. Colour-level detection (env sniffing) lives here
// because it is a property of the style->SGR mapping, not of the
// terminal connection; the term module calls it once at open.
#ifndef CCB_TUI_STYLE_H
#define CCB_TUI_STYLE_H

#include "ccb_common.h"
#include "ccb_str.h"
#include "ccb_tui_cell.h"

// Colour fidelity of the terminal. Decided once at startup from
// the environment (see ccb_tui_detect_color_level); the diff
// quantises colours down to this level at emit time.
typedef enum {
  CCB_TUI_LEVEL_NONE = 0, // NO_COLOR, or TERM=dumb: no colour codes
  CCB_TUI_LEVEL_16,       // 16 ANSI colours
  CCB_TUI_LEVEL_256,      // xterm 256-colour palette
  CCB_TUI_LEVEL_RGB       // 24-bit truecolour
} ccb_tui_color_level;

// Detect the colour level from the environment. NO_COLOR (set to
// anything, even empty) wins over everything. Otherwise:
// COLORTERM containing "truecolor" or "24bit" -> RGB;
// TERM containing "256color" -> 256; any other TERM except "dumb"
// -> 16; TERM unset or "dumb" -> NONE. $TERM alone is never
// trusted for truecolour (it is not forwarded reliably by tmux,
// ssh, or sudo), and no terminal probing is done.
ccb_tui_color_level ccb_tui_detect_color_level(void);

// A resolved style: independent fg/bg plus a modifier bitmask.
typedef struct {
  ccb_tui_color fg;
  ccb_tui_color bg;
  uint16_t mods; // CCB_TUI_* bitmask
} ccb_tui_style;

ccb_tui_style ccb_tui_style_default(void);
int ccb_tui_style_equal(ccb_tui_style a, ccb_tui_style b);

// Semantic styles for callers. The mapping is a fixed table in
// this module (a theme system is a later step, not v1).
typedef enum {
  CCB_TUI_STYLE_DEFAULT = 0,
  CCB_TUI_STYLE_BORDER,
  CCB_TUI_STYLE_TITLE,
  CCB_TUI_STYLE_SELECTED,
  CCB_TUI_STYLE_ERROR,
  CCB_TUI_STYLE_DIMMED,
  CCB_TUI_STYLE_COUNT
} ccb_tui_style_id;

// Resolve a semantic id to a concrete style. An unknown id
// resolves to the default style.
ccb_tui_style ccb_tui_style_resolve(ccb_tui_style_id id);

// Append the SGR bytes that change the emitted style from `from`
// to `to`, as one combined sequence (ESC[...m). Emits nothing
// when they are equal. Modifier changes use a modifier diff
// (removed modifiers get their off codes, added ones their on
// codes) instead of a blanket ESC[0m per cell; bold and dim
// share off code 22, so dropping one re-applies the other when
// it is still wanted. Colours are quantised to `level`.
void ccb_tui_sgr_diff(ccb_str *out, ccb_tui_color_level level,
  const ccb_tui_style *from, const ccb_tui_style *to);
// Append ESC[0m (reset all attributes and colours).
void ccb_tui_sgr_reset(ccb_str *out);

// Quantise RGB to the xterm 256-colour palette (6x6x6 cube plus
// the 24 greys; naive but the accepted cheap answer — perceptual
// distance is a harder problem and not worth it here).
uint8_t ccb_tui_rgb_to_256(uint8_t r, uint8_t g, uint8_t b);
// Quantise RGB to the nearest of the 16 ANSI colours (squared
// Euclidean distance over the standard xterm palette).
uint8_t ccb_tui_rgb_to_16(uint8_t r, uint8_t g, uint8_t b);

#endif
