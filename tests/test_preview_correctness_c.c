// Tests for Chunk C of the preview correctness plan (Phase 1):
// no-silence (CCB-P1025 unattributed-nav warning via a post-parse
// sweep over attributed call sites), screen candidacy by reference
// counting through the whole-project index, P1017 precision (known
// size only), and regression goldens for the real-world projects.
// All paths derive from __FILE__; scratch tests never depend on
// the working directory.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cli.h"
#include "ccb_config.h"
#include "ccb_preview.h"
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- helpers (same shapes as the Chunk B test) ----------

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

static int write_joined(const char *dir, const char *name,
    const char *text) {
  char *path = join2(dir, name);
  int rc = write_all(path, text);
  free(path);
  return rc;
}

static char *try_read(const char *path) {
  FILE *f = fopen(path, "r");
  ccb_str s;
  char buf[4096];
  size_t n = 0;
  if (f == NULL) {
    return NULL;
  }
  ccb_str_init(&s);
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    ccb_str_append_n(&s, buf, n);
  }
  fclose(f);
  return s.buf;
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
      char *text = try_read(sp);
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

// Build a scratch project dir with a flow/ subdir and a minimal
// ccb.toml carrying the given [preview.bindings] text (NULL for
// none). Returns the project dir; tests join flow/ and ccb.toml
// onto it themselves.
static char *mk_proj(const char *bindings) {
  char scratch[64];
  ccb_test_tmpdir(scratch, sizeof scratch, "cc");
  char *proj = NULL;
  char *flow = NULL;
  char *toml = NULL;
  ccb_str s;
  proj = join2(scratch, "proj");
  flow = join2(proj, "flow");
  toml = join2(proj, "ccb.toml");
  mkdir_one(proj);
  mkdir_one(flow);
  {
    char *src = join2(proj, "src");
    char *main = join2(src, "main");
    char *man = join2(main, "AndroidManifest.xml");
    mkdir_one(src);
    mkdir_one(main);
    write_all(man,
      "<manifest xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  package=\"com.example.app\">\n"
      "  <application>\n"
      "  </application>\n"
      "</manifest>\n");
    free(src);
    free(main);
    free(man);
  }
  ccb_str_init(&s);
  ccb_str_append(&s,
    "ccb_version = 1\n"
    "[project]\n"
    "name = \"chunk-c\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "[preview]\n"
    "flow = \"flow\"\n"
    "phone_width = 390\n"
    "phone_height = 844\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n");
  if (bindings != NULL) {
    ccb_str_append(&s, "[preview.bindings]\n");
    ccb_str_append(&s, bindings);
  }
  write_all(toml, s.buf != NULL ? s.buf : "");
  ccb_str_free(&s);
  free(flow);
  free(toml);
  return proj;
}

static char *real_world_dir(void) {
  char *td = test_dir();
  char *cand = join2(td, "../examples/real-world");
  struct stat st;
  if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
    free(td);
    return cand;
  }
  free(cand);
  free(td);
  cand = ccb_xstrdup("examples/real-world");
  if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
    return cand;
  }
  free(cand);
  return NULL;
}

// Golden files live under tests/golden/preview/real-world/.
// Probe the same prefixes the scenario test uses.
static char *read_golden(const char *rel) {
  static const char *suffixes[] = {
    "/golden/preview/",
    "/../tests/golden/preview/",
  };
  size_t si = 0;
  for (si = 0; si < 2; si++) {
    char *td = test_dir();
    ccb_str path;
    char *out = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, td);
    ccb_str_append(&path, suffixes[si]);
    ccb_str_append(&path, rel);
    out = try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
    free(td);
    if (out != NULL) {
      return out;
    }
  }
  {
    ccb_str path;
    char *out = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, "tests/golden/preview/");
    ccb_str_append(&path, rel);
    out = try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
    if (out != NULL) {
      return out;
    }
  }
  {
    ccb_str path;
    char *out = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, "../tests/golden/preview/");
    ccb_str_append(&path, rel);
    out = try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
    if (out != NULL) {
      return out;
    }
  }
  return NULL;
}

// Replace every occurrence of `from` in `text` with `to`.
// Returned string is malloc'd via ccb_str (caller frees it).
static char *replace_all(const char *text, const char *from,
    const char *to) {
  ccb_str out;
  const char *p = NULL;
  size_t flen = 0;
  if (text == NULL || from == NULL || from[0] == '\0' ||
      to == NULL) {
    return ccb_xstrdup(text != NULL ? text : "");
  }
  flen = strlen(from);
  ccb_str_init(&out);
  p = text;
  while (*p != '\0') {
    const char *hit = strstr(p, from);
    if (hit == NULL) {
      ccb_str_append(&out, p);
      break;
    }
    ccb_str_append_n(&out, p, (size_t)(hit - p));
    ccb_str_append(&out, to);
    p = hit + flen;
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

static void check_golden_file(const char *label, const char *actual,
    const char *golden_rel) {
  char *expected = read_golden(golden_rel);
  CHECK(expected != NULL);
  if (expected == NULL) {
    fprintf(stderr, "FAIL golden %s: cannot read %s\n", label,
      golden_rel);
    return;
  }
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "FAIL golden %s: mismatch with %s\n", label,
      golden_rel);
  }
  CHECK(strcmp(actual, expected) == 0);
  free(expected);
}

// Parse CSS, lay out a screen whose root has one child of the given
// type/label, and return the laid-out child (project + diag list
// live in the caller's arena; the caller frees p->diags). Same shape
// as the lint test's layout_one.
static pv_box *new_box(ccb_arena *a, const char *type,
    const char *label) {
  pv_box *b = ccb_arena_alloc(a, sizeof(*b));
  memset(b, 0, sizeof(*b));
  b->type = type != NULL ? ccb_arena_strdup(a, type) : NULL;
  b->label = label != NULL ? ccb_arena_strdup(a, label) : NULL;
  return b;
}

static void add_child(ccb_arena *a, pv_box *parent, pv_box *child) {
  pv_box **arr = ccb_arena_alloc(a,
    (parent->nchildren + 1) * sizeof(pv_box *));
  if (parent->nchildren > 0) {
    memcpy(arr, parent->children,
      parent->nchildren * sizeof(pv_box *));
  }
  arr[parent->nchildren] = child;
  parent->children = arr;
  parent->nchildren++;
  parent->capchildren = parent->nchildren;
}

static pv_box *layout_one(ccb_arena *a, const char *css,
    const char *type, const char *label, pv_project *p) {
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
  screen = ccb_arena_alloc(a, sizeof(*screen));
  memset(screen, 0, sizeof(*screen));
  screen->name = ccb_arena_strdup(a, "S");
  screen->root = root;
  pv_layout_screen(p, screen);
  return child;
}

static int lint_count(pv_project *p, const char *code) {
  size_t i = 0;
  int n = 0;
  for (i = 0; i < p->diags.count; i++) {
    if (strcmp(p->diags.items[i].code, code) == 0) {
      n++;
    }
  }
  return n;
}

int main(void) {
  // (1) Nested-block nav warns CCB-P1025, records no edge: the
  // P3 Nested shape (nav inside a runLater block).
  {
    char *proj = mk_proj("nestedButton = \"Nested\"\n");
    char *flow = NULL;
    char *toml = NULL;
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(proj != NULL);
    flow = join2(proj, "flow");
    toml = join2(proj, "ccb.toml");
    CHECK(write_joined(flow, "Home.kt",
      "fun Home() {\n"
      "  Column { Button(\"Nested\") }\n"
      "  nestedButton.setOnClickListener { runLater { nav(\"Detail\") } }\n"
      "}\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      CHECK(find_edge(home, "Nested") == NULL);
      CHECK(diag_count(p, "CCB-P1025") == 1);
      CHECK(diag_message_contains(p, "CCB-P1025", "unattributed"));
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }
  // (2) Depth-2 helper nav warns: mid() is followed one level from
  // the button, leaf() inside mid() is not, so leaf()'s nav is
  // unattributed — warned, never invented as an edge. Neither
  // helper is a screen (mid via the lambda, leaf via the fixed
  // point over mid's body).
  {
    char *proj = mk_proj("deepButton = \"Deep\"\n");
    char *flow = NULL;
    char *toml = NULL;
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(proj != NULL);
    flow = join2(proj, "flow");
    toml = join2(proj, "ccb.toml");
    CHECK(write_joined(flow, "Home.kt",
      "fun Home() {\n"
      "  Column { Button(\"Deep\") }\n"
      "  deepButton.setOnClickListener { mid() }\n"
      "}\n"
      "fun mid() { leaf() }\n"
      "fun leaf() { nav(\"Detail\") }\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      CHECK(find_edge(home, "Deep") == NULL);
      CHECK(find_screen(p, "mid") == NULL);
      CHECK(find_screen(p, "leaf") == NULL);
      CHECK(diag_count(p, "CCB-P1025") == 1);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }
  // (3) Handler-referencing-handler nav warns: h1 is scanned from
  // the button, but its setOnClickListener(::h2) attachment is not
  // a scan position, so h2 is never scanned and h2's nav is
  // unattributed. Both funs are handlers, not screens (h2 via the
  // :: reference from h1's body).
  {
    char *proj = mk_proj("goButton = \"Go\"\n");
    char *flow = NULL;
    char *toml = NULL;
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(proj != NULL);
    flow = join2(proj, "flow");
    toml = join2(proj, "ccb.toml");
    CHECK(write_joined(flow, "Home.kt",
      "fun Home() {\n"
      "  Column { Button(\"Go\") }\n"
      "  goButton.setOnClickListener(::h1)\n"
      "}\n"
      "fun h1(v: View) {\n"
      "  innerButton.setOnClickListener(::h2)\n"
      "}\n"
      "fun h2(v: View) { nav(\"Detail\") }\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      CHECK(find_edge(home, "Go") == NULL);
      CHECK(find_screen(p, "h1") == NULL);
      CHECK(find_screen(p, "h2") == NULL);
      CHECK(diag_count(p, "CCB-P1025") == 1);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }
  // (4) Candidacy: helper, val-held, and cross-file funs are not
  // screens, and their edges still resolve to the buttons.
  {
    char *proj = mk_proj(
      "helperButton = \"Helper nav\"\n"
      "crossButton = \"Cross file\"\n"
      "varButton = \"Var ref\"\n");
    char *flow = NULL;
    char *toml = NULL;
    pv_project *p = NULL;
    pv_screen *home = NULL;
    pv_edge *e = NULL;
    CHECK(proj != NULL);
    flow = join2(proj, "flow");
    toml = join2(proj, "ccb.toml");
    CHECK(write_joined(flow, "Home.kt",
      "fun Home() {\n"
      "  Column {\n"
      "    Button(\"Helper nav\")\n"
      "    Button(\"Cross file\")\n"
      "    Button(\"Var ref\")\n"
      "  }\n"
      "  helperButton.setOnClickListener { openDetail() }\n"
      "  crossButton.setOnClickListener(::onCrossClick)\n"
      "  val h = ::onVarClick\n"
      "  varButton.setOnClickListener(h)\n"
      "}\n"
      "fun openDetail() { nav(\"Detail\") }\n"
      "fun onVarClick(v: View) { nav(\"Form\") }\n") == 0);
    CHECK(write_joined(flow, "Other.kt",
      "fun onCrossClick(v: View) { nav(\"Form\") }\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      e = find_edge(home, "Helper nav");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Detail") == 0);
      e = find_edge(home, "Cross file");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Form") == 0);
      e = find_edge(home, "Var ref");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Form") == 0);
      CHECK(find_screen(p, "openDetail") == NULL);
      CHECK(find_screen(p, "onVarClick") == NULL);
      CHECK(find_screen(p, "onCrossClick") == NULL);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }
  // (5) Screen composition is preserved: a screen fun called as a
  // component from another screen's UI body stays a screen. The
  // candidacy pass only counts listener-lambda and handler-body
  // calls, never UI-body composition.
  {
    char *proj = mk_proj(NULL);
    char *flow = NULL;
    char *toml = NULL;
    pv_project *p = NULL;
    pv_screen *detail = NULL;
    CHECK(proj != NULL);
    flow = join2(proj, "flow");
    toml = join2(proj, "ccb.toml");
    CHECK(write_joined(flow, "Home.kt",
      "fun Home() {\n"
      "  Column {\n"
      "    Text(\"Home\")\n"
      "    Detail()\n"
      "  }\n"
      "}\n"
      "fun Detail() {\n"
      "  Column { Text(\"Detail\") }\n"
      "}\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      detail = find_screen(p, "Detail");
      CHECK(detail != NULL);
      CHECK(find_screen(p, "Home") != NULL);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }
  // (6) Ambiguous handler navs are reported by P1024, not repeated
  // as P1025: the reference names the ambiguity, so the bodies'
  // navs are accounted for, not silent and not double-reported.
  {
    char *proj = mk_proj("goButton = \"Go\"\n");
    char *flow = NULL;
    char *toml = NULL;
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(proj != NULL);
    flow = join2(proj, "flow");
    toml = join2(proj, "ccb.toml");
    CHECK(write_joined(flow, "Home.kt",
      "fun Home() {\n"
      "  Column { Button(\"Go\") }\n"
      "  goButton.setOnClickListener(::shared)\n"
      "}\n") == 0);
    CHECK(write_joined(flow, "SharedA.kt",
      "fun shared(v: View) { nav(\"Detail\") }\n"
      "fun Detail() {\n"
      "  Column { Text(\"Detail\") }\n"
      "}\n") == 0);
    CHECK(write_joined(flow, "SharedB.kt",
      "fun shared(v: View) { nav(\"Form\") }\n"
      "fun Form() {\n"
      "  Column { Text(\"Form\") }\n"
      "}\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      CHECK(find_edge(home, "Go") == NULL);
      CHECK(diag_count(p, "CCB-P1024") == 1);
      CHECK(diag_count(p, "CCB-P1025") == 0);
      CHECK(find_screen(p, "shared") == NULL);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }
  // (7) P1017 precision: height without font-size is the 1-row
  // default, not a known measurement, so it never warns on its
  // own; a known-small width still warns, and a known-small height
  // (small font-size) still warns.
  {
    CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
    pv_project p;
    pv_box *child = NULL;
    // No rules at all: 38x1, height unknown -> quiet.
    child = layout_one(a, "", "button", "Go", &p);
    CHECK(child->w == 38 && child->h == 1);
    CHECK(lint_count(&p, "CCB-P1017") == 0);
    ccb_diag_list_free(&p.diags);
    // Explicit small width: 4x1, width known -> warns.
    child = layout_one(a, "button { width: 39px; }", "button", "Go",
      &p);
    CHECK(child->w == 4 && child->h == 1);
    CHECK(lint_count(&p, "CCB-P1017") == 1);
    ccb_diag_list_free(&p.diags);
    // Small font-size: 10px -> 1 row, height known -> warns.
    child = layout_one(a, "button { font-size: 10px; }", "button",
      "Go", &p);
    CHECK(child->h == 1);
    CHECK(lint_count(&p, "CCB-P1017") == 1);
    ccb_diag_list_free(&p.diags);
    // Normal font-size: 16px -> 2 rows -> quiet.
    child = layout_one(a, "button { font-size: 16px; }", "button",
      "Go", &p);
    CHECK(child->h == 2);
    CHECK(lint_count(&p, "CCB-P1017") == 0);
    ccb_diag_list_free(&p.diags);
  }
  // (8) Regression goldens: every real-world project's preview JSON
  // + logic tree, pinned. Paths inside diagnostics are normalized
  // to PROJ so the goldens are independent of the scratch copy.
  {
    static const char *names[] = {
      "p0-init-smoke", "p1-notes", "p2-catalog", "p3-messy",
    };
    char *rw = real_world_dir();
    size_t ni = 0;
    if (rw == NULL) {
      fprintf(stderr, "SKIP goldens: real-world dir not found\n");
    }
    for (ni = 0; rw != NULL && ni < 4; ni++) {
      char scratch[64];
      ccb_test_tmpdir(scratch, sizeof scratch, "cc");
      char *src = NULL;
      char *copy = NULL;
      char *copy_toml = NULL;
      pv_project *p = NULL;
      src = join2(rw, names[ni]);
      copy = join2(scratch, names[ni]);
      copy_toml = join2(copy, "ccb.toml");
      CHECK(copy_tree(src, copy) == 0);
      p = load_project(copy_toml);
      CHECK(p != NULL);
      if (p != NULL) {
        ccb_str json;
        ccb_str tree;
        ccb_str rel;
        char *norm_json = NULL;
        char *norm_tree = NULL;
        (void)pv_project_diagnose(p);
        ccb_str_init(&json);
        pv_project_json(&json, p);
        ccb_str_init(&tree);
        pv_render_logic_tree(&tree, p);
        norm_json = replace_all(json.buf != NULL ? json.buf : "",
          copy, "PROJ");
        norm_tree = replace_all(tree.buf != NULL ? tree.buf : "",
          copy, "PROJ");
        ccb_str_init(&rel);
        ccb_str_append(&rel, "real-world/");
        ccb_str_append(&rel, names[ni]);
        ccb_str_append(&rel, "/preview.json");
        check_golden_file(names[ni], norm_json, rel.buf);
        ccb_str_free(&rel);
        ccb_str_init(&rel);
        ccb_str_append(&rel, "real-world/");
        ccb_str_append(&rel, names[ni]);
        ccb_str_append(&rel, "/logic-tree.txt");
        check_golden_file(names[ni], norm_tree, rel.buf);
        ccb_str_free(&rel);
        free(norm_json);
        free(norm_tree);
        ccb_str_free(&json);
        ccb_str_free(&tree);
        pv_project_free(p);
      }
      free(src);
      free(copy);
      free(copy_toml);
    }
    free(rw);
  }
  TEST_MAIN_END();
}
