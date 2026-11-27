// Tests for real character widths in the preview renderer: wide
// CJK/emoji chars occupy 2 cells, ASCII 1, invalid UTF-8 bytes 1
// (as before). Wide chars are never painted partially, and the
// screen title is measured in cells, not codepoints.
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

int main(void) {
  // --- CJK text: 4 wide chars = 8 cells, padded to a 10-cell interior.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 10;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 12, 1, 0);
    CHECK_STR_EQ(out.buf,
      "+--- S ----+\n"
      "|\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C  |\n"
      "+----------+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- Emoji: "a" + U+1F600 + "b" = 4 cells, padded to 10.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 10;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "a\xF0\x9F\x98\x80" "b");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 12, 1, 0);
    CHECK_STR_EQ(out.buf,
      "+--- S ----+\n"
      "|a\xF0\x9F\x98\x80" "b      |\n"
      "+----------+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- Wide char straddling the clip limit is dropped, not split:
  // interior 5, text of 3 wide chars (6 cells) paints 2 chars + space.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 5;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xBD\xA0");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 7, 1, 0);
    CHECK_STR_EQ(out.buf,
      "+- S -+\n"
      "|\xE4\xBD\xA0\xE5\xA5\xBD |\n"
      "+-----+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- Invalid UTF-8 bytes stay width 1, as before.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 10;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "\xFF\xFE" "abc");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 12, 1, 0);
    CHECK_STR_EQ(out.buf,
      "+--- S ----+\n"
      "|\xFF\xFE" "abc     |\n"
      "+----------+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- Title measured in cells: "你好" is 4 cells, +2 spaces = 6,
  // so it centers in a 10-cell interior with 2 dashes each side.
  // The old codepoint count (2 + 2 = 4) would give 3 dashes each side.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 10;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    screen = make_screen(a, "\xE4\xBD\xA0\xE5\xA5\xBD", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 12, 1, 0);
    CHECK_STR_EQ(out.buf,
      "+-- \xE4\xBD\xA0\xE5\xA5\xBD --+\n"
      "+----------+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- Wide label in a button, and wide text triggering CCB-P1009
  // only when its cell width (not codepoint count) exceeds the box.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *btn = NULL;
    pv_box *wide = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    size_t i = 0;
    int p1009 = 0;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 10;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    col = new_box(a, "column", NULL);
    btn = new_box(a, "button", "\xE4\xBD\xA0\xE5\xA5\xBD");
    wide = new_box(a, "text", "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C\xE4\xBD\xA0\xE5\xA5\xBD");
    add_child(a, col, btn);
    add_child(a, col, wide);
    add_child(a, root, col);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 12, 1, 0);
    CHECK(ccb_test_contains(out.buf, "[ \xE4\xBD\xA0\xE5\xA5\xBD ]"));
    ccb_str_free(&out);
    for (i = 0; i < p.diags.count; i++) {
      if (strcmp(p.diags.items[i].code, "CCB-P1009") == 0) {
        p1009++;
      }
    }
    // 6 wide chars = 12 cells > 10-cell box: clipped. (A 6-codepoint
    // ASCII string would not be, but a 5-codepoint wide string at
    // 10 cells would also not be; the wide case is what matters.)
    CHECK(p1009 == 1);
    ccb_diag_list_free(&p.diags);
  }
  TEST_MAIN_END();
}
