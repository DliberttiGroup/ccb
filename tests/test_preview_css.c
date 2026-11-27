// Tests for the preview CSS parser, box layout, and wireframe renderer.
// All inputs are string literals, except the sample-app test, which
// reads the checked-in sample styles via a path derived from __FILE__.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_str.h"
#include <stdio.h>

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

static const pv_rule *find_rule(pv_rule *rules, size_t n, const char *sel) {
  size_t i = 0;
  for (i = 0; i < n; i++) {
    if (rules[i].selector != NULL && strcmp(rules[i].selector, sel) == 0) {
      return &rules[i];
    }
  }
  return NULL;
}

static int diag_count(ccb_diag_list *d, const char *code) {
  size_t i = 0;
  int n = 0;
  for (i = 0; i < d->count; i++) {
    if (strcmp(d->items[i].code, code) == 0) {
      n++;
    }
  }
  return n;
}

static pv_rule *parse_css(ccb_arena *a, const char *text, const char *file,
    int strict, ccb_diag_list *diags, pv_rule **rules_io, size_t *n_io) {
  CHECK(pv_css_parse(a, text, file, strict, diags, rules_io, n_io) == CCB_OK);
  return *rules_io;
}

static void setup_project(pv_project *p, ccb_arena *a, pv_rule *rules,
    size_t n) {
  memset(p, 0, sizeof(*p));
  p->arena = a;
  p->rules = rules;
  p->nrules = n;
  p->frame_width = 40;
  p->interior_cols = 38;
  p->phone_dp_width = 390;
  ccb_diag_list_init(&p->diags);
}

static char *read_sample_style(const char *rel) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  size_t dir_len = slash != NULL ? (size_t)(slash - file) : 0;
  static const char *suffixes[] = {
    "/../examples/preview-sample/",
    "/examples/preview-sample/",
  };
  size_t i = 0;
  char *out = NULL;
  ccb_str path;
  for (i = 0; i < 2; i++) {
    ccb_str_init(&path);
    if (dir_len > 0) {
      ccb_str_append_n(&path, file, dir_len);
    }
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, rel);
    out = ccb_test_try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
    if (out != NULL) {
      return out;
    }
  }
  ccb_str_init(&path);
  ccb_str_append(&path, "examples/preview-sample/");
  ccb_str_append(&path, rel);
  out = ccb_test_try_read(path.buf != NULL ? path.buf : "");
  ccb_str_free(&path);
  if (out != NULL) {
    return out;
  }
  ccb_str_init(&path);
  ccb_str_append(&path, "../examples/preview-sample/");
  ccb_str_append(&path, rel);
  out = ccb_test_try_read(path.buf != NULL ? path.buf : "");
  ccb_str_free(&path);
  return out;
}

int main(void) {
  // --- CSS parser: merging, shorthand, recorded-only font-size.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *button = NULL;
    const pv_rule *text = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a,
      "button { color: #ffffff; margin: 8px; }\n"
      "button { color: #000000; padding: 4px 8px; }\n"
      "text { font-size: 16px; }\n",
      "a.css", 0, &diags, &rules, &n);
    CHECK(n == 2);
    CHECK(diags.count == 0);
    button = find_rule(rules, n, "button");
    CHECK(button != NULL);
    CHECK_STR_EQ(button->fg, "#000000");
    CHECK(button->has_margin && button->margin[0] == 8 &&
      button->margin[1] == 8 && button->margin[2] == 8 &&
      button->margin[3] == 8);
    CHECK(button->has_padding && button->padding[0] == 4 &&
      button->padding[1] == 8 && button->padding[2] == 4 &&
      button->padding[3] == 8);
    CHECK(button->line == 1);
    text = find_rule(rules, n, "text");
    CHECK(text != NULL && text->has_font_size && text->font_size == 16);
    ccb_diag_list_free(&diags);
  }
  // --- Shorthand: 1, 2, 3, 4 values.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *r = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a,
      "a { padding: 1px; }\n"
      "b { padding: 1px 2px; }\n"
      "c { padding: 1px 2px 3px; }\n"
      "d { padding: 1px 2px 3px 4px; }\n",
      "b.css", 0, &diags, &rules, &n);
    CHECK(diags.count == 0);
    r = find_rule(rules, n, "a");
    CHECK(r != NULL && r->padding[0] == 1 && r->padding[1] == 1 &&
      r->padding[2] == 1 && r->padding[3] == 1);
    r = find_rule(rules, n, "b");
    CHECK(r != NULL && r->padding[0] == 1 && r->padding[1] == 2 &&
      r->padding[2] == 1 && r->padding[3] == 2);
    r = find_rule(rules, n, "c");
    CHECK(r != NULL && r->padding[0] == 1 && r->padding[1] == 2 &&
      r->padding[2] == 3 && r->padding[3] == 2);
    r = find_rule(rules, n, "d");
    CHECK(r != NULL && r->padding[0] == 1 && r->padding[1] == 2 &&
      r->padding[2] == 3 && r->padding[3] == 4);
    ccb_diag_list_free(&diags);
  }
  // --- Unknown property, unsupported value, file+line on diagnostics.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *r = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a,
      "button {\n"
      "  bogus-prop: 1;\n"
      "  display: grid;\n"
      "  width: 10em;\n"
      "  color: #111111;\n"
      "}\n",
      "test.css", 0, &diags, &rules, &n);
    CHECK(diag_count(&diags, "CCB-P1001") == 1);
    CHECK(diag_count(&diags, "CCB-P1014") == 2);
    CHECK(diags.count == 3);
    CHECK_STR_EQ(diags.items[0].file, "test.css");
    CHECK(diags.items[0].line == 2);
    r = find_rule(rules, n, "button");
    CHECK(r != NULL && !r->has_display && !r->has_width);
    CHECK_STR_EQ(r->fg, "#111111");
    ccb_diag_list_free(&diags);
  }
  // --- Strict mode still emits the same codes (severity is counted
  // by the caller; ccb_diag carries no severity field).
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    ccb_diag_list_init(&diags);
    parse_css(a, "text { nope: 1; }", "s.css", 1, &diags, &rules, &n);
    CHECK(diag_count(&diags, "CCB-P1001") == 1);
    ccb_diag_list_free(&diags);
  }
  // --- Parse errors keep what parsed; garbage never crashes.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *r = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "text { color: #222222; ", "u.css", 0, &diags, &rules, &n);
    CHECK(diag_count(&diags, "CCB-P1002") == 1);
    r = find_rule(rules, n, "text");
    CHECK(r != NULL && r->fg != NULL && strcmp(r->fg, "#222222") == 0);
    ccb_diag_list_free(&diags);
    ccb_diag_list_init(&diags);
    rules = NULL;
    n = 0;
    parse_css(a, "button { color red; margin: 8px; }", "m.css", 0, &diags,
      &rules, &n);
    CHECK(diag_count(&diags, "CCB-P1002") == 1);
    r = find_rule(rules, n, "button");
    CHECK(r != NULL && r->has_margin && r->margin[0] == 8);
    ccb_diag_list_free(&diags);
    ccb_diag_list_init(&diags);
    rules = NULL;
    n = 0;
    parse_css(a, "", "e.css", 0, &diags, &rules, &n);
    parse_css(a, "}{{{;}}", "g.css", 0, &diags, &rules, &n);
    parse_css(a, NULL, "n.css", 0, &diags, &rules, &n);
    CHECK(n == 0);
    ccb_diag_list_free(&diags);
  }
  // --- Layout: column stacking, vertical margins, px scaling.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *t = NULL;
    pv_box *b = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "screen { padding: 39px; }\nbutton { margin: 39px; }",
      "l.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    col = new_box(a, "column", NULL);
    t = new_box(a, "text", "Hi");
    b = new_box(a, "button", "Save");
    add_child(a, col, t);
    add_child(a, col, b);
    add_child(a, root, col);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    // 39px at 38 cols / 390 dp scales to 4.
    CHECK(root->x == 0 && root->y == 0 && root->w == 38);
    CHECK(col->x == 4 && col->y == 4 && col->w == 30);
    CHECK(t->x == 4 && t->y == 4 && t->w == 30 && t->h == 1);
    // Button: margin 4 top/bottom, full width minus margins (30-8).
    CHECK(b->x == 8 && b->y == 9 && b->w == 22 && b->h == 1);
    CHECK(col->h == 1 + 4 + 1 + 4);
    CHECK(root->h == 4 + col->h + 4);
    CHECK(diag_count(&p.diags, "CCB-P1003") == 0);
    CHECK(diag_count(&p.diags, "CCB-P1004") == 0);
    CHECK(diag_count(&p.diags, "CCB-P1009") == 0);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  // --- Layout: row fixed-first + equal auto share + gap; percent width.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *row = NULL;
    pv_box *fixed = NULL;
    pv_box *auto1 = NULL;
    pv_box *auto2 = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a,
      "fixed { width: 195px; }\nrow { gap: 39px; }\nwide { width: 50%; }",
      "r.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    row = new_box(a, "row", NULL);
    fixed = new_box(a, "fixed", "F");
    auto1 = new_box(a, "auto", "A");
    auto2 = new_box(a, "auto", "B");
    add_child(a, row, fixed);
    add_child(a, row, auto1);
    add_child(a, row, auto2);
    add_child(a, root, row);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    // Content 38, gap 4 spaces, fixed 19: remainder 38-19-8=11,
    // split 6/5 (first auto gets the extra column).
    CHECK(fixed->x == 0 && fixed->w == 19);
    CHECK(auto1->x == 23 && auto1->w == 6);
    CHECK(auto2->x == 33 && auto2->w == 5);
    CHECK(auto1->y == fixed->y && auto2->y == fixed->y);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
    // Percent width in a column: 50% of 38 = 19.
    {
      pv_project p2;
      pv_box *root2 = new_box(a, "screen", NULL);
      pv_box *wide = new_box(a, "wide", "W");
      pv_screen *s2 = NULL;
      setup_project(&p2, a, rules, n);
      add_child(a, root2, wide);
      s2 = make_screen(a, "S", root2);
      pv_layout_screen(&p2, s2);
      CHECK(wide->w == 19);
      ccb_diag_list_free(&p2.diags);
    }
  }
  // --- Layout: flex-direction row on a box; column gap rows.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *box = NULL;
    pv_box *t1 = NULL;
    pv_box *t2 = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "box { display: flex; flex-direction: row; }",
      "f.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    box = new_box(a, "box", NULL);
    t1 = new_box(a, "text", "A");
    t2 = new_box(a, "text", "B");
    add_child(a, box, t1);
    add_child(a, box, t2);
    add_child(a, root, box);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(t1->x == 0 && t1->w == 19 && t2->x == 19 && t2->w == 19);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *t1 = NULL;
    pv_box *t2 = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "column { gap: 39px; }", "g.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    col = new_box(a, "column", NULL);
    t1 = new_box(a, "text", "A");
    t2 = new_box(a, "text", "B");
    add_child(a, col, t1);
    add_child(a, col, t2);
    add_child(a, root, col);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(t1->y == 0 && t2->y == 5 && col->h == 6);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  // --- Layout: leaf heights, spacer rule height, absolute positioning.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *list = NULL;
    pv_box *img = NULL;
    pv_box *sp = NULL;
    pv_box *abs = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a,
      "spacer { height: 78px; }\n"
      "abs { position: absolute; left: 39px; top: 78px; }",
      "h.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    col = new_box(a, "column", NULL);
    list = new_box(a, "list", "notes");
    img = new_box(a, "image", NULL);
    sp = new_box(a, "spacer", NULL);
    abs = new_box(a, "abs", "Over");
    add_child(a, col, list);
    add_child(a, col, img);
    add_child(a, col, sp);
    add_child(a, col, abs);
    add_child(a, root, col);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(list->h == 4 && img->h == 3 && sp->h == 8);
    CHECK(abs->x == 4 && abs->y == 8);
    // Absolute boxes do not affect parent height: 4 + 3 + 8 = 15.
    CHECK(col->h == 15 && root->h == 15);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  // --- Layout diagnostics: overlap, off-screen, clip, each capped at 10.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_screen *screen = NULL;
    int i = 0;
    ccb_diag_list_init(&diags);
    parse_css(a, "text { position: absolute; left: 0px; top: 0px; }",
      "o.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    for (i = 0; i < 15; i++) {
      add_child(a, root, new_box(a, "text", "X"));
    }
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(diag_count(&p.diags, "CCB-P1003") == 10);
    CHECK(diag_count(&p.diags, "CCB-P1004") == 10);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *wide = NULL;
    pv_box *longtext = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "wide { width: 999px; }", "w.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    wide = new_box(a, "wide", NULL);
    longtext = new_box(a, "text",
      "this label is far too long for a thirty-eight column frame");
    add_child(a, root, wide);
    add_child(a, root, longtext);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(diag_count(&p.diags, "CCB-P1004") >= 1);
    CHECK(diag_count(&p.diags, "CCB-P1009") == 1);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  // --- Renderer: exact small frame, Unicode + ASCII, title overflow.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "Hi");
    add_child(a, root, t);
    screen = make_screen(a, "Home", root);
    p.interior_cols = 10;
    p.phone_dp_width = 390;
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 12, 0, 0);
    CHECK_STR_EQ(out.buf,
      "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80 Home \xE2\x94\x80\xE2\x94\x80\xE2\x94\x90\n"
      "\xE2\x94\x82Hi        \xE2\x94\x82\n"
      "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x98\n");
    CHECK(!ccb_test_contains(out.buf, "\x1b["));
    ccb_str_free(&out);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 12, 1, 0);
    CHECK_STR_EQ(out.buf, "+-- Home --+\n|Hi        |\n+----------+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- Renderer: empty screen, long title falls back to a plain border.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 8;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    screen = make_screen(a, "Empty", root);
    pv_layout_screen(&p, screen);
    CHECK(root->h == 0);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 10, 1, 0);
    CHECK_STR_EQ(out.buf, "+ Empty -+\n+--------+\n");
    ccb_str_free(&out);
    // Title longer than the interior: plain border, no name.
    screen->name = ccb_arena_strdup(a, "VeryLongScreenName");
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 10, 1, 0);
    CHECK_STR_EQ(out.buf, "+--------+\n+--------+\n");
    CHECK(!ccb_test_contains(out.buf, "VeryLong"));
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- Renderer: button / textfield / list / image / generic / branch.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *btn = NULL;
    pv_box *tf = NULL;
    pv_box *list = NULL;
    pv_box *img = NULL;
    pv_box *gen = NULL;
    pv_box *branch = NULL;
    pv_box *entry = NULL;
    pv_box *bt = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 38;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    col = new_box(a, "column", NULL);
    btn = new_box(a, "button", "Save");
    tf = new_box(a, "textfield", "Title");
    list = new_box(a, "list", "notes");
    img = new_box(a, "image", NULL);
    gen = new_box(a, "widget", NULL);
    branch = new_box(a, "branch", NULL);
    entry = new_box(a, "branch-entry", NULL);
    bt = new_box(a, "text", "Branch text");
    add_child(a, entry, bt);
    add_child(a, branch, entry);
    add_child(a, col, btn);
    add_child(a, col, tf);
    add_child(a, col, list);
    add_child(a, col, img);
    add_child(a, col, gen);
    add_child(a, col, branch);
    add_child(a, root, col);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 0, 0);
    CHECK(ccb_test_contains(out.buf, "[ Save ]"));
    CHECK(ccb_test_contains(out.buf, "[Title]"));
    CHECK(ccb_test_contains(out.buf, "\xE2\x80\xA2 item"));
    CHECK(ccb_test_contains(out.buf, "[ image ]"));
    CHECK(ccb_test_contains(out.buf, "[Widget]"));
    CHECK(ccb_test_contains(out.buf, "Branch text"));
    ccb_str_free(&out);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 1, 0);
    CHECK(ccb_test_contains(out.buf, "* item"));
    CHECK(!ccb_test_contains(out.buf, "\xE2\x80\xA2"));
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- Renderer: colors (ANSI 256) when enabled, none when disabled.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    ccb_diag_list_init(&diags);
    parse_css(a, "screen { background-color: #ffffff; }\ntext { color: red; }",
      "c.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "Hi");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 0, 1);
    // #ffffff -> 231; red (255,0,0) -> 196.
    CHECK(ccb_test_contains(out.buf, "48;5;231"));
    CHECK(ccb_test_contains(out.buf, "38;5;196"));
    ccb_str_free(&out);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 0, 0);
    CHECK(!ccb_test_contains(out.buf, "\x1b["));
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  // --- Sample app: Home screen from the checked-in styles.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *t = NULL;
    pv_box *list = NULL;
    pv_box *row = NULL;
    pv_box *b1 = NULL;
    pv_box *b2 = NULL;
    pv_box *settings = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    char *common = read_sample_style("styles/common.css");
    char *home = read_sample_style("styles/Home.css");
    CHECK(common != NULL);
    CHECK(home != NULL);
    ccb_diag_list_init(&diags);
    if (common != NULL) {
      parse_css(a, common, "common.css", 0, &diags, &rules, &n);
      free(common);
    }
    if (home != NULL) {
      parse_css(a, home, "Home.css", 0, &diags, &rules, &n);
      free(home);
    }
    CHECK(diags.count == 0);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    col = new_box(a, "column", NULL);
    t = new_box(a, "text", "My Notes");
    list = new_box(a, "list", "notes");
    row = new_box(a, "row", NULL);
    b1 = new_box(a, "button", "Open note");
    b2 = new_box(a, "button", "New note");
    settings = new_box(a, "button", "Settings");
    add_child(a, row, b1);
    add_child(a, row, b2);
    add_child(a, col, t);
    add_child(a, col, list);
    add_child(a, col, row);
    add_child(a, col, settings);
    add_child(a, root, col);
    screen = make_screen(a, "Home", root);
    pv_layout_screen(&p, screen);
    // Screen padding 16px -> 2; text 2 (16px line height), list 4,
    // row 6 (sum of two buttons with 1-row margins), settings 3;
    // root = 2+15+2 = 19.
    CHECK(t->x == 2 && t->y == 2 && t->w == 34 && t->h == 2);
    CHECK(list->h == 4);
    CHECK(row->y == 8 && row->h == 6);
    CHECK(b1->x == 3 && b1->w == 15 && b1->y == 9);
    CHECK(b2->x == 21 && b2->w == 14 && b2->y == 9);
    CHECK(settings->x == 3 && settings->y == 15 && settings->w == 32);
    CHECK(col->h == 15 && root->h == 19);
    CHECK(diag_count(&p.diags, "CCB-P1003") == 0);
    CHECK(diag_count(&p.diags, "CCB-P1004") == 0);
    CHECK(diag_count(&p.diags, "CCB-P1009") == 0);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 0, 0);
    CHECK(ccb_test_contains(out.buf, "My Notes"));
    CHECK(ccb_test_contains(out.buf, "[ Open note ]"));
    CHECK(ccb_test_contains(out.buf, "[ New note ]"));
    CHECK(ccb_test_contains(out.buf, "[ Settings ]"));
    CHECK(ccb_test_contains(out.buf, "\xE2\x80\xA2 item"));
    CHECK(!ccb_test_contains(out.buf, "\x1b["));
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  // --- New properties: text-align parse + render offsets, incl.
  // clamping when the string is wider than the box.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *r = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "text { text-align: center; }\n"
      "button { text-align: right; }",
      "ta.css", 0, &diags, &rules, &n);
    CHECK(diags.count == 0);
    r = find_rule(rules, n, "text");
    CHECK(r != NULL && r->has_text_align && r->text_align == 1);
    r = find_rule(rules, n, "button");
    CHECK(r != NULL && r->has_text_align && r->text_align == 2);
    ccb_diag_list_free(&diags);
    ccb_diag_list_init(&diags);
    rules = NULL;
    n = 0;
    parse_css(a, "text { text-align: justify; }", "tj.css", 0, &diags,
      &rules, &n);
    CHECK(diag_count(&diags, "CCB-P1014") == 1);
    r = find_rule(rules, n, "text");
    CHECK(r != NULL && !r->has_text_align);
    ccb_diag_list_free(&diags);
  }
  {
    // Center: "Hi" (2 cells) in 38 columns -> 17 leading spaces.
    // Right: 36 leading spaces. Left (default): no offset.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    ccb_diag_list_init(&diags);
    parse_css(a, "text { text-align: center; }", "c.css", 0, &diags,
      &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "Hi");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 1, 0);
    // (38 - 2) / 2 = 18 leading spaces.
    CHECK(ccb_test_contains(out.buf, "|                  Hi"));
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    ccb_diag_list_init(&diags);
    parse_css(a, "text { text-align: right; }", "r.css", 0, &diags,
      &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "Hi");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 1, 0);
    CHECK(ccb_test_contains(out.buf, "|                                    Hi"));
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  {
    // Clamping: a 14-cell label in a 9-column box starts at the
    // content edge and clips to 9 cells, centered or not.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    ccb_diag_list_init(&diags);
    parse_css(a, "text { width: 97px; text-align: center; }", "cl.css",
      0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "HelloWorldLong");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(t->w == 9);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 1, 0);
    CHECK(ccb_test_contains(out.buf, "|HelloWorl "));
    CHECK(!ccb_test_contains(out.buf, "HelloWorld"));
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  // --- New properties: border-width / border-color, outline + inset
  // + height growth, and the scaling-to-zero / zero cases.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *tf = NULL;
    pv_screen *screen = NULL;
    ccb_str out;
    ccb_diag_list_init(&diags);
    parse_css(a, "textfield { border-width: 195px; border-color: red; }",
      "b.css", 0, &diags, &rules, &n);
    CHECK(diags.count == 0);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    tf = new_box(a, "textfield", "Title");
    add_child(a, root, tf);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(tf->has_border == 1 && tf->h == 3 && tf->w == 38);
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 1, 0);
    // ASCII outline: top row, content row with side bars, bottom row.
    CHECK(ccb_test_contains(out.buf, "+--------------------------------------+\n"));
    CHECK(ccb_test_contains(out.buf, "||[Title]"));
    ccb_str_free(&out);
    // Border color, not the text color, drives the outline's ANSI
    // code when color is enabled (the rule sets no text color).
    ccb_str_init(&out);
    pv_render_screen(&out, screen, 40, 0, 1);
    CHECK(ccb_test_contains(out.buf, "38;5;196"));
    ccb_str_free(&out);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  {
    // Border on a container insets its children by 1 cell.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "column { border-width: 390px; }", "cb.css", 0, &diags,
      &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    col = new_box(a, "column", NULL);
    t = new_box(a, "text", "Hi");
    add_child(a, col, t);
    add_child(a, root, col);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(col->has_border == 1 && col->h == 3);
    CHECK(t->x == 1 && t->y == 1 && t->w == 36);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  {
    // border-width: 0, and a value scaling to 0 cells, draw nothing.
    // Negative and invalid border colors warn (CCB-P1014).
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *r = NULL;
    pv_project p;
    pv_box *root = NULL;
    pv_box *tf = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "textfield { border-width: 0; }", "z.css", 0, &diags,
      &rules, &n);
    r = find_rule(rules, n, "textfield");
    CHECK(r != NULL && r->has_border_width && r->border_width == 0);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    tf = new_box(a, "textfield", "Title");
    add_child(a, root, tf);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(tf->has_border == 0 && tf->h == 1);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
    ccb_diag_list_init(&diags);
    rules = NULL;
    n = 0;
    parse_css(a, "textfield { border-width: 1px; }", "s.css", 0, &diags,
      &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    tf = new_box(a, "textfield", "Title");
    add_child(a, root, tf);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(tf->has_border == 0);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
    ccb_diag_list_init(&diags);
    rules = NULL;
    n = 0;
    parse_css(a,
      "a { border-width: -1px; }\n"
      "b { border-color: #12; }\n"
      "c { border-color: @color/nope; }",
      "bad.css", 0, &diags, &rules, &n);
    CHECK(diag_count(&diags, "CCB-P1014") == 3);
    ccb_diag_list_free(&diags);
  }
  // --- New properties: flex-wrap on rows, and the column warning.
  {
    // Two 20-column fixed children in a 38-column row: nowrap keeps
    // the second at x=20 on row 0; wrap moves it to a new line.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *row = NULL;
    pv_box *c1 = NULL;
    pv_box *c2 = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a,
      "a { width: 205px; }\n"
      "b { width: 205px; }\n"
      "row { flex-wrap: wrap; }",
      "w.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    row = new_box(a, "row", NULL);
    c1 = new_box(a, "a", "A");
    c2 = new_box(a, "b", "B");
    add_child(a, row, c1);
    add_child(a, row, c2);
    add_child(a, root, row);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(c1->x == 0 && c1->y == 0 && c1->w == 20);
    CHECK(c2->x == 0 && c2->y == 1);
    // Total row height is unchanged by wrapping (sum of children).
    CHECK(row->h == 2);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  {
    // No-wrap control at a wide width: both children share row 0.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *row = NULL;
    pv_box *c1 = NULL;
    pv_box *c2 = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a,
      "a { width: 205px; }\n"
      "b { width: 205px; }\n"
      "row { flex-wrap: nowrap; }",
      "nw.css", 0, &diags, &rules, &n);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    row = new_box(a, "row", NULL);
    c1 = new_box(a, "a", "A");
    c2 = new_box(a, "b", "B");
    add_child(a, row, c1);
    add_child(a, row, c2);
    add_child(a, root, row);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(c2->x == 20 && c2->y == 0);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
  }
  {
    // flex-wrap on a column container warns (CCB-P1014) and is
    // ignored; an unknown flex-wrap value warns at parse time.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    pv_project p;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    ccb_diag_list_init(&diags);
    parse_css(a, "column { flex-wrap: wrap; }", "cw.css", 0, &diags,
      &rules, &n);
    CHECK(diags.count == 0);
    setup_project(&p, a, rules, n);
    root = new_box(a, "screen", NULL);
    col = new_box(a, "column", NULL);
    t = new_box(a, "text", "Hi");
    add_child(a, col, t);
    add_child(a, root, col);
    screen = make_screen(a, "S", root);
    pv_layout_screen(&p, screen);
    CHECK(diag_count(&p.diags, "CCB-P1014") == 1);
    CHECK(ccb_test_contains(p.diags.items[0].message, "column"));
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&p.diags);
    ccb_diag_list_init(&diags);
    rules = NULL;
    n = 0;
    parse_css(a, "row { flex-wrap: sometime; }", "fw.css", 0, &diags,
      &rules, &n);
    CHECK(diag_count(&diags, "CCB-P1014") == 1);
    ccb_diag_list_free(&diags);
  }
  // --- Deliberately NOT added: the rejected properties still warn
  // as unknown (CCB-P1001), they are not silently accepted.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    ccb_diag_list_init(&diags);
    parse_css(a,
      "row { justify-content: center; align-items: center; }\n"
      "text { border-radius: 4px; line-height: 20px; }\n"
      "box { border: 1px solid red; }",
      "rej.css", 0, &diags, &rules, &n);
    CHECK(diag_count(&diags, "CCB-P1001") == 5);
    ccb_diag_list_free(&diags);
  }
  // --- Invalid UTF-8 bytes, moved here from the corpus file so the
  // tree stays valid UTF-8 (text snapshots reject invalid bytes).
  // The exact bytes of the old tests/corpus/preview/css-garbage.css:
  // ASCII garbage plus FF FE 80 9F, which are never valid UTF-8.
  // The parser must survive them and parse deterministically.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    ccb_diag_list diags2;
    pv_rule *rules = NULL;
    pv_rule *rules2 = NULL;
    size_t n = 0;
    size_t n2 = 0;
    const char *garbage =
      "}{{{;}} \xff\xfe\x80\x9f !!! @@@ ### $$$ %%% ^^^ &&& ***\n";
    ccb_diag_list_init(&diags);
    parse_css(a, garbage, "garbage.css", 0, &diags, &rules, &n);
    ccb_diag_list_init(&diags2);
    parse_css(a, garbage, "garbage.css", 0, &diags2, &rules2, &n2);
    CHECK(n == n2);
    ccb_diag_list_free(&diags);
    ccb_diag_list_free(&diags2);
  }
  TEST_MAIN_END();
}
