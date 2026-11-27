// Tests for Chunk A of the preview correctness plan (Phase 1):
// fallback existence checks, package-aware normalization, graph
// destinations as nodes, manifest activities as P1005 exemptions,
// and the P1 fallback repro. All paths derive from __FILE__;
// scratch tests never depend on the working directory.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cli.h"
#include "ccb_config.h"
#include "ccb_preview.h"
#include "ccb_preview_walk.h"
#include "ccb_str.h"
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- helpers ----------

static char *test_dir(void) {
  const char *f = __FILE__;
  const char *slash = strrchr(f, '/');
  size_t n = 0;
  char *out = NULL;
  if (slash == NULL) {
    return ccb_xstrdup(".");
  }
  n = (size_t)(slash - f);
  out = ccb_xmalloc(n + 1);
  memcpy(out, f, n);
  out[n] = '\0';
  return out;
}

static char *join2(const char *a, const char *b) {
  ccb_str s;
  char *out = NULL;
  ccb_str_init(&s);
  ccb_str_append(&s, a);
  ccb_str_append(&s, "/");
  ccb_str_append(&s, b);
  out = ccb_xstrdup(s.buf != NULL ? s.buf : "");
  ccb_str_free(&s);
  return out;
}

static void mkdir_one(const char *path) {
  (void)mkdir(path, 0777);
}

static int write_all(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    return -1;
  }
  fputs(text, f);
  fclose(f);
  return 0;
}

// Write text to dir/name, freeing the joined path (LSan-clean).
static int write_joined(const char *dir, const char *name,
    const char *text) {
  char *path = join2(dir, name);
  int rc = write_all(path, text);
  free(path);
  return rc;
}

// Create dir/name, freeing the joined path (LSan-clean).
static void mkdir_joined(const char *dir, const char *name) {
  char *path = join2(dir, name);
  mkdir_one(path);
  free(path);
}

// Recursively copy a directory tree, skipping build/ output dirs.
static int copy_tree(const char *src, const char *dst) {
  DIR *d = opendir(src);
  struct dirent *e = NULL;
  if (d == NULL) {
    return -1;
  }
  mkdir_one(dst);
  while ((e = readdir(d)) != NULL) {
    char *sp = NULL;
    char *dp = NULL;
    struct stat st;
    int rc = 0;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
        strcmp(e->d_name, "build") == 0) {
      continue;
    }
    sp = join2(src, e->d_name);
    dp = join2(dst, e->d_name);
    if (stat(sp, &st) != 0) {
      free(sp);
      free(dp);
      closedir(d);
      return -1;
    }
    if (S_ISDIR(st.st_mode)) {
      rc = copy_tree(sp, dp);
    } else {
      char *text = ccb_test_try_read(sp);
      rc = text == NULL ? -1 : write_all(dp, text);
      free(text);
    }
    free(sp);
    free(dp);
    if (rc != 0) {
      closedir(d);
      return -1;
    }
  }
  closedir(d);
  return 0;
}

// Run the CLI inside workdir, capturing stdout to cap_path.
static int run_cli(const char *workdir, const char *cap_path,
    int argc, char **argv) {
  char cwd[PATH_MAX];
  int saved = -1;
  int fd = -1;
  int rc = -1;
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    return -1;
  }
  if (chdir(workdir) != 0) {
    return -1;
  }
  fd = open(cap_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    { int rc2 = chdir(cwd); (void)rc2; }
    return -1;
  }
  saved = dup(STDOUT_FILENO);
  if (saved < 0) {
    close(fd);
    { int rc2 = chdir(cwd); (void)rc2; }
    return -1;
  }
  (void)dup2(fd, STDOUT_FILENO);
  close(fd);
  rc = ccb_cli_main(argc, argv);
  (void)dup2(saved, STDOUT_FILENO);
  close(saved);
  { int rc2 = chdir(cwd); (void)rc2; }
  return rc;
}

static pv_project *load_project(const char *toml_path) {
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
  if (pv_project_load(cfg, 40, 0, 0, NULL, &p) != CCB_OK) {
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
  if (p == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, name) == 0) {
      return p->screens[i];
    }
  }
  return NULL;
}

static pv_edge *find_edge(pv_screen *screen, const char *from) {
  size_t i = 0;
  if (screen == NULL || from == NULL) {
    return NULL;
  }
  for (i = 0; i < screen->nedges; i++) {
    if (screen->edges[i].from != NULL &&
        strcmp(screen->edges[i].from, from) == 0) {
      return &screen->edges[i];
    }
  }
  return NULL;
}

// Strip the explicit layout = / nav_graph = lines from a ccb.toml,
// leaving every other line untouched.
static int strip_layout_nav_keys(const char *toml_path) {
  char *text = ccb_test_try_read(toml_path);
  ccb_str out;
  const char *cur = NULL;
  if (text == NULL) {
    return -1;
  }
  ccb_str_init(&out);
  cur = text;
  while (*cur != '\0') {
    const char *nl = strchr(cur, '\n');
    size_t len = nl != NULL ? (size_t)(nl - cur) + 1 : strlen(cur);
    if (strncmp(cur, "layout =", 8) != 0 &&
        strncmp(cur, "nav_graph =", 12) != 0) {
      ccb_str_append_n(&out, cur, len);
    }
    cur += len;
  }
  free(text);
  if (write_all(toml_path, out.buf != NULL ? out.buf : "") != 0) {
    ccb_str_free(&out);
    return -1;
  }
  ccb_str_free(&out);
  return 0;
}

static char *p1_dir(void) {
  char *td = test_dir();
  char *cand = join2(td, "../examples/real-world/p1-notes");
  struct stat st;
  if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
    free(td);
    return cand;
  }
  free(cand);
  free(td);
  cand = ccb_xstrdup("examples/real-world/p1-notes");
  if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
    return cand;
  }
  free(cand);
  return NULL;
}

int main(void) {
  // (1) Fallback existence checks on the path functions directly.
  {
    char dir[64];
    ccb_test_tmpdir(dir, sizeof dir, "ca");
    char *res = NULL;
    char *layout = NULL;
    char *nav = NULL;
    char *values = NULL;
    char *res_layout = NULL;
    ccb_config cfg;
    char *p = NULL;
    res = join2(dir, "res");
    layout = join2(res, "layout");
    nav = join2(res, "navigation");
    values = join2(res, "values");
    mkdir_one(res);
    mkdir_one(layout);
    mkdir_one(nav);
    mkdir_one(values);
    // Values-dir resources + set res: res wins.
    memset(&cfg, 0, sizeof(cfg));
    cfg.res = res;
    cfg.preview_resources = values;
    p = pv_preview_layout_path(&cfg);
    CHECK(p != NULL && strcmp(p, layout) == 0);
    free(p);
    p = pv_preview_nav_path(&cfg);
    CHECK(p != NULL && strcmp(p, nav) == 0);
    free(p);
    // Resources as the res root, res unset: resources wins.
    memset(&cfg, 0, sizeof(cfg));
    cfg.preview_resources = res;
    p = pv_preview_layout_path(&cfg);
    CHECK(p != NULL && strcmp(p, layout) == 0);
    free(p);
    p = pv_preview_nav_path(&cfg);
    CHECK(p != NULL && strcmp(p, nav) == 0);
    free(p);
    // Res root present but layout missing there: fall through to
    // the resources layout dir (here: a second res root).
    res_layout = join2(values, "layout");
    mkdir_one(res_layout);
    memset(&cfg, 0, sizeof(cfg));
    cfg.res = res;
    cfg.preview_resources = values;
    p = pv_preview_layout_path(&cfg);
    CHECK(p != NULL && strcmp(p, layout) == 0);
    free(p);
    // Explicit key is strict: returned as-is, even nonexistent.
    memset(&cfg, 0, sizeof(cfg));
    cfg.preview_layout = "no/such/layout";
    cfg.res = res;
    p = pv_preview_layout_path(&cfg);
    CHECK(p != NULL && strcmp(p, "no/such/layout") == 0);
    free(p);
    // Nothing configured: NULL.
    memset(&cfg, 0, sizeof(cfg));
    CHECK(pv_preview_layout_path(&cfg) == NULL);
    CHECK(pv_preview_nav_path(&cfg) == NULL);
    free(res);
    free(layout);
    free(nav);
    free(values);
    free(res_layout);
  }

  // (2) Package-aware normalization and suffix-aware matching.
  {
    const char *pkg = "com.example.notes";
    char *n = NULL;
    n = pv_normalize_class(pkg, "com.example.notes.DetailActivity");
    CHECK(n != NULL && strcmp(n, "DetailActivity") == 0);
    free(n);
    n = pv_normalize_class(pkg, ".DetailActivity");
    CHECK(n != NULL && strcmp(n, "DetailActivity") == 0);
    free(n);
    n = pv_normalize_class(pkg, "DetailActivity");
    CHECK(n != NULL && strcmp(n, "DetailActivity") == 0);
    free(n);
    n = pv_normalize_class(pkg, "com.other.DetailActivity");
    CHECK(n != NULL && strcmp(n, "DetailActivity") == 0);
    free(n);
    n = pv_normalize_class(NULL, ".MainActivity");
    CHECK(n != NULL && strcmp(n, "MainActivity") == 0);
    free(n);
    CHECK(pv_normalize_class(pkg, NULL) == NULL);
    CHECK(pv_names_match(pkg, "DetailActivity", "Detail") == 1);
    CHECK(pv_names_match(pkg, "Detail", "DetailActivity") == 1);
    CHECK(pv_names_match(pkg, "DetailActivity", "DetailFragment") == 0);
    CHECK(pv_names_match(pkg, "Detail", "Settings") == 0);
    CHECK(pv_names_match(pkg, "Detail", "Detail") == 1);
    CHECK(pv_names_match(pkg, "com.example.DetailActivity",
      "Detail") == 1);
    CHECK(pv_names_match(pkg, "Fragment", "Fragment") == 1);
    CHECK(pv_names_match(pkg, "DetailFragment", "Detail") == 1);
  }

  // (3) Graph nodes, fully qualified destinations, manifest
  // exemption, and P1005 still firing for a true unknown.
  {
    char scratch[64];
    ccb_test_tmpdir(scratch, sizeof scratch, "ca");
    char *proj = NULL;
    char *toml = NULL;
    char *flowdir = NULL;
    char *stylesdir = NULL;
    char *navdir = NULL;
    char *srcdir = NULL;
    pv_project *p = NULL;
    pv_screen *home = NULL;
    pv_screen *extra = NULL;
    pv_edge *e = NULL;
    long errors = 0;
    proj = join2(scratch, "proj");
    flowdir = join2(proj, "flow");
    stylesdir = join2(proj, "styles");
    navdir = join2(proj, "res/navigation");
    srcdir = join2(proj, "src/main");
    mkdir_one(proj);
    mkdir_one(flowdir);
    mkdir_one(stylesdir);
    mkdir_joined(proj, "res");
    mkdir_one(navdir);
    mkdir_joined(proj, "src");
    mkdir_one(srcdir);
    toml = join2(proj, "ccb.toml");
    CHECK(write_all(toml,
      "ccb_version = 1\n"
      "[project]\n"
      "name = \"chunk-a\"\n"
      "package = \"com.example.app\"\n"
      "version = \"1.0\"\n"
      "version_code = 1\n"
      "min_sdk = 26\n"
      "target_sdk = 35\n"
      "compile_sdk = 35\n"
      "[preview]\n"
      "styles = \"styles\"\n"
      "flow = \"flow\"\n"
      "nav_graph = \"res/navigation\"\n"
      "phone_width = 390\n"
      "phone_height = 844\n"
      "[preview.bindings]\n"
      "bButton = \"Go extra\"\n"
      "cButton = \"Go fq\"\n"
      "[paths]\n"
      "manifest = \"src/main/AndroidManifest.xml\"\n") == 0);
    CHECK(write_joined(flowdir, "Home.kt",
      "fun Home() {\n"
      "  Column {\n"
      "    Button(\"Open\", onClick = { nav(\"DetailActivity\") })\n"
      "    Button(\"Main\", onClick = { nav(\"MainActivity\") })\n"
      "    Button(\"Bad\", onClick = { nav(\"Nope\") })\n"
      "    Button(\"Go extra\")\n"
      "    Button(\"Go fq\")\n"
      "  }\n"
      "  bButton.setOnClickListener {\n"
      "    navController.navigate(R.id.extra)\n"
      "  }\n"
      "  cButton.setOnClickListener {\n"
      "    navController.navigate(R.id.fq)\n"
      "  }\n"
      "}\n"
      "fun Detail() {\n"
      "  Column { Text(\"Detail\") }\n"
      "}\n") == 0);
    CHECK(write_joined(navdir, "nav_graph.xml",
      "<navigation "
      "xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
      "  <fragment android:id=\"@+id/extra\" "
      "android:name=\"com.example.Extra\" />\n"
      "  <fragment android:id=\"@+id/fq\" "
      "android:name=\"com.example.DetailActivity\" />\n"
      "</navigation>\n") == 0);
    CHECK(write_joined(srcdir, "AndroidManifest.xml",
      "<manifest "
      "xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
      "  package=\"com.example.app\">\n"
      "  <application>\n"
      "    <activity android:name=\".MainActivity\" />\n"
      "    <activity android:name=\".DetailActivity\" />\n"
      "  </application>\n"
      "</manifest>\n") == 0);
    CHECK(write_joined(stylesdir, "common.css",
      "screen { padding: 0px; }\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      pv_walk w;
      ccb_str tree;
      ccb_str json;
      size_t k = 0;
      errors = pv_project_diagnose(p);
      // Home + Detail + one node for Extra; the fq destination
      // matches the Detail screen and adds no node, and the
      // manifest MainActivity adds no node either.
      CHECK(p->nscreens == 3);
      extra = find_screen(p, "Extra");
      CHECK(extra != NULL && extra->graph_only == 1);
      CHECK(extra != NULL && extra->nedges == 0);
      CHECK(find_screen(p, "MainActivity") == NULL);
      // Exactly one P1005, for the true unknown "Nope"; the
      // suffix-matched, graph-matched, and manifest-matched
      // targets all pass.
      CHECK(diag_count(p, "CCB-P1005") == 1);
      CHECK(diag_message_contains(p, "CCB-P1005", "Nope") == 1);
      CHECK(errors == 1);
      // No empty-screen warning for the graph node, and no
      // unreachable warning: every screen is reachable.
      CHECK(diag_count(p, "CCB-P1007") == 0);
      CHECK(diag_count(p, "CCB-P1006") == 0);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      e = find_edge(home, "Open");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "DetailActivity") == 0);
      e = find_edge(home, "Go extra");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Extra") == 0);
      e = find_edge(home, "Go fq");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "DetailActivity") == 0);
      ccb_str_init(&tree);
      pv_render_logic_tree(&tree, p);
      CHECK(ccb_test_contains(tree.buf, "Extra [graph]"));
      ccb_str_free(&tree);
      ccb_str_init(&json);
      pv_project_json(&json, p);
      CHECK(ccb_test_contains(json.buf, "\"graph_only\":true"));
      CHECK(ccb_test_contains(json.buf, "\"name\":\"Extra\""));
      ccb_str_free(&json);
      // Walk: following the suffix-matched "Open" edge lands on
      // the Detail screen, not on an unknown target.
      CHECK(pv_walk_start(&w, p, "Home") == 0);
      for (k = 0; k < home->nedges; k++) {
        const pv_edge *sel = pv_walk_selected_edge(&w);
        if (sel != NULL && sel->from != NULL &&
            strcmp(sel->from, "Open") == 0) {
          break;
        }
        pv_walk_apply_key(&w, PV_WALK_KEY_DOWN);
      }
      pv_walk_apply_key(&w, PV_WALK_KEY_ENTER);
      CHECK_STR_EQ(pv_walk_current_name(&w), "Detail");
      pv_walk_free(&w);
      pv_project_free(p);
    }
    free(proj);
    free(toml);
    free(flowdir);
    free(stylesdir);
    free(navdir);
    free(srcdir);
  }

  // (4) Fallback discovery end-to-end: values-dir resources plus
  // a set res root, with no layout/nav_graph keys, must still
  // find the layout onClick and the nav-graph destination.
  {
    char scratch[64];
    ccb_test_tmpdir(scratch, sizeof scratch, "ca");
    char *proj = NULL;
    char *toml = NULL;
    char *flowdir = NULL;
    char *stylesdir = NULL;
    char *layoutdir = NULL;
    char *navdir = NULL;
    char *valuesdir = NULL;
    char *srcdir = NULL;
    pv_project *p = NULL;
    pv_screen *home = NULL;
    pv_edge *e = NULL;
    long errors = 0;
    proj = join2(scratch, "proj");
    flowdir = join2(proj, "flow");
    stylesdir = join2(proj, "styles");
    layoutdir = join2(proj, "res/layout");
    navdir = join2(proj, "res/navigation");
    valuesdir = join2(proj, "res/values");
    srcdir = join2(proj, "src/main");
    mkdir_one(proj);
    mkdir_one(flowdir);
    mkdir_one(stylesdir);
    mkdir_joined(proj, "res");
    mkdir_one(layoutdir);
    mkdir_one(navdir);
    mkdir_one(valuesdir);
    mkdir_joined(proj, "src");
    mkdir_one(srcdir);
    toml = join2(proj, "ccb.toml");
    CHECK(write_all(toml,
      "ccb_version = 1\n"
      "[project]\n"
      "name = \"chunk-a-fb\"\n"
      "package = \"com.example.fb\"\n"
      "version = \"1.0\"\n"
      "version_code = 1\n"
      "min_sdk = 26\n"
      "target_sdk = 35\n"
      "compile_sdk = 35\n"
      "[preview]\n"
      "styles = \"styles\"\n"
      "flow = \"flow\"\n"
      "resources = \"res/values\"\n"
      "phone_width = 390\n"
      "phone_height = 844\n"
      "[preview.bindings]\n"
      "detailButton = \"Open detail\"\n"
      "gButton = \"Graph\"\n"
      "[paths]\n"
      "res = \"res\"\n"
      "manifest = \"src/main/AndroidManifest.xml\"\n") == 0);
    CHECK(write_joined(flowdir, "Home.kt",
      "fun Home() {\n"
      "  Column { Button(\"Open detail\") Button(\"Graph\") }\n"
      "  gButton.setOnClickListener {\n"
      "    navController.navigate(R.id.detail)\n"
      "  }\n"
      "}\n"
      "fun openDetail(v: View) { nav(\"Detail\") }\n"
      "fun Detail() {\n"
      "  Column { Text(\"Detail\") }\n"
      "}\n") == 0);
    CHECK(write_joined(layoutdir, "activity_main.xml",
      "<LinearLayout "
      "xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
      "  <Button android:id=\"@+id/detailButton\" "
      "android:onClick=\"openDetail\" />\n"
      "</LinearLayout>\n") == 0);
    CHECK(write_joined(navdir, "nav_graph.xml",
      "<navigation "
      "xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
      "  <fragment android:id=\"@+id/detail\" "
      "android:name=\"Detail\" />\n"
      "</navigation>\n") == 0);
    CHECK(write_joined(valuesdir, "strings.xml",
      "<resources><string name=\"app_name\">Fallback</string>"
      "</resources>\n") == 0);
    CHECK(write_joined(srcdir, "AndroidManifest.xml",
      "<manifest />\n") == 0);
    CHECK(write_joined(stylesdir, "common.css",
      "screen { padding: 0px; }\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(p->nscreens == 2);
      CHECK(errors == 0);
      CHECK(diag_count(p, "CCB-P1005") == 0);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      // The layout fallback supplied the onClick edge; the
      // graph fallback supplied the navigate(R.id.detail) edge.
      e = find_edge(home, "Open detail");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Detail") == 0);
      e = find_edge(home, "Graph");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Detail") == 0);
      pv_project_free(p);
    }
    free(proj);
    free(toml);
    free(flowdir);
    free(stylesdir);
    free(layoutdir);
    free(navdir);
    free(valuesdir);
    free(srcdir);
  }

  // (5) P1 repro, as a test: the pristine project and the same
  // project with only the layout + nav_graph keys stripped must
  // produce the same tree, with no P1005 and no errors.
  {
    char *p1 = p1_dir();
    char *p1_toml = NULL;
    char scratch[64];
    ccb_test_tmpdir(scratch, sizeof scratch, "ca");
    char *copy = NULL;
    char *copy_toml = NULL;
    char *cap = NULL;
    pv_project *pa = NULL;
    pv_project *pb = NULL;
    ccb_str json_a;
    ccb_str json_b;
    long errors_a = 0;
    long errors_b = 0;
    CHECK(p1 != NULL);
    if (p1 != NULL) {
      p1_toml = join2(p1, "ccb.toml");
      copy = join2(scratch, "p1");
      copy_toml = join2(copy, "ccb.toml");
      cap = join2(scratch, "cap.txt");
      CHECK(copy_tree(p1, copy) == 0);
      pa = load_project(p1_toml);
      CHECK(pa != NULL);
      if (pa != NULL) {
        errors_a = pv_project_diagnose(pa);
        CHECK(pa->nscreens == 4);
        CHECK(errors_a == 0);
        CHECK(diag_count(pa, "CCB-P1005") == 0);
        ccb_str_init(&json_a);
        pv_project_json(&json_a, pa);
      } else {
        ccb_str_init(&json_a);
      }
      CHECK(strip_layout_nav_keys(copy_toml) == 0);
      pb = load_project(copy_toml);
      CHECK(pb != NULL);
      if (pb != NULL) {
        errors_b = pv_project_diagnose(pb);
        CHECK(pb->nscreens == 4);
        CHECK(errors_b == 0);
        CHECK(diag_count(pb, "CCB-P1005") == 0);
        ccb_str_init(&json_b);
        pv_project_json(&json_b, pb);
      } else {
        ccb_str_init(&json_b);
      }
      if (pa != NULL && pb != NULL) {
        CHECK(json_a.buf != NULL && json_b.buf != NULL &&
          strcmp(json_a.buf, json_b.buf) == 0);
        pv_project_free(pa);
        pv_project_free(pb);
      }
      ccb_str_free(&json_a);
      ccb_str_free(&json_b);
      {
        char *argv[] = { "ccb", "preview", "--json", NULL };
        char *out = NULL;
        CHECK(run_cli(copy, cap, 3, argv) == 0);
        out = ccb_test_try_read(cap);
        CHECK(ccb_test_contains(out, "\"name\":\"Home\""));
        CHECK(ccb_test_contains(out, "\"name\":\"Detail\""));
        free(out);
      }
      free(p1_toml);
      free(copy);
      free(copy_toml);
      free(cap);
    }
    free(p1);
  }

  TEST_MAIN_END();
}
