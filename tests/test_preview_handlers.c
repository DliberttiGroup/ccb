// Tests for Slice 1 click-handler edges (docs/preview.md): listener
// lambdas (trailing and parenthesized), onClick assignments, named
// handlers via direct :: references, and startActivity class
// literals, bound through [preview.bindings]. Each unknown case
// asserts an unknown edge or the unbound warning, never a guessed
// destination. The checked-in fixture lives under
// tests/fixtures/preview/handlers/; the remaining cases use scratch
// projects (mkdtemp) so every binding variation is self-contained.
// All paths derive from __FILE__ for the fixture; scratch tests
// never depend on the working dir.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_config.h"
#include "ccb_str.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_dir(ccb_str *out) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  if (slash != NULL && slash != file) {
    ccb_str_append_n(out, file, (size_t)(slash - file));
  } else {
    ccb_str_append(out, ".");
  }
}

static int write_all(const char *path, const char *data) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return -1;
  }
  if (fwrite(data, 1, strlen(data), f) != strlen(data)) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

static char *join2(const char *a, const char *b) {
  ccb_str s;
  char *out = NULL;
  ccb_str_init(&s);
  ccb_str_append(&s, a);
  ccb_str_append(&s, "/");
  ccb_str_append(&s, b);
  out = ccb_xstrdup(s.buf);
  ccb_str_free(&s);
  return out;
}

static char *fixture_toml_path(void) {
  ccb_str dir;
  ccb_str path;
  FILE *f = NULL;
  ccb_str_init(&dir);
  test_dir(&dir);
  ccb_str_init(&path);
  ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
  ccb_str_append(&path, "/fixtures/preview/handlers/ccb.toml");
  f = fopen(path.buf != NULL ? path.buf : "", "rb");
  if (f != NULL) {
    fclose(f);
    {
      char *out = ccb_xstrdup(path.buf);
      ccb_str_free(&path);
      ccb_str_free(&dir);
      return out;
    }
  }
  ccb_str_free(&path);
  ccb_str_free(&dir);
  return ccb_xstrdup("tests/fixtures/preview/handlers/ccb.toml");
}

static pv_project *load_project(const char *toml_path, int strict) {
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  pv_project *p = NULL;
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
  if (pv_project_load(cfg, 40, 0, strict, NULL, &p) != CCB_OK) {
    ccb_config_free(cfg);
    return NULL;
  }
  ccb_config_free(cfg);
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

// True when the screen has an edge from `from` to `target`
// (target NULL matches an unknown edge).
static int has_edge(pv_screen *s, const char *from, const char *target) {
  size_t i = 0;
  if (s == NULL) {
    return 0;
  }
  for (i = 0; i < s->nedges; i++) {
    pv_edge *e = &s->edges[i];
    int same_target = (target == NULL && e->target == NULL) ||
      (target != NULL && e->target != NULL &&
       strcmp(e->target, target) == 0);
    if (strcmp(e->from, from) == 0 && same_target) {
      return 1;
    }
  }
  return 0;
}

// True when the screen has an edge from `from` to `target` whose
// branch annotation is exactly `branch` (NULL matches an
// unconditional edge).
static int has_edge_branch(pv_screen *s, const char *from,
    const char *target, const char *branch) {
  size_t i = 0;
  if (s == NULL) {
    return 0;
  }
  for (i = 0; i < s->nedges; i++) {
    pv_edge *e = &s->edges[i];
    int same_target = (target == NULL && e->target == NULL) ||
      (target != NULL && e->target != NULL &&
       strcmp(e->target, target) == 0);
    int same_branch = (branch == NULL && e->branch == NULL) ||
      (branch != NULL && e->branch != NULL &&
       strcmp(e->branch, branch) == 0);
    if (strcmp(e->from, from) == 0 && same_target && same_branch) {
      return 1;
    }
  }
  return 0;
}

static int has_edge_from(pv_screen *s, const char *from) {
  size_t i = 0;
  if (s == NULL) {
    return 0;
  }
  for (i = 0; i < s->nedges; i++) {
    if (strcmp(s->edges[i].from, from) == 0) {
      return 1;
    }
  }
  return 0;
}

static int contains(const char *hay, const char *needle) {
  return hay != NULL && strstr(hay, needle) != NULL;
}

// Build a scratch project: flow text plus an optional
// [preview.bindings] body (NULL means no bindings table). Returns
// the malloc'd project dir, or NULL.
static char *make_scratch(const char *flow, const char *bindings) {
  char tmpl[] = "/tmp/ccb-hand-XXXXXX";
  char *dir = mkdtemp(tmpl);
  char *sub = NULL;
  char *path = NULL;
  ccb_str toml;
  if (dir == NULL) {
    return NULL;
  }
  dir = ccb_xstrdup(dir);
  sub = join2(dir, "flow");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "styles");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "src");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "src/main");
  mkdir(sub, 0777);
  free(sub);
  ccb_str_init(&toml);
  ccb_str_append(&toml,
    "ccb_version = 1\n"
    "[project]\n"
    "name = \"scratch\"\n"
    "package = \"com.example.scratch\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "[preview]\n"
    "styles = \"styles\"\n"
    "flow = \"flow\"\n"
    "phone_width = 390\n"
    "phone_height = 844\n");
  if (bindings != NULL) {
    ccb_str_append(&toml, "[preview.bindings]\n");
    ccb_str_append(&toml, bindings);
  }
  ccb_str_append(&toml,
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n");
  path = join2(dir, "ccb.toml");
  if (write_all(path, toml.buf) != 0) {
    free(path);
    ccb_str_free(&toml);
    return dir;
  }
  free(path);
  ccb_str_free(&toml);
  path = join2(dir, "src/main/AndroidManifest.xml");
  if (write_all(path, "<manifest />\n") != 0) {
    free(path);
    return dir;
  }
  free(path);
  path = join2(dir, "flow/Home.kt");
  if (write_all(path, flow) != 0) {
    free(path);
    return dir;
  }
  free(path);
  path = join2(dir, "styles/common.css");
  if (write_all(path, "screen { padding: 0px; }\n") != 0) {
    free(path);
    return dir;
  }
  free(path);
  return dir;
}

static pv_project *load_scratch(char *dir) {
  char *toml = join2(dir, "ccb.toml");
  pv_project *p = load_project(toml, 0);
  free(toml);
  return p;
}

// Every scratch flow below declares Detail/Form targets so a
// missing-target error never masks the handler assertion.
static const char *k_targets =
  "fun Detail() { Column { Text(\"Detail\") } }\n"
  "fun Form() { Column { Text(\"Form\") } }\n";

int main(void) {
  // --- Checked-in fixture: trailing lambda, onClick assignment,
  // named handler, startActivity class literal, unbound handler.
  // The named handler fun is NOT a screen.
  {
    char *toml = fixture_toml_path();
    pv_project *p = load_project(toml, 0);
    pv_screen *home = NULL;
    ccb_str tree;
    ccb_str json;
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      CHECK(p->nscreens == 4);
      CHECK(find_screen(p, "onOpenClick") == NULL);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 4);
        CHECK(has_edge(home, "Open note", "Detail"));
        CHECK(has_edge(home, "Detail button", "Form"));
        CHECK(has_edge(home, "Named button", "Form"));
        CHECK(has_edge(home, "Activity button", "DetailActivity"));
        CHECK(!has_edge_from(home, "strayButton"));
      }
      // One unbound-handler warning, naming the receiver.
      CHECK(diag_count(p, "CCB-P1019") == 1);
      CHECK(diag_message_contains(p, "CCB-P1019", "strayButton"));
      // Derived edges render exactly like declared ones: the tree
      // line and the JSON edge carry no derived marker.
      ccb_str_init(&tree);
      pv_render_logic_tree(&tree, p);
      CHECK(contains(tree.buf, "-> Detail via \"Open note\""));
      ccb_str_free(&tree);
      ccb_str_init(&json);
      pv_project_json(&json, p);
      CHECK(contains(json.buf, "\"from\":\"Open note\",\"target\":\"Detail\""));
      CHECK(!contains(json.buf, "derived"));
      ccb_str_free(&json);
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Same fixture under --strict: CCB-P1019 stays a warning,
  // never an error, never a gate failure.
  {
    char *toml = fixture_toml_path();
    pv_project *p = load_project(toml, 1);
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      CHECK(diag_count(p, "CCB-P1019") == 1);
      CHECK(pv_diag_is_error("CCB-P1019", 1) == 0);
      CHECK(pv_diag_is_error("CCB-P1020", 1) == 0);
      pv_project_free(p);
    }
    free(toml);
  }
  // --- (b) Parenthesized listener form, plus a chained receiver
  // (binding.views.openButton resolves to the last identifier).
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"Open note\") Button(\"Chained\") }\n"
      "  openButton.setOnClickListener({ nav(\"Detail\") })\n"
      "  views.openButton.setOnClickListener { nav(\"Form\") }\n"
      "}\n",
      "openButton = \"Open note\"\n");
    // The chained attachment reuses the same receiver key, so both
    // edges share the bound label; add the target screens.
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    // Append the target screens to the flow file for this case.
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(has_edge(home, "Open note", "Detail"));
        CHECK(has_edge(home, "Open note", "Form"));
      }
      CHECK(diag_count(p, "CCB-P1019") == 0);
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (f) Computed destination: an unknown edge, no guessed target.
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"Open note\") }\n"
      "  openButton.setOnClickListener { nav(target) }\n"
      "}\n",
      "openButton = \"Open note\"\n");
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 1);
        CHECK(has_edge(home, "Open note", NULL));
        CHECK(home->edges[0].unknown == 1);
      }
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (g) Nav nested in a lambda/callback inside the handler:
  // no edge at all, not even an unknown edge.
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"A\") Button(\"B\") }\n"
      "  aButton.setOnClickListener { launch { nav(\"Detail\") } }\n"
      "  bButton.setOnClickListener { fetch { r -> nav(\"Detail\") } }\n"
      "}\n",
      "aButton = \"A\"\nbButton = \"B\"\n");
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 0);
      }
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (h) Conditional nav inside a handler (braced if, braceless
  // if, when block): Slice 2 extends Slice 1 here — each becomes a
  // branch-annotated edge (condition labeled, never evaluated).
  // This is the ONE intentional Slice 1 behavior change in Slices
  // 2/3; every other case in this file is unchanged.
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"A\") Button(\"B\") Button(\"C\") }\n"
      "  aButton.setOnClickListener { if (ready) { nav(\"Detail\") } }\n"
      "  bButton.setOnClickListener { if (ready) nav(\"Detail\") }\n"
      "  cButton.setOnClickListener { when (state) { is Loaded -> nav(\"Detail\") } }\n"
      "}\n",
      "aButton = \"A\"\nbButton = \"B\"\ncButton = \"C\"\n");
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 3);
        CHECK(has_edge_branch(home, "A", "Detail", "if ready: then"));
        CHECK(has_edge_branch(home, "B", "Detail", "if ready: then"));
        CHECK(has_edge_branch(home, "C", "Detail",
          "when state: is Loaded"));
      }
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (i) Handler that merely calls a shared helper: no edge
  // attributed to the button. The helper fun itself follows the
  // existing top-level-fun rules (it is a screen, not an edge).
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"Open note\") }\n"
      "  openButton.setOnClickListener { openDetail() }\n"
      "}\n"
      "fun openDetail() { nav(\"Detail\") }\n",
      "openButton = \"Open note\"\n");
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(!has_edge_from(home, "Open note"));
      }
      CHECK(find_screen(p, "openDetail") != NULL);
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (j) Variable-held method reference: no edge. The fun is
  // not referenced directly, so it stays a screen by the existing
  // top-level-fun rules.
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"Open note\") }\n"
      "  val handler = ::onOpenClick\n"
      "  openButton.setOnClickListener(handler)\n"
      "}\n"
      "fun onOpenClick(v: View) { nav(\"Detail\") }\n",
      "openButton = \"Open note\"\n");
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(!has_edge_from(home, "Open note"));
      }
      CHECK(find_screen(p, "onOpenClick") != NULL);
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (k) Unbound named handler: CCB-P1019, no edge.
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"Named\") }\n"
      "  namedButton.setOnClickListener(::onOpenClick)\n"
      "}\n"
      "fun onOpenClick(v: View) { nav(\"Detail\") }\n",
      NULL);
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 0);
      }
      CHECK(diag_count(p, "CCB-P1019") == 1);
      CHECK(diag_message_contains(p, "CCB-P1019", "namedButton"));
      CHECK(find_screen(p, "onOpenClick") == NULL);
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (l) Declared beats derived, same destination: dedup, no
  // warning, exactly one edge.
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"Open note\", onClick = { nav(\"Detail\") }) }\n"
      "  openButton.setOnClickListener { nav(\"Detail\") }\n"
      "}\n",
      "openButton = \"Open note\"\n");
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 1);
        CHECK(has_edge(home, "Open note", "Detail"));
      }
      CHECK(diag_count(p, "CCB-P1020") == 0);
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (m) Declared vs derived disagreement: CCB-P1020 naming the
  // button and both destinations, declared edge stands, derived
  // edge dropped. The warning is a warning even under strict.
  {
    char *dir = make_scratch(
      "fun Home() {\n"
      "  Column { Button(\"Open note\", onClick = { nav(\"Form\") }) }\n"
      "  openButton.setOnClickListener { nav(\"Detail\") }\n"
      "}\n",
      "openButton = \"Open note\"\n");
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    {
      char *path = join2(dir, "flow/Home.kt");
      FILE *f = fopen(path, "ab");
      if (f != NULL) {
        fwrite(k_targets, 1, strlen(k_targets), f);
        fclose(f);
      }
      free(path);
    }
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 1);
        CHECK(has_edge(home, "Open note", "Form"));
        CHECK(!has_edge(home, "Open note", "Detail"));
      }
      CHECK(diag_count(p, "CCB-P1020") == 1);
      CHECK(diag_message_contains(p, "CCB-P1020", "Open note"));
      CHECK(diag_message_contains(p, "CCB-P1020", "Form"));
      CHECK(diag_message_contains(p, "CCB-P1020", "Detail"));
      CHECK(pv_diag_is_error("CCB-P1020", 1) == 0);
      pv_project_free(p);
    }
    free(dir);
  }
  // --- (n) Bindings-table parsing: a non-string value is the
  // existing wrong-type error (CCB-E1007) and config load fails.
  {
    char *dir = make_scratch(
      "fun Home() { Column { Text(\"Hi\") } }\n",
      "openButton = 42\n");
    ccb_diag_list diags;
    ccb_config *cfg = NULL;
    char *toml = NULL;
    int found = 0;
    size_t i = 0;
    CHECK(dir != NULL);
    toml = join2(dir, "ccb.toml");
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(toml, NULL, &cfg, &diags) != CCB_OK);
    CHECK(cfg == NULL);
    for (i = 0; i < diags.count; i++) {
      if (strcmp(diags.items[i].code, "CCB-E1007") == 0) {
        found = 1;
      }
    }
    CHECK(found == 1);
    ccb_diag_list_free(&diags);
    free(toml);
    free(dir);
  }
  TEST_MAIN_END();
}
