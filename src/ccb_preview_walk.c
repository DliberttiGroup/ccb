// ccb_preview_walk: interactive walk mode for ccb preview (P3).
// The state machine (pv_walk_*) has no terminal I/O; the terminal
// layer (pv_walk_run) owns raw mode, signal handling, and key
// decoding. Static exploration only: nothing here evaluates a
// condition or executes project code.
#include "ccb_common.h"
#include "ccb_preview_walk.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include <signal.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

// ---------- state machine helpers ----------

// Find a screen by name, or NULL.
static pv_screen *walk_find(pv_project *p, const char *name) {
  size_t i = 0;
  if (p == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, name) == 0) {
      return p->screens[i];
    }
  }
  return NULL;
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

void pv_walk_render(ccb_str *out, const pv_walk *w, long frame_width,
    int ascii, int color) {
  size_t i = 0;
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

// ---------- terminal layer ----------

// Saved terminal state, restored on every exit path. The signal
// handler only records the signal; the loop restores the terminal
// itself, so restoration never runs inside a handler.
static struct termios walk_saved_termios;
static int walk_termios_saved;
static volatile sig_atomic_t walk_got_signal;

static void walk_on_signal(int sig) {
  walk_got_signal = sig;
}

static void walk_restore_termios(void) {
  if (walk_termios_saved) {
    tcsetattr(STDIN_FILENO, TCSANOW, &walk_saved_termios);
    walk_termios_saved = 0;
  }
}

// Read one walk key from stdin (raw mode). Returns 1 and sets *key,
// 0 on EOF (clean end), or -1 on read error / interrupt.
static int walk_read_key(pv_walk_key *key) {
  unsigned char c = 0;
  ssize_t n = 0;
  // Outer loop: unknown bytes are skipped without recursion, so a
  // long garbage paste cannot grow the stack.
  for (;;) {
    for (;;) {
      n = read(STDIN_FILENO, &c, 1);
      if (n == 1) {
        break;
      }
      if (n == 0) {
        return 0;
      }
      if (walk_got_signal) {
        return -1;
      }
      // EINTR without a recorded signal: retry.
    }
  switch (c) {
    case 'q':
      *key = PV_WALK_KEY_QUIT;
      return 1;
    case 'j':
      *key = PV_WALK_KEY_DOWN;
      return 1;
    case 'k':
      *key = PV_WALK_KEY_UP;
      return 1;
    case 'b':
      *key = PV_WALK_KEY_BACK;
      return 1;
    case 't':
      *key = PV_WALK_KEY_BRANCH;
      return 1;
    case '\r':
    case '\n':
      *key = PV_WALK_KEY_ENTER;
      return 1;
    case 0x7f:
    case 0x08:
      *key = PV_WALK_KEY_BACK;
      return 1;
    case 0x03: // Ctrl-C with ISIG off arrives as a byte: quit cleanly
      *key = PV_WALK_KEY_QUIT;
      return 1;
    case 0x1b: {
      // Arrow keys arrive as ESC [ A/B (or ESC O A/B). A bare Esc
      // (nothing follows within 50 ms) exits branch mode.
      fd_set rfds;
      struct timeval tv;
      FD_ZERO(&rfds);
      FD_SET(STDIN_FILENO, &rfds);
      tv.tv_sec = 0;
      tv.tv_usec = 50000;
      if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0) {
        *key = PV_WALK_KEY_BRANCH_EXIT;
        return 1;
      }
      if (read(STDIN_FILENO, &c, 1) != 1) {
        *key = PV_WALK_KEY_BRANCH_EXIT;
        return 1;
      }
      if (c != '[' && c != 'O') {
        *key = PV_WALK_KEY_BRANCH_EXIT;
        return 1;
      }
      if (read(STDIN_FILENO, &c, 1) != 1) {
        *key = PV_WALK_KEY_BRANCH_EXIT;
        return 1;
      }
      if (c == 'A') {
        *key = PV_WALK_KEY_UP;
      } else if (c == 'B') {
        *key = PV_WALK_KEY_DOWN;
      } else {
        *key = PV_WALK_KEY_BRANCH_EXIT;
      }
      return 1;
    }
    default:
      // Unknown byte: ignore it and read the next one.
      continue;
  }
  }
}

int pv_walk_run(pv_project *proj, const char *start_name, long frame_width,
    int ascii, int color, const char *record_path) {
  pv_walk w;
  struct termios raw;
  struct sigaction sa, old_int, old_term;
  int have_old = 0;
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
  // Install signal handlers that only record the signal; the loop
  // below restores the terminal on the next iteration.
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = walk_on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, &old_int) == 0 &&
      sigaction(SIGTERM, &sa, &old_term) == 0) {
    have_old = 1;
  }
  if (tcgetattr(STDIN_FILENO, &walk_saved_termios) != 0) {
    fprintf(stderr, "error: cannot read terminal settings\n");
    if (have_old) {
      sigaction(SIGINT, &old_int, NULL);
      sigaction(SIGTERM, &old_term, NULL);
    }
    pv_walk_free(&w);
    return 1;
  }
  walk_termios_saved = 1;
  raw = walk_saved_termios;
  cfmakeraw(&raw);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    fprintf(stderr, "error: cannot set terminal raw mode\n");
    walk_restore_termios();
    if (have_old) {
      sigaction(SIGINT, &old_int, NULL);
      sigaction(SIGTERM, &old_term, NULL);
    }
    pv_walk_free(&w);
    return 1;
  }
  // Key loop: render, read one key, apply it. Every exit below
  // restores the terminal before returning.
  while (!w.quit && !walk_got_signal) {
    ccb_str out;
    pv_walk_key key = PV_WALK_KEY_QUIT;
    int kr = 0;
    ccb_str_init(&out);
    pv_walk_render(&out, &w, frame_width, ascii, color);
    fputs("\x1b[2J\x1b[H", stdout);
    fputs(out.buf != NULL ? out.buf : "", stdout);
    fflush(stdout);
    ccb_str_free(&out);
    kr = walk_read_key(&key);
    if (kr <= 0) {
      break; // EOF, read error, or signal: restore and exit
    }
    pv_walk_apply_key(&w, key);
  }
  rc = 0;
  walk_restore_termios();
  if (have_old) {
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
  }
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
