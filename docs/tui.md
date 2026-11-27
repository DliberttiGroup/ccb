# Terminal UI library (ccb_tui)

*ccb-tui*

A minimal terminal UI library in C, libc only, no ncurses, no
terminfo database. It exists for one consumer: the preview
render engine (the swap is a later phase). Preview owns layout;
this library only paints cells.

## Model

*ccb-tui-model*

Declarative and frame-based, not curses-like. The caller
builds a frame — a grid of cells, plain data — and hands it to
the library; the library diffs it against the previous frame
and emits only the changed cells as ANSI. Same screens in,
same bytes out, every time. No global state, no windows,
panels, or forms, no widget toolkit.

One cell is one terminal column: one Unicode codepoint as
UTF-8 plus a style (fg, bg, bold/dim/italic/underline/reverse/
strike). Colours are per-cell and independent (no colour-pair
registry): default, 16 ANSI, 256-palette, or 24-bit RGB. A
wide (2-cell) glyph occupies its cell plus a continuation
cell, which is never drawn on its own. One cell is about
twice as tall as it is wide (`CCB_TUI_CELL_W_UNITS` 1,
`CCB_TUI_CELL_H_UNITS` 2) — preview's dp→cell scaling
compensates with those units; the library never scales.

## Modules

*ccb-tui-modules*

One responsibility each, one `.h`/`.c` pair under `src/`:

| Module | What it does | What it does not do |
| --- | --- | --- |
| `ccb_tui_cell` | Cell type, UTF-8 codec, display-width table (UAX #11) | No I/O, no terminal state |
| `ccb_tui_screen` | Frame: grid of cells, put/box/line/fill, half-block cells, plain-text dump | No I/O, no escape sequences |
| `ccb_tui_diff` | Pure function: old screen + new screen → ANSI bytes | No terminal state, no writes |
| `ccb_tui_term` | The only module that touches the terminal: raw mode, alt screen, DEC modes, resize, present | No drawing, no input parsing |
| `ccb_tui_input` | Byte stream → key/paste events (state machine, ESC timeout, kitty keyboard, bracketed paste) | No terminal modes |
| `ccb_tui_style` | Semantic styles (border, title, selected, error), SGR emission, colour quantising | No screens, no terminal |

## Pipeline

*ccb-tui-pipeline*

```
build frame (screen) -> diff vs previous frame -> one write()
```

The app builds a fresh frame each tick, then calls
`ccb_tui_term_present()`, which diffs, wraps the bytes in
DEC 2026 synchronised output with the cursor hidden, and
issues a single `write()`. An unchanged frame writes
nothing. A resize, or a frame of different dimensions,
forces a full repaint (a diff against a blank screen).
Text output for a pipe goes through `ccb_tui_screen_dump()`,
never through the terminal path.

## ANSI subset

*ccb-tui-ansi*

Hardcoded xterm-like escapes; no terminfo is read.

- Cursor: `ESC[H` (home), `ESC[<r>;<c>H` (CUP), `ESC[<n>C` /
  `ESC[<n>D` (short hops), CR/LF for column 0 of the next row.
- Erase: `ESC[K` (clear to end of line, used for trailing
  blank runs when the background is default).
- Style: `ESC[...m` (one combined SGR per change, modifier
  diff instead of a blanket reset); colours as `30–37` /
  `90–97` (16), `38;5;n` / `48;5;n` (256),
  `38;2;r;g;b` / `48;2;r;g;b` (truecolour), `39`/`49`
  (default). `ESC[0m` resets at the end of a frame.
- Modes: `ESC[?1049h/l` (alternate screen),
  `ESC[?25l/h` (cursor hide/show), `ESC[?2004h/l`
  (bracketed paste), `ESC[?2026h/l` (synchronised output),
  `ESC[>3u` / `ESC[<u` (kitty keyboard flags 1|2, opt-in).

Terminals that do not recognise a mode ignore it, so DEC
2026 and bracketed paste are emitted unconditionally.
Colour level is decided once from the environment:
`COLORTERM` containing `truecolor`/`24bit` → RGB;
`TERM` containing `256color` → 256; any other `TERM`
except `dumb` → 16; `NO_COLOR` (set to anything) or
`TERM=dumb` → none. `$TERM` alone is never trusted for
truecolour. RGB is quantised down to the level at emit time
(6×6×6 cube + greys for 256, nearest of 16 for ANSI).

## Input

*ccb-tui-input*

Keys arrive as a byte stream parsed by a state machine:
plain UTF-8 characters, control bytes (`Ctrl+letter`,
Enter, Tab, Backspace), legacy `ESC [ …` / `ESC O …`
sequences (arrows, Home/End, PgUp/PgDn, Insert/Delete,
F1–F12, Shift-Tab), and the kitty keyboard protocol
(`CSI … u`, flags 1|2: disambiguated codes and press /
repeat / release event types). A lone `ESC` is
disambiguated by a 50 ms timeout — if nothing follows,
it is an Esc key. Bracketed paste arrives as one paste
event (literal text, never interpreted as keys). SGR/X10
mouse reports and focus events are consumed and ignored
in v1. Modifiers come from the CSI parameter (`1 +` the
Shift/Alt/Ctrl/Super bitmask).

## Resize and signals

*ccb-tui-resize*

`SIGWINCH` is delivered through a self-pipe: the handler
only writes one byte; the poll loop does the `ioctl`,
reallocates, and repaints fully. The size is also
re-queried on every present, as a backstop. A `0x0` size
at startup falls back to `80x24`. `SIGINT`/`SIGTERM`/
`SIGHUP`/`SIGQUIT` arrive as an interrupt event (the app
closes the term and exits); `Ctrl-Z` is handled inside
the poll loop — the terminal is restored, the process
stops, and on continue the terminal is re-entered. An
`atexit` handler restores the terminal if the process
exits without closing it. At most one terminal is open
at a time.

## What it does not do

*ccb-tui-limits*

- No layout engine, no widgets, no windows or layers.
  Callers compose into one buffer before diffing.
- No mouse handling (reports are dropped), no focus
  tracking, no scroll regions, no insert/delete-line
  planning — a full repaint covers resizes.
- No grapheme clusters: one codepoint per cell, combining
  marks are dropped in v1. Ambiguous-width characters
  are treated as narrow.
- No terminal probing: capabilities come from the
  environment, not from queries (except the size).
- Termux behaviour (kitty keyboard, DEC 2026, soft-
  keyboard resizes) is unprobed until tested on the
  phone — the generic rules above apply, but nothing
  Termux-specific is verified yet.

## Tests

*ccb-tui-tests*

`tests/test_tui.c` (`fast;tui`): cell codec and widths,
style resolution and SGR bytes, screen ops (text, box,
clipping, resize, fill, half-block), diff output as exact
byte strings (identical frames → zero bytes, one-cell
change, runs, gaps, style changes, wide glyphs, trailing
blank EL), input parsing (all key forms, kitty, paste,
mouse dropping, lone-ESC timeout), and the terminal
itself against a pty (open, present, unchanged-frame
silence, resize, key events, second-open refusal).
