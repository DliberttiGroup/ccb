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

## Phase 2 (NOT this round)

A real-code front end: parse the app's own Kotlin + XML
(Activities: `onCreate` -> `setContentView` -> layout; layouts:
buttons, onClick; manifest; nav graphs) and derive screens/edges
from those, with the flow sketch + CSS supplying only the
wireframe. This makes the sketch optional instead of required.
Do not start it in this round.

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
