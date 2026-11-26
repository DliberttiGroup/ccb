# Styles: one CSS file per screen, plus common.css

[Docs index](index.md) · [Preview](preview.md)

*ccb-preview-styles*

- Scope: `common.css` (when present in the styles directory) applies to
  every screen. `<Screen>.css` applies to that screen only, and only when
  a screen of that name exists in the flow files. Any other CSS file is
  ignored with a CCB-P1015 warning and is never parsed. Files are parsed
  in sorted order, `common.css` first, then per-screen files. A per-screen
  file redeclaring a selector keeps the earlier rule for other screens and
  adds a scoped copy for its own screen: the copy carries the earlier
  properties plus the per-screen file's declarations, and layout prefers
  the exact-scope rule over the common one.
- Selectors are `screen` (the root box) or a lowercase component type
  (`column`, `row`, `text`, `button`, `list`, `textfield`, `image`, `box`,
  `spacer`). Later declarations for the same selector override earlier
  ones property by property.
- Supported properties: `display` (block, flex), `flex-direction` (row,
  column), `width`/`height` (px, %), `padding`, `margin` (px, 1–4 values
  as in CSS), `background-color`, `color` (hex or named, or an
  `@color/name` reference resolved via `[preview] resources`), `font-size`
  (px, sets the text-leaf line height — round(font-size * 1.2 / 10) rows,
  minimum 1, 1 row when no rule sets it — scales the `text-align` width by
  font-size bucket (small < 14px: 4/5, base 14–23px: 1, large ≥ 24px: 5/4),
  and selects the CCB-P1018 contrast threshold), `position` (static,
  absolute), `top`/`left` (px, absolute only), `gap` (px), `text-align`
  (left, center, right), `border-width` (px), `border-color` (hex or
  named), `flex-wrap` (nowrap, wrap; row containers only).
- NOT supported (each warns as an unknown property, CCB-P1001):
  `justify-content`, `align-items`, `border-radius`, `line-height`, and
  the `border` shorthand (use `border-width` + `border-color`).
- Anything else warns with the property name and location (CCB-P1001
  unknown property, CCB-P1014 unsupported value) and rendering continues
  with a fallback. Under `--strict` (or `[preview] strict = true`) those
  warnings become errors.

Example:

```css
screen { background-color: #ffffff; padding: 16px; }
text { color: #111111; font-size: 16px; }
button { background-color: #6200ee; color: #ffffff; margin: 8px; }
```

## Layout model (terminal columns and rows)

*ccb-preview-layout*

- The phone frame is `--width` columns wide (default 40); the content
  interior is frame minus the two border columns.
- px values scale to columns/rows by `interior / phone_width` (so 16px of
  padding on a 390dp phone at 40 columns is about 2 rows). `%` widths and
  heights are relative to the parent content size.
- Column containers stack children vertically, each full width minus
  margins. Row containers split the width: fixed sizes honored first, the
  remainder shared equally among auto-sized children.
- Text leaves (`text`, `button`, `textfield`) are as tall as their line
  height: round(font-size * 1.2 / 10) rows, minimum 1 row, 1 row when no
  rule sets `font-size` — so a 16px label is 2 rows tall and a 24px label
  is 3. The painted text itself still sits on one row (top-aligned) inside
  the leaf, clipped to its box (clipping warns, CCB-P1009). Buttons render
  as `[ label ]`, text fields as `[label]`, lists as one `• <label>` row
  per child item when the List box has children (a child with no label
  renders as `• item`); a bare List with no children renders 4 `• item`
  placeholder rows and emits no diagnostic for the placeholder. A list
  with children is exactly one row per child tall (plus padding); a bare
  list is 4 rows tall (plus padding); images as a `[ image ]` block, 3
  rows tall, or the rule's declared `height` when one is set (a declared
  `width` is honored as for any box). Unknown components render as
  `[TypeName]`.
- Leaf content is inset by the leaf's own scaled padding: a leaf paints at
  (box x + left padding, box y + top padding), clipped to the box width
  minus left and right padding. Parent padding insets children the same
  way, through the parent content box.
- `text-align` offsets the painted string within the leaf's content width
  (left = content edge, center = centered, right = flush right, clamped at
  the content edge when the string is wider); it applies to `text`,
  `button`, `textfield`, and list rows. For `text`, `button`, and
  `textfield` the offset uses the font-size-scaled string width (buckets
  above), not the raw painted width — only the offset shifts, painting
  still clips at the box edge.
- Every box's size is also reported in dp in JSON (`w_dp`, `h_dp`): w_dp =
  round(w * phone_width / interior), h_dp = h * 10 (one row is ~10dp).
  Estimates from the cell grid, not measurements.
- A `border-width` that scales to at least 1 cell draws a 1-cell outline
  around the box (Unicode frame chars, or ASCII under `--ascii`) in the
  `border-color`, else the rule's `color`, else the terminal default.
  Content is inset 1 cell on all sides, so a bordered box's content box
  shrinks by 1 cell per side and its height grows by 2 rows (top + bottom
  border); for containers the outline goes around the container box and
  children are laid out inside the inset content box. `border-width: 0`
  (or any value scaling to 0) means no border; a negative width or an
  invalid border color warns (CCB-P1014).
- In a row container with `flex-wrap: wrap`, children are placed
  left-to-right and a child that would exceed the content width starts a
  new line; the new line's y advances by the previous line's height (its
  tallest occupied child height). Total row height is unchanged by
  wrapping: it still sums every child's height, as in the nowrap model.
  `flex-wrap` on a non-row container is ignored with a CCB-P1014 warning
  naming the selector.
- Absolute-positioned boxes do not affect parent height. Boxes that overlap
  (CCB-P1003) or extend past the frame (CCB-P1004) warn.

## Lint diagnostics (after layout)

*ccb-preview-lint*

- CCB-P1017 — tap target too small (warning). Applies to `button` and
  `textfield` boxes only, after layout. Warns when `w < 8` columns OR (`h
  < 2` rows with a known height), in wireframe cells. Height counts only
  when the effective rule sets `font-size` (without one, the 1-row default
  is not a measurement). A button with no sizing rules at all is 38x1 by
  default and no longer warns. The message reports the dp size alongside
  the cells, e.g. `tap target too small: button "Go" is 5x1 cells (51dp x
  10dp; minimum 8x2 cells)`. At the default frame: 8 columns ≈ 82dp wide,
  2 rows = 20dp tall.
- CCB-P1018 — text contrast below threshold (warning). Applies to `text`,
  `button`, and `textfield` leaves, after layout. Foreground = the leaf
  rule's `color`; background = the leaf rule's `background-color`, else
  the `screen` rule's `background-color`. If either side is missing or
  unresolvable to RGB (hex `#rgb`/`#rrggbb` or the renderer's named-color
  table), the check skips silently. Resolved `@color` values count as
  resolved. Ratio is WCAG relative luminance, contrast = (L1 + 0.05) /
  (L2 + 0.05). Threshold: **4.5:1** normally, **3.0:1** when the leaf
  rule's `font-size` is ≥ 24px (WCAG large-text threshold). The message
  includes the ratio to one decimal, e.g. `text contrast 2.1:1 below
  4.5:1 for text "Note detail"`.

Both are warnings always — even under `--strict` — so they never fail the
build gate, which fails only on errors.
