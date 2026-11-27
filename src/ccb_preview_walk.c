// ccb_preview_walk: interactive walk mode for ccb preview (P3).
// The state machine (pv_walk_*) has no terminal I/O; the terminal
// layer (pv_walk_run) owns the ccb_tui terminal and the key loop.
// Static exploration only: nothing here evaluates a condition or
// executes project code.
#include "ccb_common.h"
#include "ccb_preview_walk.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include "ccb_tui_screen.h"
#include "ccb_tui_term.h"
#include <unistd.h>

// ---------- state machine helpers ----------

// Find a screen by name, or NULL. Chunk A: delegates to
// pv_screen_find, so walk follows suffix-matched edges (an edge to
// DetailActivity lands on the Detail screen) instead of showing
// unknown for an edge the diagnostics pass accepts.
static pv_screen *walk_find(pv_project *p, const char *name) {
  if (p == NULL || name == NULL) {
    return NULL;
  }
  return pv_screen_find(p, name);
}

// The entry screen: "Home" when present, else the first screen.
static pv_screen *walk_entry(pv_project *p) {
  pv_screen *home = walk_find(p, "Home");
  if (home != NULL) {
    return home;
  }
  return p->nscreens > 0 ? p->screens[0] : NULL;
}

// True when any box in the subtree has the given label.
static int subtree_has_label(pv_box *box, const char *label) {
  size_t i = 0;
  if (box == NULL || label == NULL) {
    return 0;
  }
  if (box->label != NULL && strcmp(box->label, label) == 0) {
    return 1;
  }
  for (i = 0; i < box->nchildren; i++) {
    if (subtree_has_label(box->children[i], label)) {
      return 1;
    }
  }
  return 0;
}

// First screen edge whose `from` names a component inside the entry's
// subtree, or NULL. This is the only static link the flow parser
// records between a branch entry and an outgoing edge.
static const pv_edge *branch_entry_edge(pv_walk *w, pv_box *entry) {
  size_t i = 0;
  if (w->current == NULL) {
    return NULL;
  }
  for (i = 0; i < w->current->nedges; i++) {
    const pv_edge *e = &w->current->edges[i];
    if (e->from != NULL && subtree_has_label(entry, e->from)) {
      return e;
    }
  }
  return NULL;
}

static void walk_branches_grow(pv_walk *w) {
  size_t ncap = 0;
  if (w->nbranches < w->capbranches) {
    return;
  }
  ncap = w->capbranches == 0 ? 8 : w->capbranches * 2;
  if (w->capbranches != 0 &&
      __builtin_mul_overflow(w->capbranches, (size_t)2, &ncap)) {
    ncap = (size_t)-1;
  }
  w->branches = ccb_xrealloc(w->branches,
    ncap * sizeof(*w->branches));
  w->capbranches = ncap;
}

// Depth-first collection of (branch, entry) pairs under box.
static void walk_collect_branches(pv_walk *w, pv_box *box) {
  size_t i = 0;
  if (box == NULL) {
    return;
  }
  if (box->is_branch) {
    for (i = 0; i < box->nchildren; i++) {
      pv_box *child = box->children[i];
      pv_walk_branch_entry *be = NULL;
      if (child->type == NULL ||
          strcmp(child->type, "branch-entry") != 0) {
        continue;
      }
      walk_branches_grow(w);
      be = &w->branches[w->nbranches++];
      be->branch_label = box->label;
      be->entry_label = child->label;
      be->entry = child;
      be->edge = branch_entry_edge(w, child);
    }
  }
  for (i = 0; i < box->nchildren; i++) {
    walk_collect_branches(w, box->children[i]);
  }
}

// Rebuild the flattened branch list for the current screen.
static void walk_rebuild_branches(pv_walk *w) {
  w->nbranches = 0;
  w->branch_sel = 0;
  w->in_branch = 0;
  if (w->current != NULL && w->current->root != NULL) {
    walk_collect_branches(w, w->current->root);
  }
}

// Clear the outcome notes (unknown edge, branch pick).
static void walk_clear_notes(pv_walk *w) {
  w->unknown_shown = 0;
  w->unknown_from = NULL;
  w->unknown_target = NULL;
  w->branch_picked = 0;
  w->picked_branch = NULL;
  w->picked_entry = NULL;
  w->picked_has_edge = 0;
}

// Push the current screen on the back stack.
static void walk_push(pv_walk *w) {
  size_t ncap = 0;
  if (w->nback >= w->capback) {
    ncap = w->capback == 0 ? 8 : w->capback * 2;
    if (w->capback != 0 &&
        __builtin_mul_overflow(w->capback, (size_t)2, &ncap)) {
      ncap = (size_t)-1;
    }
    w->back = ccb_xrealloc(w->back, ncap * sizeof(*w->back));
    w->capback = ncap;
  }
  w->back[w->nback++] = w->current;
}

// Append one recorded step. Screen, edge label, branch label, and
// argument arrays are borrowed from the project arena; only the
// steps array itself is owned by the walk. A NULL edge records a
// pick-only step (a branch picked without navigating).
static void walk_record(pv_walk *w, const char *screen,
    const pv_edge *e, const char *branch) {
  pv_trace_step *step = NULL;
  if (w == NULL || screen == NULL) {
    return;
  }
  if (w->nrecord >= w->caprecord) {
    size_t ncap = w->caprecord == 0 ? 8 : w->caprecord * 2;
    if (w->caprecord != 0 &&
        __builtin_mul_overflow(w->caprecord, (size_t)2, &ncap)) {
      ncap = (size_t)-1;
    }
    w->record = ccb_xrealloc(w->record, ncap * sizeof(*w->record));
    w->caprecord = ncap;
  }
  step = &w->record[w->nrecord++];
  step->screen = screen;
  step->edge = e != NULL ? e->from : NULL;
  step->branch = branch;
  step->args = e != NULL ? (const char **)e->args : NULL;
  step->nargs = e != NULL ? e->nargs : 0;
}

// The branch-entry label an edge-list follow ran under, if the edge
// is the first edge of some branch entry of the current screen.
// Branch annotations on the edge itself are labels, not entry links,
// so the flattened branch list is the exact link.
static const char *walk_edge_branch(pv_walk *w, const pv_edge *e) {
  size_t i = 0;
  if (w == NULL || e == NULL) {
    return NULL;
  }
  for (i = 0; i < w->nbranches; i++) {
    if (w->branches[i].edge == e) {
      return w->branches[i].entry_label;
    }
  }
  return NULL;
}

// Move to a known target screen: push, switch, reset selection and
// notes, rebuild the branch list.
static void walk_go(pv_walk *w, pv_screen *target) {
  walk_push(w);
  w->current = target;
  w->sel = 0;
  walk_clear_notes(w);
  walk_rebuild_branches(w);
}

// ---------- state machine ----------

int pv_walk_start(pv_walk *w, pv_project *proj, const char *start_name) {
  pv_screen *start = NULL;
  if (w == NULL || proj == NULL || proj->nscreens == 0) {
    return -1;
  }
  memset(w, 0, sizeof(*w));
  w->proj = proj;
  if (start_name != NULL) {
    start = walk_find(proj, start_name);
  } else {
    start = walk_entry(proj);
  }
  if (start == NULL) {
    return -1;
  }
  w->current = start;
  walk_rebuild_branches(w);
  return 0;
}

void pv_walk_free(pv_walk *w) {
  if (w == NULL) {
    return;
  }
  free(w->back);
  free(w->branches);
  // The record array only: step strings and argument arrays are
  // borrowed from the project arena and must never be freed.
  free(w->record);
  memset(w, 0, sizeof(*w));
}

const char *pv_walk_current_name(const pv_walk *w) {
  if (w == NULL || w->current == NULL) {
    return NULL;
  }
  return w->current->name;
}

const pv_edge *pv_walk_selected_edge(const pv_walk *w) {
  if (w == NULL || w->current == NULL || w->in_branch) {
    return NULL;
  }
  if (w->sel >= w->current->nedges) {
    return NULL;
  }
  return &w->current->edges[w->sel];
}

void pv_walk_move_up(pv_walk *w) {
  if (w == NULL || w->current == NULL) {
    return;
  }
  if (w->in_branch) {
    if (w->branch_sel > 0) {
      w->branch_sel--;
    }
  } else if (w->sel > 0) {
    w->sel--;
  }
}

void pv_walk_move_down(pv_walk *w) {
  if (w == NULL || w->current == NULL) {
    return;
  }
  if (w->in_branch) {
    if (w->nbranches > 0 && w->branch_sel + 1 < w->nbranches) {
      w->branch_sel++;
    }
  } else if (w->current->nedges > 0 &&
      w->sel + 1 < w->current->nedges) {
    w->sel++;
  }
}

void pv_walk_follow(pv_walk *w) {
  const pv_edge *e = NULL;
  pv_screen *target = NULL;
  if (w == NULL || w->current == NULL) {
    return;
  }
  if (w->in_branch) {
    pv_walk_branch_pick(w);
    return;
  }
  if (w->current->nedges == 0 || w->sel >= w->current->nedges) {
    return;
  }
  e = &w->current->edges[w->sel];
  // Unknown edge, or a target screen that does not exist: show it
  // as unknown and stay. Never invent a destination.
  if (e->unknown || e->target == NULL) {
    w->unknown_shown = 1;
    w->unknown_from = e->from;
    w->unknown_target = e->target;
    return;
  }
  target = walk_find(w->proj, e->target);
  if (target == NULL) {
    w->unknown_shown = 1;
    w->unknown_from = e->from;
    w->unknown_target = e->target;
    return;
  }
  // Record before the move: walk_go rebuilds the branch list, and
  // the branch lookup above reads it.
  walk_record(w, w->current->name, e, walk_edge_branch(w, e));
  walk_go(w, target);
}

void pv_walk_back(pv_walk *w) {
  pv_screen *prev = NULL;
  if (w == NULL || w->current == NULL) {
    return;
  }
  if (w->in_branch) {
    pv_walk_branch_exit(w);
    return;
  }
  if (w->nback == 0) {
    return; // at the root: back is a no-op
  }
  prev = w->back[--w->nback];
  w->current = prev;
  w->sel = 0;
  walk_clear_notes(w);
  walk_rebuild_branches(w);
}

int pv_walk_branch_enter(pv_walk *w) {
  if (w == NULL || w->current == NULL) {
    return 0;
  }
  if (w->in_branch) {
    return 1;
  }
  if (w->nbranches == 0) {
    return 0;
  }
  w->in_branch = 1;
  w->branch_sel = 0;
  w->unknown_shown = 0;
  w->unknown_from = NULL;
  w->unknown_target = NULL;
  return 1;
}

void pv_walk_branch_exit(pv_walk *w) {
  if (w == NULL) {
    return;
  }
  w->in_branch = 0;
  w->branch_sel = 0;
}

void pv_walk_branch_pick(pv_walk *w) {
  pv_walk_branch_entry *be = NULL;
  pv_screen *target = NULL;
  if (w == NULL || w->current == NULL || !w->in_branch) {
    return;
  }
  if (w->branch_sel >= w->nbranches) {
    w->in_branch = 0;
    return;
  }
  be = &w->branches[w->branch_sel];
  // Record the pick first; a move to a new screen clears the notes,
  // so only stay-cases keep the record visible.
  w->branch_picked = 1;
  w->picked_branch = be->branch_label;
  w->picked_entry = be->entry_label;
  w->picked_has_edge = be->edge != NULL;
  w->in_branch = 0;
  w->branch_sel = 0;
  if (be->edge == NULL) {
    // Entry has no outgoing edge: stay, pick recorded as a
    // pick-only step (the branch fact is real even without a move).
    walk_record(w, w->current->name, NULL, be->entry_label);
    return;
  }
  if (be->edge->unknown || be->edge->target == NULL) {
    w->unknown_shown = 1;
    w->unknown_from = be->edge->from;
    w->unknown_target = be->edge->target;
    // Same: the pick happened, the navigation did not.
    walk_record(w, w->current->name, NULL, be->entry_label);
    return;
  }
  target = walk_find(w->proj, be->edge->target);
  if (target == NULL) {
    w->unknown_shown = 1;
    w->unknown_from = be->edge->from;
    w->unknown_target = be->edge->target;
    walk_record(w, w->current->name, NULL, be->entry_label);
    return;
  }
  walk_record(w, w->current->name, be->edge, be->entry_label);
  walk_go(w, target);
}

void pv_walk_apply_key(pv_walk *w, pv_walk_key key) {
  if (w == NULL) {
    return;
  }
  switch (key) {
    case PV_WALK_KEY_UP:
      pv_walk_move_up(w);
      break;
    case PV_WALK_KEY_DOWN:
      pv_walk_move_down(w);
      break;
    case PV_WALK_KEY_ENTER:
      pv_walk_follow(w);
      break;
    case PV_WALK_KEY_BACK:
      pv_walk_back(w);
      break;
    case PV_WALK_KEY_QUIT:
      w->quit = 1;
      break;
    case PV_WALK_KEY_BRANCH:
      (void)pv_walk_branch_enter(w);
      break;
    case PV_WALK_KEY_BRANCH_EXIT:
      pv_walk_branch_exit(w);
      break;
    default:
      break;
  }
}

// ---------- rendering (text only, no terminal I/O) ----------

// The text after the wireframe: edge/branch list, back stack, notes,
// footer hints. Shared by the string form (pv_walk_render) and the
// terminal frame builder (pv_walk_build_frame), so both show the same
// lines.
static void walk_render_text(ccb_str *out, const pv_walk *w) {
  size_t i = 0;
  if (out == NULL || w == NULL || w->current == NULL) {
    return;
  }
  if (w->in_branch) {
    ccb_str_append(out, "Branches (conditions are shown, never "
      "evaluated):\n");
    if (w->nbranches == 0) {
      ccb_str_append(out, "  (no branches on this screen)\n");
    }
    for (i = 0; i < w->nbranches; i++) {
      const pv_walk_branch_entry *be = &w->branches[i];
      ccb_str_append(out, i == w->branch_sel ? "> " : "  ");
      ccb_str_append(out, be->branch_label != NULL ?
        be->branch_label : "?");
      ccb_str_append(out, ": ");
      ccb_str_append(out, be->entry_label != NULL ?
        be->entry_label : "?");
      if (be->edge != NULL && be->edge->target != NULL &&
          !be->edge->unknown) {
        ccb_str_append(out, " -> ");
        ccb_str_append(out, be->edge->target);
      } else if (be->edge != NULL) {
        ccb_str_append(out, " -> unknown");
      } else {
        ccb_str_append(out, " (no outgoing edge)");
      }
      ccb_str_append(out, "\n");
    }
  } else {
    ccb_str_append(out, "Edges:\n");
    if (w->current->nedges == 0) {
      ccb_str_append(out, "  (no outgoing edges)\n");
    }
    for (i = 0; i < w->current->nedges; i++) {
      const pv_edge *e = &w->current->edges[i];
      ccb_str_append(out, i == w->sel ? "> " : "  ");
      ccb_str_append(out, "-> ");
      if (e->unknown || e->target == NULL) {
        ccb_str_append(out, "unknown");
      } else {
        ccb_str_append(out, e->target);
      }
      pv_render_edge_args(out, e);
      ccb_str_append(out, " via \"");
      ccb_str_append(out, e->from != NULL ? e->from : "?");
      ccb_str_append(out, "\"");
      if (e->branch != NULL) {
        ccb_str_append(out, " [");
        ccb_str_append(out, e->branch);
        ccb_str_append(out, "]");
      }
      ccb_str_append(out, "\n");
    }
  }
  ccb_str_append(out, "Back: ");
  if (w->nback == 0) {
    ccb_str_append(out, "(root)");
  } else {
    for (i = 0; i < w->nback; i++) {
      if (i > 0) {
        ccb_str_append(out, " > ");
      }
      ccb_str_append(out, w->back[i]->name != NULL ?
        w->back[i]->name : "?");
    }
  }
  ccb_str_append(out, "\n");
  if (w->unknown_shown) {
    ccb_str_append(out, "unknown edge: via \"");
    ccb_str_append(out, w->unknown_from != NULL ?
      w->unknown_from : "?");
    ccb_str_append(out, "\"");
    if (w->unknown_target != NULL) {
      ccb_str_append(out, " to missing screen '");
      ccb_str_append(out, w->unknown_target);
      ccb_str_append(out, "'");
    } else {
      ccb_str_append(out, " (destination could not be resolved)");
    }
    ccb_str_append(out, " — staying here\n");
  }
  if (w->branch_picked) {
    ccb_str_append(out, "picked: ");
    ccb_str_append(out, w->picked_branch != NULL ?
      w->picked_branch : "?");
    ccb_str_append(out, " = ");
    ccb_str_append(out, w->picked_entry != NULL ?
      w->picked_entry : "?");
    if (!w->picked_has_edge) {
      ccb_str_append(out, " (no outgoing edge — staying here)");
    }
    ccb_str_append(out, "\n");
  }
  if (w->in_branch) {
    ccb_str_append(out, "up/down or j/k: move | Enter: pick | "
      "Esc/b: back to edges | q: quit\n");
  } else {
    ccb_str_append(out, "up/down or j/k: move | Enter: follow | "
      "b/Backspace: back | t: branches | q: quit\n");
  }
}

void pv_walk_render(ccb_str *out, const pv_walk *w, long frame_width,
    int ascii, int color) {
  if (out == NULL || w == NULL || w->current == NULL) {
    return;
  }
  if (frame_width <= 0) {
    frame_width = w->proj != NULL ? w->proj->frame_width : 40;
  }
  if (w->current->root != NULL) {
    pv_render_screen(out, w->current, frame_width, ascii, color);
  } else {
    ccb_str_append(out, "(no wireframe for this screen)\n");
  }
  walk_render_text(out, w);
}

// ---------- terminal layer (ccb_tui) ----------

// Translate one tui input event to a walk key. Returns 1 and sets
// *key, or 0 when the event has no walk action. The mapping preserves
// the old byte reader's behavior: Ctrl-C quits; a bare Esc exits
// branch mode; any other escape-derived key (left/right arrows,
// Home/End, PgUp/PgDn, F-keys) also exits branch mode, as the old
// reader did for every unknown sequence; a paste is ignored (the old
// reader would have acted on pasted bytes one by one). LF arrives as
// a Ctrl-J char key and follows, as the old reader's '\n' case did.
int pv_walk_key_from_tui_event(const ccb_tui_event *ev,
    pv_walk_key *key) {
  if (ev == NULL || key == NULL || ev->kind != CCB_TUI_EV_KEY) {
    return 0;
  }
  if (ev->evtype == 3) {
    return 0; // key release: no action
  }
  switch (ev->key) {
    case CCB_TUI_KEY_UP:
      *key = PV_WALK_KEY_UP;
      return 1;
    case CCB_TUI_KEY_DOWN:
      *key = PV_WALK_KEY_DOWN;
      return 1;
    case CCB_TUI_KEY_ENTER:
      *key = PV_WALK_KEY_ENTER;
      return 1;
    case CCB_TUI_KEY_BACKSPACE:
      *key = PV_WALK_KEY_BACK;
      return 1;
    case CCB_TUI_KEY_ESC:
      *key = PV_WALK_KEY_BRANCH_EXIT;
      return 1;
    case CCB_TUI_KEY_CHAR:
      break; // plain character keys: handled below
    case CCB_TUI_KEY_UNKNOWN:
    case CCB_TUI_KEY_TAB:
      return 0;
    default:
      // Any other escape-derived key: exit branch mode, as before.
      *key = PV_WALK_KEY_BRANCH_EXIT;
      return 1;
  }
  if (ev->ch == 'k' && ev->mods == 0) {
    *key = PV_WALK_KEY_UP;
    return 1;
  }
  if (ev->ch == 'j' && ev->mods == 0) {
    *key = PV_WALK_KEY_DOWN;
    return 1;
  }
  if (ev->ch == 'b' && ev->mods == 0) {
    *key = PV_WALK_KEY_BACK;
    return 1;
  }
  if (ev->ch == 't' && ev->mods == 0) {
    *key = PV_WALK_KEY_BRANCH;
    return 1;
  }
  if (ev->ch == 'q' && ev->mods == 0) {
    *key = PV_WALK_KEY_QUIT;
    return 1;
  }
  if (ev->ch == 'c' && (ev->mods & CCB_TUI_MOD_CTRL) != 0) {
    *key = PV_WALK_KEY_QUIT;
    return 1;
  }
  if (ev->ch == 'j' && (ev->mods & CCB_TUI_MOD_CTRL) != 0) {
    *key = PV_WALK_KEY_ENTER;
    return 1;
  }
  return 0;
}

// Put one text line into the frame, clipped to the screen width.
// Lines are put one at a time (not as one multi-line string): a line
// longer than the screen clips, but the lines after it still render.
static void walk_put_line(ccb_tui_screen *s, int row, const char *line,
    size_t len) {
  ccb_str tmp;
  ccb_tui_style st = ccb_tui_style_default();
  if (s == NULL || line == NULL) {
    return;
  }
  ccb_str_init(&tmp);
  ccb_str_append_n(&tmp, line, len);
  ccb_tui_screen_put_text(s, 0, row, tmp.buf != NULL ? tmp.buf : "",
    st);
  ccb_str_free(&tmp);
}

// Build the whole walk view into a screen: the current screen's
// wireframe at (0,0), then the edge/branch list, back stack, notes,
// and footer hints as text lines below it. The screen is the
// caller's: terminal-sized in the loop (a narrow terminal clips the
// frame instead of wrapping it, as the old full print did), or
// content-sized in tests. Lines are the same bytes pv_walk_render
// emits, so the frame and the string can never drift apart.
void pv_walk_build_frame(ccb_tui_screen *s, pv_walk *w,
    long frame_width, int ascii, int color) {
  long fw = 0;
  long fh = 0;
  long row = 0;
  ccb_str text;
  const char *p = NULL;
  if (s == NULL || w == NULL) {
    return;
  }
  ccb_tui_screen_clear(s);
  ccb_str_init(&text);
  if (w->current != NULL && w->current->root != NULL) {
    pv_render_frame_geom(w->current, frame_width, &fw, &fh);
    pv_render_frame_tui(s, w->current, frame_width, ascii, color);
    row = fh;
  } else {
    ccb_str_append(&text, "(no wireframe for this screen)\n");
  }
  walk_render_text(&text, w);
  p = text.buf != NULL ? text.buf : "";
  while (*p != '\0' && row < ccb_tui_screen_rows(s)) {
    const char *nl = strchr(p, '\n');
    size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
    walk_put_line(s, (int)row, p, len);
    row++;
    if (nl == NULL) {
      break;
    }
    p = nl + 1;
  }
  ccb_str_free(&text);
}

int pv_walk_run(pv_project *proj, const char *start_name, long frame_width,
    int ascii, int color, const char *record_path) {
  pv_walk w;
  ccb_tui_term *term = NULL;
  ccb_tui_term_opts opts;
  ccb_tui_screen *scr = NULL;
  int rc = 0;
  if (proj == NULL) {
    return 1;
  }
  // Refuse non-TTY stdio before touching the terminal: never hang.
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "error: walk mode requires a terminal: stdin and "
      "stdout must both be TTYs\n");
    return 2;
  }
  memset(&w, 0, sizeof(w));
  if (pv_walk_start(&w, proj, start_name) != 0) {
    if (start_name != NULL) {
      fprintf(stderr, "error: no screen named '%s'\n", start_name);
    } else {
      fprintf(stderr, "error: no screens to walk\n");
    }
    pv_walk_free(&w);
    return 1;
  }
  if (frame_width <= 0) {
    frame_width = proj->frame_width;
  }
  // The tui terminal owns raw mode, the alternate screen, DEC 2026
  // synchronised output, the resize self-pipe, and the signal
  // handlers from here on; closing it restores everything, on every
  // exit path below.
  ccb_tui_term_opts_default(&opts);
  if (ccb_tui_term_open(&opts, &term) != CCB_OK) {
    fprintf(stderr, "error: cannot open terminal\n");
    pv_walk_free(&w);
    return 1;
  }
  scr = ccb_tui_screen_new(ccb_tui_term_cols(term),
    ccb_tui_term_rows(term));
  if (scr == NULL) {
    ccb_tui_term_close(term);
    pv_walk_free(&w);
    return 1;
  }
  // Key loop: build the frame, present it (the diff emits only what
  // changed — no full clear per frame, which is what flickered in
  // Termux), read one event, apply it. Every exit below closes the
  // terminal before returning.
  while (!w.quit) {
    ccb_tui_event ev;
    pv_walk_key key = PV_WALK_KEY_QUIT;
    pv_walk_build_frame(scr, &w, frame_width, ascii, color);
    if (ccb_tui_term_present(term, scr) != CCB_OK) {
      break;
    }
    if (ccb_tui_term_poll_event(term, -1, &ev) != CCB_OK) {
      break;
    }
    if (ev.kind == CCB_TUI_EV_INTERRUPT || ev.kind == CCB_TUI_EV_EOF) {
      break;
    }
    if (ev.kind == CCB_TUI_EV_RESIZE) {
      // Rebuild at the new size on the next iteration; present()
      // forces a full repaint for the changed dimensions.
      if (ccb_tui_screen_resize(scr, ccb_tui_term_cols(term),
          ccb_tui_term_rows(term)) != CCB_OK) {
        break;
      }
      continue;
    }
    if (ev.kind != CCB_TUI_EV_KEY) {
      continue; // paste and anything else: ignored
    }
    if (!pv_walk_key_from_tui_event(&ev, &key)) {
      continue;
    }
    pv_walk_apply_key(&w, key);
  }
  rc = 0;
  ccb_tui_term_close(term);
  ccb_tui_screen_free(scr);
  // Chunk 2: write the recorded trace after the terminal is
  // restored but before the walk is freed (steps borrow project
  // strings, which are still alive; only the steps array is freed
  // with the walk). A write failure is a usage error, not a
  // silent loss of the recording.
  if (record_path != NULL) {
    ccb_str out;
    ccb_str_init(&out);
    pv_trace_write(&out, w.record, w.nrecord);
    if (ccb_write_file(record_path, out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : "")) != CCB_OK) {
      fprintf(stderr, "error: cannot write trace to '%s'\n",
        record_path);
      rc = 2;
    }
    ccb_str_free(&out);
  }
  pv_walk_free(&w);
  fputs("\n", stdout);
  fflush(stdout);
  return rc;
}
