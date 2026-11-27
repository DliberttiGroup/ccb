// Tests for preview walk mode (P3): the pv_walk state machine only,
// driven by scripted keys with no TTY and no terminal I/O. The
// terminal loop (pv_walk_run) is not exercised here: it refuses
// non-TTY stdio, so it cannot run inside this harness by design.
// The key translation (pv_walk_key_from_tui_event) and the frame
// builder (pv_walk_build_frame) ARE exercised: they are pure and
// need no terminal.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_preview_walk.h"
#include "ccb_diag.h"
#include "ccb_tui_screen.h"
#include <stdio.h>

static const char *kHome =
  "fun Home() {\n"
  "  Column {\n"
  "    Text(\"My Notes\")\n"
  "    Row {\n"
  "      Button(\"Open note\", onClick = { nav(\"Detail\") })\n"
  "      Button(\"New note\", onClick = { nav(\"Form\") })\n"
  "    }\n"
  "    Button(\"Settings\", onClick = { nav(\"Settings\") })\n"
  "  }\n"
  "}\n";

static const char *kDetail =
  "fun Detail() {\n"
  "  Column {\n"
  "    Text(\"Note detail\")\n"
  "    when (state) {\n"
  "      is Loading -> Text(\"Loading...\")\n"
  "      is Loaded -> Text(\"Note body\")\n"
  "      else -> Text(\"Empty\")\n"
  "    }\n"
  "    Button(\"Back\", onClick = { nav(\"Home\") })\n"
  "  }\n"
  "}\n";

static const char *kForm =
  "fun Form() {\n"
  "  Column {\n"
  "    Text(\"New note\")\n"
  "    Button(\"Save\", onClick = { nav(\"Home\") })\n"
  "    Button(\"More\", onClick = { nav(nextScreen) })\n"
  "  }\n"
  "}\n";

// A project with Home, Detail, and Form, built from flow text only
// (no layout needed: the state machine never renders boxes).
// The arena and screens outlive the walk; the caller frees the arena.
static void build_project(ccb_arena **arena_out, pv_project *proj) {
  ccb_arena *arena = ccb_arena_create();
  ccb_diag_list diags;
  pv_screen **screens = NULL;
  size_t nscreens = 0;
  ccb_diag_list_init(&diags);
  pv_flow_parse(arena, kHome, "Home.kt", &diags, &screens, &nscreens);
  pv_flow_parse(arena, kDetail, "Detail.kt", &diags, &screens, &nscreens);
  pv_flow_parse(arena, kForm, "Form.kt", &diags, &screens, &nscreens);
  ccb_diag_list_free(&diags);
  memset(proj, 0, sizeof(*proj));
  proj->arena = arena;
  proj->screens = screens;
  proj->nscreens = nscreens;
  proj->frame_width = 40;
  proj->interior_cols = 38;
  *arena_out = arena;
}

static pv_box *new_box(ccb_arena *a, const char *type,
    const char *label) {
  pv_box *b = ccb_arena_alloc(a, sizeof(*b));
  memset(b, 0, sizeof(*b));
  b->type = type != NULL ? ccb_arena_strdup(a, type) : NULL;
  b->label = label != NULL ? ccb_arena_strdup(a, label) : NULL;
  return b;
}

static void add_child(ccb_arena *a, pv_box *parent, pv_box *child) {
  pv_box **arr = ccb_arena_alloc(a,
    (parent->nchildren + 1) * sizeof(pv_box *));
  if (parent->nchildren > 0) {
    memcpy(arr, parent->children, parent->nchildren * sizeof(pv_box *));
  }
  arr[parent->nchildren] = child;
  parent->children = arr;
  parent->nchildren++;
  parent->capchildren = parent->nchildren;
}

static pv_screen *make_screen(ccb_arena *a, const char *name,
    pv_box *root) {
  pv_screen *s = ccb_arena_alloc(a, sizeof(*s));
  memset(s, 0, sizeof(*s));
  s->name = ccb_arena_strdup(a, name);
  s->root = root;
  return s;
}

// Trace line for the scripted demo: key in, screen/notes out.
static void trace_step(const char *key, const pv_walk *w) {
  const pv_edge *e = pv_walk_selected_edge(w);
  printf("key %-6s -> screen=%s sel=%s back=%zu unknown=%d picked=%s\n",
    key, pv_walk_current_name(w) != NULL ?
    pv_walk_current_name(w) : "?",
    e != NULL && e->from != NULL ? e->from : "-",
    w->nback, w->unknown_shown,
    w->branch_picked && w->picked_entry != NULL ? w->picked_entry : "-");
}

int main(void) {
  ccb_arena *arena = NULL;
  pv_project proj;
  pv_walk w;
  build_project(&arena, &proj);
  CHECK(proj.nscreens == 3);

  // (a) Home -> follow "Open note" -> Detail -> back -> Home -> quit.
  CHECK(pv_walk_start(&w, &proj, NULL) == 0);
  CHECK_STR_EQ(pv_walk_current_name(&w), "Home");
  CHECK(pv_walk_selected_edge(&w) != NULL);
  CHECK_STR_EQ(pv_walk_selected_edge(&w)->from, "Open note");
  pv_walk_apply_key(&w, PV_WALK_KEY_ENTER);
  CHECK_STR_EQ(pv_walk_current_name(&w), "Detail");
  CHECK(w.nback == 1);
  CHECK_STR_EQ(w.back[0]->name, "Home");
  pv_walk_apply_key(&w, PV_WALK_KEY_BACK);
  CHECK_STR_EQ(pv_walk_current_name(&w), "Home");
  CHECK(w.nback == 0);
  pv_walk_apply_key(&w, PV_WALK_KEY_QUIT);
  CHECK(w.quit == 1);
  pv_walk_free(&w);

  // (b) Branch pick on Detail's when-branch: pick "is Loaded".
  // The entry holds only text, so the pick is recorded and the walk
  // stays on Detail; the condition is never evaluated.
  CHECK(pv_walk_start(&w, &proj, "Detail") == 0);
  CHECK(pv_walk_branch_enter(&w) == 1);
  CHECK(w.in_branch == 1);
  CHECK(w.nbranches == 3);
  CHECK_STR_EQ(w.branches[0].entry_label, "is Loading");
  CHECK_STR_EQ(w.branches[1].entry_label, "is Loaded");
  CHECK_STR_EQ(w.branches[2].entry_label, "else");
  CHECK_STR_EQ(w.branches[0].branch_label, "when state");
  pv_walk_apply_key(&w, PV_WALK_KEY_DOWN); // to "is Loaded"
  CHECK(w.branch_sel == 1);
  pv_walk_apply_key(&w, PV_WALK_KEY_ENTER); // pick
  CHECK(w.in_branch == 0);
  CHECK_STR_EQ(pv_walk_current_name(&w), "Detail");
  CHECK(w.branch_picked == 1);
  CHECK_STR_EQ(w.picked_branch, "when state");
  CHECK_STR_EQ(w.picked_entry, "is Loaded");
  CHECK(w.picked_has_edge == 0);
  pv_walk_free(&w);

  // Branch enter on a screen without branches is a no-op.
  CHECK(pv_walk_start(&w, &proj, "Home") == 0);
  CHECK(pv_walk_branch_enter(&w) == 0);
  CHECK(w.in_branch == 0);
  pv_walk_free(&w);

  // (c) Follow an unknown edge (Form "More") stays, unknown shown.
  CHECK(pv_walk_start(&w, &proj, NULL) == 0);
  pv_walk_apply_key(&w, PV_WALK_KEY_DOWN); // to "New note"
  pv_walk_apply_key(&w, PV_WALK_KEY_ENTER); // Home -> Form
  CHECK_STR_EQ(pv_walk_current_name(&w), "Form");
  pv_walk_apply_key(&w, PV_WALK_KEY_DOWN); // to "More"
  CHECK_STR_EQ(pv_walk_selected_edge(&w)->from, "More");
  CHECK(pv_walk_selected_edge(&w)->unknown == 1);
  pv_walk_apply_key(&w, PV_WALK_KEY_ENTER);
  CHECK_STR_EQ(pv_walk_current_name(&w), "Form");
  CHECK(w.unknown_shown == 1);
  CHECK_STR_EQ(w.unknown_from, "More");
  CHECK(w.unknown_target == NULL);
  pv_walk_free(&w);

  // (d) Quit from start.
  CHECK(pv_walk_start(&w, &proj, NULL) == 0);
  pv_walk_apply_key(&w, PV_WALK_KEY_QUIT);
  CHECK(w.quit == 1);
  CHECK_STR_EQ(pv_walk_current_name(&w), "Home");
  pv_walk_free(&w);

  // (e) Back at the root is a no-op.
  CHECK(pv_walk_start(&w, &proj, NULL) == 0);
  pv_walk_apply_key(&w, PV_WALK_KEY_BACK);
  CHECK_STR_EQ(pv_walk_current_name(&w), "Home");
  CHECK(w.nback == 0);
  pv_walk_free(&w);

  // Selection clamps, never wraps.
  CHECK(pv_walk_start(&w, &proj, NULL) == 0);
  pv_walk_apply_key(&w, PV_WALK_KEY_UP); // already at first
  CHECK(w.sel == 0);
  pv_walk_apply_key(&w, PV_WALK_KEY_DOWN);
  pv_walk_apply_key(&w, PV_WALK_KEY_DOWN);
  pv_walk_apply_key(&w, PV_WALK_KEY_DOWN); // past last
  CHECK(w.sel == 2);
  CHECK_STR_EQ(pv_walk_selected_edge(&w)->from, "Settings");
  pv_walk_free(&w);
  // Unknown start screen is rejected.
  CHECK(pv_walk_start(&w, &proj, "Ghost") != 0);
  pv_walk_free(&w);

  // Scripted demo trace (also reported to the coordinator):
  // Home -Enter-> Detail -t,down,Enter-> pick "is Loaded" (stay)
  // -b-> Home -q.
  printf("--- demo trace ---\n");
  CHECK(pv_walk_start(&w, &proj, NULL) == 0);
  trace_step("start", &w);
  pv_walk_apply_key(&w, PV_WALK_KEY_ENTER);
  trace_step("Enter", &w);
  pv_walk_apply_key(&w, PV_WALK_KEY_BRANCH);
  trace_step("t", &w);
  pv_walk_apply_key(&w, PV_WALK_KEY_DOWN);
  trace_step("down", &w);
  pv_walk_apply_key(&w, PV_WALK_KEY_ENTER);
  trace_step("Enter", &w);
  pv_walk_apply_key(&w, PV_WALK_KEY_BACK);
  trace_step("b", &w);
  pv_walk_apply_key(&w, PV_WALK_KEY_QUIT);
  trace_step("q", &w);
  CHECK_STR_EQ(pv_walk_current_name(&w), "Home");
  CHECK(w.quit == 1);
  pv_walk_free(&w);

  // --- Key mapping: tui events -> walk keys.
  {
    ccb_tui_event ev;
    pv_walk_key key = PV_WALK_KEY_QUIT;
    memset(&ev, 0, sizeof(ev));
    ev.kind = CCB_TUI_EV_KEY;
    ev.evtype = 1;
    ev.key = CCB_TUI_KEY_UP;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_UP);
    ev.key = CCB_TUI_KEY_DOWN;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_DOWN);
    ev.key = CCB_TUI_KEY_CHAR;
    ev.ch = 'k';
    ev.mods = 0;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_UP);
    ev.ch = 'j';
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_DOWN);
    ev.key = CCB_TUI_KEY_ENTER;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_ENTER);
    ev.key = CCB_TUI_KEY_BACKSPACE;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_BACK);
    ev.key = CCB_TUI_KEY_CHAR;
    ev.ch = 'b';
    ev.mods = 0;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_BACK);
    ev.ch = 'q';
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_QUIT);
    ev.ch = 'c';
    ev.mods = CCB_TUI_MOD_CTRL;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_QUIT);
    ev.ch = 't';
    ev.mods = 0;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_BRANCH);
    ev.key = CCB_TUI_KEY_ESC;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_BRANCH_EXIT);
    // Escape-derived keys exit branch mode, as the old byte reader
    // did for every unknown sequence.
    ev.key = CCB_TUI_KEY_LEFT;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_BRANCH_EXIT);
    ev.key = CCB_TUI_KEY_F1;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_BRANCH_EXIT);
    // LF arrives as Ctrl-J and follows (the old '\n' case).
    ev.key = CCB_TUI_KEY_CHAR;
    ev.ch = 'j';
    ev.mods = CCB_TUI_MOD_CTRL;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 1 &&
      key == PV_WALK_KEY_ENTER);
    // Ignored: paste, tab, unknown chars, key release, NULL args.
    ev.kind = CCB_TUI_EV_PASTE;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 0);
    ev.kind = CCB_TUI_EV_KEY;
    ev.key = CCB_TUI_KEY_TAB;
    ev.mods = 0;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 0);
    ev.key = CCB_TUI_KEY_CHAR;
    ev.ch = 'x';
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 0);
    ev.key = CCB_TUI_KEY_UP;
    ev.evtype = 3;
    CHECK(pv_walk_key_from_tui_event(&ev, &key) == 0);
    ev.evtype = 1;
    CHECK(pv_walk_key_from_tui_event(NULL, &key) == 0);
    CHECK(pv_walk_key_from_tui_event(&ev, NULL) == 0);
  }

  // --- Frame builder: the terminal frame dumps to exactly the bytes
  // pv_walk_render emits. A hand-built laid-out screen (root != NULL)
  // exercises the wireframe-in-frame path; the flow-built project
  // above only exercises the no-wireframe path. The screen is sized
  // from the string itself (longest line in bytes, which is >= its
  // cell width, and the line count); extra width is blank and the
  // dump trims it.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p2;
    pv_box *root = new_box(a, "screen", NULL);
    pv_box *t = new_box(a, "text", "My Notes");
    pv_box *btn = new_box(a, "button", "Open");
    pv_screen *screen = NULL;
    pv_screen **screens = NULL;
    pv_edge *edges = NULL;
    pv_walk w2;
    ccb_str str;
    ccb_str dumped;
    ccb_tui_screen *scr = NULL;
    long maxw = 40;
    long nlines = 0;
    long linelen = 0;
    size_t i = 0;
    add_child(a, root, t);
    add_child(a, root, btn);
    screen = make_screen(a, "Home", root);
    edges = ccb_arena_alloc(a, sizeof(*edges));
    memset(edges, 0, sizeof(*edges));
    edges[0].from = ccb_arena_strdup(a, "Open");
    edges[0].target = ccb_arena_strdup(a, "Detail");
    screen->edges = edges;
    screen->nedges = 1;
    screen->capedges = 1;
    memset(&p2, 0, sizeof(p2));
    p2.arena = a;
    screens = ccb_arena_alloc(a, sizeof(*screens));
    screens[0] = screen;
    p2.screens = screens;
    p2.nscreens = 1;
    p2.frame_width = 40;
    p2.interior_cols = 38;
    p2.phone_dp_width = 390;
    ccb_diag_list_init(&p2.diags);
    pv_layout_screen(&p2, screen);
    CHECK(pv_walk_start(&w2, &p2, NULL) == 0);
    ccb_str_init(&str);
    pv_walk_render(&str, &w2, 40, 0, 0);
    for (i = 0; str.buf != NULL && str.buf[i] != '\0'; i++) {
      if (str.buf[i] == '\n') {
        nlines++;
        if (linelen > maxw) {
          maxw = linelen;
        }
        linelen = 0;
      } else {
        linelen++;
      }
    }
    CHECK(nlines > 0);
    scr = ccb_tui_screen_new((int)maxw, (int)nlines);
    CHECK(scr != NULL);
    pv_walk_build_frame(scr, &w2, 40, 0, 0);
    ccb_str_init(&dumped);
    ccb_tui_screen_dump(scr, &dumped);
    ccb_str_append(&dumped, "\n");
    CHECK_STR_EQ(dumped.buf != NULL ? dumped.buf : "",
      str.buf != NULL ? str.buf : "");
    ccb_str_free(&dumped);
    ccb_str_free(&str);
    ccb_tui_screen_free(scr);
    pv_walk_free(&w2);
    ccb_diag_list_free(&p2.diags);
  }

  ccb_arena_destroy(arena);
  TEST_MAIN_END();
}
