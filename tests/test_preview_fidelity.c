// Tests for preview fidelity Part A (docs/fidelity-behavior-plan.md):
// F1 font-size line heights + width buckets, F2 DP reporting,
// F3 image declared heights. Layouts are built by hand like in
// test_preview_lint.c, so each case controls sizes exactly.
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

static int contains(const char *hay, const char *needle) {
  return hay != NULL && needle != NULL && strstr(hay, needle) != NULL;
}

// Parse CSS, lay out a screen whose root has one child of the given
// type/label, and return the laid-out child (project + diag list
// live in the caller's arena; the caller frees p->diags).
static pv_box *layout_one(ccb_arena *a, const char *css, const char *type,
    const char *label, pv_project *p) {
  ccb_diag_list diags;
  pv_rule *rules = NULL;
  size_t n = 0;
  pv_box *root = NULL;
  pv_box *child = NULL;
  pv_screen *screen = NULL;
  ccb_diag_list_init(&diags);
  CHECK(pv_css_parse(a, css, "t.css", 0, &diags, &rules, &n) == CCB_OK);
  ccb_diag_list_free(&diags);
  memset(p, 0, sizeof(*p));
  p->arena = a;
  p->rules = rules;
  p->nrules = n;
  p->frame_width = 40;
  p->interior_cols = 38;
  p->phone_dp_width = 390;
  ccb_diag_list_init(&p->diags);
  root = new_box(a, "screen", NULL);
  child = new_box(a, type, label);
  add_child(a, root, child);
  screen = make_screen(a, "S", root);
  pv_layout_screen(p, screen);
  return child;
}

// Like layout_one, but renders the laid-out screen as ASCII art.
static void render_one(ccb_arena *a, const char *css, const char *type,
    const char *label, pv_project *p, ccb_str *out) {
  ccb_diag_list diags;
  pv_rule *rules = NULL;
  size_t n = 0;
  pv_box *root = NULL;
  pv_box *child = NULL;
  pv_screen *screen = NULL;
  ccb_diag_list_init(&diags);
  CHECK(pv_css_parse(a, css, "t.css", 0, &diags, &rules, &n) == CCB_OK);
  ccb_diag_list_free(&diags);
  memset(p, 0, sizeof(*p));
  p->arena = a;
  p->rules = rules;
  p->nrules = n;
  p->frame_width = 12;
  p->interior_cols = 10;
  p->phone_dp_width = 390;
  ccb_diag_list_init(&p->diags);
  root = new_box(a, "screen", NULL);
  child = new_box(a, type, label);
  add_child(a, root, child);
  screen = make_screen(a, "Home", root);
  pv_layout_screen(p, screen);
  ccb_str_init(out);
  pv_render_screen(out, screen, 12, 1, 0);
}

static int diag_message_contains(pv_project *p, const char *code,
    const char *needle) {
  size_t i = 0;
  for (i = 0; i < p->diags.count; i++) {
    if (strcmp(p->diags.items[i].code, code) == 0 &&
        p->diags.items[i].message != NULL &&
        strstr(p->diags.items[i].message, needle) != NULL) {
      return 1;
    }
  }
  return 0;
}

int main(void) {
  // --- F1 line heights: round(font-size * 1.2 / 10) rows, min 1;
  // no font-size rule keeps the old 1-row base.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "text { font-size: 12px; }", "text", "Hi",
      &p);
    CHECK(c->h == 1);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "text { font-size: 14px; }", "text", "Hi",
      &p);
    CHECK(c->h == 2);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "text { font-size: 16px; }", "text", "Hi",
      &p);
    CHECK(c->h == 2);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "text { font-size: 24px; }", "text", "Hi",
      &p);
    CHECK(c->h == 3);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "text { font-size: 32px; }", "text", "Hi",
      &p);
    CHECK(c->h == 4);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "", "text", "Hi", &p);
    CHECK(c->h == 1);
    ccb_diag_list_free(&p.diags);
  }
  {
    // Buttons and textfields use the same line height.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "button { font-size: 24px; }", "button",
      "Go", &p);
    CHECK(c->h == 3);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "textfield { font-size: 12px; }",
      "textfield", "Title", &p);
    CHECK(c->h == 1);
    ccb_diag_list_free(&p.diags);
  }
  {
    // Line height plus vertical padding: 16px (2 rows) + 10px top
    // padding (1 row) = 3 rows.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a,
      "text { font-size: 16px; padding: 10px 0px 0px 0px; }", "text",
      "Hi", &p);
    CHECK(c->h == 3);
    ccb_diag_list_free(&p.diags);
  }
  {
    // Generic/unknown leaves do not scale: only text, button, and
    // textfield take the line height.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "widget { font-size: 24px; }", "widget",
      NULL, &p);
    CHECK(c->h == 1);
    ccb_diag_list_free(&p.diags);
  }
  // --- F1 width buckets, via pv_scaled_text_width directly.
  {
    pv_rule r;
    memset(&r, 0, sizeof(r));
    CHECK(pv_scaled_text_width(10, NULL) == 10);
    CHECK(pv_scaled_text_width(10, &r) == 10); // no font-size: base
    CHECK(pv_scaled_text_width(0, &r) == 0);
    r.has_font_size = 1;
    r.font_size = 13; // small boundary
    CHECK(pv_scaled_text_width(10, &r) == 8);
    r.font_size = 12;
    CHECK(pv_scaled_text_width(10, &r) == 8);
    CHECK(pv_scaled_text_width(9, &r) == 8); // ceil(7.2)
    CHECK(pv_scaled_text_width(1, &r) == 1); // ceil(0.8)
    r.font_size = 14; // base boundary
    CHECK(pv_scaled_text_width(10, &r) == 10);
    r.font_size = 23;
    CHECK(pv_scaled_text_width(10, &r) == 10);
    r.font_size = 24; // large boundary
    CHECK(pv_scaled_text_width(10, &r) == 13); // ceil(12.5)
    CHECK(pv_scaled_text_width(8, &r) == 10);
    CHECK(pv_scaled_text_width(2, &r) == 3); // ceil(2.5)
  }
  // --- F1 alignment: centered text aligns on the scaled width.
  {
    // 24px "Hi": scaled width 3, offset (10-3)/2 = 3, 3 rows tall.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    ccb_str out;
    render_one(a, "text { text-align: center; font-size: 24px; }",
      "text", "Hi", &p, &out);
    CHECK_STR_EQ(out.buf, "+-- Home --+\n|   Hi     |\n|          |\n"
      "|          |\n+----------+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  {
    // 12px "Hello": scaled width 4, offset (10-4)/2 = 3, 1 row tall.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    ccb_str out;
    render_one(a, "text { text-align: center; font-size: 12px; }",
      "text", "Hello", &p, &out);
    CHECK_STR_EQ(out.buf, "+-- Home --+\n|   Hello  |\n+----------+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  {
    // No font-size: unscaled, offset (10-2)/2 = 4, 1 row tall.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    ccb_str out;
    render_one(a, "text { text-align: center; }", "text", "Hi", &p,
      &out);
    CHECK_STR_EQ(out.buf, "+-- Home --+\n|    Hi    |\n+----------+\n");
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- F2 DP mapping helpers.
  {
    CHECK(pv_cols_to_dp(38, 38, 390) == 390);
    CHECK(pv_cols_to_dp(34, 38, 390) == 349);
    CHECK(pv_cols_to_dp(8, 38, 390) == 82);
    CHECK(pv_cols_to_dp(5, 38, 390) == 51);
    CHECK(pv_cols_to_dp(4, 38, 390) == 41);
    CHECK(pv_cols_to_dp(0, 38, 390) == 0);
    CHECK(pv_cols_to_dp(10, 0, 390) == 0);
    CHECK(pv_cols_to_dp(10, 38, 0) == 0);
    CHECK(pv_rows_to_dp(2) == 20);
    CHECK(pv_rows_to_dp(1) == 10);
    CHECK(pv_rows_to_dp(0) == 0);
  }
  // --- F2 JSON: every box carries w_dp/h_dp; version stays 2.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *root = NULL;
    pv_box *t = NULL;
    pv_screen *screen = NULL;
    pv_screen **screens = NULL;
    ccb_str out;
    memset(&p, 0, sizeof(p));
    ccb_diag_list_init(&p.diags);
    p.interior_cols = 38;
    p.phone_dp_width = 390;
    root = new_box(a, "screen", NULL);
    t = new_box(a, "text", "Hi");
    add_child(a, root, t);
    screen = make_screen(a, "S", root);
    screens = ccb_arena_alloc(a, sizeof(pv_screen *));
    screens[0] = screen;
    p.screens = screens;
    p.nscreens = 1;
    root->x = 0;
    root->y = 0;
    root->w = 38;
    root->h = 2;
    t->x = 0;
    t->y = 0;
    t->w = 38;
    t->h = 2;
    ccb_str_init(&out);
    pv_project_json(&out, &p);
    CHECK(contains(out.buf, "\"version\":2"));
    CHECK(contains(out.buf,
      "\"type\":\"screen\",\"label\":null,\"x\":0,\"y\":0,\"w\":38,\"h\":2,"
      "\"w_dp\":390,\"h_dp\":20"));
    CHECK(contains(out.buf,
      "\"type\":\"text\",\"label\":\"Hi\",\"x\":0,\"y\":0,\"w\":38,\"h\":2,"
      "\"w_dp\":390,\"h_dp\":20"));
    ccb_str_free(&out);
    ccb_diag_list_free(&p.diags);
  }
  // --- F2 P1017: thresholds stay in cells; the message reports DP.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a, "button { width: 39px; }", "button", "Go", &p);
    CHECK(diag_message_contains(&p, "CCB-P1017",
      "tap target too small: button \"Go\" is 4x1 cells "
      "(41dp x 10dp; minimum 8x2 cells)"));
    ccb_diag_list_free(&p.diags);
  }
  // --- F3 image heights: declared height honored, else placeholder.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "image { height: 100px; }", "image",
      NULL, &p);
    CHECK(c->h == 10);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "", "image", NULL, &p);
    CHECK(c->h == 3);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "image { height: 50%; }", "image", NULL,
      &p);
    CHECK(c->h == 19);
    ccb_diag_list_free(&p.diags);
  }
  {
    // Declared width already honored; height stays the placeholder.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a, "image { width: 195px; }", "image", NULL,
      &p);
    CHECK(c->w == 19 && c->h == 3);
    ccb_diag_list_free(&p.diags);
  }
  {
    // Declared height plus vertical padding.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *c = layout_one(a,
      "image { height: 100px; padding: 10px 0px 0px 0px; }", "image",
      NULL, &p);
    CHECK(c->h == 11);
    ccb_diag_list_free(&p.diags);
  }
  TEST_MAIN_END();
}
