// Tests for cross-module resource namespacing (ccb_resns): res-dir
// scanning (file + values entries, depth-1 only) and collision
// detection across sources in priority order. Pure unit tests, no
// toolchain.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_resns.h"
#include <stdlib.h>

static char BASE[256];
#define B(rel) ccb_test_path(BASE, rel)

static void write_text(const char *path, const char *text) {
  CHECK(ccb_write_file(path, text, strlen(text)) == CCB_OK);
}

static int has_entry(ccb_res_entry *e, size_t n, const char *type,
    const char *name) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(e[i].type, type) == 0 && strcmp(e[i].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static ccb_res_entry *mk_entries(const char *const *pairs, size_t npairs,
    size_t *n_out) {
  ccb_res_entry *e = ccb_xcalloc(npairs, sizeof(*e));
  for (size_t i = 0; i < npairs; i++) {
    const char *slash = strchr(pairs[i], '/');
    e[i].type = ccb_xstrndup(pairs[i], (size_t)(slash - pairs[i]));
    e[i].name = ccb_xstrdup(slash + 1);
  }
  *n_out = npairs;
  return e;
}

int main(void) {
  ccb_test_tmpdir(BASE, sizeof BASE, "resns");
  ccb_remove_recursive(BASE);

  // Scan: file resources, qualified dirs, values XML at depth 1.
  CHECK(ccb_mkdir_p(B("res/values")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("res/values-night")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("res/layout")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("res/drawable-hdpi")) == CCB_OK);
  CHECK(ccb_mkdir_p(B("res/xml")) == CCB_OK);
  write_text(B("res/values/strings.xml"),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- <string name=\"ghost\">not a resource</string> -->\n"
    "<resources>\n"
    "  <string name=\"app_name\">x</string>\n"
    "  <color name=\"accent\">#ff0000</color>\n"
    "  <style name=\"AppTheme\" parent=\"@android:style/Theme\">\n"
    "    <item name=\"colorPrimary\">#000000</item>\n"
    "  </style>\n"
    "  <string-array name=\"days\">\n"
    "    <item>Mon</item>\n"
    "  </string-array>\n"
    "</resources>\n");
  write_text(B("res/values-night/strings.xml"),
    "<resources>\n"
    "  <string name=\"night_only\">n</string>\n"
    "  <string name=\"app_name\">night x</string>\n"
    "</resources>\n");
  write_text(B("res/layout/activity_main.xml"), "<LinearLayout />\n");
  write_text(B("res/drawable-hdpi/icon.png"), "fake-png");
  write_text(B("res/xml/prefs.xml"), "<prefs />\n");
  ccb_res_entry *entries = NULL;
  size_t n = 0;
  CHECK(ccb_resns_scan_dir(B("res"), &entries, &n) == CCB_OK);
  CHECK(has_entry(entries, n, "string", "app_name"));
  CHECK(has_entry(entries, n, "color", "accent"));
  CHECK(has_entry(entries, n, "style", "AppTheme"));
  CHECK(has_entry(entries, n, "string-array", "days"));
  CHECK(has_entry(entries, n, "string", "night_only"));
  CHECK(has_entry(entries, n, "layout", "activity_main"));
  CHECK(has_entry(entries, n, "drawable", "icon"));
  CHECK(has_entry(entries, n, "xml", "prefs"));
  CHECK(!has_entry(entries, n, "string", "ghost"));
  CHECK(!has_entry(entries, n, "item", "colorPrimary"));
  // values + values-night define string/app_name twice in one source:
  // one entry (config variants are not a collision).
  {
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
      if (strcmp(entries[i].type, "string") == 0 &&
          strcmp(entries[i].name, "app_name") == 0) {
        count++;
      }
    }
    CHECK(count == 1);
  }
  ccb_resns_entries_free(entries, n);

  // Missing dir contributes nothing.
  entries = NULL;
  n = 0;
  CHECK(ccb_resns_scan_dir(B("nope"), &entries, &n) == CCB_OK);
  CHECK(n == 0);
  ccb_resns_entries_free(entries, n);

  // Collisions: app wins over libraries; first library wins over
  // later ones; disjoint sources never collide.
  {
    const char *app_p[] = { "string/a", "layout/m" };
    const char *lib1_p[] = { "string/a", "string/b" };
    const char *lib2_p[] = { "string/a", "string/c" };
    size_t na = 0;
    size_t n1 = 0;
    size_t n2 = 0;
    ccb_res_entry *app = mk_entries(app_p, 2, &na);
    ccb_res_entry *lib1 = mk_entries(lib1_p, 2, &n1);
    ccb_res_entry *lib2 = mk_entries(lib2_p, 2, &n2);
    char *names[] = { "app", "library one", "library two" };
    ccb_res_entry *srcs[] = { app, lib1, lib2 };
    size_t counts[] = { na, n1, n2 };
    ccb_res_collision *colls = NULL;
    size_t ncolls = 0;
    CHECK(ccb_resns_collisions(names, srcs, counts, 3, &colls,
      &ncolls) == CCB_OK);
    CHECK(ncolls == 1);
    if (ncolls == 1) {
      CHECK_STR_EQ(colls[0].type, "string");
      CHECK_STR_EQ(colls[0].name, "a");
      CHECK_STR_EQ(colls[0].winner, "app");
      CHECK(colls[0].nlosers == 2);
      if (colls[0].nlosers == 2) {
        CHECK_STR_EQ(colls[0].losers[0], "library one");
        CHECK_STR_EQ(colls[0].losers[1], "library two");
      }
    }
    ccb_resns_collisions_free(colls, ncolls);
    ccb_resns_entries_free(app, na);
    ccb_resns_entries_free(lib1, n1);
    ccb_resns_entries_free(lib2, n2);
  }
  {
    const char *lib1_p[] = { "string/x" };
    const char *lib2_p[] = { "string/x" };
    size_t n1 = 0;
    size_t n2 = 0;
    ccb_res_entry *lib1 = mk_entries(lib1_p, 1, &n1);
    ccb_res_entry *lib2 = mk_entries(lib2_p, 1, &n2);
    char *names[] = { "library one", "library two" };
    ccb_res_entry *srcs[] = { lib1, lib2 };
    size_t counts[] = { n1, n2 };
    ccb_res_collision *colls = NULL;
    size_t ncolls = 0;
    CHECK(ccb_resns_collisions(names, srcs, counts, 2, &colls,
      &ncolls) == CCB_OK);
    CHECK(ncolls == 1);
    if (ncolls == 1) {
      CHECK_STR_EQ(colls[0].winner, "library one");
      CHECK(colls[0].nlosers == 1);
      if (colls[0].nlosers == 1) {
        CHECK_STR_EQ(colls[0].losers[0], "library two");
      }
    }
    ccb_resns_collisions_free(colls, ncolls);
    ccb_resns_entries_free(lib1, n1);
    ccb_resns_entries_free(lib2, n2);
  }
  {
    const char *app_p[] = { "string/a" };
    const char *lib_p[] = { "string/b" };
    size_t na = 0;
    size_t nl = 0;
    ccb_res_entry *app = mk_entries(app_p, 1, &na);
    ccb_res_entry *lib = mk_entries(lib_p, 1, &nl);
    char *names[] = { "app", "library one" };
    ccb_res_entry *srcs[] = { app, lib };
    size_t counts[] = { na, nl };
    ccb_res_collision *colls = NULL;
    size_t ncolls = 0;
    CHECK(ccb_resns_collisions(names, srcs, counts, 2, &colls,
      &ncolls) == CCB_OK);
    CHECK(ncolls == 0);
    ccb_resns_collisions_free(colls, ncolls);
    ccb_resns_entries_free(app, na);
    ccb_resns_entries_free(lib, nl);
  }

  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
