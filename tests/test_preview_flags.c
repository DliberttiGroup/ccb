// Tests for preview CLI flags owned by worker W-C: --output,
// --update-goldens (against scratch dirs only, never the real
// tests/golden/ tree), --device presets, and the JSON "version" field.
// CLI cases run ccb_cli_main in-process after chdir into a copy or
// the sample project, with stdout redirected to a temp file so the
// "stdout carries nothing" contract for --output is checkable.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_config.h"
#include "ccb_cli.h"
#include "ccb_str.h"
#include <dirent.h>
#include <fcntl.h>
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

// First candidate directory (from __FILE__-relative suffixes) that
// contains the given probe file, or NULL.
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

static char *golden_dir(void) {
  static const char *suffixes[] = {
    "/golden/preview/",
    "/../tests/golden/preview/",
  };
  char *out = find_dir(suffixes, 2, "preview.json");
  if (out != NULL) {
    return out;
  }
  return ccb_xstrdup("tests/golden/preview/");
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

static int files_equal(const char *a_path, const char *b_path) {
  char *a = try_read(a_path);
  char *b = try_read(b_path);
  int eq = a != NULL && b != NULL && strcmp(a, b) == 0;
  free(a);
  free(b);
  return eq;
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
      char *data = try_read(sp);
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

// Count UTF-8 code points in the first line of a file (the frame
// border line), so a frame width check is glyph-based, not byte-based.
static long first_line_glyphs(const char *path) {
  char *text = try_read(path);
  long n = 0;
  size_t i = 0;
  if (text == NULL) {
    return -1;
  }
  for (i = 0; text[i] != '\0' && text[i] != '\n'; i++) {
    if ((text[i] & 0xC0) != 0x80) {
      n++;
    }
  }
  free(text);
  return n;
}

// ---------- CLI runner ----------

// Run ccb_cli_main with stdout redirected to cap_path, from workdir.
// Returns the CLI exit code; the caller reads cap_path afterwards.
// The process cwd is restored before returning.
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

static long file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;
  }
  return (long)st.st_size;
}

static pv_project *load_sample(long width) {
  char *dir = sample_dir();
  char *toml = join2(dir, "ccb.toml");
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  pv_project *p = NULL;
  ccb_diag_list_init(&diags);
  if (ccb_config_load(toml, NULL, &cfg, &diags) != CCB_OK) {
    ccb_diag_list_free(&diags);
    free(toml);
    free(dir);
    return NULL;
  }
  ccb_diag_list_free(&diags);
  if (pv_project_load(cfg, width, 0, 0, NULL, &p) != CCB_OK) {
    ccb_config_free(cfg);
    free(toml);
    free(dir);
    return NULL;
  }
  ccb_config_free(cfg);
  free(toml);
  free(dir);
  return p;
}

int main(void) {
  static const char *goldens[] = {
    "Detail.txt", "Empty.txt", "Form.txt", "Home.txt", "Settings.txt",
    "logic-tree.txt", "preview.json",
  };
  char *sdir = sample_dir();
  char *gdir = golden_dir();
  char scratch[64];
  ccb_test_tmpdir(scratch, sizeof scratch, "flags");

  // --- JSON carries "version":2, additively, at library level and
  // in the checked-in golden.
  {
    pv_project *p = load_sample(40);
    CHECK(p != NULL);
    if (p != NULL) {
      ccb_str out;
      ccb_str_init(&out);
      pv_project_json(&out, p);
      CHECK(out.buf != NULL);
      if (out.buf != NULL) {
        CHECK(strncmp(out.buf, "{\"event\":\"preview\",\"version\":2,",
          strlen("{\"event\":\"preview\",\"version\":2,")) == 0);
        // No existing field was dropped: spot-check survivors.
        CHECK(strstr(out.buf, "\"screens\"") != NULL);
        CHECK(strstr(out.buf, "\"diagnostics\"") != NULL);
        CHECK(strstr(out.buf, "\"boxes\"") != NULL);
      }
      ccb_str_free(&out);
      pv_project_free(p);
    }
    {
      char *gp = join2(gdir, "preview.json");
      char *text = try_read(gp);
      CHECK(text != NULL);
      if (text != NULL) {
        CHECK(strstr(text, "\"version\":2") != NULL);
      }
      free(text);
      free(gp);
    }
  }

  // --- --output (human): stdout empty, render in the file, exit 0.
  {
    char *out_path = join2(scratch, "human.txt");
    char *cap_path = join2(scratch, "cap-human.txt");
    char *argv[] = { "ccb", "preview", "--output", out_path, NULL };
    int rc = run_cli(sdir, cap_path, 4, argv);
    char *text = NULL;
    CHECK(rc == 0);
    CHECK(file_size(cap_path) == 0);
    text = try_read(out_path);
    CHECK(text != NULL);
    if (text != NULL) {
      CHECK(strstr(text, "My Notes") != NULL);
      CHECK(strstr(text, "Detail") != NULL);
    }
    free(text);
    free(out_path);
    free(cap_path);
  }

  // --- --output --json: stdout empty, JSON (with version) in file.
  {
    char *out_path = join2(scratch, "out.json");
    char *cap_path = join2(scratch, "cap-json.txt");
    char *argv[] = { "ccb", "preview", "--json", "--output", out_path,
      NULL };
    int rc = run_cli(sdir, cap_path, 5, argv);
    char *text = NULL;
    CHECK(rc == 0);
    CHECK(file_size(cap_path) == 0);
    text = try_read(out_path);
    CHECK(text != NULL);
    if (text != NULL) {
      CHECK(strncmp(text, "{\"event\":\"preview\",\"version\":2,",
        strlen("{\"event\":\"preview\",\"version\":2,")) == 0);
    }
    free(text);
    free(out_path);
    free(cap_path);
  }

  // --- --device presets, and explicit --width winning over --device.
  {
    struct { const char *args[9]; int argc; long want; } cases[] = {
      { { "ccb", "preview", "--device", "tablet", "--output", NULL },
        0, 80 },
      { { "ccb", "preview", "--device", "phone", "--output", NULL },
        0, 40 },
      { { "ccb", "preview", "--device", "tablet", "--width", "50",
          "--output", NULL }, 0, 50 },
    };
    size_t ci = 0;
    for (ci = 0; ci < 3; ci++) {
      char name[32];
      char *out_path = NULL;
      char *cap_path = NULL;
      char *argv[10];
      int ai = 0;
      int rc = -1;
      snprintf(name, sizeof(name), "dev-%zu.txt", ci);
      out_path = join2(scratch, name);
      cap_path = join2(scratch, "cap-dev.txt");
      // Splice the output path into the arg list (it replaces the
      // trailing NULL placeholder of the case's args).
      for (ai = 0; cases[ci].args[ai] != NULL; ai++) {
        argv[ai] = (char *)cases[ci].args[ai];
      }
      argv[ai++] = out_path;
      argv[ai] = NULL;
      rc = run_cli(sdir, cap_path, ai, argv);
      CHECK(rc == 0);
      CHECK(first_line_glyphs(out_path) == cases[ci].want);
      free(out_path);
      free(cap_path);
    }
  }

  // --- --device with an unknown value is a usage error (exit 2).
  {
    char *cap_path = join2(scratch, "cap-baddev.txt");
    char *argv[] = { "ccb", "preview", "--device", "watch", NULL };
    int rc = run_cli(sdir, cap_path, 4, argv);
    CHECK(rc == 2);
    free(cap_path);
  }

  // --- --output to an unwritable path is exit 2.
  {
    char *cap_path = join2(scratch, "cap-badout.txt");
    char *argv[] = { "ccb", "preview", "--output",
      "/nonexistent-dir-xyz/out.txt", NULL };
    int rc = run_cli(sdir, cap_path, 4, argv);
    CHECK(rc == 2);
    free(cap_path);
  }

  // --- --output + --update-goldens conflict is exit 2.
  {
    char *cap_path = join2(scratch, "cap-conflict.txt");
    char *argv[] = { "ccb", "preview", "--output", "x.txt",
      "--update-goldens", NULL };
    int rc = run_cli(sdir, cap_path, 5, argv);
    CHECK(rc == 2);
    free(cap_path);
  }

  // --- --update-goldens into a scratch dir writes exactly what
  // the library renders (frames + logic tree + JSON), overwrites
  // a deliberately damaged file, and, for a mutated project copy,
  // changes only the intended files (Home.txt + preview.json for
  // a Home label change). Comparison is against the live render,
  // not the checked-in goldens: frame goldens belong to another
  // worker and may legitimately lag a renderer change.
  {
    char *upd = join2(scratch, "gold");
    char *cap_path = join2(scratch, "cap-upd.txt");
    char *argv[] = { "ccb", "preview", "--update-goldens", NULL };
    size_t gi = 0;
    int rc = -1;
    mkdir(upd, 0777);
    setenv("CCB_GOLDEN_DIR", upd, 1);
    rc = run_cli(sdir, cap_path, 3, argv);
    CHECK(rc == 0);
    {
      pv_project *p = load_sample(40);
      CHECK(p != NULL);
      if (p != NULL) {
        size_t i = 0;
        pv_project_diagnose(p);
        for (i = 0; i < p->nscreens; i++) {
          ccb_str out;
          ccb_str name;
          char *path = NULL;
          ccb_str_init(&out);
          pv_render_screen(&out, p->screens[i], p->frame_width, 0, 0);
          ccb_str_init(&name);
          ccb_str_append(&name, p->screens[i]->name);
          ccb_str_append(&name, ".txt");
          path = join2(upd, name.buf);
          {
            char *actual = try_read(path);
            CHECK(actual != NULL);
            if (actual != NULL) {
              CHECK_STR_EQ(actual, out.buf != NULL ? out.buf : "");
            }
            free(actual);
          }
          free(path);
          ccb_str_free(&name);
          ccb_str_free(&out);
        }
        {
          ccb_str out;
          char *path = join2(upd, "logic-tree.txt");
          char *actual = NULL;
          ccb_str_init(&out);
          pv_render_logic_tree(&out, p);
          actual = try_read(path);
          CHECK(actual != NULL);
          if (actual != NULL) {
            CHECK_STR_EQ(actual, out.buf != NULL ? out.buf : "");
          }
          free(actual);
          free(path);
          ccb_str_free(&out);
        }
        {
          ccb_str out;
          char *path = join2(upd, "preview.json");
          char *actual = NULL;
          ccb_str_init(&out);
          pv_project_json(&out, p);
          actual = try_read(path);
          CHECK(actual != NULL);
          if (actual != NULL) {
            CHECK_STR_EQ(actual, out.buf != NULL ? out.buf : "");
          }
          free(actual);
          free(path);
          ccb_str_free(&out);
        }
        pv_project_free(p);
      }
    }
    // Deliberate damage, then regenerate: the damage must be gone
    // and the file must match the pre-damage content exactly.
    {
      char *home = join2(upd, "Home.txt");
      char *saved = try_read(home);
      CHECK(saved != NULL);
      CHECK(write_all(home, "DAMAGED\n", 8) == 0);
      rc = run_cli(sdir, cap_path, 3, argv);
      CHECK(rc == 0);
      {
        char *restored = try_read(home);
        CHECK(restored != NULL && saved != NULL);
        if (restored != NULL && saved != NULL) {
          CHECK_STR_EQ(restored, saved);
        }
        free(restored);
      }
      free(saved);
      free(home);
    }
    // Mutated project copy: one label change.
    {
      char *proj = join2(scratch, "proj");
      char *upd2 = join2(scratch, "gold2");
      char *flow = NULL;
      char *text = NULL;
      char *hit = NULL;
      mkdir(upd2, 0777);
      CHECK(copy_tree(sdir, proj) == 0);
      flow = join2(proj, "flow/Home.kt");
      // flow/Home.kt lives under flow/; join2 handles one level, so
      // build the nested path directly.
      free(flow);
      {
        ccb_str p;
        ccb_str_init(&p);
        ccb_str_append(&p, proj);
        ccb_str_append(&p, "/flow/Home.kt");
        flow = ccb_xstrdup(p.buf);
        ccb_str_free(&p);
      }
      text = try_read(flow);
      CHECK(text != NULL);
      if (text != NULL) {
        hit = strstr(text, "My Notes");
        CHECK(hit != NULL);
        if (hit != NULL) {
          ccb_str nw;
          ccb_str_init(&nw);
          ccb_str_append_n(&nw, text, (size_t)(hit - text));
          ccb_str_append(&nw, "My Notes!");
          ccb_str_append(&nw, hit + strlen("My Notes"));
          CHECK(write_all(flow, nw.buf, strlen(nw.buf)) == 0);
          ccb_str_free(&nw);
        }
        free(text);
      }
      setenv("CCB_GOLDEN_DIR", upd2, 1);
      rc = run_cli(proj, cap_path, 3, argv);
      CHECK(rc == 0);
      for (gi = 0; gi < 7; gi++) {
        char *a = join2(upd, goldens[gi]);
        char *b = join2(upd2, goldens[gi]);
        int same = files_equal(a, b);
        if (strcmp(goldens[gi], "Home.txt") == 0 ||
            strcmp(goldens[gi], "preview.json") == 0) {
          CHECK(!same);
        } else {
          CHECK(same);
        }
        free(a);
        free(b);
      }
      free(flow);
      free(upd2);
      free(proj);
    }
    unsetenv("CCB_GOLDEN_DIR");
    free(cap_path);
    free(upd);
  }

  free(sdir);
  free(gdir);
  TEST_MAIN_END();
}
