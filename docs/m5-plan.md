# M5 — Preview plan

Terminal preview for CCB UI-convention projects: static box tree + logic
tree rendered as a phone-frame wireframe. This plan is the implementation
brief for M5. The normative spec stays in `requirement.md` §10; this
document adds the considerations, edge cases, and build order.

Status: planning only (2026-10-24). `ccb preview` is currently a stub.
CCB is a private tool for one user; no public-release constraints apply.

## 1. Scope lock (decide first, defend always)

Preview is a **linter-class static approximation**, not an emulator and
not a layout engine. It never executes Kotlin, never evaluates conditions,
never fetches data, never animates. Every input outside the documented
subset becomes a warning + fallback, never a crash and never a silent
guess. The single biggest risk to M5 is fidelity creep (real flexbox,
real Kotlin semantics, real Android framework behavior). If a feature
needs any of those, it is out of scope by design.

## 2. Prerequisites before code

1. **Freeze the CCB UI convention.** Which files hold CSS, which folder
   holds flow logic, how both are declared in TOML `[preview]`. Until
   this is written down, preview has no input contract.
2. **Build the reference sample app first.** A small multi-screen app
   (home list, detail, settings, one form, one empty state) following
   the convention. It is the golden test for everything below and the
   demo for docs. No sample app, no M5.
3. **Freeze the TOML `[preview]` schema** (css paths, flow dir, screen
   width default, strict flag). Preview with no `[preview]` section
   prints "preview not applicable" and exits 0 — never an error, never
   a hallucinated render.

## 3. Architecture

One module, `ccb_preview`, with three separable stages (each unit
tested on its own):

- **CSS parser → box tree → layout.** Parses the documented CSS
  subset (block/flex, px/%, padding/margin, colors, font-size, absolute
  subset, gap). Everything else warns and falls back.
- **Kotlin structure parser → logic tree.** Screens/components, nav
  calls → edges, `when`/`if` branches labeled, never evaluated.
  Dynamic destinations become `unknown` nodes.
- **Diagnostics pass.** Runs over both trees: overlaps, off-screen
  boxes, missing nav targets (error), unreachable screens (warning),
  empty screens (warning), duplicate routes (error).
- **Renderer.** Dumb by design: it draws a box tree inside a phone
  frame. All intelligence lives in the layout and diagnostics passes,
  never in the renderer.
- **JSON emitter.** `--json` emits the same trees + diagnostics the
  human render shows. No JSON-only or human-only facts.

Reuse existing modules: `ccb_toml`, `ccb_slice`, `ccb_str`, `ccb_diag`
(codes in the `CCB-P` range), `ccb_json`, arena allocator. No new
dependencies. Pure C, no JVM, target < 1 s on small apps (agents will
run preview often).

## 4. Phases and acceptance criteria

- **P0 — boxes.** CSS parser + layout + wireframe renderer for one
  screen. Acceptance: the sample app's home screen renders and matches
  a checked-in golden file byte for byte.
- **P1 — logic tree.** Kotlin structure parser + nav edges + tree
  printout. Acceptance: the sample app's tree shows all screens,
  correct edges, labeled branches, and `unknown` for dynamic targets.
- **P2 — diagnostics.** Full diagnostics pass + `--strict` (warnings
  become errors, for CI). Acceptance: each diagnostic in §5 has a
  test that triggers it and only it.
- **P3 — walk mode (v2 candidate).** Interactive step-through of the
  logic tree (arrow keys / j-k, back, quit). Explicitly NOT in the
  first M5 cut; it needs a terminal input loop and is the easiest
  place to blow up scope.

## 5. Edge cases (each needs a test)

**CSS/layout:** unclosed braces; unknown properties; `%` of an unknown
parent; flex with mixed fixed/flex children; absolute positioning with
no positioned ancestor; overlapping boxes; text longer than its box
(clip, and note it — never wrap silently); empty screens; deep nesting
(depth cap + warning); `@import` (out of subset, warn); duplicate
screen names (error); zero-size boxes; negative margins; padding
larger than the box; percent rounding that sums past the parent.

**Kotlin/structure:** files that don't parse (report, don't crash);
nav calls with dynamic destinations (template strings, variables →
`unknown` edge); `when`/`if` with complex conditions (label, don't
evaluate); multiple entry points; fragments (out of convention, note
and skip); Kotlin files outside the declared flow folder (ignored with
a note); same route registered twice (error); missing nav target
(error); unreachable screen (warning); empty screen (warning).

**Project:** missing `[preview]` section (not applicable, exit 0);
missing CSS file (error with path); empty flow folder (warning, no
screens); huge projects (cap rendered screens, warn, and say so).

**Terminal:** unknown size (default 80x24, `--width` override);
width under 40 columns (minimum-width handling, not a crash); no ANSI
color (`NO_COLOR`, dumb terminals → plain output); Unicode box chars
that don't render in some fonts (`--ascii` fallback); never assume a
dark or light background — use explicit colors or default foreground.

## 6. Useful features (beyond the spec minimum)

- `ccb preview` lists all screens; `ccb preview <screen>` renders one.
- Diagnostics are first-class: numbered, coded (`CCB-P####`), and
  `--strict` turns them into CI failures.
- The logic tree marks reachability: entry screen, unreachable
  screens, dead ends, and back-navigation edges.
- Resolve `@string` references from `strings.xml` where possible;
  fall back to a placeholder when unresolvable.
- `--diff <snapshot>` (v2): compare against a saved render to catch
  regressions. Cheap once golden files exist.
- Deterministic output: same inputs → same bytes (stable ordering,
  no timestamps), so agents can diff renders and tests can compare.

## 7. Usability

- Zero config when the convention is followed: `ccb preview` just
  works from the project root (upward `ccb.toml` search already
  exists). No flags required for the common case.
- Human-first default: phone frame + screen name + footer hint.
  Agent-first option: `--json` for the same data, machine-shaped.
- One-screen focus (`ccb preview <screen>`) for the screen actually
  being worked on.
- Exit codes follow spec §6.4: 0 ok (warnings allowed), 1 preview
  errors (e.g. missing nav target), 2 configuration errors.
- Docs: `docs/preview.md` with the convention guide and a worked
  example (wireframes shown as text). Dogfood it: the sample app's
  golden renders run in CI.

## 8. Testing strategy

- **Golden files.** Checked-in expected wireframes + logic trees for
  the sample app; tests diff actual output against them. This is the
  main regression net for the renderer.
- **Unit tests** per parser (CSS edge cases, Kotlin structure cases)
  under ASan+UBSan, following the §9.3 rules.
- **Fuzz harnesses** for the CSS and Kotlin-structure parsers (same
  gap as the TOML/ZIP fuzz harnesses — consider doing all four
  together).
- **Never-crash corpus.** A directory of malformed inputs (truncated
  CSS, broken Kotlin, empty files) that preview must survive with
  diagnostics, not crashes.

## 9. Risks and mitigations

- **Fidelity creep** → scope lock (§1) enforced in review; every new
  CSS property or Kotlin construct needs a subset decision first.
- **Parser ambiguity** (Kotlin is a big language) → structure-only
  parsing with an explicit "unknown" node; a construct we don't model
  is a labeled unknown, never a misread.
- **Terminal variance** → `--ascii`, `--width`, `NO_COLOR`, and a
  minimum-width path, all tested.
- **Preview bigger than the builder core** (flagged in the spec) →
  accept it: preview is a product surface of its own. If it grows past
  one module, split the renderer from the parsers, not the stages.

## 10. Open decisions for the user

- Default phone-frame width (40 columns proposed).
- Walk mode: in M5 at all, or v2 only (recommendation: v2).
- Whether preview ever extends to XML layouts / Compose (spec Q8) —
  recommendation: no, CSS-convention-only stays the contract.
