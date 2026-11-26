# CCB Preview — the UI convention

[Docs index](index.md)

*ccb-preview*

`ccb preview` renders a terminal wireframe for projects that follow this
convention. A linter with a picture, not an emulator: it never executes
Kotlin, evaluates conditions, fetches data, or animates. Anything outside
the convention warns and falls back; it never crashes and never guesses.

Sub-pages: [handlers](preview-handlers.md) · [real code](preview-real-code.md) ·
[scenarios](preview-scenarios.md) · [traces](preview-traces.md) ·
[styles + layout](preview-styles.md) · [modes](preview-modes.md) · [LSP](preview-lsp.md)

## Config

*ccb-preview-config*

```toml
[preview]
styles = "styles"   # directory of CSS files
flow = "flow"       # directory of Kotlin flow files
resources = "src/main/res/values"   # optional: values directory holding strings.xml / colors.xml
layout = "res/layout"     # optional: layout XML dir or file
nav_graph = "res/navigation"  # optional: nav-graph XML file or dir
phone_width = 390   # dp, informational: scales px values to columns
phone_height = 844  # dp, informational
strict = false      # true: subset warnings become errors
gate = false        # true: preview errors fail ccb check / ccb build
```

- `layout` / `nav_graph` are overrides. Without the key, preview tries
  in order: `<paths res>/layout` and `<paths res>/navigation`, then
  `<resources>/layout` and `<resources>/navigation`. Each candidate is
  checked for existence; a missing candidate falls through. No candidate
  at all means an empty input set, not an error. A configured path that
  doesn't exist is a config error (CCB-E1006). A value may be one file
  or a directory (every `*.xml` under it, sorted). Note the trap:
  `<resources>/layout` only exists when `resources` is the res root —
  with the standard values-dir value it never matches, so discovery
  relies on `[paths] res` (which has no default; set it, e.g.
  `src/main/res`) or on the explicit keys. With neither, preview
  silently finds no layouts or graphs and still exits 0, with graph
  navigation unknown and layout-bound screens unreachable.
- `resources`, when set, must be an existing directory. It holds
  `strings.xml` / `colors.xml`, parsed by a subset parser:
  `<string name="n">v</string>`, `<color name="n">#rrggbb</color>`.
  Comments skipped; `&amp; &lt; &gt; &quot; &apos;` decoded in string
  values. A missing file is an empty table. A malformed entry is skipped
  with CCB-P1016 naming the file; a non-hex color the same way.
  Resolution runs at load, before layout: a label exactly
  `@string/name` is replaced by the resolved string; a `color` or
  `background-color` exactly `@color/name` is replaced by the resolved
  hex at rule load and counts as resolved for the CCB-P1018 contrast
  check. An unresolved reference warns CCB-P1016 and the literal text is
  kept as-is.
- `styles` and `flow` are optional individually, but a project with
  neither is "preview not applicable" (exit 0) — unless it has real
  Kotlin sources: a `[preview]` section with no styles and no flow still
  previews when `[paths] kotlin` (or its fallbacks) exists. With no
  `[preview]` section at all, `ccb preview` prints "preview not
  applicable to this project" and exits 0.
- Frame width is `--width` (default 40 columns, minimum 24 — smaller
  values clamp with a warning).

## Flow files: screens and components

*ccb-preview-flow*

Flow files are Kotlin files in the `flow` directory. Preview parses
structure only — no typecheck, compile, or run.

- A top-level `fun Name(` is a screen named `Name`. Inside a screen,
  component calls nest by braces.
- Known components (case-insensitive): `Column`, `Row`, `Text`, `Button`,
  `List`, `TextField`, `Image`, `Box`, `Spacer`. Anything else becomes a
  generic box and warns (CCB-P1012) — still renders, labeled with its
  type.
- A component's label is its first string argument: `Text("My Notes")`,
  `Button("Save", ...)`.
- Navigation is any `nav("Target")` or `navigateTo("Target")` call inside
  a component's arguments or block. The edge is recorded from the
  enclosing component's label. A dynamic destination (variable, template,
  any non-literal) becomes an `unknown` edge — shown, not resolved, not
  an error.
- Arguments after the target are recorded on the edge: a single string
  literal without `$` is its literal text, `name = <string literal
  without $ | number>` is the raw `name = value` text as written (never
  evaluated), anything else (identifiers, templates with `$`,
  expressions, calls) is `unknown`. A dynamic target with arguments
  keeps `target: null, unknown: true` and still records its args. Edges
  deduplicate on target, unknown flag, AND args. Tree format:
  `-> Detail("note-42", unknown) via "Open note"`; an edge with no extra
  args keeps the old format.
- `when (state) { ... }` and `if (cond) { ... }` become branch nodes
  labeled with their conditions. Branches are shown, never evaluated.
  `when` entries (`is Loading -> ...`, `else -> ...`) become entry nodes
  under the branch. An `else` after an `if` becomes an entry node labeled
  `else`.
- Entry screen: `Home` when one exists, else the first declared screen in
  sorted file order.

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

## Build gate: `ccb check` / `ccb build` fail on preview errors

*ccb-preview-gate*

Opt-in, off by default. Enable with `[preview] gate = true`, or
per-invocation with `ccb check --preview-gate` / `ccb build
--preview-gate`; `--no-preview-gate` forces it off. Precedence:
`--no-preview-gate` beats `--preview-gate` beats the config key; passing
both flags is a usage error (exit 2).

- In `check`, after toolchain validation; in `build`, before any pipeline
  stage, so a failing gate never starts a build.
- Severity: any preview ERROR fails the gate; WARNINGs alone never do.
  Which codes are errors: [diagnostics.md](diagnostics.md#preview-exit-1-unless-noted).
- Failure exits 1 with one concise line per failing diagnostic on stderr
  (`CCB-P1005: navigation to unknown screen 'Ghost'`); no screen renders
  are printed. In `--json` mode the failure still surfaces as those
  stderr lines plus the nonzero exit, not as a JSON event.
- Projects with no usable `[preview]` section pass silently. When config
  load itself fails, the gate is skipped and the normal config error is
  reported instead (check exits 2; build's pipeline reports it).

## Output and exit codes

*ccb-preview-output*

- `ccb preview` — every screen's wireframe, then the logic tree, then
  diagnostics. `ccb preview <screen>` (or `--screen NAME`) — one screen.
- `--json` — the same trees and diagnostics as structured data (see
  below). `--ascii` — ASCII frame characters instead of Unicode. Colors
  come from the CSS colors approximated to ANSI 256-color codes, unless
  `NO_COLOR` is set, the terminal is dumb, or `--ascii` is given.
- `--output FILE` — write the render to FILE instead of stdout: the
  human render, or the JSON object when `--json` is also given. Stdout
  carries nothing; diagnostics still go to stderr; exit codes unchanged
  (a preview error still exits 1 with the render written; an unwritable
  output file exits 2).
- `--device phone|tablet` — frame-width presets on top of `--width`:
  phone = 40 columns (the default), tablet = 80. An explicit `--width`
  always wins; any other value is a usage error.
- `--storyboard "A,B,C"` — render a journey, not the whole app: screen
  names in journey order (repeats allowed, each renders again). Human
  output prints each frame preceded by `Journey N/M: <Name>`, then the
  normal logic tree, then diagnostics. `--json` keeps only the journey
  screens in the `screens` array, in journey order (repeats as separate
  entries). Names are trimmed of surrounding whitespace. Errors (exit 2):
  an unknown screen name, an empty list, an empty element (`"Home,,Detail"`),
  or a whitespace-only element. Conflicts (exit 2): `--storyboard` with
  `--screen`, `--walk`, `--watch`, `--diff`, or `--update-goldens`. It
  composes with `--ascii`, `--width`, `--device`, `--output`, `--strict`,
  and `--json`.
- `--update-goldens` — regenerate the checked-in golden files under
  `tests/golden/preview/` (one `<Screen>.txt` frame per screen,
  `logic-tree.txt`, `preview.json`) from the current render. Frame goldens
  use the current `--ascii` setting and no color. Set `CCB_GOLDEN_DIR` to
  a scratch directory to test without touching the real goldens;
  otherwise the directory is found by walking up from the working
  directory to `tests/golden/preview/`.

Exit codes: see [diagnostics.md](diagnostics.md) (0 ok, warnings allowed;
1 preview errors; 2 configuration errors). Under `--strict`, subset
warnings are errors, so they exit 1 as well.

### JSON shape (`--json`)

*ccb-preview-json*

One JSON object, no trailing newline, deterministic (stable order, no
timestamps). `screens` are in flow-file order; `boxes` is the screen's box
tree with the root box as the tree root; `branches` are collected
depth-first from the tree; `diagnostics` are in emission order:

```json
{"event":"preview","version":2,
 "screens":[{"name":"Home","entry":true,"reachable":true,
   "boxes":{"type":"screen","label":null,"x":0,"y":0,"w":38,"h":18,
     "w_dp":390,"h_dp":180,
     "children":[{"type":"text","label":"My Notes","x":2,"y":2,
       "w":34,"h":1,"w_dp":349,"h_dp":10,"children":[]}]},
   "edges":[{"from":"Open note","target":"Detail","unknown":false,
     "args":["note-42"]}],
   "branches":[{"label":"when state",
     "entries":["is Loading","is Loaded","else"]}]}],
 "diagnostics":[{"code":"CCB-P1005","phase":"preview",
   "message":"navigation to unknown screen 'Ghost'","file":null,
   "line":-1}]}
```

`label`, `target`, and `file` are `null` when absent; `line` is -1 for
diagnostics with no source position. In JSON mode stdout carries only this
object; human-mode prose and diagnostics go to stderr or are suppressed.
Version 2 adds per-edge `"args"` (string-or-null array) and `"branch"`
(string-or-null), and per-box `"w_dp"`/`"h_dp"`. With `--storyboard`, the
`screens` array contains only the journey screens, in journey order.

## Worked example

*ccb-preview-example*

The reference sample app lives in `examples/preview-sample/`. Run it with:

```sh
cd examples/preview-sample
ccb preview
ccb preview Detail
ccb preview --json
```

Its checked-in renders are the golden files under `tests/golden/preview/`
— the test suite diffs actual output against them, so any renderer change
that alters output must update the goldens deliberately, never silently.
