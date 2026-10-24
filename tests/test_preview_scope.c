// Regression tests for the selector-scope merge fix: a per-screen
// file redeclaring a selector first seen elsewhere gets its own
// scoped rule carrying the merged properties, while the earlier rule
// stays untouched for other screens. (Mutating the single merged
// rule's scope would drop the earlier file's properties for every
// other screen — e.g. Form.css redeclaring common's textfield would
// steal common's margin from Settings.)
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

static const pv_rule *find_rule(pv_rule *rules, size_t n, const char *sel,
    const char *scope) {
  size_t i = 0;
  for (i = 0; i < n; i++) {
    if (rules[i].selector != NULL && strcmp(rules[i].selector, sel) == 0) {
      if (scope == NULL && rules[i].scope == NULL) {
        return &rules[i];
      }
      if (scope != NULL && rules[i].scope != NULL &&
          strcmp(rules[i].scope, scope) == 0) {
        return &rules[i];
      }
    }
  }
  return NULL;
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

static pv_box *layout_text(ccb_arena *a, pv_rule *rules, size_t n,
    const char *screen_name, pv_project *p) {
  pv_box *root = NULL;
  pv_box *text = NULL;
  pv_screen *screen = NULL;
  setup_project(p, a, rules, n);
  root = new_box(a, "screen", NULL);
  text = new_box(a, "text", "Some text");
  add_child(a, root, text);
  screen = make_screen(a, screen_name, root);
  pv_layout_screen(p, screen);
  return text;
}

int main(void) {
  // --- common.css declares text; Home.css redeclares it. The parse
  // keeps the common rule (scope NULL, color only) and adds a Home
  // rule (scope Home, color + padding). Layout gives Home the
  // padding, and Other the common rule with no padding.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *common = NULL;
    const pv_rule *home = NULL;
    pv_project home_p;
    pv_project other_p;
    pv_box *home_text = NULL;
    pv_box *other_text = NULL;
    ccb_diag_list_init(&diags);
    CHECK(pv_css_parse(a, "text { color: #111111; }", "styles/common.css",
      0, &diags, &rules, &n) == CCB_OK);
    CHECK(pv_css_parse(a, "text { padding: 39px; }", "styles/Home.css",
      0, &diags, &rules, &n) == CCB_OK);
    CHECK(n == 2);
    CHECK(diags.count == 0);
    common = find_rule(rules, n, "text", NULL);
    home = find_rule(rules, n, "text", "Home");
    CHECK(common != NULL && common->scope == NULL);
    CHECK_STR_EQ(common->fg, "#111111");
    CHECK(!common->has_padding);
    CHECK(home != NULL);
    CHECK_STR_EQ(home->scope, "Home");
    CHECK_STR_EQ(home->fg, "#111111");
    CHECK(home->has_padding && home->padding[3] == 39);
    ccb_diag_list_free(&diags);
    home_text = layout_text(a, rules, n, "Home", &home_p);
    // 39px at 38 cols / 390 dp scales to 4 columns of padding.
    CHECK(home_text->pad_left_cols == 4);
    CHECK(home_text->rule != NULL);
    CHECK_STR_EQ(home_text->rule->fg, "#111111");
    ccb_diag_list_free(&home_p.diags);
    other_text = layout_text(a, rules, n, "Other", &other_p);
    CHECK(other_text->pad_left_cols == 0);
    CHECK(other_text->rule != NULL);
    CHECK_STR_EQ(other_text->rule->fg, "#111111");
    ccb_diag_list_free(&other_p.diags);
  }
  // --- Two blocks for the same selector in one file merge into one
  // rule (no copy), and two blocks in a per-screen file after a
  // common declaration merge into that file's single copy.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *button = NULL;
    const pv_rule *home = NULL;
    ccb_diag_list_init(&diags);
    CHECK(pv_css_parse(a,
      "button { color: #ffffff; }\nbutton { color: #000000; }",
      "a.css", 0, &diags, &rules, &n) == CCB_OK);
    CHECK(n == 1);
    button = find_rule(rules, n, "button", NULL);
    CHECK(button != NULL && button->scope == NULL);
    CHECK_STR_EQ(button->fg, "#000000");
    ccb_diag_list_free(&diags);
    ccb_diag_list_init(&diags);
    rules = NULL;
    n = 0;
    CHECK(pv_css_parse(a, "text { color: #111111; }", "common.css",
      0, &diags, &rules, &n) == CCB_OK);
    CHECK(pv_css_parse(a,
      "text { padding: 39px; }\ntext { font-size: 16px; }",
      "Home.css", 0, &diags, &rules, &n) == CCB_OK);
    CHECK(n == 2);
    home = find_rule(rules, n, "text", "Home");
    CHECK(home != NULL);
    CHECK(home->has_padding && home->padding[3] == 39);
    CHECK(home->has_font_size && home->font_size == 16);
    ccb_diag_list_free(&diags);
  }
  // --- A second per-screen file gets its own copy and does not
  // disturb the first screen's rule (the Empty/Settings column case
  // in the sample app): Empty keeps padding without Settings' gap,
  // Settings gets padding + gap.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *empty = NULL;
    const pv_rule *settings = NULL;
    pv_project empty_p;
    pv_project settings_p;
    pv_box *empty_col = NULL;
    pv_box *settings_col = NULL;
    pv_box *empty_root = NULL;
    pv_box *settings_root = NULL;
    pv_screen *empty_screen = NULL;
    pv_screen *settings_screen = NULL;
    ccb_diag_list_init(&diags);
    CHECK(pv_css_parse(a, "column { padding: 24px; }", "styles/Empty.css",
      0, &diags, &rules, &n) == CCB_OK);
    CHECK(pv_css_parse(a, "column { gap: 8px; }", "styles/Settings.css",
      0, &diags, &rules, &n) == CCB_OK);
    CHECK(n == 2);
    empty = find_rule(rules, n, "column", NULL);
    settings = find_rule(rules, n, "column", "Settings");
    CHECK(empty != NULL && empty->has_padding && !empty->has_gap);
    CHECK(settings != NULL && settings->has_padding && settings->has_gap);
    ccb_diag_list_free(&diags);
    setup_project(&empty_p, a, rules, n);
    empty_root = new_box(a, "screen", NULL);
    empty_col = new_box(a, "column", NULL);
    add_child(a, empty_root, empty_col);
    empty_screen = make_screen(a, "Empty", empty_root);
    pv_layout_screen(&empty_p, empty_screen);
    // 24px scales to 2 columns of padding, and no gap rule applies.
    CHECK(empty_col->pad_left_cols == 2);
    ccb_diag_list_free(&empty_p.diags);
    setup_project(&settings_p, a, rules, n);
    settings_root = new_box(a, "screen", NULL);
    settings_col = new_box(a, "column", NULL);
    add_child(a, settings_root, settings_col);
    settings_screen = make_screen(a, "Settings", settings_root);
    pv_layout_screen(&settings_p, settings_screen);
    CHECK(settings_col->pad_left_cols == 2);
    CHECK(settings_col->rule != NULL);
    CHECK(settings_col->rule->has_gap);
    ccb_diag_list_free(&settings_p.diags);
  }
  // --- Merging from common.css after a per-screen copy does not
  // un-scope it: common's later property lands on the common rule,
  // the Home copy keeps its scope.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    ccb_diag_list diags;
    pv_rule *rules = NULL;
    size_t n = 0;
    const pv_rule *home = NULL;
    const pv_rule *common = NULL;
    ccb_diag_list_init(&diags);
    CHECK(pv_css_parse(a, "text { color: #111111; }", "common.css",
      0, &diags, &rules, &n) == CCB_OK);
    CHECK(pv_css_parse(a, "text { padding: 39px; }", "Home.css",
      0, &diags, &rules, &n) == CCB_OK);
    CHECK(pv_css_parse(a, "text { font-size: 16px; }", "common.css",
      0, &diags, &rules, &n) == CCB_OK);
    home = find_rule(rules, n, "text", "Home");
    common = find_rule(rules, n, "text", NULL);
    CHECK(home != NULL);
    CHECK_STR_EQ(home->scope, "Home");
    CHECK(common != NULL);
    CHECK(common->has_font_size && common->font_size == 16);
    ccb_diag_list_free(&diags);
  }
  TEST_MAIN_END();
}
