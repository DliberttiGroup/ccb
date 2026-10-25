// ccb_preview_lsp: `ccb lsp` — JSON-RPC/LSP server over stdio for
// preview (Chunk 3 of docs/preview-runtime-plan.md).
//
// The server holds the project's logic tree in memory and serves it
// to any LSP client plus the chunk-4 terminal client. It never
// evaluates state: the served tree is the static tree plus the
// chunk 1/2 runtime overlays (scenario/trace), merged by the same
// code the CLI uses. Runtime is a data input, as in every chunk.
//
// Incrementality, honestly: flow and CSS files are parsed once into
// per-file staging arenas and re-parsed ONLY when that file changes
// (editor didChange, or a disk change seen by the watch snapshot
// diff). A rebuild deep-copies the staging fragments into a fresh
// project arena and runs the shared pv_project_assemble, so staging
// data is never mutated by layout or overlays. ccb.toml, nav-graph,
// and layout XML are read from disk (unsaved editor changes to them
// take effect on save); resources are re-read on every build. A
// bindings or nav/layout change invalidates the whole flow cache,
// because bindings and the graph are flow-parse inputs.
//
// Single-threaded, no threads (v1 rule): one loop polls stdin and
// the watch context's inotify wakeup. Disk changes debounce 200ms
// (the watch module's debounce); editor changes rebuild immediately.
//
// Protocol: JSON-RPC over stdio with LSP Content-Length framing.
// Standard methods: initialize, initialized, shutdown, exit,
// textDocument/didOpen, didChange (full sync), didSave, didClose.
// Custom methods: ccb/logicTree (the pv_project_json object),
// ccb/screenInfo (one screen object plus its incoming edges),
// ccb/diagnostics (the project's diagnostics array). Push:
// ccb/publishTree, sent after initialize and after every rebuild
// that changes the tree JSON. Unknown methods with an id get
// -32601; unparseable JSON gets -32700 with a null id; requests
// after shutdown get -32600. Exit/EOF without a prior shutdown
// exits 1, with one exits 0.
#ifndef CCB_PREVIEW_LSP_H
#define CCB_PREVIEW_LSP_H

#include "ccb_preview.h"

// One cached flow file: the screens + pending edges
// pv_flow_parse_full produced for that file alone, in a staging
// arena that lives until the file is re-parsed or removed. Parse
// diagnostics are cached with the file and replayed into every
// build, so unchanged files keep their diagnostics without
// re-parsing. parse_count counts parses (1 after the initial
// parse); tests use it to prove a rebuild re-parsed only the
// changed file. dirty marks an entry for re-parse at the next build.
typedef struct {
  char *path; // malloc'd, owned by the entry
  ccb_arena *arena; // staging arena, owned by the entry
  pv_screen **screens;
  size_t nscreens;
  pv_pending_edge **pending;
  size_t npending;
  ccb_diag_list diags; // parse diagnostics, owned by the entry
  long parse_count;
  int dirty;
} pv_lsp_flow_entry;

// One cached CSS file: the same shape, for pv_css_parse's per-file
// rules (merged within the file only; cross-file merging happens in
// assembly via pv_css_rule_merge).
typedef struct {
  char *path;
  ccb_arena *arena;
  pv_rule *rules;
  size_t nrules;
  ccb_diag_list diags;
  long parse_count;
  int dirty;
} pv_lsp_css_entry;

// The server. Opaque: tests use the accessors below.
typedef struct pv_lsp_server pv_lsp_server;

// Open a server for the project at toml_path (absolute path to
// ccb.toml). scenario_name/trace_path select the chunk 1/2 overlays
// for the session (NULL for none); an unknown scenario name or an
// unreadable/malformed trace file fails the open. Builds the initial
// project. Returns NULL with *err set (malloc'd, caller frees) on
// failure: no [preview] section, config load failure, unknown
// scenario, trace failure.
pv_lsp_server *pv_lsp_open(const char *toml_path,
  const char *scenario_name, const char *trace_path, char **err);
void pv_lsp_close(pv_lsp_server *s);

// Current tree JSON (pv_project_json over the current project), or
// NULL when no project is loaded. Borrowed: valid until the next
// rebuild or close.
const char *pv_lsp_tree_json(pv_lsp_server *s);
// Current project, or NULL. Borrowed, same lifetime as tree_json.
pv_project *pv_lsp_project(pv_lsp_server *s);
// Parse counts for incrementality tests: parses so far for the file
// at path, or -1 when the file has no cache entry.
long pv_lsp_parse_count(pv_lsp_server *s, const char *path);

// Rebuild the served project when anything is dirty (file changes,
// open-doc updates, config reloads). Returns 1 when the tree JSON
// changed (caller publishes), 0 when it did not change, -1 when the
// rebuild failed and the last good project was kept.
int pv_lsp_rebuild(pv_lsp_server *s);

// Editor document updates (paths are absolute file paths, text is
// the document's full content). Open flow/CSS docs override disk
// content for parsing until closed; a didChange/didClose marks the
// file dirty. ccb.toml and nav/layout XML changes take effect on
// save (they are read from disk); resources changes trigger a
// rebuild, which re-reads them from disk. Files that are not preview
// inputs (not a flow .kt, styles .css, ccb.toml, nav/layout XML, or
// resources values file) are ignored.
void pv_lsp_did_open(pv_lsp_server *s, const char *path,
  const char *text);
void pv_lsp_did_change(pv_lsp_server *s, const char *path,
  const char *text);
void pv_lsp_did_close(pv_lsp_server *s, const char *path);

// Detect disk changes (watch wakeup + snapshot diff + extra-file
// stats for nav/layout/resources). Marks changed files dirty (disk
// changes debounce; see pv_lsp_rebuild's caller). Returns 1 when
// anything was marked dirty, 0 when nothing changed.
int pv_lsp_scan_disk(pv_lsp_server *s);

// Run the server loop on in_fd/out_fd (normally stdin/stdout):
// JSON-RPC framing, method dispatch, publishTree pushes, and the
// single-threaded loop over input + inotify. Returns the process
// exit code: 0 when shutdown preceded exit/EOF, 1 otherwise.
int pv_lsp_run(pv_lsp_server *s, int in_fd, int out_fd);

#endif
