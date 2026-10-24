# Preview deeper coverage — plan (implemented 2026-10-24, one section)

All five items below ship together. The scope lock from `docs/m5-plan.md`
§1 still governs: preview is a linter-class static approximation. It
never executes Kotlin, never evaluates conditions, never fetches data.
Unknown → warning + placeholder, never a silent guess. No XML/Compose
support and no real layout fidelity — both stay out of scope.

## 0. Shared decisions

- **New diagnostic codes** (all warnings, always — even under
  `--strict`; `pv_diag_is_error` is unchanged, so the build gate is
  unaffected because it fails only on errors):
  - `CCB-P1016` unresolved `@string/` / `@color/` reference.
  - `CCB-P1017` tap target too small.
  - `CCB-P1018` text contrast below threshold.
  `docs/diagnostics.md` gains all three rows.
- **JSON version 1 → 2.** The only shape change: each logic-tree edge
  gains `"args"` (array of string-or-null, see §2). Resolved
  `@string` labels appear as plain `label` values (a value change,
  not a shape change, and only in projects that use `@string`). With
  `--storyboard`, the JSON `screens` array contains only the journey
  screens, in journey order (repeats appear once per occurrence).
  Everything else in the version-1 shape is unchanged.
- **Golden policy.** The sample app changes deliberately (see §6);
  regenerate with `ccb preview --update-goldens` and verify the diff
  is exactly the intended changes, nothing else. All other golden
  changes are bugs until proven otherwise.
- **Untouched behavior.** Watch, walk, gate, and diff keep their
  suites green; their semantics do not change except: walk's edge
  list shows edge arguments (§2), and the gate's error set does not
  grow (all new codes are warnings).

## 1. More CSS properties

New subset properties, each visibly affecting the wireframe:

- `text-align: left | center | right` (default left). Applies to
  `text`, `button`, `textfield`, and list-row painting: the painted
  string is offset within the leaf's content width (left = x,
  center = x + (cw - sw)/2, right = x + cw - sw, clamped at x).
  Unknown value → `CCB-P1014`.
- `border-width: <px>` (default 0). A scaled value ≥ 1 cell draws a
  1-cell outline around the box (Unicode frame chars, or ASCII under
  `--ascii`) in the border color; content is inset 1 cell on all
  sides. Layout accounts for it: a bordered box's content box shrinks
  by 1 cell per side, and its height grows by 2 rows (top + bottom
  border). Applies to leaves and containers alike; for containers
  the outline is drawn around the container box and children are
  laid out inside the inset content box. `border-width: 0` (or any
  value scaling to 0) means no border. Negative → `CCB-P1014`.
- `border-color: <hex|named>` (default: the rule's `color`, else the
  terminal default). Invalid color → `CCB-P1014`.
- `flex-wrap: nowrap | wrap` (default nowrap), row containers only.
  When wrapping, children are placed left-to-right and a child that
  would exceed the content width starts a new line; the new line's y
  advances by the previous line's height. Total row height follows
  the existing convention (sum of children's heights, now summed per
  line), so wrapping changes placement, not total height. Unknown
  value → `CCB-P1014`; `flex-wrap` on a non-row container is ignored
  with a `CCB-P1014` warning naming the selector.

Deliberately NOT added (with reasons, so nobody re-litigates):
`justify-content` / `align-items` (distribution semantics the
sum-based row/column model cannot express honestly),
`border-radius` (no wireframe-visible rounding at 1-cell scale),
`line-height` (text is single-row by design; it would only pad),
`border` shorthand (the two longhands above are the subset).

Implementation notes: `pv_rule` gains `has_text_align` (+ enum),
`has_border_width` + `border_width` (px), `border_color` string,
`has_flex_wrap` + `flex_wrap`. The CSS dispatch in `css_apply`
handles them like the existing properties. The renderer paints
alignment offsets in `paint_box` and draws outlines for bordered
boxes; layout insets bordered content boxes. Existing goldens must
not move except the deliberate sample changes (§6).

Tests: alignment offsets for left/center/right (incl. clamping when
the string is wider than the box); border outline + 1-cell inset +
height growth; border-color invalid → P1014; wrap line break at a
narrow width + no-wrap control at a wide width; wrap on a column →
P1014.

## 2. Screen arguments in the logic tree

The flow parser captures arguments on nav calls. `nav("Detail")`
today records only the target; after this change:

- `pv_edge` gains `char **args; size_t nargs;` (arena-owned). Each
  entry is either the argument's literal text or `NULL` (= unknown).
- Parsing (in `pv_nav_at`): after the target string argument, split
  the remaining top-level arguments by commas at paren depth 1 /
  brace depth 0. Classify each argument's raw trimmed text:
  - a single string literal without `$` → its literal text;
  - `name = <string literal without $ | number>` → the raw
    `name = value` text (static text, shown as written, never
    evaluated);
  - anything else (identifiers, templates with `$`, expressions,
    calls) → `NULL` (unknown).
- Edge dedup (`pv_edge_add`) compares target, unknown flag, AND args.
- Logic-tree rendering: `-> Detail("note-42", unknown) via "Open note"`.
  No extra args → the existing format, unchanged.
- JSON (version 2): edges gain `"args": ["note-42", null]`; an edge
  with no extra args carries `"args": []`.
- Walk mode's edge list shows the same argument text.
- A dynamic target with arguments (e.g. `nav(screen, "x")`) keeps
  `target: null, unknown: true` and still records its args.

Tests: literal arg, named arg, numeric named arg, dynamic arg →
null, mixed args, dedup with different args (two edges, not one),
dynamic target with args, tree format with and without args, JSON
args array incl. nulls.

## 3. `@string` / `@color` resolution

Lookup design (one mechanism, no magic fallback): a new optional
`[preview]` key:

```toml
[preview]
resources = "res"   # directory holding strings.xml and colors.xml
```

- The directory is resolved against the project root like `styles`
  and `flow`, validated as an existing directory when set, and added
  to the `[preview]` key allowlist (unknown key → existing config
  error, as for any other key).
- Files: `<dir>/strings.xml` and `<dir>/colors.xml`, Android values
  format, parsed with a tiny subset parser (no XML library):
  `<string name="n">v</string>`, `<color name="n">#rrggbb</color>`.
  XML comments skipped; `&amp; &lt; &gt; &quot; &apos;` decoded in
  string values. A malformed entry is skipped with a `CCB-P1016`
  warning naming the file; a missing file is simply an empty table
  (no diagnostic — a project may define only strings, only colors,
  or neither).
- Resolution points, both at load time in `pv_project_load`:
  - Component labels: a label that is exactly `@string/name`
    (the whole label, not embedded text) is replaced by the resolved
    string. Resolution happens after flow parsing and before layout,
    so clip widths are computed on the resolved text.
  - CSS colors: a `color` or `background-color` value that is
    exactly `@color/name` is replaced by the resolved hex at rule
    load (post-parse pass over the rule list). Only hex values are
    accepted from colors.xml; a non-hex color value is skipped with
    a `CCB-P1016` warning.
- Unresolved reference (name not in the table, or no resources
  directory configured): `CCB-P1016` warning naming the reference,
  and the literal `@string/name` / `@color/name` text is kept —
  rendered as-is, never a crash, never an invented value.
- JSON: resolved labels appear as `label` values; unresolved ones
  appear literally. No new JSON fields for resources.

Fixture: `tests/fixtures/preview/resources/` — its own `ccb.toml`
with `resources = "res"`, a flow file using `@string/title` and an
unresolved `@string/missing`, a stylesheet using `@color/primary`
and an unresolved `@color/missing`. The sample app itself does NOT
gain resources (its goldens stay clean of this feature).

Tests: string resolution, color resolution, unresolved string →
P1016 + literal, unresolved color → P1016 + literal, malformed
entry skipped with P1016, missing colors.xml file → no diagnostic,
entity decoding in string values.

## 4. Journey storyboards

Primary UX (one form, no edge-following ambiguity):

```sh
ccb preview --storyboard "Home,Detail,Form"
ccb preview --storyboard "Home,Detail" --json
```

- The argument is a comma-separated list of screen names, in journey
  order. Repeats are allowed (a journey may revisit a screen); each
  occurrence renders again.
- Human output: each screen's frame is preceded by a header line
  `Journey N/M: <Name>` (N = 1-based position, M = journey length),
  frames in order, then the normal logic tree, then diagnostics —
  exactly like a multi-screen render, with the header lines added.
- `--json`: the `screens` array contains only the journey screens,
  in journey order (repeats included as separate entries).
- Errors (exit 2): unknown screen name in the list, empty list,
  empty element (`"Home,,Detail"`), whitespace-only element. Names
  are trimmed of surrounding whitespace (`"Home, Detail"` works).
- Conflicts (exit 2, same style as the other preview conflicts):
  `--storyboard` with `--screen` (positional screen counts as
  `--screen`), `--walk`, `--watch`, `--diff`, or `--update-goldens`.
  `--storyboard` composes with `--ascii`, `--width`, `--device`,
  `--output`, `--strict`, `--json`.
- Walk/watch are untouched: storyboard is a render mode, not a loop.

Tests: order + header lines, repeats render twice, unknown name →
exit 2, empty list → exit 2, conflict with `--screen` → exit 2,
JSON screens array filtered + ordered, `--output` writes the
storyboard render.

## 5. More lint diagnostics

### CCB-P1017 — tap target too small (warning)

- Applies to `button` and `textfield` boxes only, after layout.
- Threshold (in wireframe cells, stated here and in `docs/preview.md`,
  not buried in code): warn when `w < 8` columns OR `h < 2` rows.
- Why cells, not dp: the wireframe's vertical quantum is one row
  (≈10dp at the default 40-column frame), so a 44dp height rule
  would flag every unpadded button including all reasonable
  defaults. The cell threshold targets genuinely degenerate cases
  (a button squeezed to a few columns by a row split, a 1-row
  button with zero padding). Documented dp equivalents at the
  default frame: 8 columns ≈ 82dp wide, 2 rows ≈ 21dp tall.
- Message names the component and its size, e.g.
  `tap target too small: button "Go" is 5x1 cells (minimum 8x2)`.

### CCB-P1018 — text contrast below threshold (warning)

- Applies to `text`, `button`, and `textfield` leaves, after layout.
- Colors: foreground = the leaf rule's `color`; background = the
  leaf rule's `background-color`, else the `screen` rule's
  `background-color`. If either side is missing or unresolvable to
  RGB (hex `#rgb`/`#rrggbb` or the renderer's named-color table),
  skip silently — no diagnostic, because the contrast cannot be
  judged. Resolved `@color` values (§3) count as resolved.
- Ratio: WCAG relative luminance, contrast = (L1 + 0.05) /
  (L2 + 0.05). Threshold: **4.5:1** normally, **3.0:1** when the leaf
  rule's `font-size` is ≥ 24px (WCAG large-text threshold).
- Message includes the ratio to one decimal, e.g.
  `text contrast 2.1:1 below 4.5:1 for text "Note detail"`.
- Implementation: a shared `pv_color_rgb()` helper (declared in
  `ccb_preview.h`, defined next to the named-color table in
  `ccb_preview_render.c`) so the diagnose pass and the renderer use
  one RGB resolution.

Tests: tap small-width, small-height, exact-boundary pass (8x2),
large target pass; contrast fail, pass, large-text 3.0 pass at a
ratio between 3.0 and 4.5, missing-fg skip, missing-bg skip,
named-color resolution, `@color`-resolved background.

## 6. Sample-app changes (deliberate golden changes only)

- `flow/Home.kt`: `nav("Detail")` → `nav("Detail", "note-42")`
  (literal-arg demo on the "Open note" edge).
- `flow/Detail.kt`: `nav("Home")` → `nav("Home", currentId)`
  (unknown-arg demo on the "Back" edge).
- `styles/Detail.css`: add `text { text-align: center; }`
  (alignment demo; verify the Detail frame moves its text).
- `styles/Form.css`: add a `border-width` + `border-color` rule on
  `textfield` (border demo; verify the Form frame gains outlines).
- Nothing else in the sample app changes. Flex-wrap is demonstrated
  by fixture + unit tests only, so the Home frame's row does not
  move for unrelated reasons. After regenerating goldens, diff them
  against the pre-change snapshot and confirm the diff contains
  exactly: the two logic-tree edge changes, the Detail text shift,
  the Form textfield outlines, and the JSON `args` + version bump —
  no other frame moves.

## 7. Test and verification plan

- Extend `tests/test_preview_css.c` (new properties), extend
  `tests/test_preview_flow.c` (args).
- New suites: `tests/test_preview_resources.c`,
  `tests/test_preview_storyboard.c`, `tests/test_preview_lint.c`
  (P1017 + P1018). Wire all new binaries into CMake/CTest the same
  way the existing preview suites are wired.
- Existing suites must pass untouched, including watch, walk, gate,
  diff, flags, scope, lists, width, and both fuzz harnesses
  (CSS + flow corpus + mutations, zero crashes).
- Full `ctest` plain AND `-DCCB_SANITIZE=ON` (ASan+UBSan+LSan).
- Live demo (for the final report): one render showing a new CSS
  property, one logic-tree edge with arguments, one resolved
  `@string`/`@color`, one storyboard run, one run triggering each
  new diagnostic (P1016, P1017, P1018).
- Docs updated in the same change: `docs/preview.md` (all five
  items, thresholds, storyboard UX, resource lookup, JSON v2 note),
  `requirement.md` §10 (new bullets + §10.3/§10.4 updates),
  `docs/diagnostics.md` (P1016/P1017/P1018 rows).
