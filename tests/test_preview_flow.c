// Tests for the Kotlin flow parser and logic tree renderer.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_diag.h"
#include "ccb_preview.h"
#include "ccb_str.h"

static long count_diag(ccb_diag_list *diags, const char *code) {
  long n = 0;
  for (size_t i = 0; i < diags->count; i++) {
    if (strcmp(diags->items[i].code, code) == 0) {
      n++;
    }
  }
  return n;
}

static void parse(const char *text, ccb_arena *arena, ccb_diag_list *diags,
    pv_screen ***screens, size_t *nscreens) {
  ccb_status st = pv_flow_parse(arena, text, "test.kt", diags, screens, nscreens);
  CHECK(st == CCB_OK);
}

static void render(pv_project *p, ccb_str *out) {
  pv_render_logic_tree(out, p);
  CHECK(out->buf != NULL);
}

int main(void) {
  // Sample Home + Detail: golden logic tree from the task spec.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    parse("fun Home() {\n"
      "  Column {\n"
      "    Text(\"My Notes\")\n"
      "    List(\"notes\")\n"
      "    Row {\n"
      "      Button(\"Open note\", onClick = { nav(\"Detail\") })\n"
      "      Button(\"New note\", onClick = { nav(\"Form\") })\n"
      "    }\n"
      "    Button(\"Settings\", onClick = { nav(\"Settings\") })\n"
      "  }\n"
      "}\n"
      "fun Detail() {\n"
      "  Column {\n"
      "    Text(\"Note detail\")\n"
      "    when (state) {\n"
      "      is Loading -> Text(\"Loading...\")\n"
      "      is Loaded -> Text(\"Note body\")\n"
      "      else -> Text(\"Empty\")\n"
      "    }\n"
      "    Button(\"Delete\", onClick = { nav(\"Empty\") })\n"
      "    Button(\"Back\", onClick = { nav(\"Home\") })\n"
      "  }\n"
      "}\n", arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 2);
    CHECK_STR_EQ(screens[0]->name, "Home");
    CHECK_STR_EQ(screens[0]->root->type, "screen");
    CHECK(screens[0]->root->nchildren == 1);
    CHECK_STR_EQ(screens[0]->root->children[0]->type, "column");
    CHECK(screens[0]->nedges == 3);
    CHECK_STR_EQ(screens[0]->edges[0].from, "Open note");
    CHECK_STR_EQ(screens[0]->edges[0].target, "Detail");
    CHECK(screens[1]->nedges == 2);
    pv_project p;
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.screens = screens;
    p.nscreens = nscreens;
    ccb_str out;
    ccb_str_init(&out);
    render(&p, &out);
    CHECK_STR_EQ(out.buf,
      "Home (entry)\n"
      "  -> Detail via \"Open note\"\n"
      "  -> Form via \"New note\"\n"
      "  -> Settings via \"Settings\"\n"
      "Detail\n"
      "  branches: when state [is Loading, is Loaded, else]\n"
      "  -> Empty via \"Delete\"\n"
      "  -> Home via \"Back\"\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
  }
  // Entry fallback: no Home screen, first screen is the entry.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    parse("fun Alpha() { Text(\"A\") } fun Beta() { Text(\"B\") }",
      arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 2);
    pv_project p;
    memset(&p, 0, sizeof(p));
    p.screens = screens;
    p.nscreens = nscreens;
    ccb_str out;
    ccb_str_init(&out);
    render(&p, &out);
    CHECK_STR_EQ(out.buf, "Alpha (entry)\nBeta\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
  }
  // if/else-if/else branches, navigateTo, unknown edges, dedup.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    parse("fun Settings() {\n"
      "  Column {\n"
      "    Text(\"Settings\")\n"
      "    if (darkMode) {\n"
      "      Text(\"Dark mode on\")\n"
      "    } else if (autoMode) {\n"
      "      Text(\"Auto\")\n"
      "    } else {\n"
      "      Text(\"Dark mode off\")\n"
      "    }\n"
      "    Button(\"Back\", onClick = { navigateTo(\"Home\") })\n"
      "    Button(\"Back\", onClick = { navigateTo(\"Home\") })\n"
      "    Button(\"More\", onClick = { nav(nextScreen) })\n"
      "    Button(onClick = { nav(\"Home\") })\n"
      "  }\n"
      "}\n", arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 1);
    CHECK(screens[0]->nedges == 3);
    CHECK_STR_EQ(screens[0]->edges[0].from, "Back");
    CHECK_STR_EQ(screens[0]->edges[0].target, "Home");
    CHECK(screens[0]->edges[1].unknown == 1);
    CHECK(screens[0]->edges[1].target == NULL);
    CHECK_STR_EQ(screens[0]->edges[1].from, "More");
    // Unlabeled button: the edge source is the component type.
    CHECK_STR_EQ(screens[0]->edges[2].from, "button");
    pv_project p;
    memset(&p, 0, sizeof(p));
    p.screens = screens;
    p.nscreens = nscreens;
    ccb_str out;
    ccb_str_init(&out);
    render(&p, &out);
    CHECK_STR_EQ(out.buf,
      "Settings (entry)\n"
      "  branches: if darkMode [then, else if autoMode, else]\n"
      "  -> Home via \"Back\"\n"
      "  -> unknown via \"More\"\n"
      "  -> Home via \"button\"\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
  }
  // Generic Capitalized box warns once per distinct name; lowercase
  // calls are ignored; component names match case-insensitively.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    parse("fun Home() {\n"
      "  COLUMN {\n"
      "    TEXT(\"Hi\")\n"
      "    CustomCard(\"Card\")\n"
      "    CustomCard(\"Again\")\n"
      "    helper(\"ignored\")\n"
      "  }\n"
      "}\n", arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 1);
    pv_box *col = screens[0]->root->children[0];
    CHECK_STR_EQ(col->type, "column");
    CHECK(col->nchildren == 3);
    CHECK_STR_EQ(col->children[0]->type, "text");
    CHECK_STR_EQ(col->children[1]->type, "customcard");
    CHECK_STR_EQ(col->children[1]->label, "Card");
    CHECK_STR_EQ(col->children[2]->type, "customcard");
    CHECK(count_diag(&diags, "CCB-P1012") == 1);
    ccb_diag_list_free(&diags);
  }
  // Comments, string escapes, and a when entry with a block.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    parse("// Text(\"Nope\")\n"
      "/* Text(\"Also nope\") */\n"
      "fun Home() {\n"
      "  Column {\n"
      "    Text(\"a \\\"b\\\" c\")\n"
      "    when (mode) {\n"
      "      is Edit -> {\n"
      "        Text(\"Editing\")\n"
      "        Button(\"Done\", onClick = { nav(\"Home\") })\n"
      "      }\n"
      "      else -> Text(\"View\")\n"
      "    }\n"
      "  }\n"
      "}\n", arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 1);
    pv_box *col = screens[0]->root->children[0];
    CHECK(col->nchildren == 2);
    CHECK_STR_EQ(col->children[0]->label, "a \"b\" c");
    pv_box *branch = col->children[1];
    CHECK(branch->is_branch == 1);
    CHECK_STR_EQ(branch->label, "when mode");
    CHECK(branch->nchildren == 2);
    CHECK_STR_EQ(branch->children[0]->label, "is Edit");
    CHECK(branch->children[0]->nchildren == 2);
    CHECK_STR_EQ(branch->children[1]->label, "else");
    CHECK(screens[0]->nedges == 1);
    ccb_diag_list_free(&diags);
  }
  // Unbalanced braces: CCB-P1010, partial results kept.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    parse("fun Broken() { Column { Text(\"Hi\")", arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 1);
    CHECK_STR_EQ(screens[0]->name, "Broken");
    CHECK(screens[0]->root->children[0]->nchildren == 1);
    CHECK_STR_EQ(screens[0]->root->children[0]->children[0]->label, "Hi");
    CHECK(count_diag(&diags, "CCB-P1010") == 1);
    ccb_diag_list_free(&diags);
  }
  // Nav at screen level falls back to the screen name; template and
  // empty nav destinations are unknown; garbage input never crashes.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    // The template and empty destinations are both (Solo, unknown),
    // so they deduplicate to a single unknown edge.
    parse("fun Solo() { nav(\"Other\") nav(\"Detail/$id\") nav() }",
      arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 1);
    CHECK(screens[0]->nedges == 2);
    CHECK_STR_EQ(screens[0]->edges[0].from, "Solo");
    CHECK_STR_EQ(screens[0]->edges[0].target, "Other");
    CHECK(screens[0]->edges[1].unknown == 1);
    CHECK(screens[0]->edges[1].target == NULL);
    ccb_diag_list_free(&diags);
    ccb_diag_list_init(&diags);
    screens = NULL;
    nscreens = 0;
    parse("} { { \"unterminated", arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 0);
    ccb_diag_list_free(&diags);
    ccb_diag_list_init(&diags);
    screens = NULL;
    nscreens = 0;
    parse("", arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 0);
    pv_project p;
    memset(&p, 0, sizeof(p));
    p.screens = screens;
    p.nscreens = nscreens;
    ccb_str out;
    ccb_str_init(&out);
    pv_render_logic_tree(&out, &p);
    CHECK(out.len == 0);
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
  }
  // Duplicate screen names are kept, both of them.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    parse("fun Home() { Text(\"One\") } fun Home() { Text(\"Two\") }",
      arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 2);
    CHECK_STR_EQ(screens[0]->root->children[0]->label, "One");
    CHECK_STR_EQ(screens[1]->root->children[0]->label, "Two");
    ccb_diag_list_free(&diags);
  }
  // Nav arguments: literal, named (string + numeric), dynamic,
  // mixed, template, and a dynamic target that still records args.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    pv_edge *e = NULL;
    parse("fun Home() {\n"
      "  Column {\n"
      "    Button(\"Lit\", onClick = { nav(\"Detail\", \"note-42\") })\n"
      "    Button(\"Named\", onClick = { nav(\"Detail\", id = \"note-42\") })\n"
      "    Button(\"Num\", onClick = { nav(\"Detail\", count = 3) })\n"
      "    Button(\"Dyn\", onClick = { nav(\"Detail\", currentId) })\n"
      "    Button(\"Mix\", onClick = { nav(\"Detail\", \"a\", other, id = \"x\") })\n"
      "    Button(\"Tpl\", onClick = { nav(\"Detail\", \"a/$id\") })\n"
      "    Button(\"DynTgt\", onClick = { nav(screen, \"x\") })\n"
      "  }\n"
      "}\n"
      "fun Detail() { Text(\"D\") }\n",
      arena, &diags, &screens, &nscreens);
    CHECK(nscreens == 2);
    CHECK(screens[0]->nedges == 7);
    e = &screens[0]->edges[0];
    CHECK_STR_EQ(e->target, "Detail");
    CHECK(e->nargs == 1 && e->args[0] != NULL);
    CHECK_STR_EQ(e->args[0], "note-42");
    CHECK(e->args_named[0] == 0);
    e = &screens[0]->edges[1];
    CHECK(e->nargs == 1 && e->args_named[0] == 1);
    CHECK_STR_EQ(e->args[0], "id = \"note-42\"");
    e = &screens[0]->edges[2];
    CHECK(e->nargs == 1 && e->args_named[0] == 1);
    CHECK_STR_EQ(e->args[0], "count = 3");
    e = &screens[0]->edges[3];
    CHECK(e->nargs == 1 && e->args[0] == NULL);
    e = &screens[0]->edges[4];
    CHECK(e->nargs == 3);
    CHECK_STR_EQ(e->args[0], "a");
    CHECK(e->args[1] == NULL);
    CHECK_STR_EQ(e->args[2], "id = \"x\"");
    CHECK(e->args_named[2] == 1);
    e = &screens[0]->edges[5];
    CHECK(e->nargs == 1 && e->args[0] == NULL);
    e = &screens[0]->edges[6];
    CHECK(e->target == NULL && e->unknown == 1);
    CHECK(e->nargs == 1);
    CHECK_STR_EQ(e->args[0], "x");
    ccb_diag_list_free(&diags);
  }
  // Dedup compares args: same from + target with different args is
  // two edges; identical args deduplicate to one.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    parse("fun Home() {\n"
      "  Column {\n"
      "    Button(\"Go\", onClick = { nav(\"Detail\") })\n"
      "    Button(\"Go\", onClick = { nav(\"Detail\", \"x\") })\n"
      "    Button(\"Go\", onClick = { nav(\"Detail\", \"x\") })\n"
      "    Button(\"Go\", onClick = { nav(\"Detail\", \"y\") })\n"
      "  }\n"
      "}\n"
      "fun Detail() { Text(\"D\") }\n",
      arena, &diags, &screens, &nscreens);
    CHECK(screens[0]->nedges == 3);
    CHECK(screens[0]->edges[0].nargs == 0);
    CHECK(screens[0]->edges[1].nargs == 1);
    CHECK_STR_EQ(screens[0]->edges[1].args[0], "x");
    CHECK_STR_EQ(screens[0]->edges[2].args[0], "y");
    ccb_diag_list_free(&diags);
  }
  // Tree format with and without args, and JSON args incl. nulls.
  {
    CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    pv_project p;
    ccb_str out;
    parse("fun Home() {\n"
      "  Column {\n"
      "    Button(\"Open\", onClick = { nav(\"Detail\", \"note-42\", other) })\n"
      "    Button(\"Named\", onClick = { nav(\"Detail\", id = \"note-42\") })\n"
      "    Button(\"Plain\", onClick = { nav(\"Detail\") })\n"
      "  }\n"
      "}\n"
      "fun Detail() { Text(\"D\") }\n",
      arena, &diags, &screens, &nscreens);
    memset(&p, 0, sizeof(p));
    p.screens = screens;
    p.nscreens = nscreens;
    ccb_str_init(&out);
    render(&p, &out);
    CHECK_STR_EQ(out.buf,
      "Home (entry)\n"
      "  -> Detail(\"note-42\", unknown) via \"Open\"\n"
      "  -> Detail(id = \"note-42\") via \"Named\"\n"
      "  -> Detail via \"Plain\"\n"
      "Detail\n");
    ccb_str_free(&out);
    ccb_str_init(&out);
    pv_project_json(&out, &p);
    CHECK(out.buf != NULL);
    if (out.buf != NULL) {
      CHECK(strstr(out.buf, "\"version\":2") != NULL);
      CHECK(strstr(out.buf,
        "\"args\":[\"note-42\",null]") != NULL);
      CHECK(strstr(out.buf,
        "\"args\":[\"id = \\\"note-42\\\"\"]") != NULL);
      CHECK(strstr(out.buf, "\"args\":[]") != NULL);
    }
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
  }
  TEST_MAIN_END();
}
