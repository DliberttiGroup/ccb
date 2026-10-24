// Tests for preview walk mode (P3): the pv_walk state machine only,
// driven by scripted keys with no TTY and no terminal I/O. The
// terminal layer (pv_walk_run) is not exercised here: it refuses
// non-TTY stdio, so it cannot run inside this harness by design.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_preview_walk.h"
#include "ccb_diag.h"
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

  ccb_arena_destroy(arena);
  TEST_MAIN_END();
}
