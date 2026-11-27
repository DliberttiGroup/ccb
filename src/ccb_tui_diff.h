// ccb_tui_diff: the pure renderer — old screen + new screen ->
// ANSI bytes. No I/O, no terminal state, no allocation beyond
// the caller's output buffer: the same screens always produce
// the same bytes, which is what makes it unit-testable and what
// the preview's golden tests will assert against later.
//
// Emission rules (ratatui/crossterm's cheap 90% of mvcur):
// only changed cells are emitted, in row-major runs; the cursor
// is tracked in software and moved only when the next cell is
// not contiguous (same row, next column), with CR/LF for column
// 0 of the next row and short CUF/CUB hops, CUP otherwise.
// After a wide glyph, or after printing in the last column, the
// cursor is treated as unknown and re-asserted with CUP — the
// terminal may disagree about the width, and the last column
// leaves the cursor in wrap-pending state. SGR changes are
// batched (one combined sequence per style change). A trailing
// run of changed blank cells is cleared with EL instead of
// spaces when that is shorter and the background is default.
// The frame ends with SGR reset when the style is not default.
#ifndef CCB_TUI_DIFF_H
#define CCB_TUI_DIFF_H

#include "ccb_common.h"
#include "ccb_str.h"
#include "ccb_tui_cell.h"
#include "ccb_tui_screen.h"
#include "ccb_tui_style.h"

// Append the ANSI bytes that turn `old_s` into `new_s`. Both
// screens must have the same dimensions; mismatched dimensions
// or NULL arguments append nothing. Emits nothing when the
// screens are equal. Colours are quantised to `level`.
void ccb_tui_diff(const ccb_tui_screen *old_s,
  const ccb_tui_screen *new_s, ccb_tui_color_level level,
  ccb_str *out);

#endif
