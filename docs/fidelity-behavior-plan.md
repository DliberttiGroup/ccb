# Fork 2 fidelity + Level 2 behavior — plan (Part B Slices 1–3 implemented 2026-10-24; Part A fidelity design only)

User clarification 2026-10-24: "Fork 2" means real layout fidelity, and
the behavior level wanted is Level 2 (derived from code), narrowed for
now to buttons that take you from one screen to another. Part B
Slices 1–3 are implemented (2026-10-24); Part A fidelity is not
implemented yet. The governing constraint for both
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

Today a text leaf's width comes from character cells (wcwidth) and its
height from the row-sum convention, regardless of `font-size`. Change:

- Line height for a text leaf = `font-size * 1.2 + vertical padding`,
  rounded to rows. A 24px label is taller than a 14px label, which is
  the single most visible fidelity gap in the current frames.
- Text width scales with a font-size bucket (small / base / large):
  width in cells = ceil(chars * bucket factor). Bucket factors are
  documented constants, not glyph measurements — statistical, and
  labeled as such in the docs.
- `text-align` (already shipped) keeps working on the scaled width.

### F2 — An explicit DP mapping, and tap targets in DP

The codebase already knows the phone width: `phone_dp_width = 390`
against a 40-column frame (38 interior columns), so 1 column is
~10.26dp and 1 row is ~10dp. Today that mapping is implicit. Make it
explicit:

- Every box's JSON gains its size in DP alongside cells
  (`w_dp`, `h_dp`, computed from the mapping, not measured).
- Diagnostic CCB-P1017 (tap target) migrates from cell thresholds
  (8 cols / 2 rows) to DP thresholds (48dp wide; height from the
  F1 computed line height, NOT from row count). The cell threshold
  exists precisely because a naive 48dp height rule would flag every
  unpadded button at 10dp/row; computing height from font-size +
  padding first is what makes the DP rule honest.
- Renders stay cell grids. DP numbers are estimates, and the docs
  say so — no false precision.

### F3 — Content-driven sizing made explicit

Mostly documenting and finishing what the sum convention already
implies, so the rules are stated instead of emergent:

- Text height = lines * line height (F1).
- Image boxes size from declared width/height (aspect preserved
  when only one side is declared).
- Button height = text lines + vertical padding (no more 1-row
  buttons by default unless padding is genuinely zero — which is
  exactly what P1017 should then flag).

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
2. P1017 migration: move tap thresholds to DP as part of F2, or
   keep cells and report DP alongside? Recommendation: migrate —
   a threshold that speaks the platform's unit is the point of F2.
3. Slice order: Slice 1 first, obviously; whether Slice 2 rides
   along or waits depends on whether conditional nav matters to
   the user's apps yet.
