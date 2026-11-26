# Preview correctness plan — Phase 1 (2026-10-24)

Goal: preview stays correct on ordinary Android code. Standing
principle, non-negotiable: PROJECT-FIRST. Android's own
declarations (manifest, layouts, nav graph, `[paths]`) are ground
truth; the flow sketch is an overlay; derived edges come last.
Precedence: manifest/graph/layout > flow sketch > derived.
Conflicts warn (like P1020), never silently win. Two invariants
hold across every chunk: never claim a false edge, and never
silently drop a real nav call.

Source of the work: the real-world testing round
(`docs/real-world-testing.md`, commit 03edc02) — P0 init-smoke,
P1 notes, P2 catalog, P3 messy under `examples/real-world/`.

## Chunk A — Discovery + normalization

- A1, fallback existence checks: `pv_preview_layout_path` /
  `pv_preview_nav_path` commit to the `<resources>/layout` (or
  `/navigation`) branch without checking existence, so a values-dir
  `[preview] resources` shadows the correct `<paths res>` fallback
  and yields an empty input set with exit 0. Fix: existence-check
  each fallback candidate and fall through to the next when it is
  absent. Explicit `[preview] layout` / `nav_graph` keys stay
  strict (required path, as today). Prefer `[paths] res` — the
  build already knows the res root. Docs say layout/nav_graph
  keys are overrides, not requirements.
- A2, package-aware normalization: a graph destination arrives as
  `.DetailActivity`, `com.example.DetailActivity`, or
  `DetailActivity` — all the same class. Strip the `[project]
  package` prefix and any leading dot before comparing. Graph
  destinations become NODES in their own right (a destination
  without a flow sketch is a node with unknown layout, not an
  error). P1005 fires only when a nav target matches NOTHING: no
  screen, no graph destination, no manifest activity. Suffix-aware
  matching (`DetailActivity` <-> `Detail`) is a documented,
  deterministic convenience on top, never a guess.
- Regression: the report's P1 repro (delete the explicit `layout`
  + `nav_graph` keys, run `ccb preview --json`) must now produce
  the correct tree. Tests + docs with every behavior change.

### Chunk A as built (2026-10-24)

- Fallback order is res-first, as the plan's "Prefer `[paths]
  res`" instruction: explicit key, then `<paths res>/layout` /
  `navigation`, then `<resources>/layout` / `navigation`, each
  existence-checked. This reverses the older docs' order, which
  listed resources first; `docs/preview.md` now records the
  res-first order and the override (not requirement) wording.
- Graph nodes are appended after flow screens in
  `pv_project_assemble`, before distribution, CSS merge, scenario
  application, and layout, so they flow through the pipeline like
  any screen; the root-NULL guards in those steps skip them
  naturally. Nodes are marked `graph_only`: `[graph]` in the logic
  tree, `"graph_only":true` in JSON (only when set, so output
  without graph nodes is byte-identical). P1007 skips graph nodes;
  P1006 unreachable warnings still apply to them (warnings only).
- The manifest scan is a new `pv_manifest_activities` helper over
  the existing XML tag scanner: only exact `activity` elements,
  `android:name` only, normalized with the project package,
  deduplicated, arena-owned. Missing/unreadable/malformed manifest
  yields whatever scans cleanly, never an error. `ccb_manifest.c`
  was deliberately not reused: it is a build-pipeline merger, not
  an activity-name extractor. Manifest activities exempt an edge
  from P1005 but never create a node.
- Reachability BFS and P1005 both resolve through
  `pv_screen_find` (normalized, suffix-aware), so an edge to
  `DetailActivity` marks a `Detail` screen reachable. Walk mode
  delegates to `pv_screen_find` too, so following a suffix-matched
  edge lands on the matched screen.
- Follow-ups deliberately left for later chunks, not done here:
  tree-live and CLI `--screen` selection still use exact-name
  lookup (inconsistent with the new matching, noted for a later
  chunk); the LSP server does not watch the manifest file itself,
  so a manifest edit alone does not trigger a rebuild (the next
  flow, CSS, config, or XML change picks it up).
- Tests: `tests/test_preview_correctness_a.c` (79 checks),
  covering the path functions, normalization, matching, graph
  nodes, manifest exemption, P1005 still firing, walk following a
  suffix-matched edge, fallback discovery end-to-end, and the P1
  repro as a test (pristine vs key-stripped JSON byte-identical,
  plus a `ccb preview --json` CLI smoke test with exit 0).

## Chunk B — Whole-project function index

Parsing is per-file today, which is why cross-file handlers,
helpers, and variable-held refs resolve to nothing. Build one
index of every top-level fun across all flow files, then:

- Resolve `::` method references across files.
- Follow a helper call one level (cycle guard, depth cap) so a
  handler calling `openDetail()` attributes the helper's nav.
- Track local `val h = ::name` assignments within a body, so a
  reference held in a variable first still resolves.
- Ambiguous match (same name, two files) resolves NOTHING and
  warns, never guesses. Still static, still conservative.
- Tests + docs with every behavior change.

### Chunk B as built (2026-10-24)

- Project load reads all flow texts first, builds one
  `pv_fun_index` (name -> flow file, over borrowed label/text
  pairs), then parses each file with `pv_flow_parse_indexed`;
  the texts outlive every parse that uses the index. The old
  `pv_flow_parse_full` is a NULL-index wrapper, so index-less
  parses keep the exact old behavior (pinned by a test).
- Cross-file bodies are scanned with a scratch parser that
  re-tokenizes the retained foreign text and shares the caller's
  arena, diagnostics, bindings, graph, and index, with
  `p->screen` set to the button's screen, so edges land
  correctly with dedup/conflict behavior inherited. Same-file
  resolution shadows cross-file resolution.
- Ambiguity counts DISTINCT files, not definitions: two
  definitions in one file resolve to the first, while one
  definition in each of two files warns CCB-P1024 (once per
  referencing file and name, covering both `::` refs and helper
  calls) and records nothing. P1024 falls through to the default
  warning-always classification, like P1023, so no gate change
  was needed; `docs/diagnostics.md` and the `ccb_preview.h`
  comment record it.
- Helper following is a `helper_depth` parser field: 0 in a
  handler body (calls followed), 1 inside a followed helper
  (calls not followed), which alone guarantees termination for
  self- and mutual recursion. The current branch annotation
  propagates through the followed helper. Dotted calls and
  calls without `(` are not helpers; unmatched ordinary calls
  warn about nothing.
- Val tracking is a per-parser map, reset at each new screen
  fun, recording only the plain `val`/`var NAME = ::HANDLER`
  shape (contiguous tokens). The typed form
  (`val h: Handler = ::x`) is a documented limitation that
  keeps the old behavior (no edge, no warning).
- Screen candidacy deliberately NOT changed here: cross-file
  handler funs (and same-file helper funs) still appear as
  screens until Chunk C's reference counting — the P3 test
  asserts edge facts only, not phantom absence. Chunk A's
  follow-ups (tree-live / CLI `--screen` exact-name lookup,
  LSP manifest watching) also untouched, per the task.
- P3-messy outcome on device and VM alike: "Helper nav" ->
  Detail, "Cross file" -> Form, "Var ref" -> Form now resolve;
  "Nested" stays missing (Chunk C territory), "Mystery" stays
  P1019, "Dynamic" stays unknown.
- Tests: `tests/test_preview_correctness_b.c` (64 checks),
  covering cross-file `::` resolution, helper inlining
  (same-file, cross-file, depth cap, branch propagation),
  self- and mutual-recursion termination, val/var-held refs
  plus the typed-form limitation plus `onClick = h`,
  ambiguous `::` refs and helper calls with dedup,
  same-file shadowing, two-definitions-one-file, NULL-index
  old behavior, and the real P3 tree plus a CLI `--json`
  smoke test. Two pre-existing handler tests were deliberately
  updated (not weakened): the helper-call and val-held-ref
  cases in `tests/test_preview_handlers.c` asserted the old
  no-edge outcomes that Chunk B reverses by design; they now
  assert the edge exists, keeping the still-a-screen checks
  that Chunk C will revisit. No other test changed.

## Chunk C — No silence + diagnostic precision + regression corpus

- Unattributed-nav warning (new P-code): EVERY nav call found
  anywhere is attributed to a button, marked unknown, or reported
  with file:line. Silence is the bug; a warning turns a silent
  gap into a visible one.
- Screen candidacy by reference counting: a top-level fun
  referenced from anywhere (via chunk B's index) is NOT a screen
  candidate. This kills the phantom screens P3 showed.
- P1017 precision: known-too-small warns; unknown size (no
  font-size rule anywhere) stays quiet. General rule, document it:
  report what you know, stay quiet about what you don't. A found
  nav with unknown attribution warns (something real exists); an
  unset size warns about nothing.
- Regression corpus: pin P1 and P3 preview JSON + tree as
  goldens (P0/P2 too if cheap). Coverage counts
  (resolved/unknown/missing) from P3 are the metric to watch;
  they must not regress.

### Chunk C as built (2026-10-25)

- No-silence is a single post-parse sweep, not drop-site
  warnings: every nav call consumed as an edge is marked
  attributed (file label + token start offset, stored on the
  index), and `pv_sweep_unattributed_navs` warns CCB-P1025
  once per remaining site. The sweep exists because candidacy
  plus the depth-1 helper cap would otherwise create NEW
  silence for depth-2+ helpers and never-scanned handler
  bodies; the plan's "never silently drop a real nav call"
  invariant rules that out. Navs already reported by another
  diagnostic are marked accounted-for, not re-warned: unbound
  handlers (P1019, same- and cross-file) and ambiguous
  references (P1024, all defining files). P1025 is
  warning-always like P1022-P1024, so no gate change.
  Marking is index-less-parse-safe (NULL index no-ops), and
  the tokenizer skips comments and treats strings as single
  tokens, so the sweep has no comment/string false positives.
- Screen candidacy runs through the index after the per-file
  pruning: a fun is disqualified when referenced via `::`
  anywhere in a fun body, from a setOnClickListener/onClick
  lambda block, or from a handler fun's body (iterated to a
  fixed point, so helper chains count). A bare call directly
  in a screen's UI body does NOT disqualify (screen
  composition, e.g. `Detail()` as a component); a self-
  reference alone does not disqualify; an ambiguous name
  disqualifies every definition. The pre-pass comment claiming
  per-file pruning "until Chunk C" was updated with this.
- P1017 precision: warn iff `w < 8` (width is always
  layout-computed) OR (`h < 2` AND the effective rule sets
  `font-size`); height without a font-size rule is the 1-row
  default, not a measurement. Explicit-small-width and
  small-font-size cases still warn; the no-rule 38x1 case
  goes quiet. `docs/preview.md` (unknown-cases table,
  scope-limits paragraph, P1017 section) and
  `docs/diagnostics.md` (P1017 row, new P1025 row) updated.
- Goldens: all four real-world projects pinned
  (`tests/golden/preview/real-world/<proj>/{preview.json,
  logic-tree.txt}`, scratch path normalized to PROJ). The
  sample preview.json + scenario-loaded + trace-walk goldens
  were regenerated: the ONLY change in each is the
  diagnostics array (P1017 entries removed; scenario-loaded
  keeps its 2 P1009s). P3 coverage did not regress: Home
  edges Dynamic unknown, Go -> Detail, Helper nav ->
  Detail, Cross file -> Form, Var ref -> Form, Share ->
  Settings; Nested warns P1025 (was silent); Mystery stays
  the single P1019 (its fix is a bindings line in the
  project config, not tool work — the project was not
  changed).
- Test fallout, all deliberate consequences of the above:
  `test_preview_handlers.c` (i)/(j) flipped to assert the
  helper/val-held funs are NOT screens (Chunk B's note said
  Chunk C would revisit these); P1017 counts in
  `test_preview.c` (10 -> 0), `test_preview_scenario.c`
  (10 -> 0, total 12 -> 2), `test_preview_lint.c` (no-rule
  38x1 case now quiet), and `test_preview_lsp.c`
  (diagnostics endpoint now returns an empty array for the
  sample) updated. One latent harness bug surfaced and was
  fixed at root cause: `test_preview_correctness_b.c`'s
  `run_cli` never flushed stdout before restoring the fd,
  so the P3 CLI JSON leaked to the terminal and the capture
  file came up empty once Chunk C shrank the output below
  the stdio buffer; the gate-diff test's harness already
  flushed. No src change was needed for that failure.
- Tests: `tests/test_preview_correctness_c.c` (89 checks),
  covering Nested -> P1025, depth-2 helper chain -> P1025,
  handler-referencing-handler -> P1025, candidacy
  (helper/val/cross-file not screens, composition preserved,
  ambiguous -> P1024 with no P1025), P1017 precision
  (four hand-built layouts), and the four real-world
  goldens. Chunk A's follow-ups still untouched, per the
  task; Phase 2 not started.

## Phase 2 (built 2026-11-25)

A real-code front end: parse the app's own Kotlin + XML
(Activities: `onCreate` -> `setContentView` -> layout; layouts:
buttons, onClick; manifest; nav graphs) and derive screens/edges
from those, with the flow sketch + CSS supplying only the
wireframe. The sketch is optional instead of required.

### Phase 2 as built (2026-11-25)

Chunk 1 — real-code screens + edges + merge with sketch.

- One new module, `src/ccb_preview_real.c` (declared in
  `src/ccb_preview.h`): for each Activity class it synthesizes a
  small flow text (screen fun + `Button` components from the
  layout + listener attachments + referenced member funs) and
  feeds it to the existing flow parser
  (`pv_flow_parse_indexed`), so handler shapes, one-level
  helpers, branch annotations, args, `android:onClick`,
  `navigate()`, and the CCB-P1025 sweep are all reused, not
  reimplemented. The synthesis normalizes only what the flow
  parser cannot see: `findViewById` chains become bare view ids,
  `startActivity(intentVar)` becomes the direct `Intent` form,
  and `XActivity::class.java` becomes `X::class.java`, so real
  edges dedup against sketch edges to the same screen.
- Recognized shapes only: Activity classes with `onCreate` ->
  `setContentView`; `findViewById<Button>(R.id.x)
  .setOnClickListener` (lambda, parenthesized lambda,
  `.onClick =`) inside `onCreate`; member funs referenced from a
  listener or a layout `android:onClick`, plus their one-level
  helper closure; `startActivity(Intent(...))`,
  `nav("Y")`/`navigateTo("Y")`, `navigate(R.id.y)` /
  `navigate("Y")` via the nav-graph table. A listener method
  reference with no definition in any scanned Activity warns
  CCB-P1025 once per method; an unresolvable listener (variable
  receiver, val-held view, the p2-style product loop) warns
  CCB-P1025 with the true source file:line when its body holds a
  nav; a dropped member fun holding a nav warns CCB-P1025 once
  (file:line of the fun). Top-level funs in real Kotlin files are
  not extracted; only Activity member funs are.
- Merge in `pv_project_load_scenario`, after the sketch parse and
  before assemble: real screens merge into sketch screens by
  `pv_names_match`, or a launcher real screen merges into a sketch
  screen named `Home`; same button + destination (spelled with or
  without the `Activity` suffix) dedups silently keeping the
  sketch edge; a conflicting destination warns CCB-P1020 and the
  real-code edge stands. Real-only screens append as-is; the
  manifest launcher moves to index 0 when no screen is named
  `Home`, and a nav-graph destination named `Home` means the
  launcher screen in that case (otherwise assemble would create
  an empty graph-only `Home` node and the entry rule would pick
  it). `pv_preview_applicable` (styles OR flow OR real Kotlin
  sources) is the single definition shared by the loader, the
  preview command, and the build gate.
- One flow-parser change: a helper call's literal arguments now
  propagate onto the edges its body produces when those edges
  carry no arguments of their own (`openDetail("note-1")` ->
  edge args `["note-1"]`; a variable argument becomes an unknown
  arg entry). An edge whose nav already has arguments keeps its
  own.
- One scan change: `pv_preview_scan_xml` falls back to the
  layout's own `android:text` for an `android:onClick` label when
  no `[preview.bindings]` entry names the button (kept raw — an
  `@string/...` reference resolves with the other component
  labels); CCB-P1019 only fires when there is neither.
- Two bugs found by the new tests and fixed at root cause, not
  worked around: the screens/pending arrays are arena-grown by
  the flow parser (`pv_screen_add`/`pv_pending_add`), so merging
  with `ccb_xrealloc` (and freeing the real parse's arrays in the
  loader) was an invalid free — the merge now grows them the
  arena way; and the `Intent`-val rewrite in the synthesis
  forgot to strip the `Activity`/`Fragment` suffix, so
  `DetailActivity` did not dedup against sketch `Detail` and a
  false CCB-P1020 fired (the merge now also compares targets
  with `pv_names_match`, not `strcmp`).
- Goldens: p1 gains the three real `Note 1/2/3 -> Detail`
  edges (args `note-1/2/3`); p2 gains two CCB-P1025s (the
  product loop, real line 24, and the never-referenced
  `openDetail` nav, line 31); p3 gains the real `Open detail ->
  Detail` edge. p0 unchanged. New no-sketch golden
  `examples/real-world/p1-notes-real/` (a copy of p1-notes
  `src/` + a `[preview]`-only ccb.toml) with its pinned
  `preview.json`/`logic-tree.txt`: entry `Main`, all real edges,
  no diagnostics.
- Tests: `tests/test_preview_phase2.c` (104 checks in Chunk 1,
  139 after Chunk 2), covering the no-sketch P1-style project
  (screens, entry = launcher, helper args literal/variable,
  onClick via layout text, Intent-val shape), merge with a
  sketch (silent dedup, CCB-P1020 conflict with real standing,
  sketch Home stays entry), nested-lambda nav -> no edge +
  CCB-P1025 naming the real file, variable-receiver loop -> no
  edge + two CCB-P1025s with the real line, Activity without
  `setContentView` -> no screen, `navigate(R.id.x)` via the
  graph, and the p1-notes-real golden.

Chunk 2 — layout-derived wireframe when no sketch.

- `rv_layout_wireframe` in the new module builds the box tree
  from the layout XML with a small recursive-descent parser
  (the tag scanners only report open tags, so no nesting info):
  `LinearLayout` vertical -> Column, horizontal -> Row
  (horizontal is Android's default when no `android:orientation`
  is given), `TextView` -> Text, `Button` -> Button, `EditText`
  -> TextField, `ImageView` -> Image, anything else -> a generic
  box (including `ConstraintLayout`: no constraint solving,
  children still mapped). `android:text` is the label (kept raw,
  `@string` resolved by the existing pass); `<include>` is a
  generic box with no children; `<merge>` as a root stacks its
  children vertically. Depth cap 32; malformed XML stops the
  subtree, never crashes. The wireframe replaces the synthetic
  Button root of each freshly parsed real screen (merged screens
  keep the sketch root), and carries the same button labels, so
  onClick distribution by label is unaffected. No layout-engine
  change: the existing pass sizes the tree.
- Two fixes the wireframe surfaced, both at root cause:
  onClick distribution (`pv_tree_has_label`) matched ANY box
  with the label, so a title `TextView` (e.g. `"Settings"` on
  the Settings screen) attracted the button's edge and created a
  spurious self-edge — it now matches BUTTON boxes only, which
  also removed the same spurious edge from the p1-notes sketch
  golden; and a childless generic box summed to zero rows and
  vanished (the Settings `Switch`), so `box_is_container` now
  treats a childless `box` as a leaf (1 row). No existing golden
  changed for the latter.
- Docs: `docs/preview.md` (new "Real code (Phase 2)" section,
  applicability + unknown-cases + scope-limits updates),
  `requirement.md` §10 (XML layouts now understood; Compose
  still not) and open question 8 (XML layouts done, Compose
  still open). No new diagnostic codes, so
  `docs/diagnostics.md` needed no change.
- Deliberate deviations / follow-ups left: diagnostics on the
  synthesized flow text name the real `.kt` file but carry
  synthesized line numbers (only the real module's own
  CCB-P1025s carry true file:line); top-level funs in real
  Kotlin files are not extracted; `<include>` layouts are not
  resolved; the LSP server still uses styles/flow only (real
  code in the LSP is a follow-up, not this chunk). Chunk 1's
  tests + goldens updated only where intended (p1-notes lost
  the spurious Settings self-edge; p1-notes-real boxes are now
  the layout wireframe).

## Standing rules for every chunk

- User C style: C17, 2-space indent, `//` comments, K&R braces,
  `-Wall -Wextra -Werror -Wpedantic`, CMake-only, libc only,
  arena allocation, no `system()`, no threads in v1.
- Verify each chunk: VM ctest plain + sanitizer green, run
  SEQUENTIALLY, never concurrently (chunk 3 lesson: concurrent
  runs collide on shared fixed `/tmp/ccb_test_*` paths). Phone
  test on the real-world projects, then push.
- Push rule (user, emphatic): pushes go through the phone
  terminal with the stored token ONLY, never the connector. The
  VM tree is NOT a git repo; transfer VM -> phone with
  `tar czf - <files> | ~/.ssh/phone_ssh.sh -- 'tar xzf - -C
  ~/ccb'`. Check phone git state first before committing.
- Token cap: 90% of the free weekly limit. Work in bounded
  chunks, report at the end of each, stop before the cap.
- User is at work until 22:00 PDT; this round started late and
  will run past that. Routine progress chat-only; Termux
  notification only for a blocker/decision or verified ready.
  Sectioned summary when the round is verified complete.
