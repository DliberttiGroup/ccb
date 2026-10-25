// Tests for Chunk 2 trace overlay: trace JSON parsing and writing,
// walk recording, trace application (taken marks, positional arg
// fills, branch picks, [not taken] greying, CCB-P1023 warnings),
// scenario+trace composition, trace goldens for the sample app, and
// the CLI flags. All paths derive from __FILE__; the tests never
// depend on the working directory. CLI cases run ccb_cli_main
// in-process after chdir into a scratch copy of the sample project.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_preview_scenario.h"
#include "ccb_preview_trace.h"
#include "ccb_preview_walk.h"
#include "ccb_cli.h"
#include "ccb_str.h"
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- path + file helpers ----------

static void test_dir(ccb_str *out) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  if (slash != NULL && slash != file) {
    ccb_str_append_n(out, file, (size_t)(slash - file));
  } else {
    ccb_str_append(out, ".");
  }
}

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

static int write_all(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return -1;
  }
  if (len > 0 && fwrite(data, 1, len, f) != len) {
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
  out = ccb_xstrdup(s.buf != NULL ? s.buf : "");
  ccb_str_free(&s);
  return out;
}

// Recursively copy a directory tree (sample project -> scratch).
static int copy_tree(const char *src, const char *dst) {
  DIR *d = opendir(src);
  struct dirent *e = NULL;
  if (d == NULL) {
    return -1;
  }
  mkdir(dst, 0777);
  while ((e = readdir(d)) != NULL) {
    char *sp = NULL;
    char *dp = NULL;
    struct stat st;
    int rc = 0;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
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
      char *data = try_read(sp);
      rc = data == NULL ? -1 :
        write_all(dp, data, strlen(data));
      free(data);
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

// Run ccb_cli_main with stdout redirected to cap_path, from workdir.
// Returns the CLI exit code; the caller reads cap_path afterwards.
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
  saved = dup(1);
  if (saved < 0) {
    close(fd);
    { int rc2 = chdir(cwd); (void)rc2; }
    return -1;
  }
  fflush(stdout);
  dup2(fd, 1);
  close(fd);
  rc = ccb_cli_main(argc, argv);
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  { int rc2 = chdir(cwd); (void)rc2; }
  return rc;
}

static int contains(const char *hay, const char *needle) {
  return hay != NULL && needle != NULL && strstr(hay, needle) != NULL;
}

static char *read_first(const char *suffixes[], size_t n,
    const char *rel) {
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

static char *sample_dir(void) {
  static const char *suffixes[] = {
    "/../examples/preview-sample/",
    "/examples/preview-sample/",
  };
  char *out = read_first(suffixes, 2, "");
  if (out != NULL) {
    return out;
  }
  {
    char *probe = try_read("examples/preview-sample/ccb.toml");
    if (probe != NULL) {
      free(probe);
      return ccb_xstrdup("examples/preview-sample/");
    }
  }
  return ccb_xstrdup("../examples/preview-sample/");
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

static void check_golden_file(const char *label, const char *actual,
    const char *golden_rel) {
  char *expected = read_golden(golden_rel);
  CHECK(expected != NULL);
  if (expected == NULL) {
    fprintf(stderr, "FAIL golden %s: cannot read %s\n", label,
      golden_rel);
    return;
  }
  CHECK(actual != NULL && strcmp(actual, expected) == 0);
  if (actual == NULL || strcmp(actual, expected) != 0) {
    fprintf(stderr, "FAIL golden %s: mismatch with %s\n", label,
      golden_rel);
  }
  free(expected);
}

// ---------- project helpers ----------

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

static pv_box *find_box(pv_box *box, const char *type,
    const char *label) {
  size_t i = 0;
  if (box == NULL) {
    return NULL;
  }
  if (box->type != NULL && strcmp(box->type, type) == 0 &&
      box->label != NULL && label != NULL &&
      strcmp(box->label, label) == 0) {
    return box;
  }
  for (i = 0; i < box->nchildren; i++) {
    pv_box *found = find_box(box->children[i], type, label);
    if (found != NULL) {
      return found;
    }
  }
  return NULL;
}

static pv_edge *find_edge(pv_screen *screen, const char *from) {
  size_t i = 0;
  if (screen == NULL) {
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

static long diag_count(ccb_diag_list *diags, const char *code) {
  long n = 0;
  size_t i = 0;
  if (diags == NULL) {
    return 0;
  }
  for (i = 0; i < diags->count; i++) {
    if (diags->items[i].code != NULL &&
        strcmp(diags->items[i].code, code) == 0) {
      n++;
    }
  }
  return n;
}

// ---------- main ----------

int main(void) {
  // --- Parse: a full trace document.
  {
    static const char *text =
      "{\"version\":1,\"steps\":["
      "{\"screen\":\"Home\",\"edge\":\"Open\",\"branch\":null,"
      "\"args\":[\"note-42\",null]},"
      "{\"screen\":\"Detail\",\"edge\":null,\"branch\":\"is Loaded\","
      "\"args\":[]},"
      "{\"screen\":\"Detail\",\"edge\":\"Back\"}]}";
    char *err = NULL;
    pv_trace *t = pv_trace_parse(text, &err);
    CHECK(t != NULL);
    CHECK(err == NULL);
    if (t != NULL) {
      CHECK(t->nsteps == 3);
      if (t->nsteps == 3) {
        CHECK_STR_EQ(t->steps[0].screen, "Home");
        CHECK_STR_EQ(t->steps[0].edge, "Open");
        CHECK(t->steps[0].branch == NULL);
        CHECK(t->steps[0].nargs == 2);
        if (t->steps[0].nargs == 2) {
          CHECK_STR_EQ(t->steps[0].args[0], "note-42");
          CHECK(t->steps[0].args[1] == NULL);
        }
        CHECK_STR_EQ(t->steps[1].screen, "Detail");
        CHECK(t->steps[1].edge == NULL);
        CHECK_STR_EQ(t->steps[1].branch, "is Loaded");
        CHECK(t->steps[1].nargs == 0);
        CHECK_STR_EQ(t->steps[2].screen, "Detail");
        CHECK_STR_EQ(t->steps[2].edge, "Back");
        CHECK(t->steps[2].branch == NULL);
        CHECK(t->steps[2].nargs == 0);
      }
    }
    free(err);
    pv_trace_free(t);
  }
  // --- Parse: missing version is accepted; unknown fields ignored.
  {
    static const char *text =
      "{\"steps\":[{\"screen\":\"Home\",\"edge\":\"Go\",\"extra\":1}]}";
    char *err = NULL;
    pv_trace *t = pv_trace_parse(text, &err);
    CHECK(t != NULL && err == NULL);
    if (t != NULL) {
      CHECK(t->nsteps == 1);
    }
    free(err);
    pv_trace_free(t);
  }
  // --- Parse errors: bad JSON, wrong version, missing steps,
  // non-object step, missing screen, wrong arg type.
  {
    static const char *bad[] = {
      "not json",
      "{\"version\":2,\"steps\":[]}",
      "{\"version\":1}",
      "{\"steps\":[42]}",
      "{\"steps\":[{\"edge\":\"Go\"}]}",
      "{\"steps\":[{\"screen\":\"Home\",\"args\":[1]}]}",
      "{\"steps\":[{\"screen\":\"Home\",\"edge\":3}]}",
      "[{\"screen\":\"Home\"}]",
      NULL,
    };
    int i = 0;
    for (i = 0; bad[i] != NULL; i++) {
      char *err = NULL;
      pv_trace *t = pv_trace_parse(bad[i], &err);
      CHECK(t == NULL);
      CHECK(err != NULL);
      free(err);
      pv_trace_free(t);
    }
  }
  // --- Write + round-trip: the writer's output parses back equal.
  {
    static const char *a0[] = { "note-42", NULL };
    pv_trace_step steps[2];
    ccb_str out;
    char *err = NULL;
    pv_trace *t = NULL;
    memset(steps, 0, sizeof(steps));
    steps[0].screen = "Home";
    steps[0].edge = "Open";
    steps[0].branch = NULL;
    steps[0].args = a0;
    steps[0].nargs = 2;
    steps[1].screen = "Detail";
    steps[1].edge = NULL;
    steps[1].branch = "is Loaded";
    steps[1].args = NULL;
    steps[1].nargs = 0;
    ccb_str_init(&out);
    pv_trace_write(&out, steps, 2);
    CHECK_STR_EQ(out.buf,
      "{\"version\":1,\"steps\":["
      "{\"screen\":\"Home\",\"edge\":\"Open\",\"branch\":null,"
      "\"args\":[\"note-42\",null]},"
      "{\"screen\":\"Detail\",\"edge\":null,\"branch\":\"is Loaded\","
      "\"args\":[]}]}");
    t = pv_trace_parse(out.buf != NULL ? out.buf : "", &err);
    CHECK(t != NULL && err == NULL);
    if (t != NULL) {
      CHECK(t->nsteps == 2);
      if (t->nsteps == 2) {
        CHECK_STR_EQ(t->steps[0].args[0], "note-42");
        CHECK(t->steps[0].args[1] == NULL);
        CHECK_STR_EQ(t->steps[1].branch, "is Loaded");
      }
    }
    free(err);
    pv_trace_free(t);
    ccb_str_free(&out);
  }
  // --- Apply: taken marks, positional fills, branch picks,
  // greying, and static literals untouched.
  {
    static const char *flow_text =
      "fun Home() {\n"
      "  Column {\n"
      "    Button(\"Go\", onClick = { nav(\"Detail\", itemId) })\n"
      "    Button(\"Fixed\", onClick = { nav(\"Detail\", \"lit\") })\n"
      "    Button(\"Multi\", onClick = { nav(\"Detail\", a, b) })\n"
      "  }\n"
      "}\n"
      "fun Detail() {\n"
      "  Column {\n"
      "    Text(\"D\")\n"
      "    when (state) {\n"
      "      is Loading -> Text(\"Wait\")\n"
      "      is Loaded -> Text(\"Body\")\n"
      "    }\n"
      "    Button(\"Back\", onClick = { nav(\"Home\", currentId) })\n"
      "  }\n"
      "}\n";
    static const char *trace_text =
      "{\"version\":1,\"steps\":["
      "{\"screen\":\"Home\",\"edge\":\"Go\",\"branch\":null,"
      "\"args\":[\"v1\"]},"
      "{\"screen\":\"Detail\",\"edge\":null,\"branch\":\"is Loaded\","
      "\"args\":[]},"
      "{\"screen\":\"Detail\",\"edge\":\"Back\",\"branch\":null,"
      "\"args\":[\"42\"]}]}";
    ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    pv_project p;
    pv_trace *t = NULL;
    char *err = NULL;
    ccb_diag_list_init(&diags);
    CHECK(pv_flow_parse(arena, flow_text, "T.kt", &diags,
      &screens, &nscreens) == CCB_OK);
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.screens = screens;
    p.nscreens = nscreens;
    t = pv_trace_parse(trace_text, &err);
    CHECK(t != NULL && err == NULL);
    free(err);
    pv_trace_apply(&p, t, &diags);
    CHECK(p.trace_applied == 1);
    CHECK(p.trace_step_count == 3);
    CHECK(diag_count(&diags, "CCB-P1023") == 0);
    {
      pv_screen *home = find_screen(&p, "Home");
      pv_screen *detail = find_screen(&p, "Detail");
      pv_edge *go = find_edge(home, "Go");
      pv_edge *fixed = find_edge(home, "Fixed");
      pv_edge *back = find_edge(detail, "Back");
      pv_box *loaded = find_box(detail->root, "branch-entry",
        "is Loaded");
      pv_box *loading = find_box(detail->root, "branch-entry",
        "is Loading");
      CHECK(go != NULL && go->trace_taken == 1);
      if (go != NULL && go->nargs == 1) {
        CHECK_STR_EQ(go->args[0], "v1");
        CHECK(go->args_trace != NULL && go->args_trace[0] == 1);
        CHECK(go->args_scenario == NULL ||
          go->args_scenario[0] == 0);
      }
      CHECK(back != NULL && back->trace_taken == 1);
      if (back != NULL && back->nargs == 1) {
        CHECK_STR_EQ(back->args[0], "42");
        CHECK(back->args_trace != NULL && back->args_trace[0] == 1);
      }
      CHECK(fixed != NULL && fixed->trace_taken == 0);
      if (fixed != NULL && fixed->nargs == 1) {
        CHECK_STR_EQ(fixed->args[0], "lit");
        CHECK(fixed->args_trace == NULL ||
          fixed->args_trace[0] == 0);
      }
      CHECK(loaded != NULL && loaded->trace_picked == 1);
      CHECK(loading != NULL && loading->trace_picked == 0);
    }
    {
      ccb_str out;
      ccb_str_init(&out);
      pv_render_logic_tree(&out, &p);
      CHECK(contains(out.buf,
        "-> Detail(\"v1\" [trace]) via \"Go\" [trace]"));
      CHECK(contains(out.buf,
        "branches: when state [is Loading [not taken], "
        "is Loaded [trace]]"));
      CHECK(contains(out.buf,
        "-> Home(\"42\" [trace]) via \"Back\" [trace]"));
      CHECK(contains(out.buf,
        "-> Detail(\"lit\") via \"Fixed\""));
      ccb_str_free(&out);
    }
    {
      ccb_str out;
      ccb_str_init(&out);
      pv_project_json(&out, &p);
      CHECK(contains(out.buf, "\"trace\":{\"steps\":3}"));
      CHECK(contains(out.buf, "\"taken\":true"));
      CHECK(contains(out.buf,
        "\"args_provenance\":[\"trace\"]"));
      CHECK(contains(out.buf,
        "\"picked\":{\"entry\":\"is Loaded\","
        "\"provenance\":\"trace\"}"));
      ccb_str_free(&out);
    }
    pv_trace_free(t);
    ccb_diag_list_free(&diags);
    ccb_arena_destroy(arena);
  }
  // --- Apply warnings: unknown screen, unknown edge, unknown
  // branch, neither edge nor branch, branch not containing the
  // button. Mismatches warn and skip; the rest still applies.
  {
    static const char *flow_text =
      "fun Home() {\n"
      "  Column {\n"
      "    Button(\"Go\", onClick = { nav(\"Detail\") })\n"
      "  }\n"
      "}\n"
      "fun Detail() {\n"
      "  Column {\n"
      "    when (state) {\n"
      "      is Loading -> Text(\"Wait\")\n"
      "      is Loaded -> Text(\"Body\")\n"
      "    }\n"
      "    Button(\"Back\", onClick = { nav(\"Home\") })\n"
      "  }\n"
      "}\n";
    static const char *trace_text =
      "{\"version\":1,\"steps\":["
      "{\"screen\":\"Nope\",\"edge\":\"Go\"},"
      "{\"screen\":\"Home\",\"edge\":\"Ghost\"},"
      "{\"screen\":\"Detail\",\"edge\":null,\"branch\":\"is Missing\"},"
      "{\"screen\":\"Home\"},"
      "{\"screen\":\"Home\",\"edge\":\"Go\",\"branch\":\"is Loaded\"},"
      "{\"screen\":\"Detail\",\"edge\":\"Back\",\"branch\":\"is Loaded\"},"
      "{\"screen\":\"Home\",\"edge\":\"Go\"}]}";
    ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    pv_project p;
    pv_trace *t = NULL;
    char *err = NULL;
    ccb_diag_list_init(&diags);
    CHECK(pv_flow_parse(arena, flow_text, "T.kt", &diags,
      &screens, &nscreens) == CCB_OK);
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.screens = screens;
    p.nscreens = nscreens;
    t = pv_trace_parse(trace_text, &err);
    CHECK(t != NULL && err == NULL);
    free(err);
    pv_trace_apply(&p, t, &diags);
    // Unknown screen, unknown edge, unknown branch, neither
    // edge nor branch, branch-not-found, branch-not-containing
    // the button: 6 warnings.
    CHECK(diag_count(&diags, "CCB-P1023") == 6);
    {
      pv_screen *home = find_screen(&p, "Home");
      pv_screen *detail = find_screen(&p, "Detail");
      pv_edge *go = find_edge(home, "Go");
      pv_edge *back = find_edge(detail, "Back");
      CHECK(go != NULL && go->trace_taken == 1);
      // The mismatched-branch Back is still marked taken.
      CHECK(back != NULL && back->trace_taken == 1);
    }
    pv_trace_free(t);
    ccb_diag_list_free(&diags);
    ccb_arena_destroy(arena);
  }
  // --- Composition: scenario fills one slot, trace fills another,
  // each keeping its own provenance; scenario wins on its slot.
  {
    static const char *flow_text =
      "fun Home() {\n"
      "  Column {\n"
      "    Button(\"Multi\", onClick = { nav(\"Detail\", a, b) })\n"
      "  }\n"
      "}\n"
      "fun Detail() {\n"
      "  Column {\n"
      "    Text(\"D\")\n"
      "  }\n"
      "}\n";
    static const char *trace_text =
      "{\"version\":1,\"steps\":["
      "{\"screen\":\"Home\",\"edge\":\"Multi\",\"branch\":null,"
      "\"args\":[\"s1\", \"t2\"]}]}";
    ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    pv_project p;
    ccb_scenario sc;
    ccb_scenario_args args[1];
    char *values[1];
    pv_trace *t = NULL;
    char *err = NULL;
    ccb_diag_list_init(&diags);
    CHECK(pv_flow_parse(arena, flow_text, "T.kt", &diags,
      &screens, &nscreens) == CCB_OK);
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.screens = screens;
    p.nscreens = nscreens;
    memset(&sc, 0, sizeof(sc));
    sc.name = "t";
    values[0] = "s1";
    args[0].label = "Multi";
    args[0].values = values;
    args[0].nvalues = 1;
    sc.args = args;
    sc.nargs = 1;
    pv_scenario_apply(&p, &sc, &diags);
    t = pv_trace_parse(trace_text, &err);
    CHECK(t != NULL && err == NULL);
    free(err);
    pv_trace_apply(&p, t, &diags);
    {
      pv_screen *home = find_screen(&p, "Home");
      pv_edge *multi = find_edge(home, "Multi");
      CHECK(multi != NULL && multi->nargs == 2);
      if (multi != NULL && multi->nargs == 2) {
        // Scenario filled slot 0 first; the trace's positional
        // "s1" for slot 0 is refused (static by then) and "t2"
        // fills slot 1.
        CHECK_STR_EQ(multi->args[0], "s1");
        CHECK_STR_EQ(multi->args[1], "t2");
        CHECK(multi->args_scenario != NULL &&
          multi->args_scenario[0] == 1);
        CHECK(multi->args_trace != NULL &&
          multi->args_trace[1] == 1);
        CHECK(multi->args_trace[0] == 0);
      }
    }
    {
      ccb_str out;
      ccb_str_init(&out);
      pv_project_json(&out, &p);
      CHECK(contains(out.buf,
        "\"args_provenance\":[\"scenario\",\"trace\"]"));
      ccb_str_free(&out);
    }
    pv_trace_free(t);
    ccb_diag_list_free(&diags);
    ccb_arena_destroy(arena);
  }
  // --- Walk recording: scripted keys produce the exact trace a
  // real walk would write, including a branch pick and a pick-only
  // step; back navigation and unknown follows record nothing.
  {
    static const char *home_text =
      "fun Home() {\n"
      "  Column {\n"
      "    Button(\"Open\", onClick = { nav(\"Detail\") })\n"
      "    Button(\"New\", onClick = { nav(\"Form\") })\n"
      "  }\n"
      "}\n";
    static const char *detail_text =
      "fun Detail() {\n"
      "  Column {\n"
      "    when (state) {\n"
      "      is Loading -> Text(\"Wait\")\n"
      "      is Loaded -> Button(\"Edit\", onClick = { nav(\"Form\") })\n"
      "    }\n"
      "    Button(\"Back\", onClick = { nav(\"Home\") })\n"
      "  }\n"
      "}\n";
    static const char *form_text =
      "fun Form() {\n"
      "  Column {\n"
      "    Text(\"F\")\n"
      "    Button(\"More\", onClick = { nav(nextScreen) })\n"
      "  }\n"
      "}\n";
    ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    pv_project p;
    pv_walk w;
    ccb_str out;
    ccb_diag_list_init(&diags);
    CHECK(pv_flow_parse(arena, home_text, "Home.kt", &diags,
      &screens, &nscreens) == CCB_OK);
    CHECK(pv_flow_parse(arena, detail_text, "Detail.kt", &diags,
      &screens, &nscreens) == CCB_OK);
    CHECK(pv_flow_parse(arena, form_text, "Form.kt", &diags,
      &screens, &nscreens) == CCB_OK);
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.screens = screens;
    p.nscreens = nscreens;
    p.frame_width = 40;
    p.interior_cols = 38;
    memset(&w, 0, sizeof(w));
    CHECK(pv_walk_start(&w, &p, NULL) == 0);
    pv_walk_apply_key(&w, PV_WALK_KEY_ENTER); // Home -> Detail
    CHECK_STR_EQ(pv_walk_current_name(&w), "Detail");
    pv_walk_apply_key(&w, PV_WALK_KEY_BRANCH); // branch mode
    pv_walk_apply_key(&w, PV_WALK_KEY_DOWN); // to "is Loaded"
    pv_walk_apply_key(&w, PV_WALK_KEY_ENTER); // pick -> Form
    CHECK_STR_EQ(pv_walk_current_name(&w), "Form");
    pv_walk_apply_key(&w, PV_WALK_KEY_BACK); // back: no record
    CHECK_STR_EQ(pv_walk_current_name(&w), "Detail");
    // Selection moves only; with no ENTER nothing further records,
    // and the second DOWN clamps at the last edge.
    pv_walk_apply_key(&w, PV_WALK_KEY_DOWN);
    pv_walk_apply_key(&w, PV_WALK_KEY_DOWN);
    pv_walk_apply_key(&w, PV_WALK_KEY_QUIT);
    CHECK(w.nrecord == 2);
    if (w.nrecord == 2) {
      CHECK_STR_EQ(w.record[0].screen, "Home");
      CHECK_STR_EQ(w.record[0].edge, "Open");
      CHECK(w.record[0].branch == NULL);
      CHECK_STR_EQ(w.record[1].screen, "Detail");
      CHECK_STR_EQ(w.record[1].edge, "Edit");
      CHECK_STR_EQ(w.record[1].branch, "is Loaded");
    }
    ccb_str_init(&out);
    pv_trace_write(&out, w.record, w.nrecord);
    CHECK_STR_EQ(out.buf,
      "{\"version\":1,\"steps\":["
      "{\"screen\":\"Home\",\"edge\":\"Open\",\"branch\":null,"
      "\"args\":[]},"
      "{\"screen\":\"Detail\",\"edge\":\"Edit\",\"branch\":"
      "\"is Loaded\",\"args\":[]}]}");
    ccb_str_free(&out);
    pv_walk_free(&w);
    // Pick-only + unknown-follow recording, on a fresh walk.
    memset(&w, 0, sizeof(w));
    CHECK(pv_walk_start(&w, &p, "Detail") == 0);
    pv_walk_apply_key(&w, PV_WALK_KEY_BRANCH);
    pv_walk_apply_key(&w, PV_WALK_KEY_ENTER); // pick "is Loading"
    CHECK_STR_EQ(pv_walk_current_name(&w), "Detail");
    CHECK(w.nrecord == 1);
    if (w.nrecord == 1) {
      CHECK_STR_EQ(w.record[0].screen, "Detail");
      CHECK(w.record[0].edge == NULL);
      CHECK_STR_EQ(w.record[0].branch, "is Loading");
    }
    pv_walk_free(&w);
    // Round-trip: the recorded trace replays onto a fresh project
    // with the same marks.
    {
      ccb_arena *arena2 = ccb_arena_create();
      ccb_diag_list diags2;
      pv_screen **screens2 = NULL;
      size_t nscreens2 = 0;
      pv_project p2;
      pv_trace *t = NULL;
      char *err = NULL;
      ccb_str rec;
      ccb_diag_list_init(&diags2);
      CHECK(pv_flow_parse(arena2, home_text, "Home.kt", &diags2,
        &screens2, &nscreens2) == CCB_OK);
      CHECK(pv_flow_parse(arena2, detail_text, "Detail.kt",
        &diags2, &screens2, &nscreens2) == CCB_OK);
      CHECK(pv_flow_parse(arena2, form_text, "Form.kt", &diags2,
        &screens2, &nscreens2) == CCB_OK);
      memset(&p2, 0, sizeof(p2));
      p2.arena = arena2;
      p2.screens = screens2;
      p2.nscreens = nscreens2;
      // Re-record the two-step walk on this project, then replay.
      {
        pv_walk w2;
        memset(&w2, 0, sizeof(w2));
        CHECK(pv_walk_start(&w2, &p2, NULL) == 0);
        pv_walk_apply_key(&w2, PV_WALK_KEY_ENTER);
        pv_walk_apply_key(&w2, PV_WALK_KEY_BRANCH);
        pv_walk_apply_key(&w2, PV_WALK_KEY_DOWN);
        pv_walk_apply_key(&w2, PV_WALK_KEY_ENTER);
        ccb_str_init(&rec);
        pv_trace_write(&rec, w2.record, w2.nrecord);
        pv_walk_free(&w2);
      }
      t = pv_trace_parse(rec.buf != NULL ? rec.buf : "", &err);
      CHECK(t != NULL && err == NULL);
      free(err);
      pv_trace_apply(&p2, t, &diags2);
      {
        pv_screen *detail = find_screen(&p2, "Detail");
        pv_edge *edit = find_edge(detail, "Edit");
        pv_box *loaded = find_box(detail->root, "branch-entry",
          "is Loaded");
        CHECK(edit != NULL && edit->trace_taken == 1);
        CHECK(loaded != NULL && loaded->trace_picked == 1);
      }
      ccb_str_free(&rec);
      pv_trace_free(t);
      ccb_diag_list_free(&diags2);
      ccb_arena_destroy(arena2);
    }
    ccb_diag_list_free(&diags);
    ccb_arena_destroy(arena);
  }
  // --- Trace goldens for the sample app: tree + JSON with the
  // recorded walk applied (Home -> Detail, pick is Loaded,
  // Detail -> Home with the unknown arg filled).
  {
    static const char *trace_text =
      "{\"version\":1,\"steps\":["
      "{\"screen\":\"Home\",\"edge\":\"Open note\",\"branch\":null,"
      "\"args\":[\"note-42\"]},"
      "{\"screen\":\"Detail\",\"edge\":null,\"branch\":\"is Loaded\","
      "\"args\":[]},"
      "{\"screen\":\"Detail\",\"edge\":\"Back\",\"branch\":null,"
      "\"args\":[\"42\"]}]}";
    char *sdir = sample_dir();
    char tmpl[] = "/tmp/ccb-trace-XXXXXX";
    char *scratch = mkdtemp(tmpl);
    char *proj = NULL;
    char *toml = NULL;
    ccb_config *cfg = NULL;
    ccb_diag_list diags;
    pv_project *p = NULL;
    pv_trace *t = NULL;
    char *err = NULL;
    CHECK(scratch != NULL);
    if (scratch != NULL) {
      proj = join2(scratch, "proj");
      CHECK(copy_tree(sdir, proj) == 0);
      toml = join2(proj, "ccb.toml");
      ccb_diag_list_init(&diags);
      CHECK(ccb_config_load(toml, NULL, &cfg, &diags) == CCB_OK);
      ccb_diag_list_free(&diags);
      CHECK(cfg != NULL);
      if (cfg != NULL) {
        CHECK(pv_project_load(cfg, 40, 0, 0, NULL, &p) == CCB_OK);
        CHECK(p != NULL);
        if (p != NULL) {
          long errors = 0;
          t = pv_trace_parse(trace_text, &err);
          CHECK(t != NULL && err == NULL);
          free(err);
          pv_trace_apply(p, t, &p->diags);
          errors = pv_project_diagnose(p);
          CHECK(errors == 0);
          CHECK(diag_count(&p->diags, "CCB-P1023") == 0);
          {
            ccb_str out;
            ccb_str_init(&out);
            pv_render_logic_tree(&out, p);
            check_golden_file("trace-walk logic-tree",
              out.buf != NULL ? out.buf : "",
              "trace-walk/logic-tree.txt");
            ccb_str_free(&out);
          }
          {
            ccb_str out;
            ccb_str_init(&out);
            pv_project_json(&out, p);
            check_golden_file("trace-walk preview.json",
              out.buf != NULL ? out.buf : "",
              "trace-walk/preview.json");
            ccb_str_free(&out);
          }
          pv_trace_free(t);
          pv_project_free(p);
        }
        ccb_config_free(cfg);
      }
      free(toml);
      free(proj);
    }
    free(sdir);
  }
  // --- CLI: --trace renders the overlay from a file; conflicts and
  // bad files fail with exit 2.
  {
    char *sdir = sample_dir();
    char tmpl[] = "/tmp/ccb-trace-cli-XXXXXX";
    char *scratch = mkdtemp(tmpl);
    char *proj = NULL;
    char *trace_path = NULL;
    char *cap = NULL;
    CHECK(scratch != NULL);
    if (scratch != NULL) {
      proj = join2(scratch, "proj");
      CHECK(copy_tree(sdir, proj) == 0);
      trace_path = join2(proj, "walk.json");
      cap = join2(scratch, "cap.txt");
      {
        static const char *cli_trace =
          "{\"version\":1,\"steps\":["
          "{\"screen\":\"Home\",\"edge\":\"Open note\"},"
          "{\"screen\":\"Detail\",\"edge\":\"Back\","
          "\"args\":[\"42\"]}]}";
        CHECK(write_all(trace_path, cli_trace,
          strlen(cli_trace)) == 0);
      }
      {
        char *argv[] = { "ccb", "preview", "--trace", trace_path,
          NULL };
        int rc = run_cli(proj, cap, 4, argv);
        char *out = try_read(cap);
        CHECK(rc == 0);
        CHECK(contains(out,
          "-> Detail(\"note-42\") via \"Open note\" [trace]"));
        CHECK(contains(out,
          "-> Home(\"42\" [trace]) via \"Back\" [trace]"));
        free(out);
      }
      {
        char *argv[] = { "ccb", "preview", "--trace", trace_path,
          "--json", NULL };
        int rc = run_cli(proj, cap, 5, argv);
        char *out = try_read(cap);
        CHECK(rc == 0);
        CHECK(contains(out, "\"trace\":{\"steps\":2}"));
        CHECK(contains(out, "\"taken\":true"));
        free(out);
      }
      {
        char *argv[] = { "ccb", "preview", "--trace", trace_path,
          "--walk", NULL };
        int rc = run_cli(proj, cap, 5, argv);
        CHECK(rc == 2);
      }
      {
        char *argv[] = { "ccb", "preview", "--record", "x.json",
          NULL };
        int rc = run_cli(proj, cap, 4, argv);
        CHECK(rc == 2);
      }
      {
        char *argv[] = { "ccb", "preview", "--trace",
          "missing.json", NULL };
        int rc = run_cli(proj, cap, 4, argv);
        CHECK(rc == 2);
      }
      {
        char *bad_path = join2(proj, "bad.json");
        char *argv[] = { "ccb", "preview", "--trace", bad_path,
          NULL };
        int rc = 0;
        CHECK(write_all(bad_path, "nope", 4) == 0);
        rc = run_cli(proj, cap, 4, argv);
        CHECK(rc == 2);
        free(bad_path);
      }
      free(cap);
      free(trace_path);
      free(proj);
    }
    free(sdir);
  }
  TEST_MAIN_END();
}
