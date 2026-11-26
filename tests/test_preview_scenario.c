// Tests for Chunk 1 scenario rendering: config loading of
// [preview.scenario.*] tables, the merge (precedence, provenance,
// warnings), scenario goldens for the sample app, and the CLI flag.
// All paths derive from __FILE__; the tests never depend on the
// working directory. CLI cases run ccb_cli_main in-process after
// chdir into a scratch copy of the sample project.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_config.h"
#include "ccb_preview.h"
#include "ccb_preview_scenario.h"
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
  struct stat st;
  FILE *f = NULL;
  long size = 0;
  char *buf = NULL;
  // Only regular files: ftell on a directory stream returns LONG_MAX
  // on some filesystems, and malloc(size + 1) then wraps to 2^63
  // and aborts. A directory is not readable content, so fail
  // cleanly instead of trusting ftell.
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return NULL;
  }
  f = fopen(path, "rb");
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

static int append_all(const char *path, const char *data) {
  FILE *f = fopen(path, "ab");
  if (f == NULL) {
    return -1;
  }
  if (strlen(data) > 0 && fwrite(data, 1, strlen(data), f) !=
      strlen(data)) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
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

// ---------- project helpers ----------

static long diag_count(pv_project *p, const char *code) {
  long n = 0;
  size_t i = 0;
  if (p == NULL) {
    return 0;
  }
  for (i = 0; i < p->diags.count; i++) {
    if (strcmp(p->diags.items[i].code, code) == 0) {
      n++;
    }
  }
  return n;
}

// Load the sample project with a scenario by name. The config is
// freed inside; the project owns its arena + diag list.
static pv_project *load_sample_scenario(const char *toml_path,
    const char *scenario_name) {
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  const ccb_scenario *sc = NULL;
  pv_project *p = NULL;
  ccb_status rc = CCB_OK;
  ccb_diag_list_init(&diags);
  if (ccb_config_load(toml_path, NULL, &cfg, &diags) != CCB_OK) {
    ccb_diag_list_free(&diags);
    return NULL;
  }
  ccb_diag_list_free(&diags);
  sc = ccb_config_scenario(cfg, scenario_name);
  if (sc == NULL) {
    ccb_config_free(cfg);
    return NULL;
  }
  rc = pv_project_load_scenario(cfg, 40, 0, 0, NULL, sc, &p);
  ccb_config_free(cfg);
  if (rc != CCB_OK || p == NULL) {
    return NULL;
  }
  return p;
}

static pv_screen *find_screen(pv_project *p, const char *name) {
  size_t i = 0;
  for (i = 0; i < p->nscreens; i++) {
    if (strcmp(p->screens[i]->name, name) == 0) {
      return p->screens[i];
    }
  }
  return NULL;
}

static pv_edge *find_edge(pv_screen *s, const char *from) {
  size_t i = 0;
  if (s == NULL) {
    return NULL;
  }
  for (i = 0; i < s->nedges; i++) {
    if (s->edges[i].from != NULL &&
        strcmp(s->edges[i].from, from) == 0) {
      return &s->edges[i];
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
      box->label != NULL && strcmp(box->label, label) == 0) {
    return box;
  }
  for (i = 0; i < box->nchildren; i++) {
    pv_box *hit = find_box(box->children[i], type, label);
    if (hit != NULL) {
      return hit;
    }
  }
  return NULL;
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

// ---------- main ----------

int main(void) {
  // --- Loader: the checked-in sample scenario parses as declared.
  {
    char *toml = sample_toml_path();
    ccb_diag_list diags;
    ccb_config *cfg = NULL;
    const ccb_scenario *sc = NULL;
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(toml, NULL, &cfg, &diags) == CCB_OK);
    ccb_diag_list_free(&diags);
    CHECK(cfg != NULL);
    if (cfg != NULL) {
      CHECK(cfg->preview_scenario_count == 1);
      sc = ccb_config_scenario(cfg, "loaded");
      CHECK(sc != NULL);
      CHECK(ccb_config_scenario(cfg, "nope") == NULL);
      if (sc != NULL) {
        CHECK(sc->nbranches == 2);
        CHECK(sc->nlists == 1);
        CHECK(sc->nfields == 1);
        CHECK(sc->nargs == 1);
        if (sc->nbranches == 2) {
          CHECK_STR_EQ(sc->branches[0].screen, "Detail");
          CHECK_STR_EQ(sc->branches[0].entry, "is Loaded");
          CHECK_STR_EQ(sc->branches[1].screen, "Settings");
          CHECK_STR_EQ(sc->branches[1].entry, "else");
        }
        if (sc->nlists == 1) {
          CHECK_STR_EQ(sc->lists[0].label, "notes");
          CHECK(sc->lists[0].nrows == 2);
          if (sc->lists[0].nrows == 2) {
            CHECK_STR_EQ(sc->lists[0].rows[0], "Buy milk");
            CHECK_STR_EQ(sc->lists[0].rows[1], "Call Ana");
          }
        }
        if (sc->nfields == 1) {
          CHECK_STR_EQ(sc->fields[0].label, "Title");
          CHECK_STR_EQ(sc->fields[0].text, "Milk");
        }
        if (sc->nargs == 1) {
          CHECK_STR_EQ(sc->args[0].label, "Back");
          CHECK(sc->args[0].nvalues == 1);
          if (sc->args[0].nvalues == 1) {
            CHECK_STR_EQ(sc->args[0].values[0], "42");
          }
        }
      }
      ccb_config_free(cfg);
    }
    free(toml);
  }
  // --- Loader errors: a wrong-typed value and an unknown section
  // each fail the config load (CCB-E1007 / CCB-E1001).
  {
    char *sdir = sample_dir();
    char scratch[64];
    ccb_test_tmpdir(scratch, sizeof scratch, "scenario");
    char *proj = NULL;
    char *toml = NULL;
    ccb_diag_list diags;
    ccb_config *cfg = NULL;
    proj = join2(scratch, "proj");
    CHECK(copy_tree(sdir, proj) == 0);
    toml = join2(proj, "ccb.toml");
    CHECK(append_all(toml,
      "[preview.scenario.broken.branches]\n"
      "Detail = 7\n") == 0);
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(toml, NULL, &cfg, &diags) != CCB_OK);
    CHECK(cfg == NULL);
    ccb_diag_list_free(&diags);
    // A fresh copy with an unknown section fails the same way.
    {
      char *proj2 = join2(scratch, "proj2");
      char *toml2 = NULL;
      CHECK(copy_tree(sdir, proj2) == 0);
      toml2 = join2(proj2, "ccb.toml");
      CHECK(append_all(toml2,
        "[preview.scenario.broken.widgets]\n"
        "Detail = \"x\"\n") == 0);
      ccb_diag_list_init(&diags);
      CHECK(ccb_config_load(toml2, NULL, &cfg, &diags) !=
        CCB_OK);
      CHECK(cfg == NULL);
      ccb_diag_list_free(&diags);
      free(toml2);
      free(proj2);
    }
    free(toml);
    free(proj);
    free(sdir);
  }
  // --- Golden: the sample app rendered with scenario "loaded".
  {
    char *toml = sample_toml_path();
    pv_project *p = load_sample_scenario(toml, "loaded");
    long errors = 0;
    size_t i = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      // All scenario entries matched, so no P1022 fires. Since
      // Chunk C, no P1017 fires: the sample's buttons carry no
      // font-size rules, so their heights are unknown. The 2 P1009
      // clip warnings are the pre-existing static-list-children
      // behavior (a flow-declared list with children warns the same
      // way) — scenario row boxes are laid out like static rows.
      CHECK(diag_count(p, "CCB-P1017") == 0);
      CHECK(diag_count(p, "CCB-P1009") == 2);
      CHECK(diag_count(p, "CCB-P1022") == 0);
      CHECK((long)p->diags.count == 2);
      CHECK(p->nscreens == 5);
      CHECK(p->scenario_name != NULL &&
        strcmp(p->scenario_name, "loaded") == 0);
      // Shared button label: Detail's Back edge takes the fill,
      // Settings' Back edge (no arguments) is untouched.
      {
        pv_edge *detail_back = find_edge(find_screen(p, "Detail"),
          "Back");
        pv_edge *settings_back = find_edge(
          find_screen(p, "Settings"), "Back");
        CHECK(detail_back != NULL && detail_back->nargs == 1);
        if (detail_back != NULL && detail_back->nargs == 1) {
          CHECK_STR_EQ(detail_back->args[0], "42");
          CHECK(detail_back->args_scenario != NULL &&
            detail_back->args_scenario[0] == 1);
          CHECK_STR_EQ(detail_back->target, "Home");
        }
        CHECK(settings_back != NULL && settings_back->nargs == 0);
      }
      for (i = 0; i < p->nscreens; i++) {
        ccb_str out;
        ccb_str golden;
        ccb_str_init(&out);
        pv_render_screen(&out, p->screens[i], p->frame_width, 0, 0);
        ccb_str_init(&golden);
        ccb_str_append(&golden, "scenario-loaded/");
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
        check_golden_file("scenario-loaded logic-tree",
          out.buf != NULL ? out.buf : "",
          "scenario-loaded/logic-tree.txt");
        ccb_str_free(&out);
      }
      {
        ccb_str out;
        ccb_str_init(&out);
        pv_project_json(&out, p);
        check_golden_file("scenario-loaded preview.json",
          out.buf != NULL ? out.buf : "",
          "scenario-loaded/preview.json");
        ccb_str_free(&out);
      }
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Merge unit tests: precedence, provenance, warnings.
  {
    static const char *flow_text =
      "fun Home() {\n"
      "  Column {\n"
      "    List(\"notes\")\n"
      "    List(\"full\") {\n"
      "      Text(\"Static row\")\n"
      "    }\n"
      "    TextField(\"Search\")\n"
      "    Button(\"Go\", onClick = { nav(\"Detail\", itemId) })\n"
      "    Button(\"Fixed\", onClick = { nav(\"Detail\", \"lit\") })\n"
      "  }\n"
      "}\n"
      "fun Detail() {\n"
      "  Column {\n"
      "    Text(\"D\")\n"
      "    when (state) {\n"
      "      is Loading -> Text(\"Wait\")\n"
      "      is Loaded -> Text(\"Body\")\n"
      "    }\n"
      "  }\n"
      "}\n";
    ccb_arena *arena = ccb_arena_create();
    ccb_diag_list diags;
    pv_screen **screens = NULL;
    size_t nscreens = 0;
    pv_project p;
    ccb_scenario sc;
    ccb_scenario_branch branches[3];
    ccb_scenario_list lists[3];
    ccb_scenario_field fields[2];
    ccb_scenario_args args[3];
    char *notes_rows[2];
    char *full_rows[1];
    char *ghost_rows[1];
    char *go_values[1];
    char *fixed_values[1];
    char *ghost_values[1];
    ccb_diag_list_init(&diags);
    CHECK(pv_flow_parse(arena, flow_text, "Home.kt", &diags,
      &screens, &nscreens) == CCB_OK);
    CHECK(nscreens == 2);
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.screens = screens;
    p.nscreens = nscreens;
    memset(&sc, 0, sizeof(sc));
    sc.name = "t";
    branches[0].screen = "Detail";
    branches[0].entry = "is Loaded";
    branches[1].screen = "Nope";
    branches[1].entry = "x";
    branches[2].screen = "Detail";
    branches[2].entry = "is Missing";
    sc.branches = branches;
    sc.nbranches = 3;
    notes_rows[0] = "a";
    notes_rows[1] = "b";
    full_rows[0] = "x";
    ghost_rows[0] = "y";
    lists[0].label = "notes";
    lists[0].rows = notes_rows;
    lists[0].nrows = 2;
    lists[1].label = "full";
    lists[1].rows = full_rows;
    lists[1].nrows = 1;
    lists[2].label = "ghost";
    lists[2].rows = ghost_rows;
    lists[2].nrows = 1;
    sc.lists = lists;
    sc.nlists = 3;
    fields[0].label = "Search";
    fields[0].text = "q";
    fields[1].label = "Missing";
    fields[1].text = "z";
    sc.fields = fields;
    sc.nfields = 2;
    go_values[0] = "v1";
    fixed_values[0] = "v2";
    ghost_values[0] = "v3";
    args[0].label = "Go";
    args[0].values = go_values;
    args[0].nvalues = 1;
    args[1].label = "Fixed";
    args[1].values = fixed_values;
    args[1].nvalues = 1;
    args[2].label = "Ghost";
    args[2].values = ghost_values;
    args[2].nvalues = 1;
    sc.args = args;
    sc.nargs = 3;
    pv_scenario_apply(&p, &sc, &diags);
    {
      pv_screen *home = find_screen(&p, "Home");
      pv_screen *detail = find_screen(&p, "Detail");
      pv_box *notes = NULL;
      pv_box *full = NULL;
      pv_box *search = NULL;
      pv_box *loaded = NULL;
      pv_box *loading = NULL;
      pv_edge *go = NULL;
      pv_edge *fixed = NULL;
      CHECK(home != NULL && detail != NULL);
      if (detail != NULL) {
        loaded = find_box(detail->root, "branch-entry", "is Loaded");
        loading = find_box(detail->root, "branch-entry",
          "is Loading");
        CHECK(loaded != NULL && loaded->scenario_picked == 1);
        CHECK(loading != NULL && loading->scenario_picked == 0);
      }
      if (home != NULL) {
        notes = find_box(home->root, "list", "notes");
        full = find_box(home->root, "list", "full");
        search = find_box(home->root, "textfield", "Search");
        go = find_edge(home, "Go");
        fixed = find_edge(home, "Fixed");
      }
      CHECK(notes != NULL && notes->nchildren == 2);
      if (notes != NULL && notes->nchildren == 2) {
        CHECK_STR_EQ(notes->children[0]->label, "a");
        CHECK_STR_EQ(notes->children[1]->label, "b");
        CHECK(notes->children[0]->from_scenario == 1);
        CHECK(notes->children[1]->from_scenario == 1);
      }
      CHECK(full != NULL && full->nchildren == 1);
      if (full != NULL && full->nchildren == 1) {
        CHECK_STR_EQ(full->children[0]->label, "Static row");
        CHECK(full->children[0]->from_scenario == 0);
      }
      CHECK(search != NULL);
      if (search != NULL) {
        CHECK_STR_EQ(search->label, "Search");
        CHECK(search->scenario_text != NULL &&
          strcmp(search->scenario_text, "q") == 0);
      }
      CHECK(go != NULL && go->nargs == 1);
      if (go != NULL && go->nargs == 1) {
        CHECK_STR_EQ(go->args[0], "v1");
        CHECK(go->args_scenario != NULL &&
          go->args_scenario[0] == 1);
        CHECK_STR_EQ(go->target, "Detail");
      }
      // The static literal stands; the scenario value is dropped.
      CHECK(fixed != NULL && fixed->nargs == 1);
      if (fixed != NULL && fixed->nargs == 1) {
        CHECK_STR_EQ(fixed->args[0], "lit");
        CHECK(fixed->args_scenario == NULL ||
          fixed->args_scenario[0] == 0);
      }
      // 7 warnings: unknown screen, unknown entry, static-rows
      // list, unknown list, unknown field, no-slot button, unknown
      // button — and nothing else.
      {
        long n1022 = 0;
        size_t di = 0;
        for (di = 0; di < diags.count; di++) {
          if (strcmp(diags.items[di].code, "CCB-P1022") == 0) {
            n1022++;
          }
        }
        CHECK(n1022 == 7);
        CHECK((long)diags.count == n1022);
      }
      // Tree marks: picked branch, filled arg, list rows, field.
      {
        ccb_str out;
        ccb_str_init(&out);
        pv_render_logic_tree(&out, &p);
        CHECK(contains(out.buf, "is Loaded [scenario]"));
        CHECK(contains(out.buf, "(\"v1\" [scenario])"));
        CHECK(contains(out.buf,
          "scenario: list \"notes\" = [\"a\", \"b\"] [scenario]"));
        CHECK(contains(out.buf,
          "scenario: field \"Search\" = \"q\" [scenario]"));
        ccb_str_free(&out);
      }
    }
    ccb_diag_list_free(&diags);
    ccb_arena_destroy(arena);
  }
  // --- CLI: flag wiring, conflicts, unknown name.
  {
    char *sdir = sample_dir();
    char scratch[64];
    ccb_test_tmpdir(scratch, sizeof scratch, "scencli");
    char *proj = NULL;
    char *cap = NULL;
    proj = join2(scratch, "proj");
    cap = join2(scratch, "cap.txt");
    CHECK(copy_tree(sdir, proj) == 0);
    {
      char *argv[] = { "ccb", "preview", "--scenario", "loaded",
        NULL };
      char *out = NULL;
      CHECK(run_cli(proj, cap, 4, argv) == 0);
      out = try_read(cap);
      CHECK(contains(out, "scenario: list \"notes\""));
      CHECK(contains(out, "[scenario]"));
      free(out);
    }
    {
      char *argv[] = { "ccb", "preview", "--scenario", "loaded",
        "--json", NULL };
      char *out = NULL;
      CHECK(run_cli(proj, cap, 5, argv) == 0);
      out = try_read(cap);
      CHECK(contains(out, "\"scenario\":\"loaded\""));
      CHECK(contains(out, "\"provenance\":\"scenario\""));
      free(out);
    }
    {
      char *argv[] = { "ccb", "preview", "--scenario", "loaded",
        "--screen", "Home", NULL };
      char *out = NULL;
      CHECK(run_cli(proj, cap, 6, argv) == 0);
      out = try_read(cap);
      CHECK(contains(out, "Buy milk"));
      free(out);
    }
    {
      char *argv[] = { "ccb", "preview", "--scenario", "nope",
        NULL };
      CHECK(run_cli(proj, cap, 4, argv) == 2);
    }
    {
      char *argv[] = { "ccb", "preview", "--scenario", "loaded",
        "--walk", NULL };
      CHECK(run_cli(proj, cap, 5, argv) == 2);
    }
    {
      char *argv[] = { "ccb", "preview", "--scenario", "loaded",
        "--watch", NULL };
      CHECK(run_cli(proj, cap, 5, argv) == 2);
    }
    {
      char *argv[] = { "ccb", "preview", "--scenario", "loaded",
        "--diff", "snap", NULL };
      CHECK(run_cli(proj, cap, 6, argv) == 2);
    }
    {
      char *argv[] = { "ccb", "preview", "--scenario", "loaded",
        "--storyboard", "Home", NULL };
      CHECK(run_cli(proj, cap, 6, argv) == 2);
    }
    {
      char *argv[] = { "ccb", "preview", "--scenario", "loaded",
        "--update-goldens", NULL };
      CHECK(run_cli(proj, cap, 5, argv) == 2);
    }
    free(cap);
    free(proj);
    free(sdir);
  }
  TEST_MAIN_END();
}
