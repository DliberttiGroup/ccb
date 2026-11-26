// Tests for CCB preview Slices 2 and 3 (docs/preview.md): conditional
// nav -> branch-annotated edges, literal arguments on handler edges,
// XML android:onClick handlers, and NavController navigate() through
// a nav graph. Unknown cases assert an unknown edge or a warning,
// never a guessed destination. Checked-in fixtures live under
// tests/fixtures/preview/{branches,litargs,onclick,navgraph}/; the
// remaining cases use scratch projects (mkdtemp). All paths derive
// from __FILE__ for fixtures; scratch tests never depend on the
// working dir.
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

static char *fixture_toml_path(const char *subdir) {
  ccb_str dir;
  ccb_str path;
  FILE *f = NULL;
  ccb_str_init(&dir);
  test_dir(&dir);
  ccb_str_init(&path);
  ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
  ccb_str_append(&path, "/fixtures/preview/");
  ccb_str_append(&path, subdir);
  ccb_str_append(&path, "/ccb.toml");
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
  {
    ccb_str fallback;
    ccb_str_init(&fallback);
    ccb_str_append(&fallback, "tests/fixtures/preview/");
    ccb_str_append(&fallback, subdir);
    ccb_str_append(&fallback, "/ccb.toml");
    {
      char *out = ccb_xstrdup(fallback.buf);
      ccb_str_free(&fallback);
      return out;
    }
  }
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

// True when the edge from `from` to `target` (first match) has the
// given unknown flag.
static int edge_unknown_is(pv_screen *s, const char *from,
    const char *target, int want) {
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
      return e->unknown == want;
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

// The edge from `from` to `target` (first match) carries exactly
// these args: count + per-arg literal-or-NULL equality.
static int edge_args_is(pv_screen *s, const char *from,
    const char *target, const char **args, size_t nargs) {
  size_t i = 0;
  size_t k = 0;
  if (s == NULL) {
    return 0;
  }
  for (i = 0; i < s->nedges; i++) {
    pv_edge *e = &s->edges[i];
    int same_target = (target == NULL && e->target == NULL) ||
      (target != NULL && e->target != NULL &&
       strcmp(e->target, target) == 0);
    if (strcmp(e->from, from) != 0 || !same_target) {
      continue;
    }
    if (e->nargs != nargs) {
      return 0;
    }
    for (k = 0; k < nargs; k++) {
      int same = (args[k] == NULL && e->args[k] == NULL) ||
        (args[k] != NULL && e->args[k] != NULL &&
         strcmp(e->args[k], args[k]) == 0);
      if (!same) {
        return 0;
      }
    }
    return 1;
  }
  return 0;
}

static int contains(const char *hay, const char *needle) {
  return hay != NULL && strstr(hay, needle) != NULL;
}

// Scratch flags for make_scratch2.
#define SCR_LAYOUT_KEY 1
#define SCR_GRAPH_KEY 2
#define SCR_RESOURCES 4

// Build a scratch project: flow text, an optional [preview.bindings]
// body, an optional layout XML file, and an optional nav-graph XML
// file. Flags select whether the layout/graph are pointed at by an
// explicit [preview] key or found via [preview] resources = "res".
// Returns the malloc'd project dir, or NULL.
static char *make_scratch2(const char *flow, const char *bindings,
    const char *layout, const char *graph, int flags) {
  char buf[64];
  ccb_test_tmpdir(buf, sizeof buf, "s23");
  char *sub = NULL;
  char *path = NULL;
  ccb_str toml;
  char *dir = ccb_xstrdup(buf);
  sub = join2(dir, "flow");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "styles");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "res");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "res/layout");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "res/navigation");
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
  if ((flags & SCR_LAYOUT_KEY) != 0) {
    ccb_str_append(&toml, "layout = \"res/layout\"\n");
  }
  if ((flags & SCR_GRAPH_KEY) != 0) {
    ccb_str_append(&toml, "nav_graph = \"res/navigation\"\n");
  }
  if ((flags & SCR_RESOURCES) != 0) {
    ccb_str_append(&toml, "resources = \"res\"\n");
  }
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
  if (layout != NULL) {
    path = join2(dir, "res/layout/activity_main.xml");
    if (write_all(path, layout) != 0) {
      free(path);
      return dir;
    }
    free(path);
  }
  if (graph != NULL) {
    path = join2(dir, "res/navigation/nav_graph.xml");
    if (write_all(path, graph) != 0) {
      free(path);
      return dir;
    }
    free(path);
  }
  return dir;
}

static pv_project *load_scratch(char *dir) {
  char *toml = join2(dir, "ccb.toml");
  pv_project *p = load_project(toml, 0);
  free(toml);
  return p;
}

static void append_targets(char *dir) {
  char *path = join2(dir, "flow/Home.kt");
  FILE *f = fopen(path, "ab");
  if (f != NULL) {
    fwrite("fun Detail() { Column { Text(\"Detail\") } }\n"
      "fun Form() { Column { Text(\"Form\") } }\n"
      "fun DetailActivity() { Column { Text(\"Detail activity\") } }\n",
      1, strlen("fun Detail() { Column { Text(\"Detail\") } }\n"
      "fun Form() { Column { Text(\"Form\") } }\n"
      "fun DetailActivity() { Column { Text(\"Detail activity\") } }\n"),
      f);
    fclose(f);
  }
  free(path);
}

static const char *k_graph =
  "<navigation xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
  "  <fragment android:id=\"@+id/detail\" android:name=\"com.example.Detail\" />\n"
  "  <fragment android:id=\"@+id/form\" android:name=\"com.example.Form\" />\n"
  "</navigation>\n";

int main(void) {
  // --- Fixture: conditional branches (if/else, when, else-if
  // chain, startActivity in a branch, named handler with a branch).
  // A nav inside a nested lambda in a branch is still no edge.
  {
    char *toml = fixture_toml_path("branches");
    pv_project *p = load_project(toml, 0);
    pv_screen *home = NULL;
    ccb_str tree;
    ccb_str json;
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      CHECK(p->nscreens == 4);
      CHECK(find_screen(p, "onNamed") == NULL);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 9);
        CHECK(has_edge_branch(home, "Cond", "Detail", "if ready: then"));
        CHECK(has_edge_branch(home, "Cond", "Form", "if ready: else"));
        CHECK(has_edge_branch(home, "When", "Detail",
          "when state: is Loaded"));
        CHECK(has_edge_branch(home, "When", "Form", "when state: else"));
        CHECK(has_edge_branch(home, "Chain", "Detail", "if a: then"));
        CHECK(has_edge_branch(home, "Chain", "Form", "else if b: then"));
        CHECK(has_edge_branch(home, "Chain", "Detail", "else if b: else"));
        CHECK(has_edge_branch(home, "Act", "DetailActivity",
          "if ready: then"));
        CHECK(has_edge_branch(home, "Named", "Form", "if ready: then"));
        CHECK(!has_edge_from(home, "Nested"));
      }
      ccb_str_init(&tree);
      pv_render_logic_tree(&tree, p);
      CHECK(contains(tree.buf,
        "-> Detail via \"Cond\" [if ready: then]"));
      CHECK(contains(tree.buf,
        "-> Form via \"When\" [when state: else]"));
      ccb_str_free(&tree);
      ccb_str_init(&json);
      pv_project_json(&json, p);
      CHECK(contains(json.buf, "\"branch\":\"if ready: then\""));
      CHECK(contains(json.buf, "\"branch\":\"when state: else\""));
      ccb_str_free(&json);
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Fixture: literal arguments on handler edges.
  {
    char *toml = fixture_toml_path("litargs");
    pv_project *p = load_project(toml, 0);
    pv_screen *home = NULL;
    ccb_str tree;
    const char *a_args[] = { "note-42" };
    const char *b_args[] = { "id = 7" };
    const char *c_args[] = { NULL };
    const char *d_args[] = { "note-42" };
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 4);
        CHECK(edge_args_is(home, "A", "Detail", a_args, 1));
        CHECK(edge_args_is(home, "B", "Detail", b_args, 1));
        CHECK(edge_args_is(home, "C", "Detail", c_args, 1));
        CHECK(edge_args_is(home, "D", NULL, d_args, 1));
        CHECK(edge_unknown_is(home, "A", "Detail", 0));
        CHECK(edge_unknown_is(home, "D", NULL, 1));
      }
      ccb_str_init(&tree);
      pv_render_logic_tree(&tree, p);
      CHECK(contains(tree.buf, "-> Detail(\"note-42\") via \"A\""));
      CHECK(contains(tree.buf, "-> Detail(id = 7) via \"B\""));
      CHECK(contains(tree.buf, "-> Detail(unknown) via \"C\""));
      CHECK(contains(tree.buf, "-> unknown(\"note-42\") via \"D\""));
      ccb_str_free(&tree);
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Fixture: XML android:onClick (Button + AppCompatButton
  // suffix rule, @id/ and @+id/ forms, branch inside an onClick
  // handler, unbound button -> P1019, label in no screen -> P1021,
  // bound method with no fun -> silence, no-nav handler -> silence,
  // onClick on a non-Button element -> ignored entirely).
  {
    char *toml = fixture_toml_path("onclick");
    pv_project *p = load_project(toml, 0);
    pv_screen *home = NULL;
    CHECK(p != NULL);
    if (p != NULL) {
      long errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      CHECK(p->nscreens == 3);
      CHECK(find_screen(p, "onOpenClick") == NULL);
      CHECK(find_screen(p, "onNewClick") == NULL);
      CHECK(find_screen(p, "onGhostClick") == NULL);
      CHECK(find_screen(p, "onSilentClick") == NULL);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 2);
        CHECK(has_edge_branch(home, "Open note", "Detail", NULL));
        CHECK(has_edge_branch(home, "New note", "Form",
          "if ready: then"));
        CHECK(!has_edge_from(home, "Silent"));
        CHECK(!has_edge_from(home, "Missing button"));
      }
      CHECK(diag_count(p, "CCB-P1019") == 1);
      CHECK(diag_message_contains(p, "CCB-P1019", "stray_button"));
      CHECK(diag_count(p, "CCB-P1021") == 1);
      CHECK(diag_message_contains(p, "CCB-P1021", "onGhostClick"));
      CHECK(diag_message_contains(p, "CCB-P1021", "Missing button"));
      CHECK(pv_diag_is_error("CCB-P1021", 1) == 0);
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Fixture: NavController navigate() through a nav graph
  // (R.id.x, chained findNavController(), string form, unknown id,
  // action ids are not destinations, navigate() outside a handler
  // is not an edge).
  {
    char *toml = fixture_toml_path("navgraph");
    pv_project *p = load_project(toml, 0);
    pv_screen *home = NULL;
    CHECK(p != NULL);
    if (p != NULL) {
      long errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 4);
        CHECK(has_edge(home, "A", "Detail"));
        CHECK(has_edge(home, "B", "Form"));
        CHECK(has_edge(home, "C", NULL));
        CHECK(edge_unknown_is(home, "C", NULL, 1));
        CHECK(has_edge(home, "D", "Detail"));
        CHECK(!has_edge_from(home, "E"));
        CHECK(!has_edge_from(home, "Home"));
      }
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Scratch: navigate() with no graph file at all -> unknown
  // edge, never a guess, no crash.
  {
    char *dir = make_scratch2(
      "fun Home() {\n"
      "  Column { Button(\"A\") }\n"
      "  aButton.setOnClickListener { navController.navigate(R.id.detail) }\n"
      "}\n",
      "aButton = \"A\"\n", NULL, NULL, 0);
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    append_targets(dir);
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 1);
        CHECK(has_edge(home, "A", NULL));
        CHECK(home->edges[0].unknown == 1);
      }
      pv_project_free(p);
    }
    free(dir);
  }
  // --- Scratch: navigate() with a non-literal destination, and
  // navigate() with literal arguments through an explicit graph.
  {
    char *dir = make_scratch2(
      "fun Home() {\n"
      "  Column { Button(\"A\") Button(\"B\") }\n"
      "  aButton.setOnClickListener { navController.navigate(destId) }\n"
      "  bButton.setOnClickListener { navController.navigate(R.id.detail, \"note-42\") }\n"
      "}\n",
      "aButton = \"A\"\nbButton = \"B\"\n", NULL, k_graph, SCR_GRAPH_KEY);
    pv_project *p = NULL;
    pv_screen *home = NULL;
    const char *b_args[] = { "note-42" };
    CHECK(dir != NULL);
    append_targets(dir);
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 2);
        CHECK(has_edge(home, "A", NULL));
        CHECK(has_edge(home, "B", "Detail"));
        CHECK(edge_args_is(home, "B", "Detail", b_args, 1));
      }
      pv_project_free(p);
    }
    free(dir);
  }
  // --- Scratch: nav graph found via [preview] resources fallback
  // (no nav_graph key), and nav_graph as a single file.
  {
    char *dir = make_scratch2(
      "fun Home() {\n"
      "  Column { Button(\"A\") }\n"
      "  aButton.setOnClickListener { navController.navigate(R.id.form) }\n"
      "}\n",
      "aButton = \"A\"\n", NULL, k_graph, SCR_RESOURCES);
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    append_targets(dir);
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(has_edge(home, "A", "Form"));
      }
      pv_project_free(p);
    }
    free(dir);
  }
  // --- Scratch: nested conditionals compound their labels; a
  // braceless else and a braceless when-free else chain both yield
  // branch edges; a nav inside a callback in a branch is no edge.
  {
    char *dir = make_scratch2(
      "fun Home() {\n"
      "  Column { Button(\"A\") Button(\"B\") Button(\"C\") }\n"
      "  aButton.setOnClickListener { if (x) { if (y) { nav(\"Detail\") } } }\n"
      "  bButton.setOnClickListener { if (x) nav(\"Detail\") else nav(\"Form\") }\n"
      "  cButton.setOnClickListener { when (s) { is Ready -> run { nav(\"Detail\") } } }\n"
      "}\n",
      "aButton = \"A\"\nbButton = \"B\"\ncButton = \"C\"\n",
      NULL, NULL, 0);
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    append_targets(dir);
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 3);
        CHECK(has_edge_branch(home, "A", "Detail",
          "if x: then, if y: then"));
        CHECK(has_edge_branch(home, "B", "Detail", "if x: then"));
        CHECK(has_edge_branch(home, "B", "Form", "if x: else"));
        CHECK(!has_edge_from(home, "C"));
      }
      pv_project_free(p);
    }
    free(dir);
  }
  // --- Scratch: declared beats derived for branch edges too —
  // same destination dedups silently, disagreement warns P1020 and
  // the declared edge stands.
  {
    char *dir = make_scratch2(
      "fun Home() {\n"
      "  Column { Button(\"Same\", onClick = { nav(\"Detail\") }) Button(\"Diff\", onClick = { nav(\"Form\") }) }\n"
      "  sameButton.setOnClickListener { if (ready) { nav(\"Detail\") } }\n"
      "  diffButton.setOnClickListener { if (ready) { nav(\"Detail\") } }\n"
      "}\n",
      "sameButton = \"Same\"\ndiffButton = \"Diff\"\n",
      NULL, NULL, 0);
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    append_targets(dir);
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 2);
        CHECK(has_edge(home, "Same", "Detail"));
        CHECK(has_edge(home, "Diff", "Form"));
        CHECK(!has_edge(home, "Diff", "Detail"));
      }
      CHECK(diag_count(p, "CCB-P1020") == 1);
      CHECK(diag_message_contains(p, "CCB-P1020", "Diff"));
      pv_project_free(p);
    }
    free(dir);
  }
  // --- Scratch: layout comment containing a Button tag is not
  // parsed, and an onClick handler with no nav is silent.
  {
    char *dir = make_scratch2(
      "fun Home() {\n"
      "  Column { Button(\"Open note\") Button(\"Quiet\") }\n"
      "}\n"
      "fun onOpenClick(v: View) { nav(\"Detail\") }\n"
      "fun onQuietClick(v: View) { }\n"
      "fun onCommentClick(v: View) { nav(\"Form\") }\n",
      "open_button = \"Open note\"\nquiet_button = \"Quiet\"\ncomment_button = \"Open note\"\n",
      "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
      "  <!-- <Button android:id=\"@+id/comment_button\" android:onClick=\"onCommentClick\" /> -->\n"
      "  <Button android:id=\"@+id/open_button\" android:onClick=\"onOpenClick\" />\n"
      "  <Button android:id=\"@+id/quiet_button\" android:onClick=\"onQuietClick\" />\n"
      "</LinearLayout>\n",
      NULL, SCR_LAYOUT_KEY);
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    append_targets(dir);
    p = load_scratch(dir);
    CHECK(p != NULL);
    if (p != NULL) {
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL) {
        CHECK(home->nedges == 1);
        CHECK(has_edge(home, "Open note", "Detail"));
        CHECK(!has_edge(home, "Open note", "Form"));
        CHECK(!has_edge_from(home, "Quiet"));
      }
      CHECK(diag_count(p, "CCB-P1019") == 0);
      CHECK(diag_count(p, "CCB-P1021") == 0);
      pv_project_free(p);
    }
    free(dir);
  }
  TEST_MAIN_END();
}
