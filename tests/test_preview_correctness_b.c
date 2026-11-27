// Tests for Chunk B of the preview correctness plan (Phase 1):
// whole-project function index: cross-file :: references, one-level
// helper inlining (with a terminating cycle case), local val-held
// references, ambiguous-name warnings, and a P3-messy-style project
// showing previously-missing edges now resolved. All paths derive
// from __FILE__; scratch tests never depend on the working
// directory.
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
  fflush(stdout);
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

// Build a scratch project dir with a flow/ subdir and a minimal
// ccb.toml carrying the given [preview.bindings] text (NULL for
// none). Returns the project dir; tests join flow/ and ccb.toml
// onto it themselves.
static char *mk_proj(const char *bindings) {
  char scratch[64];
  ccb_test_tmpdir(scratch, sizeof scratch, "cb");
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
    "name = \"chunk-b\"\n"
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

static char *p3_dir(void) {
  char *td = test_dir();
  char *cand = join2(td, "../examples/real-world/p3-messy");
  struct stat st;
  if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
    free(td);
    return cand;
  }
  free(cand);
  free(td);
  cand = ccb_xstrdup("examples/real-world/p3-messy");
  if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
    return cand;
  }
  free(cand);
  return NULL;
}

int main(void) {
  // (1) Cross-file :: reference resolves through the index.
  {
    char *proj = mk_proj("crossButton = \"Cross\"\n");
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
      "    Button(\"Cross\")\n"
      "  }\n"
      "  crossButton.setOnClickListener(::onCrossClick)\n"
      "}\n"
      "fun Form() {\n"
      "  Column { Text(\"Form\") }\n"
      "}\n") == 0);
    CHECK(write_joined(flow, "Other.kt",
      "fun onCrossClick(v: View) { nav(\"Form\") }\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      e = find_edge(home, "Cross");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Form") == 0);
      CHECK(diag_count(p, "CCB-P1024") == 0);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }

  // (2) Helper inlining: same-file, cross-file, depth cap, and
  // branch propagation through a followed helper.
  {
    char *proj = mk_proj(
      "helpButton = \"Help\"\n"
      "xhelpButton = \"XHelp\"\n"
      "deepButton = \"Deep\"\n"
      "condButton = \"Cond\"\n");
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
      "    Button(\"Help\")\n"
      "    Button(\"XHelp\")\n"
      "    Button(\"Deep\")\n"
      "    Button(\"Cond\")\n"
      "  }\n"
      "  helpButton.setOnClickListener { openDetail() }\n"
      "  xhelpButton.setOnClickListener { openForm() }\n"
      "  deepButton.setOnClickListener { mid() }\n"
      "  condButton.setOnClickListener {\n"
      "    if (ready) { openDetail() }\n"
      "  }\n"
      "}\n"
      "fun openDetail() { nav(\"Detail\") }\n"
      "fun mid() { leaf() }\n"
      "fun leaf() { nav(\"Detail\") }\n"
      "fun Detail() {\n"
      "  Column { Text(\"Detail\") }\n"
      "}\n"
      "fun Form() {\n"
      "  Column { Text(\"Form\") }\n"
      "}\n") == 0);
    CHECK(write_joined(flow, "Other.kt",
      "fun openForm() { nav(\"Form\") }\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      e = find_edge(home, "Help");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Detail") == 0);
      e = find_edge(home, "XHelp");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Form") == 0);
      // Depth cap: mid() -> leaf() is a second level, not followed.
      CHECK(find_edge(home, "Deep") == NULL);
      // Branch annotation propagates through the followed helper.
      e = find_edge(home, "Cond");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Detail") == 0);
      CHECK(e != NULL && e->branch != NULL &&
        ccb_test_contains(e->branch, "if ready"));
      CHECK(diag_count(p, "CCB-P1024") == 0);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }

  // (3) Helper cycles terminate: self-recursion keeps its own nav,
  // mutual recursion yields nothing but still terminates.
  {
    char *proj = mk_proj(
      "loopButton = \"Loop\"\n"
      "mutButton = \"Mut\"\n");
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
      "    Button(\"Loop\")\n"
      "    Button(\"Mut\")\n"
      "  }\n"
      "  loopButton.setOnClickListener { loop() }\n"
      "  mutButton.setOnClickListener { ping() }\n"
      "}\n"
      "fun loop() { nav(\"Detail\"); loop() }\n"
      "fun ping() { pong() }\n"
      "fun pong() { ping() }\n"
      "fun Detail() {\n"
      "  Column { Text(\"Detail\") }\n"
      "}\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      e = find_edge(home, "Loop");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Detail") == 0);
      CHECK(find_edge(home, "Mut") == NULL);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }

  // (4) Val-held references: plain val and var resolve; the typed
  // form is a documented limitation and resolves nothing; the
  // onClick = h property form resolves too.
  {
    char *proj = mk_proj(
      "varButton = \"Var\"\n"
      "var2Button = \"Var2\"\n"
      "typedButton = \"Typed\"\n"
      "propButton = \"Prop\"\n");
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
      "    Button(\"Var\")\n"
      "    Button(\"Var2\")\n"
      "    Button(\"Typed\")\n"
      "    Button(\"Prop\")\n"
      "  }\n"
      "  val h = ::onVarClick\n"
      "  varButton.setOnClickListener(h)\n"
      "  var h2 = ::onVarClick\n"
      "  var2Button.setOnClickListener(h2)\n"
      "  val ht: Handler = ::onVarClick\n"
      "  typedButton.setOnClickListener(ht)\n"
      "  val hp = ::onVarClick\n"
      "  propButton.onClick = hp\n"
      "}\n"
      "fun onVarClick(v: View) { nav(\"Form\") }\n"
      "fun Form() {\n"
      "  Column { Text(\"Form\") }\n"
      "}\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      e = find_edge(home, "Var");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Form") == 0);
      e = find_edge(home, "Var2");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Form") == 0);
      // Typed form: not the plain shape, so no edge (documented).
      CHECK(find_edge(home, "Typed") == NULL);
      e = find_edge(home, "Prop");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Form") == 0);
      CHECK(diag_count(p, "CCB-P1024") == 0);
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }

  // (5) Ambiguity: same name in two files warns CCB-P1024 once and
  // records nothing, for both :: refs and helper calls. Same-file
  // shadows cross-file; two definitions in one file are not
  // ambiguous (distinct files are what count).
  {
    char *proj = mk_proj(
      "dupButton = \"Dup\"\n"
      "dup2Button = \"Dup2\"\n"
      "callButton = \"Call\"\n"
      "shadowButton = \"Shadow\"\n"
      "twiceButton = \"Twice\"\n");
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
      "    Button(\"Dup\")\n"
      "    Button(\"Dup2\")\n"
      "    Button(\"Call\")\n"
      "    Button(\"Shadow\")\n"
      "    Button(\"Twice\")\n"
      "  }\n"
      "  dupButton.setOnClickListener(::onDup)\n"
      "  dup2Button.setOnClickListener(::onDup)\n"
      "  callButton.setOnClickListener { onDup() }\n"
      "  shadowButton.setOnClickListener(::onShadow)\n"
      "  twiceButton.setOnClickListener(::onTwice)\n"
      "}\n"
      "fun onShadow(v: View) { nav(\"Detail\") }\n"
      "fun onTwice(v: View) { nav(\"Detail\") }\n"
      "fun onTwice(v: View) { nav(\"Form\") }\n"
      "fun Detail() {\n"
      "  Column { Text(\"Detail\") }\n"
      "}\n"
      "fun Form() {\n"
      "  Column { Text(\"Form\") }\n"
      "}\n") == 0);
    CHECK(write_joined(flow, "A.kt",
      "fun onDup(v: View) { nav(\"Detail\") }\n"
      "fun onShadow(v: View) { nav(\"Form\") }\n") == 0);
    CHECK(write_joined(flow, "B.kt",
      "fun onDup(v: View) { nav(\"Form\") }\n") == 0);
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      CHECK(find_edge(home, "Dup") == NULL);
      CHECK(find_edge(home, "Dup2") == NULL);
      CHECK(find_edge(home, "Call") == NULL);
      // Same-file wins over the cross-file onShadow in A.kt.
      e = find_edge(home, "Shadow");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Detail") == 0);
      // Two definitions in one file: first wins, not ambiguous.
      e = find_edge(home, "Twice");
      CHECK(e != NULL && e->target != NULL &&
        strcmp(e->target, "Detail") == 0);
      // One warning per name, not per reference: onDup is
      // referenced three times but warns once.
      CHECK(diag_count(p, "CCB-P1024") == 1);
      CHECK(diag_message_contains(p, "CCB-P1024", "onDup"));
      CHECK(diag_message_contains(p, "CCB-P1024", "2 flow files"));
      pv_project_free(p);
    }
    free(flow);
    free(toml);
    free(proj);
  }

  // (6) NULL index keeps the old per-file behavior: a cross-file
  // :: reference records no edge and warns nothing.
  {
    ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    char *keys[1];
    char *vals[1];
    size_t i = 0;
    pv_screen *home = NULL;
    pv_edge *e = NULL;
    keys[0] = "crossButton";
    vals[0] = "Cross";
    ccb_diag_list_init(&diags);
    CHECK(pv_flow_parse_full(arena,
      "fun Home() {\n"
      "  Column { Button(\"Cross\") }\n"
      "  crossButton.setOnClickListener(::onCrossClick)\n"
      "}\n"
      "fun Form() {\n"
      "  Column { Text(\"Form\") }\n"
      "}\n",
      "Home.kt", &diags, &screens, &nscreens, keys, vals, 1,
      NULL, NULL, 0, NULL, NULL, 0, NULL, NULL) == CCB_OK);
    for (i = 0; i < nscreens; i++) {
      if (screens[i]->name != NULL &&
          strcmp(screens[i]->name, "Home") == 0) {
        home = screens[i];
      }
    }
    CHECK(home != NULL);
    e = find_edge(home, "Cross");
    CHECK(e == NULL);
    for (i = 0; i < diags.count; i++) {
      CHECK(strcmp(diags.items[i].code, "CCB-P1024") != 0);
    }
    ccb_diag_list_free(&diags);
    ccb_arena_destroy(arena);
  }

  // (7) P3-messy-style project: the real p3-messy tree, where
  // Helper nav, Cross file, and Var ref were missing before
  // Chunk B, now resolve; Nested stays missing (Chunk C's
  // unattributed-nav territory) and Mystery stays P1019.
  {
    char *p3 = p3_dir();
    if (p3 != NULL) {
      char scratch[64];
      ccb_test_tmpdir(scratch, sizeof scratch, "cb");
      char *copy = NULL;
      char *copy_toml = NULL;
      char *cap = NULL;
      pv_project *p = NULL;
      pv_screen *home = NULL;
      pv_edge *e = NULL;
      copy = join2(scratch, "p3-messy");
      copy_toml = join2(copy, "ccb.toml");
      cap = join2(scratch, "out.txt");
      CHECK(copy_tree(p3, copy) == 0);
      p = load_project(copy_toml);
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
        CHECK(find_edge(home, "Nested") == NULL);
        CHECK(find_edge(home, "Mystery") == NULL);
        CHECK(diag_count(p, "CCB-P1019") == 1);
        pv_project_free(p);
      }
      {
        char *argv[] = { "ccb", "preview", "--json", NULL };
        char *out = NULL;
        CHECK(run_cli(copy, cap, 3, argv) == 0);
        out = ccb_test_try_read(cap);
        CHECK(ccb_test_contains(out, "\"from\":\"Helper nav\""));
        CHECK(ccb_test_contains(out, "\"from\":\"Cross file\""));
        CHECK(ccb_test_contains(out, "\"from\":\"Var ref\""));
        free(out);
      }
      free(copy);
      free(copy_toml);
      free(cap);
      free(p3);
    }
  }

  TEST_MAIN_END();
}
