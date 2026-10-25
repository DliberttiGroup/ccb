// Tests for preview orchestration: golden renders + JSON for the
// sample app, diagnostics fixtures, the never-crash corpus, and the
// CSS scope rule. All paths derive from __FILE__; the tests never
// depend on the working directory.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_config.h"
#include "ccb_str.h"
#include <stdio.h>

// ---------- path + file helpers ----------

// Directory containing this test file, without a trailing slash.
// Falls back to "." when __FILE__ has no directory part.
static void test_dir(ccb_str *out) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  if (slash != NULL && slash != file) {
    ccb_str_append_n(out, file, (size_t)(slash - file));
  } else {
    ccb_str_append(out, ".");
  }
}

// Read a whole file, or NULL when it cannot be read.
static char *try_read(const char *path) {
  FILE *f = fopen(path, "rb");
  long size = 0;
  char *buf = NULL;
  if (f == NULL) {
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  buf = ccb_xmalloc((size_t)size + 1);
  if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[size] = '\0';
  fclose(f);
  return buf;
}

// First candidate path (from a list of suffixes) that reads, or NULL.
static char *read_first(const char *suffixes[], size_t n, const char *rel) {
  size_t i = 0;
  ccb_str dir;
  ccb_str_init(&dir);
  test_dir(&dir);
  for (i = 0; i < n; i++) {
    ccb_str path;
    char *out = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, rel);
    out = try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
    if (out != NULL) {
      ccb_str_free(&dir);
      return out;
    }
  }
  ccb_str_free(&dir);
  return NULL;
}

static char *read_sample_toml(void) {
  static const char *suffixes[] = {
    "/../examples/preview-sample/",
    "/examples/preview-sample/",
  };
  char *out = read_first(suffixes, 2, "ccb.toml");
  if (out != NULL) {
    return out;
  }
  out = try_read("examples/preview-sample/ccb.toml");
  if (out != NULL) {
    return out;
  }
  return try_read("../examples/preview-sample/ccb.toml");
}

// The sample toml path itself (for ccb_config_load, which resolves
// preview dirs relative to the toml's directory).
static char *sample_toml_path(void) {
  static const char *suffixes[] = {
    "/../examples/preview-sample/",
    "/examples/preview-sample/",
  };
  size_t i = 0;
  ccb_str dir;
  ccb_str_init(&dir);
  test_dir(&dir);
  for (i = 0; i < 2; i++) {
    ccb_str path;
    FILE *f = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, "ccb.toml");
    f = fopen(path.buf != NULL ? path.buf : "", "rb");
    if (f != NULL) {
      char *out = NULL;
      fclose(f);
      out = ccb_xstrdup(path.buf);
      ccb_str_free(&path);
      ccb_str_free(&dir);
      return out;
    }
    ccb_str_free(&path);
  }
  ccb_str_free(&dir);
  {
    char *probe = try_read("examples/preview-sample/ccb.toml");
    if (probe != NULL) {
      free(probe);
      return ccb_xstrdup("examples/preview-sample/ccb.toml");
    }
  }
  return ccb_xstrdup("../examples/preview-sample/ccb.toml");
}

static char *read_golden(const char *rel) {
  static const char *suffixes[] = {
    "/golden/preview/",
    "/../tests/golden/preview/",
  };
  char *out = read_first(suffixes, 2, rel);
  if (out != NULL) {
    return out;
  }
  {
    ccb_str path;
    ccb_str_init(&path);
    ccb_str_append(&path, "tests/golden/preview/");
    ccb_str_append(&path, rel);
    out = try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
  }
  if (out != NULL) {
    return out;
  }
  {
    ccb_str path;
    ccb_str_init(&path);
    ccb_str_append(&path, "../tests/golden/preview/");
    ccb_str_append(&path, rel);
    out = try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
  }
  return out;
}

static char *fixture_toml_path(const char *name) {
  static const char *suffixes[] = {
    "/fixtures/preview/",
    "/../tests/fixtures/preview/",
  };
  size_t i = 0;
  ccb_str dir;
  ccb_str_init(&dir);
  test_dir(&dir);
  for (i = 0; i < 2; i++) {
    ccb_str path;
    FILE *f = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, name);
    ccb_str_append(&path, "/ccb.toml");
    f = fopen(path.buf != NULL ? path.buf : "", "rb");
    if (f != NULL) {
      char *out = NULL;
      fclose(f);
      out = ccb_xstrdup(path.buf);
      ccb_str_free(&path);
      ccb_str_free(&dir);
      return out;
    }
    ccb_str_free(&path);
  }
  ccb_str_free(&dir);
  {
    ccb_str path;
    ccb_str_init(&path);
    ccb_str_append(&path, "tests/fixtures/preview/");
    ccb_str_append(&path, name);
    ccb_str_append(&path, "/ccb.toml");
    {
      char *probe = try_read(path.buf != NULL ? path.buf : "");
      if (probe != NULL) {
        char *out = NULL;
        free(probe);
        out = ccb_xstrdup(path.buf);
        ccb_str_free(&path);
        return out;
      }
    }
    ccb_str_free(&path);
  }
  {
    ccb_str path;
    ccb_str_init(&path);
    ccb_str_append(&path, "../tests/fixtures/preview/");
    ccb_str_append(&path, name);
    ccb_str_append(&path, "/ccb.toml");
    {
      char *out = ccb_xstrdup(path.buf);
      ccb_str_free(&path);
      return out;
    }
  }
}

// ---------- project loading ----------

// Load a project from a toml path. The config is freed inside; the
// project owns its arena + diag list and is freed with pv_project_free.
static pv_project *load_project(const char *toml_path, long width, int ascii,
    int strict) {
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  pv_project *p = NULL;
  ccb_status rc = CCB_OK;
  ccb_diag_list_init(&diags);
  if (ccb_config_load(toml_path, NULL, &cfg, &diags) != CCB_OK) {
    size_t i = 0;
    fprintf(stderr, "config load failed for %s:\n", toml_path);
    for (i = 0; i < diags.count; i++) {
      fprintf(stderr, "  %s: %s\n", diags.items[i].code,
        diags.items[i].message);
    }
    ccb_diag_list_free(&diags);
    return NULL;
  }
  ccb_diag_list_free(&diags);
  rc = pv_project_load(cfg, width, ascii, strict, NULL, &p);
  ccb_config_free(cfg);
  if (rc != CCB_OK || p == NULL) {
    fprintf(stderr, "project load failed for %s (status %d)\n",
      toml_path, (int)rc);
    return NULL;
  }
  return p;
}

static int diag_count(pv_project *p, const char *code) {
  size_t i = 0;
  int n = 0;
  for (i = 0; i < p->diags.count; i++) {
    if (strcmp(p->diags.items[i].code, code) == 0) {
      n++;
    }
  }
  return n;
}

static int diag_present(pv_project *p, const char *code) {
  return diag_count(p, code) > 0;
}

static pv_screen *find_screen(pv_project *p, const char *name) {
  size_t i = 0;
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, name) == 0) {
      return p->screens[i];
    }
  }
  return NULL;
}

// First box of a given type in a tree, depth-first, or NULL.
static pv_box *find_box(pv_box *box, const char *type) {
  size_t i = 0;
  if (box == NULL) {
    return NULL;
  }
  if (box->type != NULL && strcmp(box->type, type) == 0) {
    return box;
  }
  for (i = 0; i < box->nchildren; i++) {
    pv_box *found = find_box(box->children[i], type);
    if (found != NULL) {
      return found;
    }
  }
  return NULL;
}

// Byte-compare actual against expected; on mismatch print the first
// differing line to stderr, then fail the check.
static void check_golden(const char *label, const char *actual,
    const char *expected) {
  if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
    const char *a = actual != NULL ? actual : "";
    const char *e = expected != NULL ? expected : "";
    size_t line = 1;
    size_t ai = 0;
    size_t ei = 0;
    while (a[ai] != '\0' && e[ei] != '\0' && a[ai] == e[ei]) {
      if (a[ai] == '\n') {
        line++;
      }
      ai++;
      ei++;
    }
    fprintf(stderr, "FAIL golden %s: first differing line %zu\n", label,
      line);
    {
      const char *a_end = strchr(a + ai, '\n');
      const char *e_end = strchr(e + ei, '\n');
      size_t a_len = a_end != NULL ? (size_t)(a_end - (a + ai)) :
        strlen(a + ai);
      size_t e_len = e_end != NULL ? (size_t)(e_end - (e + ei)) :
        strlen(e + ei);
      fprintf(stderr, "  actual:   %.*s\n", (int)a_len, a + ai);
      fprintf(stderr, "  expected: %.*s\n", (int)e_len, e + ei);
    }
    CHECK(0);
    return;
  }
  CHECK(1);
}

static void check_golden_file(const char *label, const char *actual,
    const char *golden_rel) {
  char *expected = read_golden(golden_rel);
  CHECK(expected != NULL);
  if (expected == NULL) {
    fprintf(stderr, "FAIL golden %s: cannot read %s\n", label, golden_rel);
    return;
  }
  check_golden(label, actual, expected);
  free(expected);
}

int main(void) {
  // --- Golden: sample app renders, logic tree, and JSON.
  {
    char *toml = sample_toml_path();
    pv_project *p = load_project(toml, 40, 0, 0);
    long errors = 0;
    size_t i = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      // Since Chunk C, no P1017 fires on the sample: its
      // buttons/textfields carry no font-size rules, so their
      // 1-row heights are the unknown default, not a measurement,
      // and every width is >= 8. No other diagnostic fires either.
      CHECK(diag_count(p, "CCB-P1017") == 0);
      CHECK((long)p->diags.count == diag_count(p, "CCB-P1017"));
      CHECK(p->nscreens == 5);
      for (i = 0; i < p->nscreens; i++) {
        ccb_str out;
        ccb_str golden;
        ccb_str_init(&out);
        pv_render_screen(&out, p->screens[i], p->frame_width, 0, 0);
        ccb_str_init(&golden);
        ccb_str_append(&golden, p->screens[i]->name);
        ccb_str_append(&golden, ".txt");
        check_golden_file(p->screens[i]->name,
          out.buf != NULL ? out.buf : "", golden.buf);
        ccb_str_free(&golden);
        ccb_str_free(&out);
      }
      {
        ccb_str out;
        ccb_str_init(&out);
        pv_render_logic_tree(&out, p);
        check_golden_file("logic-tree", out.buf != NULL ? out.buf : "",
          "logic-tree.txt");
        ccb_str_free(&out);
      }
      {
        ccb_str out;
        ccb_str out2;
        ccb_str_init(&out);
        pv_project_json(&out, p);
        check_golden_file("preview.json", out.buf != NULL ? out.buf : "",
          "preview.json");
        // Deterministic: a second render is byte-identical.
        ccb_str_init(&out2);
        pv_project_json(&out2, p);
        CHECK_STR_EQ(out.buf != NULL ? out.buf : "",
          out2.buf != NULL ? out2.buf : "");
        ccb_str_free(&out2);
        ccb_str_free(&out);
      }
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Golden helper sanity: the sample toml is readable at all.
  {
    char *text = read_sample_toml();
    CHECK(text != NULL);
    free(text);
  }
  // --- Width handling: 0 means the default, small widths clamp.
  {
    char *toml = sample_toml_path();
    pv_project *p = load_project(toml, 0, 0, 0);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(p->frame_width == 40);
      CHECK(p->interior_cols == 38);
      pv_project_free(p);
    }
    p = load_project(toml, 10, 0, 0);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(p->frame_width == 24);
      CHECK(diag_present(p, "CCB-P1013"));
      pv_project_free(p);
    }
    free(toml);
  }
  // --- No usable [preview] section is a config error from load.
  {
    char *toml = fixture_toml_path("no-preview");
    ccb_diag_list diags;
    ccb_config *cfg = NULL;
    pv_project *p = NULL;
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(toml, NULL, &cfg, &diags) == CCB_OK);
    ccb_diag_list_free(&diags);
    if (cfg != NULL) {
      CHECK(pv_project_load(cfg, 40, 0, 0, NULL, &p) == CCB_ERR_CONFIG);
      CHECK(p == NULL);
      ccb_config_free(cfg);
    }
    free(toml);
  }
  // --- Diagnostics fixtures.
  {
    char *toml = fixture_toml_path("missing-target");
    pv_project *p = load_project(toml, 40, 0, 0);
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors > 0);
      CHECK(diag_present(p, "CCB-P1005"));
      pv_project_free(p);
    }
    free(toml);
  }
  {
    char *toml = fixture_toml_path("duplicate-screen");
    pv_project *p = load_project(toml, 40, 0, 0);
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors > 0);
      CHECK(diag_present(p, "CCB-P1008"));
      pv_project_free(p);
    }
    free(toml);
  }
  {
    char *toml = fixture_toml_path("unreachable");
    pv_project *p = load_project(toml, 40, 0, 0);
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      CHECK(diag_present(p, "CCB-P1006"));
      pv_project_free(p);
    }
    free(toml);
  }
  {
    char *toml = fixture_toml_path("empty-screen");
    pv_project *p = load_project(toml, 40, 0, 0);
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      CHECK(diag_present(p, "CCB-P1007"));
      pv_project_free(p);
    }
    free(toml);
  }
  {
    char *toml = fixture_toml_path("overlap");
    pv_project *p = load_project(toml, 40, 0, 0);
    CHECK(p != NULL);
    if (p != NULL) {
      // Overlap is emitted during layout, before diagnose runs.
      CHECK(diag_present(p, "CCB-P1003"));
      pv_project_free(p);
    }
    free(toml);
  }
  {
    char *toml = fixture_toml_path("offscreen");
    pv_project *p = load_project(toml, 40, 0, 0);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(diag_present(p, "CCB-P1004"));
      pv_project_free(p);
    }
    free(toml);
  }
  {
    char *toml = fixture_toml_path("strict");
    pv_project *p = load_project(toml, 40, 0, 0);
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      CHECK(diag_present(p, "CCB-P1001"));
      pv_project_free(p);
    }
    p = load_project(toml, 40, 0, 1);
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors > 0);
      CHECK(diag_present(p, "CCB-P1001"));
      pv_project_free(p);
    }
    free(toml);
  }
  {
    char *toml = fixture_toml_path("ignored-css");
    pv_project *p = load_project(toml, 40, 0, 0);
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      CHECK(diag_present(p, "CCB-P1015"));
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Scope rule: a <Screen>.css rule applies only to that screen.
  {
    char *toml = fixture_toml_path("scoped");
    pv_project *p = load_project(toml, 40, 0, 0);
    long errors = 0;
    pv_screen *home = NULL;
    pv_screen *other = NULL;
    pv_box *home_text = NULL;
    pv_box *other_text = NULL;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      home = find_screen(p, "Home");
      other = find_screen(p, "Other");
      CHECK(home != NULL && other != NULL);
      if (home != NULL && other != NULL) {
        // 39px at 38 cols / 390 dp scales to 4 columns of padding.
        home_text = find_box(home->root, "text");
        other_text = find_box(other->root, "text");
        CHECK(home_text != NULL && other_text != NULL);
        if (home_text != NULL && other_text != NULL) {
          CHECK(home_text->pad_left_cols == 4);
          CHECK(other_text->pad_left_cols == 0);
        }
      }
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Never-crash corpus: every file parses with CCB_OK.
  {
    static const char *css_files[] = {
      "css-truncated.css", "css-garbage.css", "css-empty.css",
      "css-big-decl.css",
    };
    static const char *kt_files[] = {
      "kt-truncated.kt", "kt-garbage.kt", "kt-empty.kt", "kt-deep.kt",
    };
    static const char *corpus_suffixes[] = {
      "/corpus/preview/",
      "/../tests/corpus/preview/",
    };
    size_t i = 0;
    for (i = 0; i < 4; i++) {
      CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
      ccb_diag_list diags;
      pv_rule *rules = NULL;
      size_t n = 0;
      char *text = read_first(corpus_suffixes, 2, css_files[i]);
      CHECK(text != NULL);
      if (text != NULL) {
        ccb_diag_list_init(&diags);
        CHECK(pv_css_parse(a, text, css_files[i], 0, &diags, &rules,
          &n) == CCB_OK);
        ccb_diag_list_free(&diags);
        free(text);
      }
    }
    for (i = 0; i < 4; i++) {
      CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
      ccb_diag_list diags;
      pv_screen **screens = NULL;
      size_t n = 0;
      char *text = read_first(corpus_suffixes, 2, kt_files[i]);
      CHECK(text != NULL);
      if (text != NULL) {
        ccb_diag_list_init(&diags);
        CHECK(pv_flow_parse(a, text, kt_files[i], &diags, &screens,
          &n) == CCB_OK);
        ccb_diag_list_free(&diags);
        free(text);
      }
    }
  }
  TEST_MAIN_END();
}
