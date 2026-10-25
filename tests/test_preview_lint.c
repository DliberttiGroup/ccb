// Tests for the preview lint diagnostics: CCB-P1017 (tap target
// too small) and CCB-P1018 (text contrast below threshold), plus
// the shared pv_color_rgb() helper. Layouts are built by hand like
// in test_preview_css.c, so each case controls box sizes and colors
// exactly.
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

int main(void) {
  // --- pv_color_rgb: hex (#rgb, #rrggbb), named (case-insensitive),
  // and the unresolvable cases (unknown name, @color literal, NULL).
  {
    long r = 0;
    long g = 0;
    long b = 0;
    CHECK(pv_color_rgb("#fff", &r, &g, &b) == 1 && r == 255 && g == 255 &&
      b == 255);
    CHECK(pv_color_rgb("#123456", &r, &g, &b) == 1 && r == 0x12 &&
      g == 0x34 && b == 0x56);
    CHECK(pv_color_rgb("red", &r, &g, &b) == 1 && r == 255 && g == 0 &&
      b == 0);
    CHECK(pv_color_rgb("RED", &r, &g, &b) == 1 && r == 255);
    CHECK(pv_color_rgb("bogus", &r, &g, &b) == 0);
    CHECK(pv_color_rgb("@color/missing", &r, &g, &b) == 0);
    CHECK(pv_color_rgb(NULL, &r, &g, &b) == 0);
    CHECK(pv_color_rgb("#12", &r, &g, &b) == 0);
  }
  // --- P1017: small width (4x1) warns, and the message names the
  // component, label, and size. A no-rule 38x1 button stays quiet
  // since Chunk C: its 1-row height is the unknown default, not a
  // measurement (precision rule).
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *child = layout_one(a, "button { width: 39px; }", "button",
      "Go", &p);
    CHECK(child->w == 4 && child->h == 1);
    CHECK(diag_count(&p.diags, "CCB-P1017") == 1);
    CHECK(diag_message_contains(&p, "CCB-P1017",
      "tap target too small: button \"Go\" is 4x1 cells "
      "(41dp x 10dp; minimum 8x2 cells)"));
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *child = layout_one(a, "", "button", "Save", &p);
    CHECK(child->w == 38 && child->h == 1);
    CHECK(diag_count(&p.diags, "CCB-P1017") == 0);
    ccb_diag_list_free(&p.diags);
  }
  {
    // Textfields count too, not just buttons.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a, "textfield { width: 39px; }", "textfield",
      "Title", &p);
    CHECK(diag_count(&p.diags, "CCB-P1017") == 1);
    ccb_diag_list_free(&p.diags);
  }
  // --- P1017: exact boundary (8x2) passes, and a large target
  // (38 wide, 2 tall via padding) passes. Text is never a tap
  // target, whatever its size.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *child = layout_one(a,
      "button { width: 78px; padding: 10px 0px 0px 0px; }", "button",
      "Ok", &p);
    CHECK(child->w == 8 && child->h == 2);
    CHECK(diag_count(&p.diags, "CCB-P1017") == 0);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *child = layout_one(a,
      "button { padding: 10px 0px 0px 0px; }", "button", "Ok", &p);
    CHECK(child->w == 38 && child->h == 2);
    CHECK(diag_count(&p.diags, "CCB-P1017") == 0);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a, "", "text", "Hi", &p);
    CHECK(diag_count(&p.diags, "CCB-P1017") == 0);
    ccb_diag_list_free(&p.diags);
  }
  // --- P1018: fail (#888888 on #ffffff, ~3.5:1), pass (#111111 on
  // #ffffff), and the message carries the ratio and threshold.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a,
      "screen { background-color: #ffffff; }\n"
      "text { color: #888888; }",
      "text", "Note detail", &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 1);
    CHECK(diag_message_contains(&p, "CCB-P1018", "below 4.5:1"));
    CHECK(diag_message_contains(&p, "CCB-P1018",
      "for text \"Note detail\""));
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a,
      "screen { background-color: #ffffff; }\n"
      "text { color: #111111; }",
      "text", "Note detail", &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 0);
    ccb_diag_list_free(&p.diags);
  }
  // --- P1018: large text (font-size >= 24px) passes at a ratio
  // between 3.0 and 4.5; font-size 23px still fails.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a,
      "screen { background-color: #ffffff; }\n"
      "text { color: #888888; font-size: 24px; }",
      "text", "Big", &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 0);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a,
      "screen { background-color: #ffffff; }\n"
      "text { color: #888888; font-size: 23px; }",
      "text", "Big", &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 1);
    CHECK(diag_message_contains(&p, "CCB-P1018", "below 4.5:1"));
    ccb_diag_list_free(&p.diags);
  }
  // --- P1018: missing foreground skips, missing background skips,
  // and an unresolved @color side skips (no invented value).
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a, "screen { background-color: #ffffff; }",
      "text", "No fg", &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 0);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a, "text { color: #888888; }", "text", "No bg",
      &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 0);
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a,
      "screen { background-color: @color/missing; }\n"
      "text { color: #888888; }",
      "text", "Unresolved bg", &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 0);
    ccb_diag_list_free(&p.diags);
  }
  // --- P1018: named colors resolve (red on white is 4.0:1, a fail);
  // the leaf background wins over the screen background.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a,
      "screen { background-color: white; }\n"
      "text { color: red; }",
      "text", "Red", &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 1);
    CHECK(diag_message_contains(&p, "CCB-P1018", "4.0:1"));
    ccb_diag_list_free(&p.diags);
  }
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a,
      "screen { background-color: #ffffff; }\n"
      "text { color: #888888; background-color: #000000; }",
      "text", "On black", &p);
    // #888888 on black is ~5.9:1: a pass, proving the leaf
    // background (not the screen's white) was used.
    CHECK(diag_count(&p.diags, "CCB-P1018") == 0);
    ccb_diag_list_free(&p.diags);
  }
  {
    // And the screen background is the fallback when the leaf has
    // none: #888888 on white fails here.
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    (void)layout_one(a,
      "screen { background-color: #ffffff; }\n"
      "text { color: #888888; }",
      "text", "On white", &p);
    CHECK(diag_count(&p.diags, "CCB-P1018") == 1);
    ccb_diag_list_free(&p.diags);
  }
  TEST_MAIN_END();
}
