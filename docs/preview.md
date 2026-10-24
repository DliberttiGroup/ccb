# CCB Preview — the UI convention

Preview (`ccb preview`) renders a terminal wireframe for projects that
follow this convention. It is a linter with a picture, not an emulator:
it never executes Kotlin, evaluates conditions, fetches data, or
animates. Anything outside the convention warns and falls back; it
never crashes and never silently guesses.

## Declaring preview in ccb.toml

```toml
[preview]
styles = "styles"   # directory of CSS files
flow = "flow"       # directory of Kotlin flow files
resources = "res"   # optional: directory of strings.xml / colors.xml
phone_width = 390   # dp, informational: scales px values to columns
phone_height = 844  # dp, informational
strict = false      # true: subset warnings become errors
gate = false        # true: preview errors fail ccb check / ccb build
```

`resources`, when set, must be an existing directory (like `styles`
and `flow`). It holds Android values files, `strings.xml` and
`colors.xml`, parsed with a tiny subset parser (no XML library):
`<string name="n">v</string>` and `<color name="n">#rrggbb</color>`.
XML comments are skipped; `&amp; &lt; &gt; &quot; &apos;` are
decoded in string values. A missing file is simply an empty table
(a project may define only strings, only colors, or neither); a
malformed entry is skipped with a CCB-P1016 warning naming the
file, and a non-hex color value is skipped with CCB-P1016 the same
way. Resolution happens at load time, before layout:

- A component label that is exactly `@string/name` (the whole
  label, not embedded text) is replaced by the resolved string, so
  clip widths are computed on the resolved text.
- A `color` or `background-color` value that is exactly
  `@color/name` is replaced by the resolved hex at rule load.
  Resolved `@color` values count as resolved for the CCB-P1018
  contrast check.

An unresolved reference (name not in the table, or no resources
directory configured) warns with CCB-P1016 naming the reference,
and the literal `@string/name` / `@color/name` text is kept —
rendered as-is, never a crash, never an invented value.

`styles` and `flow` are optional individually, but a project with
neither is "preview not applicable" (exit 0). With no `[preview]`
section at all, `ccb preview` prints "preview not applicable to this
project" and exits 0. The terminal frame width is `--width` (default
40 columns, minimum 24 — smaller values clamp with a warning).

## Flow files: screens and components

Flow files are Kotlin files in the `flow` directory. Preview parses
structure only — it does not typecheck, compile, or run anything.

- A top-level `fun Name(` function is a screen named `Name`.
- Inside a screen, component calls nest by braces. Known components
  (matched case-insensitively on the call name):
  `Column`, `Row`, `Text`, `Button`, `List`, `TextField`, `Image`,
  `Box`, `Spacer`. Anything else becomes a generic box and warns
  (CCB-P1012) — it still renders, labeled with its type.
- A component's label is its first string argument:
  `Text("My Notes")`, `Button("Save", ...)`.
- Navigation is any `nav("Target")` or `navigateTo("Target")` call
  inside a component's arguments or block. The edge is recorded from
  the enclosing component's label. A dynamic destination (a variable,
  a template, any non-literal) becomes an `unknown` edge — shown, not
  resolved, and not an error.
- Arguments after the target are recorded on the edge and shown in
  the logic tree, walk mode's edge list, and JSON: a single string
  literal without `$` is its literal text, `name = <string literal
  without $ | number>` is the raw `name = value` text as written
  (never evaluated), and anything else (identifiers, templates
  with `$`, expressions, calls) is `unknown`. A dynamic target
  with arguments keeps `target: null, unknown: true` and still
  records its args. Edges deduplicate on target, unknown flag, AND
  args. The tree format is `-> Detail("note-42", unknown) via
  "Open note"`; an edge with no extra args keeps the old format.
- `when (state) { ... }` and `if (cond) { ... }` become branch nodes
  labeled with their conditions. Branches are shown, never evaluated.
  `when` entries (`is Loading -> ...`, `else -> ...`) become entry
  nodes under the branch. An `else` after an `if` becomes an entry
  node labeled `else`.
- The entry screen is the screen named `Home` when one exists, else
  the first declared screen in sorted file order.

Example:

```kotlin
fun Home() {
  Column {
    Text("My Notes")
    List("notes")
    Row {
      Button("Open note", onClick = { nav("Detail") })
      Button("New note", onClick = { nav("Form") })
    }
    Button("Settings", onClick = { nav("Settings") })
  }
}
```

## Styles: one CSS file per screen, plus common.css

- Scope rule: `common.css` (when present in the styles directory)
  applies to every screen. `<Screen>.css` applies to that screen only,
  and only when a screen of that name exists in the flow files. Any
  other CSS file is ignored with a CCB-P1015 warning and is never
  parsed. Files are parsed in sorted order, `common.css` first, then
  per-screen files. A per-screen file redeclaring a selector keeps
  the earlier rule for other screens and adds a scoped copy for its
  own screen: the copy carries the earlier properties plus the
  per-screen file's declarations, and layout prefers the exact-scope
  rule over the common one. So `common.css` properties still apply
  everywhere, per-screen properties apply only to that screen, and
  two per-screen files declaring the same selector (e.g. `Empty.css`
  and `Settings.css` both declaring `column`) each get their own
  scoped rule.
- Selectors are `screen` (the root box) or a lowercase component type
  (`column`, `row`, `text`, `button`, `list`, `textfield`, `image`,
  `box`, `spacer`). Later declarations for the same selector override
  earlier ones property by property.
- Supported properties: `display` (block, flex), `flex-direction`
  (row, column), `width`/`height` (px, %), `padding`, `margin` (px,
  1–4 values as in CSS), `background-color`, `color` (hex or named,
  or an `@color/name` reference resolved via `[preview] resources`),
  `font-size` (px, recorded only — it does not change layout, but
  it does select the CCB-P1018 contrast threshold),
  `position` (static, absolute), `top`/`left` (px, absolute only),
  `gap` (px), `text-align` (left, center, right), `border-width`
  (px), `border-color` (hex or named), `flex-wrap` (nowrap, wrap;
  row containers only).
- Deliberately NOT supported (they warn as unknown properties,
  CCB-P1001, like anything else outside the subset):
  `justify-content` / `align-items` (distribution semantics the
  sum-based row/column model cannot express honestly),
  `border-radius` (no wireframe-visible rounding at 1-cell scale),
  `line-height` (text is single-row by design; it would only pad),
  and the `border` shorthand (use the two longhands above).
- Anything else warns with the property name and location
  (CCB-P1001 unknown property, CCB-P1014 unsupported value) and
  rendering continues with a fallback. Under `--strict` (or
  `[preview] strict = true`) those warnings become errors.

Example:

```css
screen { background-color: #ffffff; padding: 16px; }
text { color: #111111; font-size: 16px; }
button { background-color: #6200ee; color: #ffffff; margin: 8px; }
```

## Layout model (terminal columns and rows)

- The phone frame is `--width` columns wide (default 40); the content
  interior is frame minus the two border columns.
- px values scale to columns/rows by `interior / phone_width` (so
  16px of padding on a 390dp phone at 40 columns is about 2 rows).
  `%` widths and heights are relative to the parent content size.
- Column containers stack children vertically, each full width minus
  margins. Row containers split the width: fixed sizes honored first,
  the remainder shared equally among auto-sized children.
- Text renders on one row, clipped to its box (clipping warns,
  CCB-P1009). Buttons render as `[ label ]`, text fields as
  `[label]`, lists as one `• <label>` row per child item when the List box has children (a child with no label renders as `• item`); a bare List with no children — its count genuinely unknown — renders 4 `• item` placeholder rows and emits no diagnostic for the placeholder. A list with children is exactly one row per child tall (plus padding); a bare list is 4 rows tall (plus padding)., images as a
  3-row `[ image ]` block. Unknown components render as
  `[TypeName]`.
- Leaf content is inset by the leaf's own scaled padding: a leaf
  paints at (box x + left padding, box y + top padding), clipped to
  the box width minus left and right padding. Parent padding insets
  children the same way, through the parent content box.
- `text-align` offsets the painted string within the leaf's content
  width (left = content edge, center = centered, right = flush
  right, clamped at the content edge when the string is wider);
  it applies to `text`, `button`, `textfield`, and list rows.
- A `border-width` that scales to at least 1 cell draws a 1-cell
  outline around the box (Unicode frame chars, or ASCII under
  `--ascii`) in the `border-color`, else the rule's `color`, else
  the terminal default. Content is inset 1 cell on all sides, so a
  bordered box's content box shrinks by 1 cell per side and its
  height grows by 2 rows (top + bottom border); for containers the
  outline goes around the container box and children are laid out
  inside the inset content box. `border-width: 0` (or any value
  scaling to 0) means no border; a negative width or an invalid
  border color warns (CCB-P1014).
- In a row container with `flex-wrap: wrap`, children are placed
  left-to-right and a child that would exceed the content width
  starts a new line; the new line's y advances by the previous
  line's height (its tallest occupied child height). Total row
  height is unchanged by wrapping: it still sums every child's
  height, as in the nowrap model. `flex-wrap` on a non-row
  container is ignored with a CCB-P1014 warning naming the
  selector.
- Absolute-positioned boxes do not affect parent height. Boxes that
  overlap (CCB-P1003) or extend past the frame (CCB-P1004) warn.

## Lint diagnostics (after layout)

- CCB-P1017 — tap target too small (warning). Applies to `button`
  and `textfield` boxes only, after layout. Warns when `w < 8`
  columns OR `h < 2` rows, in wireframe cells — not dp, because the
  wireframe's vertical quantum is one row (≈10dp at the default
  40-column frame), so a 44dp height rule would flag every
  unpadded button including all reasonable defaults. The cell
  threshold targets genuinely degenerate cases (a button squeezed
  to a few columns by a row split, a 1-row button with zero
  padding). Documented dp equivalents at the default frame: 8
  columns ≈ 82dp wide, 2 rows ≈ 21dp tall. The message names the
  component and its size, e.g. `tap target too small: button "Go"
  is 5x1 cells (minimum 8x2)`.
- CCB-P1018 — text contrast below threshold (warning). Applies to
  `text`, `button`, and `textfield` leaves, after layout.
  Foreground = the leaf rule's `color`; background = the leaf
  rule's `background-color`, else the `screen` rule's
  `background-color`. If either side is missing or unresolvable
  to RGB (hex `#rgb`/`#rrggbb` or the renderer's named-color
  table), the check skips silently — the contrast cannot be
  judged. Resolved `@color` values count as resolved. Ratio is
  WCAG relative luminance, contrast = (L1 + 0.05) / (L2 + 0.05).
  Threshold: **4.5:1** normally, **3.0:1** when the leaf rule's
  `font-size` is ≥ 24px (WCAG large-text threshold). The message
  includes the ratio to one decimal, e.g. `text contrast 2.1:1
  below 4.5:1 for text "Note detail"`.

Both are warnings always — even under `--strict` — so they never
fail the build gate, which fails only on errors.

## Output and exit codes

- `ccb preview` — every screen's wireframe, then the logic tree, then
  diagnostics. `ccb preview <screen>` (or `--screen NAME`) — one
  screen's wireframe, then the logic tree, then diagnostics.
- `--json` — the same box trees, logic tree, and diagnostics as
  structured data. `--ascii` — ASCII frame characters instead of
  Unicode. Colors come from the CSS colors approximated to ANSI
  256-color codes, unless `NO_COLOR` is set, the terminal is dumb, or
  `--ascii` is given.
- `--output FILE` — write the preview render to FILE instead of
  stdout: the human render (frames + logic tree), or the JSON object
  when `--json` is also given. Stdout carries nothing in that case;
  diagnostics still go to stderr, and exit codes are unchanged (a
  preview error still exits 1 with the render written; an unwritable
  output file exits 2).
- `--device phone|tablet` — frame-width presets on top of `--width`:
  phone = 40 columns (the default), tablet = 80 columns. An explicit
  `--width` always wins over `--device`; any other value is a usage
  error.
- `--storyboard "A,B,C"` — render a journey, not the whole app:
  the argument is a comma-separated list of screen names, in
  journey order. Repeats are allowed (a journey may revisit a
  screen); each occurrence renders again. Human output prints each
  screen's frame preceded by a header line `Journey N/M: <Name>`
  (N = 1-based position, M = journey length), frames in order,
  then the normal logic tree, then diagnostics — exactly like a
  multi-screen render, with the header lines added. `--json` keeps
  only the journey screens in the `screens` array, in journey
  order (repeats as separate entries). Names are trimmed of
  surrounding whitespace (`"Home, Detail"` works). Errors (exit
  2): an unknown screen name in the list, an empty list, an empty
  element (`"Home,,Detail"`), or a whitespace-only element.
  Conflicts (exit 2): `--storyboard` with `--screen` (a
  positional screen counts as `--screen`), `--walk`, `--watch`,
  `--diff`, or `--update-goldens`. It composes with `--ascii`,
  `--width`, `--device`, `--output`, `--strict`, and `--json`.
  Walk/watch are untouched: storyboard is a render mode, not a
  loop.
- `--update-goldens` — regenerate the checked-in golden files under
  `tests/golden/preview/` (one `<Screen>.txt` frame per screen,
  `logic-tree.txt`, `preview.json`) from the current render. Frame
  goldens use the current `--ascii` setting and no color. For testing
  without touching the real goldens, set `CCB_GOLDEN_DIR` to a scratch
  directory (test-only override); otherwise the directory is found by
  walking up from the working directory to `tests/golden/preview/`.

### JSON shape (`--json`)

One JSON object, no trailing newline, deterministic (stable order, no
timestamps). `screens` are in flow-file order; `boxes` is the screen's
box tree with the root box as the tree root; `branches` are collected
depth-first from the tree; `diagnostics` are in emission order:

```json
{"event":"preview","version":2,
 "screens":[{"name":"Home","entry":true,"reachable":true,
   "boxes":{"type":"screen","label":null,"x":0,"y":0,"w":38,"h":18,
     "children":[{"type":"text","label":"My Notes","x":2,"y":2,
       "w":34,"h":1,"children":[]}]},
   "edges":[{"from":"Open note","target":"Detail","unknown":false,
     "args":["note-42"]}],
   "branches":[{"label":"when state",
     "entries":["is Loading","is Loaded","else"]}]}],
 "diagnostics":[{"code":"CCB-P1005","phase":"preview",
   "message":"navigation to unknown screen 'Ghost'","file":null,
   "line":-1}]}
```

`label`, `target`, and `file` are `null` when absent; `line` is -1 for
diagnostics with no source position. In JSON mode stdout carries only
this object; human-mode prose and diagnostics go to stderr or are
suppressed. The object carries `"version":2` alongside
`"event":"preview"` (version 2's only shape change over version 1
is the per-edge `"args"` array, string-or-null, `[]` when the nav
call has no extra arguments; resolved `@string` labels appear as
plain `label` values, a value change, not a shape change, and only
in projects that use `@string`). With `--storyboard`, the `screens`
array contains only the journey screens, in journey order.
- Exit codes: 0 ok (warnings allowed), 1 preview errors (missing nav
  target, duplicate screen), 2 configuration errors. Under `--strict`,
  subset warnings are errors, so they exit 1 as well.

## Build gate: `ccb check` / `ccb build` fail on preview errors

Opt-in, off by default. Enable it with `[preview] gate = true` in
ccb.toml, or per-invocation with `ccb check --preview-gate` /
`ccb build --preview-gate`; `--no-preview-gate` forces it off.
Precedence: `--no-preview-gate` beats `--preview-gate` beats the
config key; passing both flags is a usage error (exit 2).

When enabled, the gate runs preview diagnostics for the project:

- In `check`, after toolchain validation; in `build`, before any
  pipeline stage, so a failing gate never starts a build.
- Severity rule: any preview ERROR diagnostic fails the gate
  (missing nav target CCB-P1005, duplicate screen CCB-P1008, and
  CCB-P1001/CCB-P1014 under strict); WARNING diagnostics alone never
  fail it.
- Failure exits 1 with one concise line per failing diagnostic on
  stderr (`CCB-P1005: navigation to unknown screen 'Ghost'`); no
  screen renders are printed. In `--json` mode the failure still
  surfaces as those stderr lines plus the nonzero exit, not as a
  JSON event.
- Projects with no usable `[preview]` section pass silently — the
  gate never forces preview onto a project that doesn't use it. When
  config load itself fails, the gate is skipped and the normal
  config error is reported instead (check exits 2; build's pipeline
  reports it).

## Diff mode: `ccb preview --diff <snapshot-dir>`

Compares the current renders against a saved snapshot directory in
the golden-file format (one `<Screen>.txt` frame per screen,
`logic-tree.txt`, `preview.json` — the same files
`--update-goldens` writes, rendered with the same calls, so a
snapshot made by `--update-goldens` diffs identical).

- Exit 0 when identical. On differences: exit 1 and one unified diff
  per differing file, headed by the file name, plus a summary line
  naming all differing files (`differing files (2): Home.txt,
  preview.json`).
- A current screen missing from the snapshot is reported as
  `missing: <file> (not in snapshot)`; a snapshot file with no
  matching current screen as `stale: <file> (no matching current
  screen)`. An unreadable snapshot directory, a project with no
  usable `[preview]` section, or an unknown `--screen` name exits 2
  with a clean diagnostic; outside any project, `--diff` is a usage
  error (exit 2) rather than plain preview's "not applicable".
  Preview errors in the project itself do not change the diff exit
  code — the render still compares.
- `--diff` + `--screen NAME` diffs only that screen's `<Screen>.txt`
  file. `--diff` + `--output FILE` writes the diff report to the
  file instead of stdout. `--diff` + `--json` emits one
  machine-readable object instead of human diff text:
  `{"event":"preview-diff","identical":bool,"files":[{"name":...,
  "status":"identical|different|missing|stale","diff":string-or-null}]}`
  (the diff string is present only for "different" files).
- Conflicts (exit 2): `--diff` cannot be combined with `--watch`,
  `--walk`, or `--update-goldens`.

## Walk mode: `ccb preview --walk [screen]`

Walk mode is static exploration of the logic tree, not an emulator:
it moves through screens and edges the project already declares. It
never executes Kotlin, never evaluates a `when`/`if` condition, never
fetches data, and never animates.

`ccb preview --walk` starts at the entry screen (`Home` when one
exists, else the first screen); `ccb preview --walk Detail` (or
`--screen Detail`) starts at the named screen. Each view shows the
screen's wireframe, its outgoing edge list (showing the same
argument text as the logic tree, e.g. `-> Detail("note-42")`) with a `>` cursor, the
back stack, and footer hints.

Keys: up/down arrows or `j`/`k` move the selection (selection clamps
at the ends, it never wraps); Enter follows the selected edge;
`b` or Backspace goes back; `q` quits. `t` enters branch-pick mode
for the current screen, listing its branch entries as
`when state: is Loaded`; up/down move among them and Enter picks one.
A pick never evaluates the condition: it follows the first edge whose
source label names a component inside the picked entry, or, when the
entry has no outgoing edge, records the pick and stays on the screen.
Esc (or `b`) leaves branch mode without picking.

Following an unknown edge — a dynamic destination, or a target screen
that does not exist — shows it as unknown and stays on the current
screen; walk mode never invents a destination. Back at the root
screen is a no-op. Ctrl-C quits and restores the terminal, as do
SIGINT/SIGTERM. Walk mode requires a terminal: with non-TTY stdin or
stdout it prints an error and exits 2 instead of hanging.

## Watch mode: `ccb preview --watch [screen]`

Watch mode renders once immediately, then re-renders on every change
to the preview inputs — no restart, no rebuild. It composes with
`--screen NAME` (or a positional screen), `--ascii`, `--width`,
`--device`, `--json`, `--strict`, and `--output`; all of them keep
their single-render meaning on every re-render.

Watched set: every CSS file under the styles directory, every file
under the flow directory, and `ccb.toml` itself. New and deleted
files count as changes, since each rescan is diffed against the
previous one. `ccb.toml` is re-loaded on every change, so editing
the `[preview]` paths re-points the watch at the new directories.

Refresh framing depends on where the render goes:

- TTY stdout: the screen is cleared first (`ESC[2J ESC[H`), then the
  fresh render. The initial render is not cleared.
- Piped human mode: renders are separated by a delimiter line on its
  own line, `--- ccb preview refresh ---`, before each re-render
  after the first.
- `--json` piped mode: no delimiter — each render is one JSON object
  plus a newline, i.e. valid JSONL, flushed after each render.
- `--output FILE`: the file is rewritten on each change, stdout stays
  empty, and a refresh notice plus diagnostics go to stderr.

Changes are debounced ~200ms: a burst of saves (or one save that
touches several files) produces a single re-render once the inputs
have been quiet for 200ms. A broken save never kills the loop: a
half-written TOML, broken CSS, or broken Kotlin prints diagnostics
and the loop keeps watching on the last good render, re-rendering
when the inputs become valid again. If the selected screen disappears
after a change, the loop prints a diagnostic naming it and keeps
watching instead of crashing or exiting.

Quit with Ctrl-C (or SIGTERM): the loop exits 0, flushing stdout
first. Watch mode never enters raw mode, so there is no terminal
state to restore.

Conflicts (exit 2, same style as the other preview conflicts):
`--watch` cannot be combined with `--update-goldens`, and it cannot
be combined with `--walk` — an interactive loop and a watch loop
cannot share stdout semantics. `--watch` with `--output` is allowed.

## Worked example

The reference sample app lives in `examples/preview-sample/`. Run it
with:

```sh
cd examples/preview-sample
ccb preview
ccb preview Detail
ccb preview --json
```

Its checked-in renders are the golden files under
`tests/golden/preview/`; the test suite diffs actual output against
them, so any renderer change that alters output must update the
goldens deliberately, never silently.
