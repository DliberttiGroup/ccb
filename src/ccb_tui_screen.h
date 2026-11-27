// ccb_tui_screen: the frame — a width x height grid of cells.
// The screen is plain data: building a frame never touches the
// terminal, so it is unit-testable without one. The diff module
// turns two screens into ANSI bytes; the term module owns the
// previous frame and calls the diff.
//
// Ownership: the screen owns an internal arena; cells live in it.
// ccb_tui_screen_free destroys the arena. Resize preserves the
// overlapping region and clears the rest.
//
// What this module does NOT do: no I/O, no escape sequences, no
// input, no layout. Text does not wrap (callers wrap); a wide
// glyph in the last column becomes a space (tcell's rule).
#ifndef CCB_TUI_SCREEN_H
#define CCB_TUI_SCREEN_H

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_tui_cell.h"
#include "ccb_tui_style.h"

// Cell aspect: one terminal cell is about twice as tall as it is
// wide. Preview's dp->cell scaling compensates with these units
// (one cell = 1 unit wide, 2 units tall); the library itself
// never scales — it only paints cells.
#define CCB_TUI_CELL_W_UNITS 1
#define CCB_TUI_CELL_H_UNITS 2

typedef struct ccb_tui_screen ccb_tui_screen;

// Create a w x h screen, all blank cells. Returns NULL when w or
// h is below 1.
ccb_tui_screen *ccb_tui_screen_new(int w, int h);
void ccb_tui_screen_free(ccb_tui_screen *s); // NULL-safe
int ccb_tui_screen_cols(const ccb_tui_screen *s);
int ccb_tui_screen_rows(const ccb_tui_screen *s);

// Resize, preserving the overlapping region; new cells are blank.
// Returns CCB_OK, or CCB_ERR_USAGE for w/h below 1.
ccb_status ccb_tui_screen_resize(ccb_tui_screen *s, int w, int h);
void ccb_tui_screen_clear(ccb_tui_screen *s);
// Copy src into dst (same dimensions required; otherwise dst is
// resized first). Returns CCB_OK, or CCB_ERR_USAGE for NULL args.
ccb_status ccb_tui_screen_copy(ccb_tui_screen *dst,
  const ccb_tui_screen *src);

// Read one cell; NULL when out of bounds (or s is NULL).
const ccb_tui_cell *ccb_tui_screen_get(const ccb_tui_screen *s,
  int x, int y);

// Put one codepoint at (x, y). Returns the columns consumed
// (0, 1, or 2). Width-0 codepoints (combining marks, controls)
// are dropped in v1 and consume 0. A wide glyph in the last
// column becomes a space. Writing over a wide glyph clears its
// continuation cell, and writing into a continuation cell clears
// the glyph to its left first. Out of bounds consumes 0.
int ccb_tui_screen_put(ccb_tui_screen *s, int x, int y, uint32_t cp,
  ccb_tui_style style);

// Put one raw byte as a width-1 cell, stored verbatim: the byte is
// NOT decoded as UTF-8, and the dump emits it as-is. For callers
// that must preserve legacy content byte-for-byte (an invalid UTF-8
// byte in a label, a control byte) — put_text normalises such bytes
// to '?', this one does not. Returns 1 when written, 0 when out of
// bounds (or s is NULL).
int ccb_tui_screen_put_byte(ccb_tui_screen *s, int x, int y,
  unsigned char b, ccb_tui_style style);

// Put UTF-8 text starting at (x, y). '\n' moves to the next line
// at the start column; '\t' advances to the next 8-column tab
// stop. Invalid bytes become '?'. No wrapping: stops at the right
// edge (a '\n' past the last row stops everything). Returns the
// number of glyph cells placed.
int ccb_tui_screen_put_text(ccb_tui_screen *s, int x, int y,
  const char *text, ccb_tui_style style);

// Box outline with Unicode box-drawing characters (corners,
// horizontal and vertical edges). w or h of 1 draws the partial
// outline that fits. Out-of-bounds parts clip silently.
void ccb_tui_screen_box(ccb_tui_screen *s, int x, int y, int w,
  int h, ccb_tui_style style);
void ccb_tui_screen_hline(ccb_tui_screen *s, int x, int y, int len,
  ccb_tui_style style);
void ccb_tui_screen_vline(ccb_tui_screen *s, int x, int y, int len,
  ccb_tui_style style);
// Fill a rectangle with one codepoint (usually a space with a
// background colour). Expects a width-1 codepoint; a wide one
// degrades to one glyph per cell.
void ccb_tui_screen_fill(ccb_tui_screen *s, int x, int y, int w,
  int h, uint32_t cp, ccb_tui_style style);
// Half-block cell: '▀' with fg=top and bg=bottom — two vertical
// pixels per cell, the 2x vertical resolution primitive. Use a
// space with bg for a solid fill, never a full block.
void ccb_tui_screen_half(ccb_tui_screen *s, int x, int y,
  ccb_tui_color top, ccb_tui_color bottom);

// Plain-text dump of a frame: each row's text (continuation cells
// skipped), trailing spaces trimmed, rows joined by '\n', no
// trailing newline. For non-TTY output and golden tests.
void ccb_tui_screen_dump(const ccb_tui_screen *s, ccb_str *out);
// True when dimensions and every cell are equal.
int ccb_tui_screen_equal(const ccb_tui_screen *a,
  const ccb_tui_screen *b);

#endif
