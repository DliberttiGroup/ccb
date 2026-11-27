// ccb_tui_cell: the cell — one Unicode codepoint as UTF-8 plus style.
// A cell is the smallest renderable unit: what one terminal column
// shows. It is plain data: no I/O, no globals, no allocation. The
// diff module compares cells with ccb_tui_cell_equal; the screen
// module stores them in a grid.
//
// What this module does NOT do: it knows nothing about terminals,
// escape sequences, or frames. Width comes from a small vendored
// UAX #11 table (ccb_tui_char_width); libc wcwidth is not used — it
// is stale on many systems and locale-dependent in a static binary.
#ifndef CCB_TUI_CELL_H
#define CCB_TUI_CELL_H

#include "ccb_common.h"

// Colour kinds. A cell stores its fg/bg independently (no ncurses
// colour-pair registry): a pair registry buys nothing on a modern
// terminal and caps the palette.
typedef enum {
  CCB_TUI_COLOR_DEFAULT = 0, // terminal default fg/bg
  CCB_TUI_COLOR_ANSI,        // idx 0..15 (standard + bright)
  CCB_TUI_COLOR_256,        // idx 0..255 (xterm 256 palette)
  CCB_TUI_COLOR_RGB         // r/g/b, 24-bit truecolour
} ccb_tui_color_kind;

typedef struct {
  uint8_t kind; // ccb_tui_color_kind
  uint8_t idx;  // palette index for ANSI / 256 kinds
  uint8_t r, g, b;
} ccb_tui_color;

ccb_tui_color ccb_tui_color_default(void);
ccb_tui_color ccb_tui_color_ansi(unsigned idx); // idx > 15 clamps to 15
ccb_tui_color ccb_tui_color_256(unsigned idx);  // idx > 255 clamps to 255
// Each of r/g/b > 255 clamps to 255.
ccb_tui_color ccb_tui_color_rgb(unsigned r, unsigned g, unsigned b);
int ccb_tui_color_equal(ccb_tui_color a, ccb_tui_color b);

// Style modifiers, a bitmask. Bold and dim share an SGR off code
// (22); the style module handles that when emitting.
#define CCB_TUI_BOLD 0x01u
#define CCB_TUI_DIM 0x02u
#define CCB_TUI_ITALIC 0x04u
#define CCB_TUI_UNDERLINE 0x08u
#define CCB_TUI_REVERSE 0x10u
#define CCB_TUI_STRIKE 0x20u

// One cell. text holds one codepoint as UTF-8 (at most 4 bytes);
// the array is 8 bytes to leave room for a future combining mark.
// width is the display width: 0 marks the continuation cell of a
// wide (2-cell) glyph — it is never drawn, never emitted by the
// diff, and always has empty text and the default style.
typedef struct {
  char text[8];
  uint8_t width; // 0, 1, or 2
  uint16_t mods; // CCB_TUI_* bitmask
  ccb_tui_color fg;
  ccb_tui_color bg;
} ccb_tui_cell;

// A blank cell: a space, default colours, no modifiers, width 1.
ccb_tui_cell ccb_tui_cell_blank(void);
// Byte-exact comparison (text, width, mods, fg, bg).
int ccb_tui_cell_equal(const ccb_tui_cell *a, const ccb_tui_cell *b);
// True for a width-1 space with default colours and no modifiers.
// A continuation cell (width 0) is never blank.
int ccb_tui_cell_is_blank(const ccb_tui_cell *c);

// Decode one UTF-8 codepoint from s[0..n). Returns the bytes
// consumed (1..4). Returns 0 when the sequence is incomplete
// (n is too short for the lead byte); returns (size_t)-1 for an
// invalid sequence (bad lead/continuation byte, overlong form,
// surrogate, or above U+10FFFF). cp is untouched on 0 / -1.
size_t ccb_tui_utf8_decode(const char *s, size_t n, uint32_t *cp);
// Encode one codepoint as UTF-8 into out (4 bytes of room).
// Returns the bytes written (1..4), or 0 for an invalid codepoint
// (surrogate or above U+10FFFF).
size_t ccb_tui_utf8_encode(uint32_t cp, char out[4]);

// Display width of one codepoint (UAX #11 East Asian Width):
// 0 for NUL, C0/C1 controls, combining marks, format characters
// and conjoining Hangul jamo; 2 for Wide and Fullwidth ranges
// (CJK ideographs, fullwidth forms, most emoji); 1 for everything
// else, including East Asian Ambiguous (treated as narrow — its
// width depends on locale, which this library does not carry).
int ccb_tui_char_width(uint32_t cp);

#endif
