// Tests for list rendering: a list with children renders one row
// per child (the child's label, or "item" when it has no label);
// a bare list with no children keeps the 4 placeholder rows.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_str.h"

static pv_box *new_box(ccb_arena *a, const char *type, const char *label) {
  pv_box *b = ccb_arena_alloc(a, sizeof(*b));
  memset(b, 0, sizeof(*b));
  b->type = type != NULL ? ccb_arena_strdup(a, type) : NULL;
  b->label = label != NULL ? ccb_arena_strdup(a, label) : NULL;
  return b;
}

static void add_child(ccb_arena *a, pv_box *parent, pv_box *child) {
  pv_box **arr = ccb_arena_alloc(a, (parent->nchildren + 1) * sizeof(pv_box *));
  if (parent->nchildren > 0) {
    memcpy(arr, parent->children, parent->nchildren * sizeof(pv_box *));
  }
  arr[parent->nchildren] = child;
  parent->children = arr;
  parent->nchildren++;
  parent->capchildren = parent->nchildren;
}

static pv_screen *make_screen(ccb_arena *a, const char *name, pv_box *root) {
  pv_screen *s = ccb_arena_alloc(a, sizeof(*s));
  memset(s, 0, sizeof(*s));
  s->name = ccb_arena_strdup(a, name);
  s->root = root;
  return s;
}

static int count_occurrences(const char *hay, const char *needle) {
  int n = 0;
  size_t m = strlen(needle);
  const char *p = hay;
  if (m == 0) {
    return 0;
  }
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += m;
  }
  return n;
}

static void render_screen(ccb_arena *a, pv_box *root, long width,
    ccb_str *out) {
  pv_project p;
  pv_screen *screen = NULL;
  memset(&p, 0, sizeof(p));
  ccb_diag_list_init(&p.diags);
  p.interior_cols = width - 2;
  p.phone_dp_width = 390;
  screen = make_screen(a, "S", root);
  pv_layout_screen(&p, screen);
  ccb_str_init(out);
  pv_render_screen(out, screen, width, 1, 0);
  ccb_diag_list_free(&p.diags);
}

int main(void) {
  // --- Bare list: height 4, exactly 4 placeholder rows.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_box *root = new_box(a, "screen", NULL);
    pv_box *list = new_box(a, "list", "notes");
    ccb_str out;
    add_child(a, root, list);
    render_screen(a, root, 40, &out);
    CHECK(list->h == 4);
    CHECK(count_occurrences(out.buf, "* item") == 4);
    ccb_str_free(&out);
  }
  // --- List with children: height = child count, one row per child;
  // an unlabeled child gets the "item" placeholder for its row.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_box *root = new_box(a, "screen", NULL);
    pv_box *col = new_box(a, "column", NULL);
    pv_box *list = new_box(a, "list", "notes");
    pv_box *after = new_box(a, "text", "After");
    ccb_str out;
    add_child(a, list, new_box(a, "text", "Alpha"));
    add_child(a, list, new_box(a, "text", "Beta"));
    add_child(a, list, new_box(a, "text", NULL));
    add_child(a, col, list);
    add_child(a, col, after);
    add_child(a, root, col);
    render_screen(a, root, 40, &out);
    CHECK(list->h == 3);
    CHECK(ccb_test_contains(out.buf, "* Alpha"));
    CHECK(ccb_test_contains(out.buf, "* Beta"));
    CHECK(count_occurrences(out.buf, "* item") == 1);
    // The box after the list sits exactly below its 3 rows.
    CHECK(after->y == 3);
    CHECK(col->h == 4);
    ccb_str_free(&out);
  }
  // --- The flow parser already carries List block children into the
  // box tree; rendering then uses them as rows (no flow change).
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_screen **screens = NULL;
    size_t n = 0;
    pv_box *list = NULL;
    pv_box *root = NULL;
    ccb_str out;
    ccb_diag_list_init(&diags);
    CHECK(pv_flow_parse(a,
      "fun Home() {\n"
      "  Column {\n"
      "    List(\"notes\") {\n"
      "      Text(\"Alpha\")\n"
      "      Text(\"Beta\")\n"
      "    }\n"
      "  }\n"
      "}\n",
      "Home.kt", &diags, &screens, &n) == CCB_OK);
    CHECK(n == 1);
    CHECK(screens[0]->root != NULL);
    root = screens[0]->root;
    if (root != NULL && root->nchildren > 0) {
      pv_box *col = root->children[0];
      if (col != NULL && col->nchildren > 0) {
        list = col->children[0];
      }
    }
    CHECK(list != NULL);
    if (list != NULL) {
      CHECK_STR_EQ(list->type, "list");
      CHECK(list->nchildren == 2);
      CHECK_STR_EQ(list->children[0]->label, "Alpha");
      CHECK_STR_EQ(list->children[1]->label, "Beta");
    }
    ccb_diag_list_free(&diags);
    // Render that parsed tree: 2 rows, no placeholders.
    {
      pv_project p;
      pv_screen *screen = NULL;
      memset(&p, 0, sizeof(p));
      ccb_diag_list_init(&p.diags);
      p.interior_cols = 38;
      p.phone_dp_width = 390;
      screen = make_screen(a, "Home", root);
      pv_layout_screen(&p, screen);
      CHECK(list->h == 2);
      ccb_str_init(&out);
      pv_render_screen(&out, screen, 40, 1, 0);
      CHECK(ccb_test_contains(out.buf, "* Alpha"));
      CHECK(ccb_test_contains(out.buf, "* Beta"));
      CHECK(count_occurrences(out.buf, "* item") == 0);
      ccb_str_free(&out);
      ccb_diag_list_free(&p.diags);
    }
  }
  // --- A 6-child list renders 6 rows (no 4-row cap for real rows).
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_box *root = new_box(a, "screen", NULL);
    pv_box *list = new_box(a, "list", "notes");
    ccb_str out;
    int i = 0;
    char label[16];
    for (i = 0; i < 6; i++) {
      // Labels are built with ccb_str to avoid sprintf.
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "Row");
      ccb_str_append_c(&s, (char)('0' + i));
      add_child(a, list, new_box(a, "text", s.buf));
      ccb_str_free(&s);
    }
    (void)label;
    add_child(a, root, list);
    render_screen(a, root, 40, &out);
    CHECK(list->h == 6);
    CHECK(ccb_test_contains(out.buf, "* Row0"));
    CHECK(ccb_test_contains(out.buf, "* Row5"));
    CHECK(count_occurrences(out.buf, "* Row") == 6);
    ccb_str_free(&out);
  }
  TEST_MAIN_END();
}
