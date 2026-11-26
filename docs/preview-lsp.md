# LSP server: `ccb lsp [--scenario NAME] [--trace FILE]`

[Docs index](index.md) · [Preview](preview.md)

*ccb-preview-lsp*

A long-running server speaking JSON-RPC over stdio with LSP-style
`Content-Length` framing. It serves the same logic tree as `ccb preview
--json` (screens, edges, branch labels, args, plus the active
scenario/trace overlay when one is selected), live: edit a file and the
tree updates without restarting. Single-threaded `select()` loop over stdin
+ inotify; no threads. The terminal tree client (`ccb preview
--tree-live`, below) and plain LSP clients both talk to this server; no
editor plugin ships with CCB itself.

Standard methods: `initialize`, `shutdown`, `exit`,
`textDocument/didOpen`, `didChange` (full sync: the last
`contentChanges[]` entry's text), `didSave` (no-op — text is already
synced), `didClose`. Custom methods: `ccb/logicTree` (the whole tree
object, spliced verbatim from the preview JSON), `ccb/screenInfo`
(`{"screen": <one screen object>, "incoming": [...]}` — incoming entries
carry `from_screen, from, target, unknown, args, branch`; self-edges
included), `ccb/diagnostics` (`{"diagnostics": [...]}` mirroring the tree's
diagnostics exactly). `ccb/publishTree` is a push notification carrying the
whole tree, sent after `initialize` and after every rebuild that changes
the tree JSON (compared by string equality; the first publish always
sends).

`initialize` result: `{"capabilities":{"textDocumentSync":1},
"serverInfo":{"name":"ccb lsp","version":<version>}}`. Root priority:
`rootUri`, then the first `workspaceFolders[].uri`, then `rootPath`. A
second `initialize` with a different root re-roots: it commits only after a
throwaway config load proves the new root has a usable `[preview]` section;
otherwise the root is ignored and the old tree keeps serving. Exit codes: 0
when `shutdown` preceded `exit`/EOF, 1 otherwise. Error codes: unparseable
JSON `-32700` (null id), unknown method `-32601`, request after shutdown
`-32600`, unknown screen `-32602`, no project `-32603`.

What re-parses when:

- Flow and CSS files parse once per file into staging arenas, re-parsing
  only when that file changes — editor `didChange` rebuilds immediately,
  disk changes debounce 200ms (the watch module's debounce).
- `ccb.toml` is read from disk only: unsaved editor changes take effect
  on save. On reload, a bindings change or a nav/layout/resources path
  change invalidates the whole flow cache; a styles/flow directory change
  rebuilds the watch context and snapshot. A failed reload (unloadable
  TOML or no usable `[preview]`) keeps the old config and the last good
  project and retries on the next change — a broken save never kills the
  server.
- Nav/layout XML is rescanned fresh on every build (small files); a
  nav/layout content change invalidates the flow cache. Resources
  (`strings.xml`/`colors.xml`) are re-read on every build as part of
  assembly, so a resources change only needs to trigger a rebuild.
- Open flow/CSS docs override disk content until closed; disk changes to
  open docs are ignored until close. Open docs for files not yet on disk
  are unioned into the file lists at resync, so unsaved new files still
  parse. A rebuild consumes pending disk changes too and resets the
  debounce.

The server tree JSON (no scenario/trace) is byte-identical to
`ccb preview --json` for the same project (modulo the CLI's trailing
newline); the test suite pins this against the sample golden.

Other preview flags are rejected for `lsp` (exit 2): it takes only
`--scenario` and `--trace`. Unknown scenario / unreadable trace / no
`ccb.toml` / preview not applicable are exit 2, matching the CLI. The
project is found by upward search from the working directory, like
`preview`.

## Tree-live mode: `ccb preview --tree-live [--scenario NAME] [--trace FILE] [screen]`

*ccb-preview-tree-live*

The terminal tree client: it spawns `ccb lsp` as a child process,
initializes it at the project directory, and renders the logic tree live in
the terminal. It never parses flow or CSS itself; everything it shows is a
published tree from the server.

The view has three parts. A screen list with a `>` cursor (`[entry]` marks
the entry screen, `(unreachable)` marks screens no edge reaches). A detail
pane for the selected screen: its outgoing edges (`-> Target(args) via
"label" [branch] [trace]`, `-> unknown` for dynamic destinations), its
incoming edges (`<- Screen via "label"` — self-edges included), and its
branches (`when state: is Loading, is Loaded, else (picked: is Loaded
[scenario])`). Diagnostics inline below, in project order (`CODE message
(file:line)`; the file part is omitted when the diagnostic names no file).
A `scenario: NAME` / `trace: N steps` line appears when an overlay is
active. Argument provenance is per-slot: a scenario- or trace-filled
literal renders as `"value" [scenario]` / `"value" [trace]`; static
literals stay unmarked, and the published JSON carries no named-arg flag,
so every literal renders quoted here.

Keys: `j`/`k` or up/down move the selection (clamps at the ends, never
wraps); `q` or Ctrl-C quits. A `ccb/publishTree` notification re-renders
the view: the selection follows the selected screen's name when it still
exists, else clamps to the previous index. Before the first publish the
view says "waiting for logic tree..." and keys still work, so a slow server
never looks hung.

Composition and exit codes: `--scenario` and `--trace` are passed to the
spawned server (an unknown scenario or an unreadable / malformed trace is
exit 2, validated before spawning); `--screen` or a positional screen
selects the initial screen after the first publish (an unknown name is exit
1 with the available screens listed). `--tree-live` rejects `--walk`,
`--watch`, `--diff`, `--storyboard`, `--output`, and `--update-goldens`
(exit 2); `--ascii`/`--width`/`--device` are accepted but have no effect on
the tree view. Non-TTY stdin or stdout is exit 2 with a diagnostic, never
a hang. The server is shut down cleanly on every exit path (shutdown +
exit, bounded wait, SIGKILL fallback), and the terminal is restored. If the
server ends without ever publishing a tree, or ends unexpectedly
mid-session, the client prints a diagnostic and exits 1. The state machine
(selection, publish application, framed-message feed) is unit-tested with
scripted keys and canned payloads in `tests/test_preview_tree.c`; the
terminal loop itself is covered by a scripted pty session on the phone,
like walk mode.
