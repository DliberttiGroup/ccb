# Fork 2 fidelity + Level 2 behavior — plan (Part A fidelity and Part B Slices 1–3 implemented 2026-10-24)

User clarification 2026-10-24: "Fork 2" means real layout fidelity, and
the behavior level wanted is Level 2 (derived from code), narrowed for
now to buttons that take you from one screen to another. Part B
Slices 1–3 are implemented (2026-10-24); Part A fidelity is
implemented (2026-10-24, see below). The governing constraint for both
halves: no interpreter, no layout engine. Every step below is
computable from static information, and anything outside the
recognized shapes is `unknown`, never guessed.

## Part A — Fidelity, bounded (Fork 2 without the engine)

Full fidelity would mean reimplementing Android's measure/layout pass
plus font metrics plus ConstraintLayout solving — and still not matching
a device exactly (font availability, OEM variation, dynamic type). The
bounded version below takes only the fidelity wins that are computable
from the box tree + rules, and keeps the "sketch, device is truth"
stance. No solver, no font files, no HarfBuzz/FreeType.

### F1 — Size-aware text (cells stay the unit, sizes stop lying)

Implemented 2026-10-24. A text leaf's height comes from its line
height, not the old 1-row base:

- Line height for a text leaf = `font-size * 1.2 + vertical padding`,
  rounded to rows at one row per ~10dp: rows = round(font-size *
  1.2 / 10), minimum 1 row, 1 row when no rule sets `font-size`.
  A 24px label is 3 rows tall, a 16px label 2, a 12px label 1.
  Applies to `text`, `button`, and `textfield` leaves; lists,
  images, spacers, and generic leaves keep their old heights.
- Text width scales with a font-size bucket (small < 14px: 4/5,
  base 14–23px: 1, large ≥ 24px: 5/4): the width used for the
  `text-align` offset is ceil(display width * factor), rounded up.
  Bucket factors are documented constants, not glyph measurements
  — statistical, and labeled as such in the docs. Painting still
  uses raw cells, so only alignment shifts; the P1009 clip check
  keeps comparing the actually-painted width (scaling it would
  warn about text the frame shows fitting, or miss text it shows
  clipped).
- `text-align` (already shipped) works on the scaled width: for
  buttons and textfields only the label text scales, the `[ ]`
  chrome does not.

### F2 — An explicit DP mapping, and tap targets in DP

Implemented 2026-10-24. The codebase already knows the phone width:
`phone_dp_width = 390` against a 40-column frame (38 interior
columns), so 1 column is ~10.26dp and 1 row is ~10dp. That mapping
is now explicit:

- Every box's JSON gains its size in DP alongside cells
  (`w_dp`, `h_dp`, computed from the mapping, not measured):
  w_dp = round(w * phone_dp_width / interior_cols), h_dp = h * 10.
  Additive per-box fields, so the JSON version stays 2.
- Diagnostic CCB-P1017 (tap target): DECIDED — the thresholds stay
  in cells (8 columns / 2 rows) and the message reports the DP
  size alongside (e.g. `is 5x1 cells (51dp x 10dp; minimum 8x2
  cells)`). A 48dp migration was rejected on the numbers: F1 line
  heights are 20–30dp for typical text, so a 48dp height rule
  would flag nearly every button including well-formed ones, and
  a 48dp width rule (≈ 5 columns) would be looser than the
  current 8 columns (≈ 82dp), un-flagging buttons squeezed by a
  row split. The cell rule's purpose — flagging genuinely
  degenerate targets — is preserved, and the dp numbers are
  visible for the designer. See `docs/preview.md` (CCB-P1017).
- Renders stay cell grids. DP numbers are estimates, and the docs
  say so — no false precision.

### F3 — Content-driven sizing made explicit

Implemented 2026-10-24:

- Text height = lines * line height (F1); leaves paint a single
  line, so one line height plus vertical padding.
- Image boxes size from declared width/height: a declared `height`
  (px scaled like the spacer convention, % of the parent content
  width) replaces the old fixed 3-row placeholder; a declared
  `width` was already honored by the fixed-width logic. Aspect
  preservation for a single declared side is NOT implemented: no
  intrinsic size exists in the convention, so there is nothing to
  preserve an aspect ratio against — the other side keeps its
  default. Documented as a limitation, not silently fudged.
- Button height = text lines + vertical padding (the F1 formula):
  no more 1-row buttons when a font-size rule is set; with no
  font-size rule the 1-row base stands (defaulting an unset size
  to 16px would be guessing).

### Explicitly OUT (the engine, still parked)

ConstraintLayout constraint solving, the full flexbox algorithm
(row sum + wrap stays), scrolling-container viewport math, baseline
alignment, RTL mirroring beyond `text-align`, real glyph metrics,
density buckets beyond the single 390dp phone width. Any of these
reopens the engine question and needs its own decision.

## Part B — Level 2 behavior, narrowed to buttons that navigate

### What already exists (the gap is smaller than it looks)

The flow parser already attributes a `nav()` call to its enclosing
component: an edge's `from` is the nearest enclosing component label
(see `pv_scan_navs` / `pv_nav_at` in `ccb_preview_flow.c`, which
recognize `nav` and `navigateTo`). So `Button("Open note")` with a
`nav("Detail")` inside its block already yields an edge from that
button. What is missing is REAL handler shapes: production Kotlin
does not write `Button { nav() }`. It writes listeners. Level 2
closes exactly that gap, for buttons only.

### Slice 1 — the recognized shapes (closed set) — implemented 2026-10-24

1. Listener lambdas: `view.setOnClickListener { nav("X") }`,
   including trailing-lambda and `onClick = { ... }` assignment
   forms, where `view` is a button variable.
2. Named handlers: `fun onOpenClick(v: View) { nav("X") }`
   referenced as `setOnClickListener(::onOpenClick)` or an equivalent
   method reference.
3. Navigation calls recognized: `nav("X")` and `navigateTo("X")`
   (already recognized), plus
   `startActivity(Intent(this, X::class.java))` where the destination
   is a class literal.

Destination must be a literal (string or class literal). Everything
else is `unknown`:

- Nav inside a conditional → Slice 2 (it becomes a branch edge,
  which the tree already supports — no evaluation, just labeling).
- Nav after async work (network callback, coroutine, posted runnable)
  → unknown.
- Nav with a computed destination (string building, `when` on a
  dynamic value, variable destination) → unknown.
- Nav inside a shared helper called from several handlers → unknown
  in Slice 1 (attribution would be guessing).

### Binding: handler to button, explicitly or not at all

The flow file's component labels are the target; the code's view
identifiers are the source. Binding is by an explicit mapping only
— a `[preview.bindings]` table mapping code identifiers to component
labels (e.g. `openButton = "Open note"`). No guessing from declaration
order, no fuzzy name matching. A handler whose button cannot be bound
produces a new warning diagnostic (next free CCB-P code): "handler
found, button not bound" — visible, honest, actionable.

### Precedence

Flow-declared `nav()` wins over a derived edge for the same button:
an explicit statement beats inference. Derived edges fill gaps for
buttons with no declaration. A derived edge and a declared edge to
different destinations for the same button is a warning (the two
sources disagree — the developer resolves it, the tool does not pick).

### Why this is not an interpreter (the core argument)

The implementation is four passes over tokens, reusing the parser
that already exists for the flow subset (functions, lambdas, call
expressions, identifiers, string literals, `if`/`when` shapes):

1. Parse the subset (already exists — extend, do not replace).
2. Handler extraction: find `setOnClickListener` / `onClick`
   attachments and capture the lambda body as a token range.
3. Nav scan inside that range (already exists: `pv_scan_navs`).
4. Binding + edge synthesis into the existing `pv_edge` structure
   (`from` = button label). Walk, storyboard, diff, and gate consume
   the edges unchanged.

At no point is anything evaluated: no state, no control-flow
execution, no value tracking, no function-call resolution beyond the
named-handler reference in Slice 1. The parser is a recognizer for
about six syntactic shapes. A shape outside the set yields `unknown`
by construction — that discipline is what keeps the work bounded.
The day the design needs "what does this variable hold" or "which
branch runs", it has become an interpreter, and that is the line
to stop at.

### Later slices (each separately shippable, stop at any)

- Slice 2 — implemented 2026-10-24: conditional nav → branch edges
  (braced and braceless `if`/`when` bodies in handlers, labeled
  never evaluated, nested conditionals compounded, `else if` links
  standalone); nav with literal arguments (extends the existing
  args machinery; non-literal arguments are `unknown` arg entries).
- Slice 3 — implemented 2026-10-24: XML `android:onClick` handlers
  (layout subset parser, `*Button` tags, `[preview.bindings]`
  view-id keys, edges attach to screens containing the bound label;
  unbound → CCB-P1019, label in no screen → CCB-P1021);
  Navigation Component (`navigate(R.id.x)` plus the string form)
  with a nav-graph file as the destination table (unlisted id or
  no graph file → unknown edge; locations: `[preview]`
  `layout`/`nav_graph`, else `<resources>/layout` /
  `<resources>/navigation`, else `<paths res>/layout` /
  `<paths res>/navigation`).

## Part C — How the two halves combine

A walk or storyboard frame then reads as a design review, not just
a picture: "tap Open note (52dp target, passes P1017) → Detail,
whose title is 24px (2 rows tall)". Fidelity makes the frames
measurable; behavior makes the transitions real (for buttons).
Neither half pretends to be the device.

## Part D — Test plan

- Fixtures per handler shape (lambda, named handler, startActivity)
  and per unknown case (async, computed destination, shared helper,
  unbound handler) — each unknown case asserts an `unknown` edge or
  the unbound warning, never a guessed destination.
- Fidelity fixtures: font-size scaling (14px vs 24px heights),
  DP reporting (known box → known DP), P1017 in DP (pass/fail
  boundaries at 48dp).
- Golden updates deliberate via `--update-goldens`, diffed against
  a snapshot: only frames whose text sizes or tap diagnostics
  change may move.
- Full ctest plain + sanitizer, all existing suites green.

## Open decisions for the user

1. Bindings mechanism: explicit `[preview.bindings]` table
   (recommended) vs a naming convention. Convention is less typing
   and more guessing; the recommendation is the table.
2. P1017 migration: RESOLVED 2026-10-24 — thresholds stay in cells,
   DP reported in the message. A 48dp height rule would flag nearly
   every button (F1 heights are 20–30dp for typical text) and a
   48dp width rule would loosen the width check from 82dp to ~5
   columns. See F2 above and `docs/preview.md` (CCB-P1017).
3. Slice order: Slice 1 first, obviously; whether Slice 2 rides
   along or waits depends on whether conditional nav matters to
   the user's apps yet.
