# Preview modes: diff, walk, watch

[Docs index](index.md) · [Preview](preview.md)

*ccb-preview-modes*

## Diff mode: `ccb preview --diff <snapshot-dir>`

*ccb-preview-diff*

Compares the current renders against a saved snapshot directory in the
golden-file format (one `<Screen>.txt` frame per screen, `logic-tree.txt`,
`preview.json` — the same files `--update-goldens` writes, rendered with
the same calls, so a snapshot made by `--update-goldens` diffs identical).

- Exit 0 when identical. On differences: exit 1 and one unified diff per
  differing file, headed by the file name, plus a summary line naming all
  differing files (`differing files (2): Home.txt, preview.json`).
- A current screen missing from the snapshot is reported as `missing:
  <file> (not in snapshot)`; a snapshot file with no matching current
  screen as `stale: <file> (no matching current screen)`. An unreadable
  snapshot directory, a project with no usable `[preview]` section, or an
  unknown `--screen` name exits 2 with a clean diagnostic; outside any
  project, `--diff` is a usage error (exit 2) rather than plain preview's
  "not applicable". Preview errors in the project itself do not change the
  diff exit code — the render still compares.
- `--diff` + `--screen NAME` diffs only that screen's `<Screen>.txt` file.
  `--diff` + `--output FILE` writes the diff report to the file instead of
  stdout. `--diff` + `--json` emits one machine-readable object instead of
  human diff text: `{"event":"preview-diff","identical":bool,"files":[{"name":...,"status":"identical|different|missing|stale","diff":string-or-null}]}` (the diff string is present only for "different" files).
- Conflicts (exit 2): `--diff` cannot be combined with `--watch`,
  `--walk`, or `--update-goldens`.

## Walk mode: `ccb preview --walk [screen]`

*ccb-preview-walk*

Static exploration of the logic tree, not an emulator: it moves through
screens and edges the project already declares. It never executes Kotlin,
never evaluates a `when`/`if` condition, never fetches data, and never
animates.

`ccb preview --walk` starts at the entry screen (`Home` when one exists,
else the first screen); `ccb preview --walk Detail` (or `--screen Detail`)
starts at the named screen. Each view shows the screen's wireframe, its
outgoing edge list (showing the same argument text as the logic tree, e.g.
`-> Detail("note-42")`) with a `>` cursor, the back stack, and footer
hints.

Keys: up/down arrows or `j`/`k` move the selection (selection clamps at the
ends, it never wraps); Enter follows the selected edge; `b` or Backspace
goes back; `q` quits. `t` enters branch-pick mode for the current screen,
listing its branch entries as `when state: is Loaded`; up/down move among
them and Enter picks one. A pick never evaluates the condition: it follows
the first edge whose source label names a component inside the picked
entry, or, when the entry has no outgoing edge, records the pick and stays
on the screen. Esc (or `b`) leaves branch mode without picking.

Following an unknown edge — a dynamic destination, or a target screen that
does not exist — shows it as unknown and stays on the current screen; walk
mode never invents a destination. Back at the root screen is a no-op.
Ctrl-C quits and restores the terminal, as do SIGINT/SIGTERM. Walk mode
requires a terminal: with non-TTY stdin or stdout it prints an error and
exits 2 instead of hanging. Each frame is painted with the tui
terminal's diffed repaint (only changed cells, inside DEC 2026
synchronised output); the screen is never fully cleared between
frames.

`ccb preview --walk --record FILE` records the walk as a
[trace](preview-traces.md) and writes it when the walk exits: one step per
forward navigation taken, plus pick-only steps for branch picks that did
not navigate. Back navigation and unknown follows record nothing.
`--record` without `--walk` is a usage error.

## Watch mode: `ccb preview --watch [screen]`

*ccb-preview-watch*

Renders once immediately, then re-renders on every change to the preview
inputs — no restart, no rebuild. It composes with `--screen NAME` (or a
positional screen), `--ascii`, `--width`, `--device`, `--json`,
`--strict`, and `--output`; all of them keep their single-render meaning
on every re-render.

Watched set: every CSS file under the styles directory, every file under
the flow directory, and `ccb.toml` itself. New and deleted files count as
changes, since each rescan is diffed against the previous one. `ccb.toml`
is re-loaded on every change, so editing the `[preview]` paths re-points
the watch at the new directories.

Refresh framing depends on where the render goes:

| Destination | Framing |
| ----------- | ------- |
| TTY stdout | Screen cleared first (`ESC[2J ESC[H`), then the fresh render. The initial render is not cleared. |
| Piped human mode | Renders separated by a delimiter line on its own line, `--- ccb preview refresh ---`, before each re-render after the first. |
| `--json` piped mode | No delimiter — each render is one JSON object plus a newline, i.e. valid JSONL, flushed after each render. |
| `--output FILE` | File rewritten on each change, stdout stays empty, refresh notice plus diagnostics go to stderr. |

Changes are debounced ~200ms: a burst of saves (or one save that touches
several files) produces a single re-render once the inputs have been quiet
for 200ms. A broken save never kills the loop: a half-written TOML, broken
CSS, or broken Kotlin prints diagnostics and the loop keeps watching on the
last good render, re-rendering when the inputs become valid again. If the
selected screen disappears after a change, the loop prints a diagnostic
naming it and keeps watching instead of crashing or exiting.

Quit with Ctrl-C (or SIGTERM): the loop exits 0, flushing stdout first.
Watch mode never enters raw mode, so there is no terminal state to restore.

Conflicts (exit 2): `--watch` cannot be combined with `--update-goldens`,
and it cannot be combined with `--walk`. `--watch` with `--output` is
allowed.
