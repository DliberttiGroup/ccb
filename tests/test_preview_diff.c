// Tests for preview --diff (worker W-DIFF), in-process via
// pv_diff_run. Snapshots are built with the same render functions
// the diff module uses, then mutated — never hand-written expected
// bytes that could drift from the renderer. All scratch lives under
// a mkdtemp directory; the real tests/golden/ tree is never touched.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_preview_diff.h"
#include "ccb_config.h"
#include "ccb_json.h"
#include "ccb_slice.h"
#include "ccb_str.h"
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- small file helpers ----------

static void test_dir(ccb_str *out) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  if (slash != NULL && slash != file) {
    ccb_str_append_n(out, file, (size_t)(slash - file));
  } else {
    ccb_str_append(out, ".");
  }
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

static char *find_dir(const char *suffixes[], size_t n,
    const char *probe) {
  size_t i = 0;
  ccb_str dir;
  ccb_str_init(&dir);
  test_dir(&dir);
  for (i = 0; i < n; i++) {
    ccb_str path;
    FILE *f = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, probe);
    f = fopen(path.buf != NULL ? path.buf : "", "rb");
    ccb_str_free(&path);
    if (f != NULL) {
      char *out = NULL;
      fclose(f);
      ccb_str_init(&path);
      ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
      ccb_str_append(&path, suffixes[i]);
      out = ccb_xstrdup(path.buf);
      ccb_str_free(&path);
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
  char *out = find_dir(suffixes, 2, "ccb.toml");
  if (out != NULL) {
    return out;
  }
  return ccb_xstrdup("examples/preview-sample/");
}

static char *nopreview_dir(void) {
  static const char *suffixes[] = {
    "/fixtures/preview/no-preview/",
    "/../tests/fixtures/preview/no-preview/",
  };
  char *out = find_dir(suffixes, 2, "ccb.toml");
  if (out != NULL) {
    return out;
  }
  return ccb_xstrdup("tests/fixtures/preview/no-preview/");
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
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    sp = join2(src, e->d_name);
    dp = join2(dst, e->d_name);
    if (stat(sp, &st) == 0 && S_ISDIR(st.st_mode)) {
      int rc = copy_tree(sp, dp);
      free(sp);
      free(dp);
      if (rc != 0) {
        closedir(d);
        return -1;
      }
    } else {
      char *data = ccb_test_try_read(sp);
      int rc = data == NULL ? -1 :
        write_all(dp, data, strlen(data));
      free(data);
      free(sp);
      free(dp);
      if (rc != 0) {
        closedir(d);
        return -1;
      }
    }
  }
  closedir(d);
  return 0;
}

static ccb_config *load_cfg(const char *dir) {
  char *toml = join2(dir, "ccb.toml");
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  ccb_diag_list_init(&diags);
  if (ccb_config_load(toml, NULL, &cfg, &diags) != CCB_OK) {
    ccb_diag_list_free(&diags);
    free(toml);
    return NULL;
  }
  ccb_diag_list_free(&diags);
  free(toml);
  return cfg;
}

// Build a snapshot directory from a config with the same renders the
// diff module compares against (frames with no color, logic tree,
// JSON with no trailing newline), mirroring --update-goldens.
static int build_snapshot(ccb_config *cfg, const char *snapdir) {
  pv_project *p = NULL;
  size_t i = 0;
  mkdir(snapdir, 0777);
  if (pv_project_load(cfg, 40, 0, 0, NULL, &p) != CCB_OK || p == NULL) {
    return -1;
  }
  pv_project_diagnose(p);
  for (i = 0; i < p->nscreens; i++) {
    ccb_str out;
    char *path = NULL;
    char *name = NULL;
    ccb_str_init(&out);
    pv_render_screen(&out, p->screens[i], p->frame_width, 0, 0);
    {
      ccb_str n;
      ccb_str_init(&n);
      ccb_str_append(&n, p->screens[i]->name);
      ccb_str_append(&n, ".txt");
      name = ccb_str_steal(&n);
    }
    path = join2(snapdir, name);
    if (write_all(path, out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : "")) != 0) {
      free(path);
      free(name);
      ccb_str_free(&out);
      pv_project_free(p);
      return -1;
    }
    free(path);
    free(name);
    ccb_str_free(&out);
  }
  {
    ccb_str out;
    char *path = join2(snapdir, "logic-tree.txt");
    ccb_str_init(&out);
    pv_render_logic_tree(&out, p);
    if (write_all(path, out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : "")) != 0) {
      free(path);
      ccb_str_free(&out);
      pv_project_free(p);
      return -1;
    }
    free(path);
    ccb_str_free(&out);
  }
  {
    ccb_str out;
    char *path = join2(snapdir, "preview.json");
    ccb_str_init(&out);
    pv_project_json(&out, p);
    if (write_all(path, out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : "")) != 0) {
      free(path);
      ccb_str_free(&out);
      pv_project_free(p);
      return -1;
    }
    free(path);
    ccb_str_free(&out);
  }
  pv_project_free(p);
  return 0;
}

static int run_diff(ccb_config *cfg, const char *snap,
    const char *screen, int json, ccb_str *out, ccb_str *err) {
  return pv_diff_run(cfg, snap, screen, 40, 0, 0, json, out, err);
}

// Find a file entry inside a parsed preview-diff JSON object by name;
// returns its status string, or NULL.
static const char *json_file_status(const ccb_json_value *root,
    const char *want) {
  const ccb_json_value *files = ccb_json_obj_get(root, "files");
  size_t i = 0;
  if (files == NULL) {
    return NULL;
  }
  for (i = 0; i < files->count; i++) {
    const ccb_json_value *f = files->items[i];
    const ccb_json_value *name = ccb_json_obj_get(f, "name");
    if (name != NULL && name->str != NULL &&
        strcmp(name->str, want) == 0) {
      const ccb_json_value *st = ccb_json_obj_get(f, "status");
      return st != NULL ? st->str : NULL;
    }
  }
  return NULL;
}

int main(void) {
  char *sdir = sample_dir();
  char scratch[64];
  ccb_test_tmpdir(scratch, sizeof scratch, "diff");
  {
    ccb_config *cfg = load_cfg(sdir);
    char *snap = join2(scratch, "snap");
    CHECK(cfg != NULL);
    CHECK(build_snapshot(cfg, snap) == 0);

    // --- Identical snapshot -> 0, one concise "identical" line.
    {
      ccb_str out, err;
      int rc = 0;
      ccb_str_init(&out);
      ccb_str_init(&err);
      rc = run_diff(cfg, snap, NULL, 0, &out, &err);
      CHECK(rc == 0);
      CHECK(out.buf != NULL && strstr(out.buf, "identical") != NULL);
      CHECK(err.buf == NULL || err.len == 0);
      ccb_str_free(&out);
      ccb_str_free(&err);
    }

    // --- Identical snapshot, JSON mode -> parseable, all identical.
    {
      ccb_str out, err;
      ccb_json_value *v = NULL;
      char *jerr = NULL;
      int rc = 0;
      ccb_str_init(&out);
      ccb_str_init(&err);
      rc = run_diff(cfg, snap, NULL, 1, &out, &err);
      CHECK(rc == 0);
      v = ccb_json_parse(ccb_slice_from(out.buf != NULL ? out.buf : ""),
        &jerr);
      CHECK(v != NULL);
      if (v != NULL) {
        const ccb_json_value *ev = ccb_json_obj_get(v, "event");
        const ccb_json_value *id = ccb_json_obj_get(v, "identical");
        const ccb_json_value *files = ccb_json_obj_get(v, "files");
        CHECK(ev != NULL && ev->str != NULL &&
          strcmp(ev->str, "preview-diff") == 0);
        CHECK(id != NULL && id->type == CCB_JSON_BOOL && id->boolean);
        CHECK(files != NULL && files->count == 7);
        CHECK(json_file_status(v, "Home.txt") != NULL &&
          strcmp(json_file_status(v, "Home.txt"), "identical") == 0);
        CHECK(json_file_status(v, "preview.json") != NULL &&
          strcmp(json_file_status(v, "preview.json"),
            "identical") == 0);
        ccb_json_free(v);
      }
      free(jerr);
      ccb_str_free(&out);
      ccb_str_free(&err);
    }

    // --- One screen changed (snapshot mutated) -> nonzero, naming
    // that screen, with a unified diff of the changed line.
    {
      char *home = join2(snap, "Home.txt");
      char *text = ccb_test_try_read(home);
      char *hit = text != NULL ? strstr(text, "My Notes") : NULL;
      CHECK(hit != NULL);
      if (hit != NULL) {
        ccb_str nw;
        ccb_str_init(&nw);
        ccb_str_append_n(&nw, text, (size_t)(hit - text));
        ccb_str_append(&nw, "My Notes!");
        ccb_str_append(&nw, hit + strlen("My Notes"));
        CHECK(write_all(home, nw.buf, strlen(nw.buf)) == 0);
        ccb_str_free(&nw);
      }
      free(text);
      {
        ccb_str out, err;
        int rc = 0;
        ccb_str_init(&out);
        ccb_str_init(&err);
        rc = run_diff(cfg, snap, NULL, 0, &out, &err);
        CHECK(rc == 1);
        CHECK(out.buf != NULL && strstr(out.buf, "Home.txt") != NULL);
        CHECK(out.buf != NULL && strstr(out.buf, "My Notes") != NULL);
        CHECK(out.buf != NULL &&
          strstr(out.buf, "differing files") != NULL);
        // Other files stay identical, so they must not be named as
        // differing (the summary lists only Home.txt).
        CHECK(out.buf != NULL &&
          strstr(out.buf, "differing files (1): Home.txt") != NULL);
        ccb_str_free(&out);
        ccb_str_free(&err);
      }
      // Restore the snapshot for the later cases.
      CHECK(build_snapshot(cfg, snap) == 0);
      free(home);
    }

    // --- Missing snapshot file -> diagnostic + nonzero, naming it.
    {
      char *detail = join2(snap, "Detail.txt");
      char *out_path = NULL;
      CHECK(unlink(detail) == 0);
      {
        ccb_str out, err;
        int rc = 0;
        ccb_str_init(&out);
        ccb_str_init(&err);
        rc = run_diff(cfg, snap, NULL, 0, &out, &err);
        CHECK(rc == 1);
        CHECK(out.buf != NULL && strstr(out.buf, "Detail.txt") != NULL);
        CHECK(out.buf != NULL && strstr(out.buf, "missing") != NULL);
        ccb_str_free(&out);
        ccb_str_free(&err);
      }
      CHECK(build_snapshot(cfg, snap) == 0);
      free(detail);
      (void)out_path;
    }

    // --- Stale snapshot file -> reported, naming it.
    {
      char *ghost = join2(snap, "Ghost.txt");
      CHECK(write_all(ghost, "stale\n", 6) == 0);
      {
        ccb_str out, err;
        int rc = 0;
        ccb_str_init(&out);
        ccb_str_init(&err);
        rc = run_diff(cfg, snap, NULL, 0, &out, &err);
        CHECK(rc == 1);
        CHECK(out.buf != NULL && strstr(out.buf, "Ghost.txt") != NULL);
        CHECK(out.buf != NULL && strstr(out.buf, "stale") != NULL);
        ccb_str_free(&out);
        ccb_str_free(&err);
      }
      CHECK(unlink(ghost) == 0);
      free(ghost);
    }

    // --- JSON mode with a difference -> parseable JSON naming the
    // differing files, diff string only for "different" files.
    {
      char *home = join2(snap, "Home.txt");
      char *text = ccb_test_try_read(home);
      char *hit = text != NULL ? strstr(text, "My Notes") : NULL;
      if (hit != NULL) {
        ccb_str nw;
        ccb_str_init(&nw);
        ccb_str_append_n(&nw, text, (size_t)(hit - text));
        ccb_str_append(&nw, "My Notes?");
        ccb_str_append(&nw, hit + strlen("My Notes"));
        CHECK(write_all(home, nw.buf, strlen(nw.buf)) == 0);
        ccb_str_free(&nw);
      }
      free(text);
      {
        ccb_str out, err;
        ccb_json_value *v = NULL;
        char *jerr = NULL;
        int rc = 0;
        ccb_str_init(&out);
        ccb_str_init(&err);
        rc = run_diff(cfg, snap, NULL, 1, &out, &err);
        CHECK(rc == 1);
        v = ccb_json_parse(ccb_slice_from(out.buf != NULL ? out.buf : ""),
          &jerr);
        CHECK(v != NULL);
        if (v != NULL) {
          const ccb_json_value *id = ccb_json_obj_get(v, "identical");
          const ccb_json_value *files = ccb_json_obj_get(v, "files");
          const char *st = json_file_status(v, "Home.txt");
          CHECK(id != NULL && id->type == CCB_JSON_BOOL && !id->boolean);
          CHECK(st != NULL && strcmp(st, "different") == 0);
          CHECK(json_file_status(v, "Detail.txt") != NULL &&
            strcmp(json_file_status(v, "Detail.txt"),
              "identical") == 0);
          if (files != NULL) {
            size_t i = 0;
            int diff_seen = 0;
            for (i = 0; i < files->count; i++) {
              const ccb_json_value *f = files->items[i];
              const ccb_json_value *d = ccb_json_obj_get(f, "diff");
              const ccb_json_value *s2 = ccb_json_obj_get(f, "status");
              if (s2 != NULL && s2->str != NULL &&
                  strcmp(s2->str, "different") == 0) {
                CHECK(d != NULL && d->type == CCB_JSON_STR &&
                  strstr(d->str, "My Notes") != NULL);
                diff_seen = 1;
              } else {
                CHECK(d != NULL && d->type == CCB_JSON_NULL);
              }
            }
            CHECK(diff_seen);
          }
          ccb_json_free(v);
        }
        free(jerr);
        ccb_str_free(&out);
        ccb_str_free(&err);
      }
      CHECK(build_snapshot(cfg, snap) == 0);
      free(home);
    }

    // --- --screen scoping: only that screen's file is compared.
    // Mutate Detail.txt (and logic-tree/preview.json are left
    // identical); scoped to Home the diff must be identical, and
    // scoped to Detail it must name only Detail.txt.
    {
      char *detail = join2(snap, "Detail.txt");
      char *text = ccb_test_try_read(detail);
      CHECK(text != NULL);
      if (text != NULL) {
        ccb_str nw;
        ccb_str_init(&nw);
        ccb_str_append(&nw, text);
        ccb_str_append(&nw, "EXTRA LINE\n");
        CHECK(write_all(detail, nw.buf, strlen(nw.buf)) == 0);
        ccb_str_free(&nw);
        free(text);
      }
      {
        ccb_str out, err;
        int rc = 0;
        ccb_str_init(&out);
        ccb_str_init(&err);
        rc = run_diff(cfg, snap, "Home", 0, &out, &err);
        CHECK(rc == 0);
        CHECK(out.buf != NULL && strstr(out.buf, "identical") != NULL);
        ccb_str_free(&out);
        ccb_str_free(&err);
      }
      {
        ccb_str out, err;
        ccb_json_value *v = NULL;
        char *jerr = NULL;
        int rc = 0;
        ccb_str_init(&out);
        ccb_str_init(&err);
        rc = run_diff(cfg, snap, "Detail", 1, &out, &err);
        CHECK(rc == 1);
        v = ccb_json_parse(ccb_slice_from(out.buf != NULL ? out.buf : ""),
          &jerr);
        CHECK(v != NULL);
        if (v != NULL) {
          const ccb_json_value *files = ccb_json_obj_get(v, "files");
          CHECK(files != NULL && files->count == 1);
          CHECK(json_file_status(v, "Detail.txt") != NULL &&
            strcmp(json_file_status(v, "Detail.txt"),
              "different") == 0);
          ccb_json_free(v);
        }
        free(jerr);
        ccb_str_free(&out);
        ccb_str_free(&err);
      }
      CHECK(build_snapshot(cfg, snap) == 0);
      free(detail);
    }

    // --- Unreadable snapshot dir -> exit 2 with a diagnostic.
    {
      char *missing = join2(scratch, "no-such-dir");
      ccb_str out, err;
      int rc = 0;
      ccb_str_init(&out);
      ccb_str_init(&err);
      rc = run_diff(cfg, missing, NULL, 0, &out, &err);
      CHECK(rc == 2);
      CHECK(err.buf != NULL && strstr(err.buf, "snapshot") != NULL);
      ccb_str_free(&out);
      ccb_str_free(&err);
      free(missing);
    }

    // --- Unknown --screen name -> exit 2.
    {
      ccb_str out, err;
      int rc = 0;
      ccb_str_init(&out);
      ccb_str_init(&err);
      rc = run_diff(cfg, snap, "NoSuchScreen", 0, &out, &err);
      CHECK(rc == 2);
      CHECK(err.buf != NULL && strstr(err.buf, "NoSuchScreen") != NULL);
      ccb_str_free(&out);
      ccb_str_free(&err);
    }

    ccb_config_free(cfg);
    free(snap);
  }

  // --- Extra current screen (mutated project copy) -> reported as a
  // missing file in the snapshot, plus the logic tree / JSON it
  // changes; the new screen's file itself must be named.
  {
    char *proj = join2(scratch, "proj2");
    char *snap = join2(scratch, "snap2");
    char *flow = NULL;
    ccb_config *cfg2 = NULL;
    CHECK(copy_tree(sdir, proj) == 0);
    {
      ccb_str p;
      ccb_str_init(&p);
      ccb_str_append(&p, proj);
      ccb_str_append(&p, "/flow/Extra.kt");
      flow = ccb_xstrdup(p.buf);
      ccb_str_free(&p);
    }
    CHECK(write_all(flow,
      "fun Extra() {\n  Column {\n    Text(\"Extra screen\")\n  }\n}\n",
      strlen("fun Extra() {\n  Column {\n    Text(\"Extra screen\")\n  }\n}\n")) == 0);
    free(flow);
    cfg2 = load_cfg(proj);
    CHECK(cfg2 != NULL);
    if (cfg2 != NULL) {
      ccb_config *orig = load_cfg(sdir);
      CHECK(orig != NULL);
      if (orig != NULL) {
        CHECK(build_snapshot(orig, snap) == 0);
        {
          ccb_str out, err;
          int rc = 0;
          ccb_str_init(&out);
          ccb_str_init(&err);
          rc = run_diff(cfg2, snap, NULL, 0, &out, &err);
          CHECK(rc == 1);
          CHECK(out.buf != NULL && strstr(out.buf, "Extra.txt") != NULL);
          ccb_str_free(&out);
          ccb_str_free(&err);
        }
        ccb_config_free(orig);
      }
      ccb_config_free(cfg2);
    }
    free(snap);
    free(proj);
  }

  // --- Preview-not-applicable project -> exit 2, not exit 0.
  {
    char *ndir = nopreview_dir();
    char *snap = join2(scratch, "snap3");
    ccb_config *cfg3 = load_cfg(ndir);
    mkdir(snap, 0777);
    CHECK(cfg3 != NULL);
    if (cfg3 != NULL) {
      ccb_str out, err;
      int rc = 0;
      ccb_str_init(&out);
      ccb_str_init(&err);
      rc = run_diff(cfg3, snap, NULL, 0, &out, &err);
      CHECK(rc == 2);
      CHECK(err.buf != NULL &&
        strstr(err.buf, "not applicable") != NULL);
      ccb_str_free(&out);
      ccb_str_free(&err);
      ccb_config_free(cfg3);
    }
    free(snap);
    free(ndir);
  }

  free(sdir);
  TEST_MAIN_END();
}
