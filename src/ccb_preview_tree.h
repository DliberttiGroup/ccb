// ccb_preview_tree: terminal tree client for ccb preview (Chunk 4
// of docs/preview-runtime-plan.md).
//
// `ccb preview --tree-live` spawns the chunk-3 `ccb lsp` server as
// a child process, talks JSON-RPC over its stdin/stdout, and
// renders the logic tree live in the terminal: an indented tree of
// screens, a detail pane for the selected screen (incoming +
// outgoing edges, branches), and diagnostics inline. A
// ccb/publishTree notification re-renders the view, so editing a
// flow, CSS, or TOML file moves the tree under the cursor.
//
// The state machine (pv_tree_*) performs NO terminal I/O and NO
// child-process I/O, so unit tests drive it with scripted keys and
// canned publishTree payloads, the way walk mode's scripted keys
// work. The terminal layer (pv_tree_run) owns raw mode, the key
// loop, and the server child.
//
// The client never evaluates state and never invents data: it
// renders exactly what the server publishes (the static tree plus
// the chunk 1/2 overlays, provenance-marked). A publish that
// changes the tree replaces it wholesale; the selection follows
// the selected screen's NAME when it still exists, else clamps to
// the previous index, the same clamp rule walk mode uses.
//
// Ownership: one arena per applied tree owns every string and
// array in it; applying a new tree destroys the old arena and
// installs the new one, so a failed apply keeps the old tree
// untouched. The server-byte feed buffer is malloc'd and owned by
// the tree separately; pv_tree_free releases both.
#ifndef CCB_PREVIEW_TREE_H
#define CCB_PREVIEW_TREE_H

#include "ccb_common.h"
#include "ccb_json.h"
#include "ccb_str.h"

// Scripted keys. The terminal layer translates raw bytes to these;
// tests feed them directly to pv_tree_apply_key.
typedef enum {
  PV_TREE_KEY_UP, // up arrow or k: move selection up (clamps)
  PV_TREE_KEY_DOWN, // down arrow or j: move selection down (clamps)
  PV_TREE_KEY_QUIT // q (or Ctrl-C in the terminal layer): stop
} pv_tree_key;

// One edge of the published tree, parsed from the server's JSON.
// Strings and arrays are arena-owned (see the header comment).
typedef struct {
  char *from; // button label; "?" when the payload omits it
  char *target; // NULL when the destination is dynamic
  int unknown;
  char **args; // entries are literals, or NULL (= unknown arg)
  size_t nargs;
  // Parallel to args: "scenario"/"trace" when that slot was filled
  // by an overlay, else NULL. NULL when the payload carries no
  // args_provenance, or when no slot was filled.
  char **args_prov;
  char *branch; // e.g. "if ready: then", or NULL when unconditional
  int taken; // set when an applied trace took this edge
} pv_tree_edge;

// One branch of a screen: a when/if label, its entry labels, and
// the overlay pick, if any.
typedef struct {
  char *label;
  char **entries;
  size_t nentries;
  char *picked; // entry label, or NULL when nothing picked it
  char *picked_prov; // "scenario"/"trace", or NULL
} pv_tree_branch;

typedef struct {
  char *name;
  int entry; // the project's entry screen (Home, else first)
  int reachable;
  pv_tree_edge *out;
  size_t nout, capout;
  pv_tree_branch *branches;
  size_t nbranches, capbranches;
} pv_tree_screen;

// One project diagnostic, as published (code/message/file/line).
typedef struct {
  char *code;
  char *message;
  char *file; // NULL when the diagnostic names no file
  long line; // 0 when the payload omits it
} pv_tree_diag;

typedef struct {
  ccb_arena *arena; // owns the current tree, or NULL before first publish
  pv_tree_screen *screens;
  size_t nscreens, capscreens;
  pv_tree_diag *diags;
  size_t ndiags, capdiags;
  size_t sel; // index into screens (selection clamps, never wraps)
  int have_tree; // 1 after the first successful publish applied
  int quit; // set by PV_TREE_KEY_QUIT
  char *scenario; // top-level "scenario" field, or NULL
  long trace_steps; // top-level "trace".steps, or -1 when none
  // Server-byte feed buffer (malloc'd): bytes not yet part of a
  // complete Content-Length frame. Grows as needed; a garbage
  // header line is discarded and framing resumes after it.
  char *feed_buf;
  size_t feed_len, feed_cap;
} pv_tree;

void pv_tree_init(pv_tree *t);
void pv_tree_free(pv_tree *t);

// Apply a publishTree params object (the pv_project_json object:
// screens, diagnostics, optional scenario/trace fields). Returns 0
// on success, -1 when the payload is not a usable tree (the old
// tree is kept). The selection follows the previously selected
// screen's name when it still exists, else clamps to the previous
// index.
int pv_tree_apply_json(pv_tree *t, const ccb_json_value *tree);
// Same, from JSON text (parses, then applies). Returns 0 on
// success, -1 on parse or shape failure.
int pv_tree_apply_publish(pv_tree *t, const char *json_text);
// Feed raw server bytes (framed JSON-RPC messages, possibly split
// across reads or several per read). Returns the number of
// publishTree notifications applied from this chunk (0 when the
// bytes only buffered, or held responses/other methods). Malformed
// frames are skipped, never fatal.
int pv_tree_feed(pv_tree *t, const char *bytes, size_t len);

// Move the selection up/down (clamps at the ends, never wraps) or
// set quit. No-op on an empty tree, except QUIT.
void pv_tree_apply_key(pv_tree *t, pv_tree_key key);
// Select the screen named name. Returns 1 on success, 0 when the
// tree has no such screen (selection unchanged).
int pv_tree_select(pv_tree *t, const char *name);
// The selected screen, or NULL when the tree is empty.
const pv_tree_screen *pv_tree_selected(const pv_tree *t);

// Render the tree view as text (no terminal I/O): header, screen
// list with a selection cursor, the selected screen's detail pane
// (outgoing edges, incoming edges, branches), diagnostics inline,
// and footer hints. Before the first publish, renders a waiting
// view instead.
void pv_tree_render(ccb_str *out, const pv_tree *t);

// Terminal loop: check for a TTY, spawn `ccb lsp` as a child
// (exe_path NULL means this executable, via /proc/self/exe),
// initialize it at root_dir, enter raw mode, and run the key +
// server-message loop until quit. The server is shut down cleanly
// on every exit path (shutdown + exit, bounded wait, SIGKILL
// fallback), and the terminal is restored before returning.
// scenario_name/trace_path are passed to the server (NULL for
// none); start_screen selects the initial screen after the first
// publish (NULL means the first screen). Returns 0 on a clean
// quit, 1 when the start screen names no screen or the server ends
// without ever publishing a tree, 2 when stdin or stdout is not a
// TTY (a clean diagnostic on stderr, never a hang), when the server
// cannot be spawned, or when the executable cannot be found.
int pv_tree_run(const char *exe_path, const char *root_dir,
  const char *scenario_name, const char *trace_path,
  const char *start_screen);

#endif
