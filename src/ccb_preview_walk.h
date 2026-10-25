// ccb_preview_walk: interactive walk mode for ccb preview (P3).
//
// Static exploration ONLY. Walk moves through the screens and edges a
// project already declares; it never executes Kotlin, never evaluates
// a when/if condition, never fetches data, and never animates. A
// branch pick only selects which labeled edge to follow (or records
// that the picked entry has no outgoing edge); the condition text is
// shown, never resolved.
//
// The state machine (pv_walk_*) performs NO terminal I/O, so unit
// tests drive it with scripted keys and no TTY. The terminal layer
// (pv_walk_run / pv_walk_render) is separate: pv_walk_render builds
// text, pv_walk_run owns raw mode and the key loop.
//
// Ownership: pv_walk borrows the pv_project (and everything in its
// arena). The caller keeps the project alive for the walk's lifetime.
// The back stack and the flattened branch-entry list are malloc'd and
// owned by the walk; free them with pv_walk_free. Branch/edge label
// pointers inside the walk are borrowed from the project arena and
// must never be freed.
//
// Selection semantics (documented, deliberate):
// - Movement CLAMPS: up at the first item stays there, down at the
//   last item stays there. Selection never wraps, so a walk through a
//   short edge list cannot silently jump from the last edge to the
//   first.
// - Back at the root is a no-op: with an empty back stack, b and
//   Backspace change nothing.
// - Following an unknown edge (a dynamic destination, or a target
//   screen that does not exist) shows it as unknown and STAYS on the
//   current screen. The walk never invents a destination.
// - Branch picking NEVER evaluates a condition and NEVER executes
//   code. It follows the first edge whose `from` label names a
//   component inside the picked entry's subtree (the only static link
//   the flow parser records); when the entry has no such edge, the
//   pick is recorded and the walk stays on the current screen.
#ifndef CCB_PREVIEW_WALK_H
#define CCB_PREVIEW_WALK_H

#include "ccb_common.h"
#include "ccb_preview_trace.h"
#include "ccb_preview.h"
#include "ccb_str.h"

// Scripted keys. The terminal layer translates raw bytes to these;
// tests feed them directly to pv_walk_apply_key.
typedef enum {
  PV_WALK_KEY_UP, // up arrow or k: move selection up (clamps)
  PV_WALK_KEY_DOWN, // down arrow or j: move selection down (clamps)
  PV_WALK_KEY_ENTER, // follow the selected edge, or pick a branch entry
  PV_WALK_KEY_BACK, // b or Backspace: pop the back stack, or exit branch mode
  PV_WALK_KEY_QUIT, // q (or Ctrl-C in the terminal layer): stop walking
  PV_WALK_KEY_BRANCH, // t: enter branch-pick mode for the current screen
  PV_WALK_KEY_BRANCH_EXIT // Esc: leave branch-pick mode without picking
} pv_walk_key;

// One flattened branch entry: a (branch label, entry label) pair from
// the current screen's box tree, plus the edge a pick would follow.
// edge is NULL when no edge in the entry's subtree matches.
typedef struct {
  const char *branch_label; // e.g. "when state", borrowed
  const char *entry_label; // e.g. "is Loaded", borrowed
  pv_box *entry; // the branch-entry box, borrowed
  const pv_edge *edge; // first matching edge in the entry subtree, or NULL
} pv_walk_branch_entry;

typedef struct {
  pv_project *proj; // borrowed, caller-owned
  pv_screen *current; // borrowed; NULL until pv_walk_start succeeds
  pv_screen **back; // malloc'd back stack, owned by the walk
  size_t nback, capback;
  size_t sel; // index into current->edges (edge-list mode)
  // Branch-pick mode: a flattened list of the current screen's
  // branch entries, rebuilt on every screen change.
  int in_branch;
  pv_walk_branch_entry *branches; // malloc'd, owned by the walk
  size_t nbranches, capbranches;
  size_t branch_sel; // index into branches (branch mode)
  // Last unknown-edge outcome: following an unknown edge sets
  // unknown_shown and leaves current unchanged. unknown_target is
  // NULL for a dynamic destination.
  int unknown_shown;
  const char *unknown_from; // borrowed edge label, or NULL
  const char *unknown_target; // borrowed target name, or NULL
  // Last branch-pick outcome (set by pv_walk_branch_pick, cleared on
  // screen change). picked_has_edge is 0 when the entry had no edge
  // and the walk stayed for that reason.
  int branch_picked;
  const char *picked_branch; // borrowed
  const char *picked_entry; // borrowed
  int picked_has_edge;
  int quit; // set by PV_WALK_KEY_QUIT
  // Chunk 2: the walk's recorded trace — one step per forward
  // navigation taken (plus pick-only steps for branch picks that
  // did not navigate). Steps borrow strings and argument arrays
  // from the project arena: the trace must be written before
  // pv_project_free. Back navigation is never recorded. The array
  // itself is malloc'd and freed by pv_walk_free; the strings and
  // argument arrays inside it are borrowed and must never be freed.
  pv_trace_step *record;
  size_t nrecord, caprecord;
} pv_walk;

// Start a walk. start_name NULL means the entry screen (the screen
// named "Home" when one exists, else the first screen). Returns 0 on
// success, -1 when the project has no screens or start_name names no
// screen (the walk is left unusable; call pv_walk_free anyway).
int pv_walk_start(pv_walk *w, pv_project *proj, const char *start_name);
void pv_walk_free(pv_walk *w);

// Current screen name, or NULL when the walk has not started.
const char *pv_walk_current_name(const pv_walk *w);
// The selected edge in edge-list mode, or NULL when the current
// screen has no edges (or the walk is in branch mode).
const pv_edge *pv_walk_selected_edge(const pv_walk *w);

// Move the selection up/down. In branch mode these move branch_sel;
// otherwise they move sel. Both clamp at the ends, never wrap.
void pv_walk_move_up(pv_walk *w);
void pv_walk_move_down(pv_walk *w);
// Follow the selected edge: push current on the back stack and move
// to the target, or (unknown edge / missing target) set unknown_shown
// and stay. No-op when the current screen has no edges. In branch
// mode this is a branch pick (see pv_walk_branch_pick).
void pv_walk_follow(pv_walk *w);
// Back: in branch mode, leave branch mode; otherwise pop the back
// stack, or no-op at the root (empty stack).
void pv_walk_back(pv_walk *w);
// Enter branch-pick mode for the current screen. Returns 1 when
// entered, 0 when the screen has no branch entries (no-op).
int pv_walk_branch_enter(pv_walk *w);
// Leave branch-pick mode without picking.
void pv_walk_branch_exit(pv_walk *w);
// Pick the selected branch entry: follow its edge when it has one
// (same rules as pv_walk_follow), or record the pick and stay when
// it has none. Always leaves branch mode. No-op outside branch mode.
// Never evaluates the branch condition.
void pv_walk_branch_pick(pv_walk *w);
// Dispatch one scripted key (see pv_walk_key). ENTER in branch mode
// is a branch pick; BACK in branch mode exits branch mode.
void pv_walk_apply_key(pv_walk *w, pv_walk_key key);

// Render the walk view as text (no terminal I/O): the current
// screen's wireframe via pv_render_screen, then the outgoing edge
// list (or the branch-entry list in branch mode) with a selection
// cursor, the back stack, any unknown/pick note, and footer hints.
// frame_width of 0 means the project's frame width.
void pv_walk_render(ccb_str *out, const pv_walk *w, long frame_width,
  int ascii, int color);

// Terminal loop: check for a TTY, enter raw mode, run the key loop
// until quit, and restore the terminal on EVERY exit path (normal
// quit, Ctrl-C byte, SIGINT/SIGTERM, read error, EOF). Returns 0 on
// a clean quit, 1 when start_name names no screen, 2 when stdin or
// stdout is not a TTY (a clean diagnostic on stderr, never a hang),
// or when a requested record file cannot be written. frame_width of
// 0 means the project's frame width. record_path NULL means no
// recording; otherwise the walk's recorded steps are written as
// trace JSON (see ccb_preview_trace.h) after the terminal is
// restored, before the walk is freed.
int pv_walk_run(pv_project *proj, const char *start_name, long frame_width,
  int ascii, int color, const char *record_path);

#endif
