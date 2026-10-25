# Preview runtime plan — scenarios, trace, LSP (2026-10-24)

Goal: real runtime behavior in preview WITHOUT an interpreter or
emulator. Standing principle, non-negotiable: runtime is a DATA
INPUT merged at render time, labeled by provenance. The static
engine (parsers, tree, diagnostics) never evaluates state. The day
a chunk adds "just a little expression evaluator", it has gone
too far — stop and say so.

## Chunk 1 — Scenario rendering (do first)

A scenario is declared runtime facts for a project, in ccb.toml:

```toml
[preview.scenario.loaded]
branches = { Detail = "is Loaded" }   # screen -> when/if entry label
lists = { Notes = ["Buy milk", "Call Ana"] }  # list label -> row strings
fields = { Search = "milk" }           # textfield label -> text
args = { "Open note" = ["note-42"] }   # button label -> fill unknown args
```

Rules:

- CLI: `ccb preview --scenario NAME` (render, `--json`, tree, and
  later walk all accept it).
- A scenario NEVER overrides a static literal. It only fills
  unknowns (unknown args, unpicked branches, placeholder rows,
  empty fields) and picks branches.
- Provenance is visible: scenario-supplied values are marked in
  human output (e.g. `[scenario]`) and as `"provenance":
  "scenario"` in JSON. Static values stay unmarked.
- A scenario naming a nonexistent screen, branch entry, list, or
  field is a warning (new P-code in the P1022+ range), never an
  error, never a silent ignore.
- Tests: unit tests for loader + merge + precedence + warnings,
  plus golden updates for the preview-sample project with a
  checked-in scenario. Phone test on the sample.

As built (Chunk 1, 2026-10-24): two deviations from the example
above, both documented in docs/preview.md. (1) Spelling: the TOML
subset has no inline tables, so the four maps are sub-tables
(`[preview.scenario.<name>.branches]` etc.) instead of
`branches = {...}` — same data model, no parser change. (2)
Branch picks mark, they do not hide: the wireframe still shows
every branch entry, and the pick is marked `[scenario]` in the
tree and as `"picked"` in JSON. Hiding unpicked entries would
destroy static information in the render; exclusive-state rendering
is a possible follow-up, not this chunk. Wireframe scenario values
(list rows, field content) are shown as content; their provenance
marking lives in the logic tree (`scenario:` lines) and JSON.

## Chunk 2 — Trace overlay (reuses chunk 1's merge)

- Trace format (JSON): an ordered list of steps
  `{screen, edge, branch, args}` — one step per navigation taken.
- `ccb preview --walk --record FILE` writes a trace of the walk
  (back stack + branch picks are already a simulated trace).
- `ccb preview --trace FILE` renders with the taken path
  highlighted, real arg values filled, untaken branches greyed.
  The merge code is chunk 1's; only the data source differs.
- Document the trace format so a tiny in-app logger can emit it
  later. No in-app logger is built in this round.

As built (Chunk 2, 2026-10-24): the merge is shared, not forked —
chunk 1's scenario merge was generalized to a provenance parameter
(`pv_prov`: scenario vs trace) with two shared primitives
(`pv_rt_mark_branch`, `pv_rt_fill_arg`); the trace module only
adds parsing, the taken-edge marking, and its own step loop.
Three documented deviations from the sketch above. (1) "Greyed"
in plain-text output is the `[not taken]` mark on untaken entries
of a picked branch (the wireframe is unchanged by a trace; the
path lives in the tree and JSON). (2) Trace args map positionally
onto the taken edge (step arg k fills edge arg k), while scenario
args fill unknown slots left-to-right for every edge with the
button label — the trace fills only the taken edge, so the two
rules differ by targeting, not by principle (never override a
static literal). (3) Matching is by label: a branch-qualified step
marks every edge whose button label appears inside the named
branch entry; flow-declared navs carry no branch annotation, so
the box-tree label link is the only one available. Mismatches
warn (CCB-P1023), never fail. `--trace` composes with `--scenario`
(scenario first, trace fills only slots still unknown).

## Chunk 3 — LSP server (`ccb lsp`) — DONE 2026-10-24

- stdio JSON-RPC with LSP-style Content-Length framing.
- Standard: `initialize`, `shutdown`, `exit`,
  `textDocument/didOpen/didChange/didSave/didClose` (a changed
  flow/CSS/TOML file re-parses just that file).
- Custom: `ccb/logicTree`, `ccb/screenInfo` (one screen, in/out
  edges), `ccb/diagnostics`, and a `ccb/publishTree` push
  notification on every tree change.
- Serves the tree PLUS the active scenario/trace overlay when one
  is selected. Single-threaded select() loop over stdin + inotify
  (watch mode already has the inotify part). No threads (v1 rule).

As built (`src/ccb_preview_lsp.c`, `src/ccb_preview_lsp.h`,
`ccb lsp [--scenario NAME] [--trace FILE]` in `src/ccb_cli.c`,
tests in `tests/test_preview_lsp.c`, docs in `docs/preview.md`):

- Frame width 40, phone DP width from the config (390 default):
  the same defaults as the CLI, so the server tree JSON (no
  scenario/trace) is byte-identical to `ccb preview --json` for
  the same project (modulo the CLI's trailing newline). Pinned
  by test against the sample golden.
- Staging + deep copy, not shared boxes: scenario/trace apply and
  resources resolution mutate boxes, so per-file parses live in
  staging arenas and each build deep-copies them into a fresh
  project arena, then runs the shared `pv_project_assemble`
  (extracted from `pv_project_load_scenario` with
  `pv_preview_scan_xml`; no forked merge). CSS merges with
  `pv_css_rule_merge`, replicating `pv_css_parse`'s cross-file
  semantics exactly — including one detail the first version got
  wrong and the merge test caught: a split copy takes the NEW
  declaration's line (the parser's split copy is returned as new
  and stamped with the declaration line), not the base rule's.
- Invalidation: flow/CSS parse once per file, re-parse only that
  file (parse counts are the test's incrementality proof).
  Editor changes rebuild immediately; disk changes debounce
  200ms. `ccb.toml` disk-only (unsaved edits apply on save);
  bindings or nav/layout/resources path change invalidates the
  whole flow cache; styles/flow dir change rebuilds the watch
  context. Failed reload keeps the old config and last good
  project. Nav/layout XML rescanned every build; resources
  re-read every build. Open docs override disk until closed.
- `didChange` is full sync (last `contentChanges[]` entry's
  text); `didSave` is a no-op. `initialize` result carries
  `textDocumentSync: 1` and `serverInfo` (`ccb lsp`,
  `CCB_VERSION`). Root priority: `rootUri` > first
  `workspaceFolders[].uri` > `rootPath`. Re-root commits only
  after a throwaway config load proves the new root usable;
  otherwise ignored. Exit 0 iff shutdown preceded exit/EOF.
  Errors: `-32700` unparseable JSON (null id), `-32601` unknown
  method, `-32600` request after shutdown, `-32602` unknown
  screen, `-32603` no project.
- First-publish guarantee (found by the phone test, fixed same
  round): the run loop publishes when the tree changed OR when
  nothing was ever published. Before the fix, a rootUri spelling
  that differed from the discovered directory (e.g. a trailing
  slash) counted as a re-root, the handler skipped the publish
  (rebuild queued), and the rebuild reported the identical tree
  as unchanged — so the initial publish never arrived. The
  reroot comparison now also ignores trailing slashes. Covered
  by a trailing-slash regression test.
- VERIFICATION: VM ctest plain 40/40 and sanitizer 40/40, both
  solo runs on the final code (`test_preview_lsp` alone: 141
  checks, 0 failures, both modes). One caution learned during
  this chunk: do NOT run the plain and sanitizer suites
  concurrently — the e2e build tests (`test_aab`,
  `test_maven`, `test_modules`) share fixed `/tmp/ccb_test_*`
  paths, and concurrent runs collide (missing generated
  `R.java` at kotlinc time); the same tests pass sequentially.
  Phone test: scripted stdio session against the native phone
  build on a scratch copy of the sample — initialize +
  publishTree (all 5 screens), logicTree byte-equal to the
  golden, screenInfo incoming edges, didOpen adding a screen
  with a publishTree push, diagnostics, shutdown, exit 0 — all
  green after the first-publish fix.

## Chunk 4 — Terminal tree client

- `ccb preview --tree-live`: spawns the chunk-3 server as a child
  and renders the tree live. Indented tree, select a screen, see
  incoming + outgoing edges, diagnostics inline, affected subtree
  refreshes on `ccb/publishTree`. This is the "render straight to
  the terminal" client; editor integration via plain LSP clients
  comes free with chunk 3.

## Parked, not this round

- Snapshot rendering (uiautomator / accessibility dump -> boxes).
- Live agent inside the app (adb/socket reporting).
- Coverage report (`--coverage`: resolved vs unknown vs missing
  edges by cause).

## Standing rules for every chunk

- User C style: C17, 2-space indent, `//` comments, K&R braces,
  `-Wall -Wextra -Werror -Wpedantic`, CMake-only, libc only,
  arena allocation, no `system()`, no threads in v1.
- Verify each chunk: VM ctest plain + sanitizer green, phone test
  on the preview sample, then push.
- Push rule (user, emphatic): pushes go through the phone
  terminal with the stored token ONLY, never the connector. The
  VM tree is NOT a git repo; transfer VM -> phone with
  `tar czf - <files> | ~/.ssh/phone_ssh.sh -- 'tar xzf - -C
  ~/ccb'`.
- Token cap: 90% of the free weekly limit. Work in bounded chunks,
  report at the end of each, stop before the cap.
- User is at work until 22:00 PDT. Routine progress chat-only;
  Termux notification only for a blocker/decision or verified
  ready. Sectioned summary at the end of the round.
