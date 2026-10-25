// Tests for Chunk 3: the preview LSP server (ccb lsp).
//
// In-process tests cover the tree equivalence with the load path
// (the deep-copy + merge + assemble chain must produce byte-identical
// JSON), per-file incrementality via parse counts, open errors, the
// CSS merge unit, and the snapshot diff. Socket tests drive the real
// server over a socketpair: scripted JSON-RPC sessions for initialize
// (split header/body writes), logicTree, screenInfo, diagnostics,
// didOpen/didChange publishTree pushes, error codes, shutdown/exit
// codes, EOF-without-shutdown, and re-rooting to a second project.
// No threads: the child runs pv_lsp_run and _exits; the parent drives
// it with select() timeouts so a hung child fails the test instead
// of hanging ctest.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_config.h"
#include "ccb_diag.h"
#include "ccb_fs.h"
#include "ccb_preview.h"
#include "ccb_preview_lsp.h"
#include "ccb_preview_watch.h"
#include "ccb_str.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------- path + file helpers (mirroring test_preview_scenario) ----------

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

static int write_all(const char *path, const char *data, size_t n) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return -1;
  }
  if (n > 0 && fwrite(data, 1, n, f) != n) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

static char *join2(const char *a, const char *b) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, a);
  if (s.len == 0 || s.buf[s.len - 1] != '/') {
    ccb_str_append(&s, "/");
  }
  ccb_str_append(&s, b);
  return s.buf;
}

static char *sample_dir(void) {
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
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, "ccb.toml");
    {
      char *probe = try_read(path.buf);
      int found = probe != NULL;
      free(probe);
      if (found) {
        char *out = ccb_xstrdup(path.buf);
        // Strip the trailing "ccb.toml", keep the directory.
        out[strlen(out) - strlen("ccb.toml")] = '\0';
        ccb_str_free(&path);
        ccb_str_free(&dir);
        return out;
      }
    }
    ccb_str_free(&path);
  }
  ccb_str_free(&dir);
  {
    char *probe = try_read("examples/preview-sample/ccb.toml");
    if (probe != NULL) {
      free(probe);
      return ccb_xstrdup("examples/preview-sample/");
    }
  }
  return ccb_xstrdup("../examples/preview-sample/");
}

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
      rc = data == NULL ? -1 : write_all(dp, data, strlen(data));
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

// Scratch copy of the sample project. Returns the project dir (caller
// frees); the parent scratch dir is left for the OS temp cleanup.
static char *scratch_sample(void) {
  char tmpl[] = "/tmp/ccb-lsp-test-XXXXXX";
  char *scratch = mkdtemp(tmpl);
  char *sdir = NULL;
  char *proj = NULL;
  if (scratch == NULL) {
    return NULL;
  }
  sdir = sample_dir();
  proj = join2(scratch, "proj");
  if (copy_tree(sdir, proj) != 0) {
    free(sdir);
    free(proj);
    return NULL;
  }
  free(sdir);
  return proj;
}

static int contains(const char *hay, const char *needle) {
  return hay != NULL && needle != NULL && strstr(hay, needle) != NULL;
}

// ---------- in-process tests ----------

static void test_equivalence(void) {
  char *proj = scratch_sample();
  char *toml = NULL;
  ccb_config *cfg = NULL;
  ccb_diag_list dl;
  pv_project *lp = NULL;
  ccb_str lj;
  char *golden = NULL;
  char *err = NULL;
  pv_lsp_server *s = NULL;
  const char *sj = NULL;
  ccb_str gdir;
  CHECK(proj != NULL);
  if (proj == NULL) {
    return;
  }
  toml = join2(proj, "ccb.toml");
  ccb_diag_list_init(&dl);
  CHECK(ccb_config_load(toml, NULL, &cfg, &dl) == CCB_OK);
  if (cfg == NULL) {
    ccb_diag_list_free(&dl);
    free(toml);
    free(proj);
    return;
  }
  // Load path (CLI defaults: width 40, no ascii, not strict). The
  // CLI runs diagnose after load; the server does the same, so the
  // test must too for the JSON to match.
  CHECK(pv_project_load_scenario(cfg, 40, 0, 0, &dl, NULL, &lp) ==
    CCB_OK);
  CHECK(lp != NULL);
  if (lp != NULL) {
    (void)pv_project_diagnose(lp);
  }
  ccb_str_init(&lj);
  if (lp != NULL) {
    pv_project_json(&lj, lp);
  }
  // The load path must match the committed golden exactly.
  ccb_str_init(&gdir);
  test_dir(&gdir);
  ccb_str_append(&gdir, "/golden/preview/preview.json");
  golden = try_read(gdir.buf);
  if (golden == NULL) {
    golden = try_read("tests/golden/preview/preview.json");
  }
  if (golden == NULL) {
    golden = try_read("../tests/golden/preview/preview.json");
  }
  CHECK(golden != NULL);
  if (golden != NULL && lj.buf != NULL) {
    CHECK_STR_EQ(lj.buf, golden);
  }
  // Server path: same project, no scenario/trace, must be identical.
  s = pv_lsp_open(toml, NULL, NULL, &err);
  CHECK(s != NULL);
  if (s != NULL) {
    sj = pv_lsp_tree_json(s);
    CHECK(sj != NULL);
    if (sj != NULL && lj.buf != NULL) {
      CHECK_STR_EQ(sj, lj.buf);
    }
    // Every flow and styles file parsed exactly once at open.
    {
      static const char *subdirs[] = { "flow", "styles" };
      size_t di = 0;
      for (di = 0; di < 2; di++) {
        char *dirp = join2(proj, subdirs[di]);
        DIR *d = opendir(dirp);
        struct dirent *e = NULL;
        CHECK(d != NULL);
        if (d != NULL) {
          while ((e = readdir(d)) != NULL) {
            char *fp = NULL;
            struct stat st;
            if (strcmp(e->d_name, ".") == 0 ||
                strcmp(e->d_name, "..") == 0) {
              continue;
            }
            fp = join2(dirp, e->d_name);
            if (stat(fp, &st) == 0 && S_ISREG(st.st_mode)) {
              CHECK(pv_lsp_parse_count(s, fp) == 1);
            }
            free(fp);
          }
          closedir(d);
        }
        free(dirp);
      }
    }
    pv_lsp_close(s);
  }
  free(err);
  free(golden);
  if (lp != NULL) {
    pv_project_free(lp);
  }
  ccb_str_free(&lj);
  ccb_str_free(&gdir);
  ccb_config_free(cfg);
  ccb_diag_list_free(&dl);
  free(toml);
  free(proj);
}

static void test_incremental(void) {
  char *proj = scratch_sample();
  char *toml = NULL;
  char *home = NULL;
  char *flow_dir = NULL;
  char *styles_dir = NULL;
  char *orig = NULL;
  char *err = NULL;
  pv_lsp_server *s = NULL;
  ccb_str extra;
  const char *tj = NULL;
  char *tj_copy = NULL;
  CHECK(proj != NULL);
  if (proj == NULL) {
    return;
  }
  toml = join2(proj, "ccb.toml");
  flow_dir = join2(proj, "flow");
  styles_dir = join2(proj, "styles");
  home = join2(flow_dir, "Home.kt");
  orig = try_read(home);
  CHECK(orig != NULL);
  s = pv_lsp_open(toml, NULL, NULL, &err);
  CHECK(s != NULL);
  if (s == NULL || orig == NULL) {
    free(orig);
    free(home);
    free(flow_dir);
    free(styles_dir);
    free(toml);
    free(proj);
    free(err);
    return;
  }
  ccb_str_init(&extra);
  ccb_str_append(&extra, orig);
  ccb_str_append(&extra,
    "\nfun Extra() {\n  Column {\n    Text(\"Extra screen\")\n"
    "    Button(\"Back\") { nav(\"Home\") }\n  }\n}\n");
  // didChange one file: only that file re-parses, tree gains Extra.
  pv_lsp_did_change(s, home, extra.buf);
  CHECK(pv_lsp_rebuild(s) == 1);
  tj = pv_lsp_tree_json(s);
  CHECK(contains(tj, "\"name\":\"Extra\""));
  CHECK(pv_lsp_parse_count(s, home) == 2);
  {
    // Every other file still at its open-time count of 1.
    char *detail = join2(flow_dir, "Detail.kt");
    char *common = join2(styles_dir, "common.css");
    CHECK(pv_lsp_parse_count(s, detail) == 1);
    CHECK(pv_lsp_parse_count(s, common) == 1);
    free(detail);
    free(common);
  }
  // didChange on a non-input file: ignored, no rebuild, no new tree.
  {
    char *junk = join2(proj, "unrelated.txt");
    pv_lsp_did_change(s, junk, "nothing");
    CHECK(pv_lsp_rebuild(s) == 0);
    free(junk);
  }
  // didClose: back to disk content (the original), one more parse.
  pv_lsp_did_close(s, home);
  CHECK(pv_lsp_rebuild(s) == 1);
  tj = pv_lsp_tree_json(s);
  CHECK(!contains(tj, "\"name\":\"Extra\""));
  CHECK(pv_lsp_parse_count(s, home) == 3);
  // Disk change: scan marks it dirty, rebuild re-parses from disk.
  CHECK(write_all(home, extra.buf, strlen(extra.buf)) == 0);
  CHECK(pv_lsp_scan_disk(s) == 1);
  CHECK(pv_lsp_rebuild(s) == 1);
  tj = pv_lsp_tree_json(s);
  CHECK(contains(tj, "\"name\":\"Extra\""));
  CHECK(pv_lsp_parse_count(s, home) == 4);
  // No further disk change: scan reports clean.
  CHECK(pv_lsp_scan_disk(s) == 0);
  // Restore disk, scan, rebuild: back to the original tree.
  CHECK(write_all(home, orig, strlen(orig)) == 0);
  CHECK(pv_lsp_scan_disk(s) == 1);
  CHECK(pv_lsp_rebuild(s) == 1);
  tj = pv_lsp_tree_json(s);
  tj_copy = tj != NULL ? ccb_xstrdup(tj) : NULL;
  CHECK(!contains(tj_copy, "\"name\":\"Extra\""));
  CHECK(pv_lsp_parse_count(s, home) == 5);
  pv_lsp_close(s);
  free(tj_copy);
  ccb_str_free(&extra);
  free(orig);
  free(home);
  free(flow_dir);
  free(styles_dir);
  free(toml);
  free(proj);
  free(err);
}

static void test_open_errors(void) {
  char tmpl[] = "/tmp/ccb-lsp-err-XXXXXX";
  char *scratch = mkdtemp(tmpl);
  char *toml = NULL;
  char *err = NULL;
  pv_lsp_server *s = NULL;
  CHECK(scratch != NULL);
  if (scratch == NULL) {
    return;
  }
  // Nonexistent toml.
  toml = join2(scratch, "missing.toml");
  s = pv_lsp_open(toml, NULL, NULL, &err);
  CHECK(s == NULL);
  CHECK(err != NULL);
  free(err);
  err = NULL;
  free(toml);
  // Toml without a [preview] section.
  toml = join2(scratch, "ccb.toml");
  CHECK(write_all(toml,
    "ccb_version = 1\n\n[project]\nname = \"nopreview\"\n",
    strlen("ccb_version = 1\n\n[project]\nname = \"nopreview\"\n")) == 0);
  s = pv_lsp_open(toml, NULL, NULL, &err);
  CHECK(s == NULL);
  CHECK(err != NULL);
  free(err);
  err = NULL;
  free(toml);
  // Unknown scenario on a real project.
  {
    char *proj = scratch_sample();
    char *stoml = NULL;
    CHECK(proj != NULL);
    if (proj != NULL) {
      stoml = join2(proj, "ccb.toml");
      s = pv_lsp_open(stoml, "no-such-scenario", NULL, &err);
      CHECK(s == NULL);
      CHECK(err != NULL);
      free(err);
      free(stoml);
      free(proj);
    }
  }
}

static int rule_eq(const pv_rule *a, const pv_rule *b) {
  size_t i = 0;
  if (strcmp(a->selector, b->selector) != 0) {
    return 0;
  }
  if ((a->scope == NULL) != (b->scope == NULL)) {
    return 0;
  }
  if (a->scope != NULL && strcmp(a->scope, b->scope) != 0) {
    return 0;
  }
  if (strcmp(a->file, b->file) != 0) {
    return 0;
  }
  if (a->line != b->line) {
    return 0;
  }
  if (a->has_display != b->has_display ||
      a->display_flex != b->display_flex ||
      a->has_flex_dir != b->has_flex_dir ||
      a->flex_row != b->flex_row ||
      a->has_width != b->has_width ||
      a->has_height != b->has_height ||
      a->width.kind != b->width.kind ||
      a->width.value != b->width.value ||
      a->height.kind != b->height.kind ||
      a->height.value != b->height.value ||
      a->has_padding != b->has_padding ||
      a->has_margin != b->has_margin ||
      a->has_font_size != b->has_font_size ||
      a->font_size != b->font_size ||
      a->has_position != b->has_position ||
      a->absolute != b->absolute ||
      a->has_top != b->has_top || a->top != b->top ||
      a->has_left != b->has_left || a->left != b->left ||
      a->has_gap != b->has_gap || a->gap != b->gap ||
      a->has_text_align != b->has_text_align ||
      a->text_align != b->text_align ||
      a->has_border_width != b->has_border_width ||
      a->border_width != b->border_width ||
      a->has_flex_wrap != b->has_flex_wrap ||
      a->flex_wrap != b->flex_wrap) {
    return 0;
  }
  for (i = 0; i < 4; i++) {
    if (a->padding[i] != b->padding[i] ||
        a->margin[i] != b->margin[i]) {
      return 0;
    }
  }
  if ((a->bg == NULL) != (b->bg == NULL) ||
      (a->bg != NULL && strcmp(a->bg, b->bg) != 0) ||
      (a->fg == NULL) != (b->fg == NULL) ||
      (a->fg != NULL && strcmp(a->fg, b->fg) != 0) ||
      (a->border_color == NULL) != (b->border_color == NULL) ||
      (a->border_color != NULL &&
       strcmp(a->border_color, b->border_color) != 0)) {
    return 0;
  }
  return 1;
}

// The server merges per-file parses with pv_css_rule_merge; the load
// path parses sequentially into one array. Same files, same rules.
static void test_css_merge(void) {
  char *sdir = sample_dir();
  char *styles = join2(sdir, "styles");
  char *common_path = join2(styles, "common.css");
  char *detail_path = join2(styles, "Detail.css");
  char *common = try_read(common_path);
  char *detail = try_read(detail_path);
  ccb_arena *arena = ccb_arena_create();
  ccb_diag_list dl;
  pv_rule *seq = NULL;
  size_t nseq = 0;
  pv_rule *per_common = NULL;
  size_t nper_common = 0;
  pv_rule *per_detail = NULL;
  size_t nper_detail = 0;
  pv_rule *merged = NULL;
  size_t nmerged = 0;
  size_t i = 0;
  CHECK(common != NULL && detail != NULL);
  ccb_diag_list_init(&dl);
  if (common != NULL && detail != NULL) {
    // Sequential: common then Detail into one array.
    CHECK(pv_css_parse(arena, common, "common.css", 0, &dl, &seq,
      &nseq) == CCB_OK);
    CHECK(pv_css_parse(arena, detail, "Detail.css", 0, &dl, &seq,
      &nseq) == CCB_OK);
    // Per-file: parse separately, then merge Detail into common.
    CHECK(pv_css_parse(arena, common, "common.css", 0, &dl,
      &per_common, &nper_common) == CCB_OK);
    CHECK(pv_css_parse(arena, detail, "Detail.css", 0, &dl,
      &per_detail, &nper_detail) == CCB_OK);
    merged = per_common;
    nmerged = nper_common;
    for (i = 0; i < nper_detail; i++) {
      pv_css_rule_merge(arena, &merged, &nmerged, &per_detail[i],
        "Detail.css", "Detail");
    }
    CHECK(nmerged == nseq);
    if (nmerged == nseq) {
      for (i = 0; i < nseq; i++) {
        CHECK(rule_eq(&seq[i], &merged[i]));
      }
    }
  }
  ccb_diag_list_free(&dl);
  ccb_arena_destroy(arena);
  free(common);
  free(detail);
  free(common_path);
  free(detail_path);
  free(styles);
  free(sdir);
}

static void test_snapshot_diff(void) {
  char *proj = scratch_sample();
  char *toml = NULL;
  char *flow_dir = NULL;
  char *styles_dir = NULL;
  pv_watch_snapshot *snap1 = NULL;
  pv_watch_snapshot *snap2 = NULL;
  pv_watch_snapshot *snap3 = NULL;
  char **paths = NULL;
  size_t npaths = 0;
  char *new_file = NULL;
  CHECK(proj != NULL);
  if (proj == NULL) {
    return;
  }
  toml = join2(proj, "ccb.toml");
  flow_dir = join2(proj, "flow");
  styles_dir = join2(proj, "styles");
  CHECK(pv_watch_snapshot_take(proj, styles_dir, flow_dir, toml,
    &snap1) == CCB_OK);
  new_file = join2(flow_dir, "Zeta.kt");
  CHECK(write_all(new_file, "fun Zeta() {\n  Column {\n  }\n}\n", 29) == 0);
  CHECK(pv_watch_snapshot_take(proj, styles_dir, flow_dir, toml,
    &snap2) == CCB_OK);
  CHECK(pv_watch_snapshot_diff(snap1, snap2, &paths, &npaths) ==
    CCB_OK);
  CHECK(npaths == 1);
  if (npaths == 1) {
    CHECK(contains(paths[0], "Zeta.kt"));
  }
  ccb_free_str_array(paths, npaths);
  paths = NULL;
  npaths = 0;
  CHECK(unlink(new_file) == 0);
  CHECK(pv_watch_snapshot_take(proj, styles_dir, flow_dir, toml,
    &snap3) == CCB_OK);
  CHECK(pv_watch_snapshot_diff(snap2, snap3, &paths, &npaths) ==
    CCB_OK);
  CHECK(npaths == 1);
  if (npaths == 1) {
    CHECK(contains(paths[0], "Zeta.kt"));
  }
  ccb_free_str_array(paths, npaths);
  paths = NULL;
  npaths = 0;
  CHECK(pv_watch_snapshot_diff(snap3, snap3, &paths, &npaths) ==
    CCB_OK);
  CHECK(npaths == 0);
  ccb_free_str_array(paths, npaths);
  pv_watch_snapshot_free(snap1);
  pv_watch_snapshot_free(snap2);
  pv_watch_snapshot_free(snap3);
  free(new_file);
  free(toml);
  free(flow_dir);
  free(styles_dir);
  free(proj);
}

// ---------- socket test harness ----------

typedef struct {
  int fd;
  char buf[65536];
  size_t len;
  int eof;
} sock_reader;

static int write_fd_all(int fd, const char *data, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, data + off, n - off);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (w == 0) {
      return -1;
    }
    off += (size_t)w;
  }
  return 0;
}

// One framed message (body only, malloc'd, NUL-terminated), or NULL on
// timeout/EOF/oversize. Frames already buffered are returned without
// waiting, so a burst of messages reads back one by one.
static char *sock_next(sock_reader *r, int timeout_ms) {
  for (;;) {
    size_t i = 0;
    size_t hdr_end = 0;
    long n = -1;
    for (i = 0; i + 3 < r->len; i++) {
      if (r->buf[i] == '\r' && r->buf[i + 1] == '\n' &&
          r->buf[i + 2] == '\r' && r->buf[i + 3] == '\n') {
        hdr_end = i + 4;
        break;
      }
    }
    if (hdr_end > 0) {
      // Parse Content-Length out of the header block.
      char hdr[4096];
      size_t hlen = hdr_end < sizeof(hdr) - 1 ? hdr_end :
        sizeof(hdr) - 1;
      size_t j = 0;
      memcpy(hdr, r->buf, hlen);
      hdr[hlen] = '\0';
      for (j = 0; j + 15 < hlen; j++) {
        if (strncasecmp(hdr + j, "content-length:", 15) == 0) {
          n = strtol(hdr + j + 15, NULL, 10);
          break;
        }
      }
      if (n < 0) {
        // No usable length: skip this header block and resync.
        memmove(r->buf, r->buf + hdr_end, r->len - hdr_end);
        r->len -= hdr_end;
        continue;
      }
      if (r->len >= hdr_end + (size_t)n) {
        char *body = ccb_xmalloc((size_t)n + 1);
        memcpy(body, r->buf + hdr_end, (size_t)n);
        body[n] = '\0';
        memmove(r->buf, r->buf + hdr_end + (size_t)n,
          r->len - hdr_end - (size_t)n);
        r->len -= hdr_end + (size_t)n;
        return body;
      }
    }
    if (r->eof) {
      return NULL;
    }
    {
      fd_set rfds;
      struct timeval tv;
      int sel = 0;
      FD_ZERO(&rfds);
      FD_SET(r->fd, &rfds);
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      sel = select(r->fd + 1, &rfds, NULL, NULL, &tv);
      if (sel <= 0) {
        return NULL;
      }
      if (r->len < sizeof(r->buf)) {
        ssize_t rd = read(r->fd, r->buf + r->len,
          sizeof(r->buf) - r->len);
        if (rd < 0) {
          if (errno == EINTR) {
            continue;
          }
          return NULL;
        }
        if (rd == 0) {
          r->eof = 1;
          continue;
        }
        r->len += (size_t)rd;
      } else {
        return NULL;
      }
    }
  }
}

static void send_frame(int fd, const char *json) {
  ccb_str frame;
  ccb_str_init(&frame);
  ccb_str_appendf(&frame, "Content-Length: %zu\r\n\r\n", strlen(json));
  ccb_str_append(&frame, json);
  (void)write_fd_all(fd, frame.buf, frame.len);
  ccb_str_free(&frame);
}

// Split write: header first, a pause, then the body. Proves the
// server's framing reader tolerates partial reads.
static void send_frame_split(int fd, const char *json) {
  ccb_str hdr;
  ccb_str_init(&hdr);
  ccb_str_appendf(&hdr, "Content-Length: %zu\r\n\r\n", strlen(json));
  (void)write_fd_all(fd, hdr.buf, hdr.len);
  ccb_str_free(&hdr);
  usleep(50000);
  (void)write_fd_all(fd, json, strlen(json));
}

static char *json_msg_id(long id, const char *method,
    const char *params) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_appendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":%s,",
    id, method);
  ccb_str_append(&s, "\"params\":");
  ccb_str_append(&s, params);
  ccb_str_append(&s, "}");
  return s.buf;
}

static char *json_notif(const char *method, const char *params) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "{\"jsonrpc\":\"2.0\",\"method\":");
  ccb_str_append(&s, method);
  ccb_str_append(&s, ",\"params\":");
  ccb_str_append(&s, params);
  ccb_str_append(&s, "}");
  return s.buf;
}

// Wait for the child and report its exit code, or -1 when it did not
// exit normally. Bounded: after ~5s the child is killed and reaped,
// so a wedged child fails the test instead of hanging ctest.
static int child_exit(pid_t pid) {
  int i = 0;
  int status = 0;
  for (i = 0; i < 50; i++) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
      return -1;
    }
    if (w < 0) {
      return -1;
    }
    usleep(100000);
  }
  kill(pid, SIGKILL);
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

// Fork a server child on a socketpair. Returns the parent fd, or -1.
static int spawn_server(const char *toml, pid_t *pid_out) {
  int sv[2];
  pid_t pid = 0;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    return -1;
  }
  if (pid == 0) {
    char *err = NULL;
    pv_lsp_server *s = NULL;
    int rc = 0;
    close(sv[0]);
    signal(SIGPIPE, SIG_IGN);
    s = pv_lsp_open(toml, NULL, NULL, &err);
    if (s == NULL) {
      _exit(2);
    }
    rc = pv_lsp_run(s, sv[1], sv[1]);
    pv_lsp_close(s);
    _exit(rc);
  }
  close(sv[1]);
  *pid_out = pid;
  return sv[0];
}

static void test_socket_session(void) {
  char *proj = scratch_sample();
  char *toml = NULL;
  char *home = NULL;
  char *flow_dir = NULL;
  char *orig = NULL;
  pid_t pid = 0;
  int fd = -1;
  sock_reader r;
  ccb_str init;
  ccb_str uri;
  char *m1 = NULL;
  char *m2 = NULL;
  char *m = NULL;
  signal(SIGPIPE, SIG_IGN);
  CHECK(proj != NULL);
  if (proj == NULL) {
    return;
  }
  toml = join2(proj, "ccb.toml");
  flow_dir = join2(proj, "flow");
  home = join2(flow_dir, "Home.kt");
  orig = try_read(home);
  CHECK(orig != NULL);
  fd = spawn_server(toml, &pid);
  CHECK(fd >= 0);
  if (fd < 0 || orig == NULL) {
    free(orig);
    free(home);
    free(flow_dir);
    free(toml);
    free(proj);
    return;
  }
  memset(&r, 0, sizeof(r));
  r.fd = fd;
  // initialize, split across writes, with the project rootUri.
  ccb_str_init(&init);
  ccb_str_append(&init,
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
    "{\"rootUri\":\"file://");
  ccb_str_append(&init, proj);
  ccb_str_append(&init, "\"}}");
  send_frame_split(fd, init.buf);
  ccb_str_free(&init);
  m1 = sock_next(&r, 5000);
  m2 = sock_next(&r, 5000);
  CHECK(m1 != NULL && m2 != NULL);
  if (m1 != NULL && m2 != NULL) {
    // One result, one publishTree, in either order.
    int m1_result = contains(m1, "\"id\":1") &&
      contains(m1, "\"textDocumentSync\":1") &&
      contains(m1, "\"name\":\"ccb lsp\"");
    int m2_result = contains(m2, "\"id\":1") &&
      contains(m2, "\"textDocumentSync\":1") &&
      contains(m2, "\"name\":\"ccb lsp\"");
    int m1_pub = contains(m1, "\"method\":\"ccb/publishTree\"") &&
      contains(m1, "\"name\":\"Home\"");
    int m2_pub = contains(m2, "\"method\":\"ccb/publishTree\"") &&
      contains(m2, "\"name\":\"Home\"");
    CHECK((m1_result && m2_pub) || (m2_result && m1_pub));
  }
  free(m1);
  free(m2);
  m1 = NULL;
  m2 = NULL;
  // logicTree: the sample tree, with edges.
  m = json_msg_id(2, "\"ccb/logicTree\"", "{}");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":2"));
    CHECK(contains(m, "\"name\":\"Home\""));
    CHECK(contains(m, "\"edges\""));
  }
  free(m);
  m = NULL;
  // screenInfo for Home: outgoing edges plus incoming from Detail.
  m = json_msg_id(3, "\"ccb/screenInfo\"", "{\"screen\":\"Home\"}");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":3"));
    CHECK(contains(m, "\"incoming\""));
    CHECK(contains(m, "\"from_screen\":\"Detail\""));
  }
  free(m);
  m = NULL;
  // diagnostics: the sample has none since Chunk C (its buttons
  // carry no font-size rules, so P1017 stays quiet); the endpoint
  // must still answer with an empty diagnostics array.
  m = json_msg_id(4, "\"ccb/diagnostics\"", "{}");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":4"));
    CHECK(contains(m, "\"diagnostics\":[]"));
  }
  free(m);
  m = NULL;
  // didOpen with a new screen: publishTree carries it.
  ccb_str_init(&uri);
  ccb_str_append(&uri, "file://");
  ccb_str_append(&uri, home);
  {
    ccb_str open_msg;
    ccb_str extra;
    ccb_str_init(&open_msg);
    ccb_str_init(&extra);
    ccb_str_append(&extra, orig);
    ccb_str_append(&extra,
      "\nfun Extra() {\n  Column {\n    Text(\"Extra screen\")\n"
      "    Button(\"Back\") { nav(\"Home\") }\n  }\n}\n");
    // JSON-escape the document text by hand: the fixture has no
    // backslashes, only quotes and newlines to escape.
    ccb_str_append(&open_msg,
      "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
      "\"params\":{\"textDocument\":{\"uri\":\"");
    ccb_str_append(&open_msg, uri.buf);
    ccb_str_append(&open_msg, "\",\"text\":\"");
    {
      const char *p = NULL;
      for (p = extra.buf; *p != '\0'; p++) {
        if (*p == '"') {
          ccb_str_append(&open_msg, "\\\"");
        } else if (*p == '\n') {
          ccb_str_append(&open_msg, "\\n");
        } else {
          ccb_str_append_n(&open_msg, p, 1);
        }
      }
    }
    ccb_str_append(&open_msg, "\"}}}");
    send_frame(fd, open_msg.buf);
    ccb_str_free(&open_msg);
    ccb_str_free(&extra);
  }
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"method\":\"ccb/publishTree\""));
    CHECK(contains(m, "\"name\":\"Extra\""));
  }
  free(m);
  m = NULL;
  // didChange with identical text: tree unchanged, no publish.
  {
    ccb_str chg;
    ccb_str_init(&chg);
    ccb_str_append(&chg,
      "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
      "\"params\":{\"textDocument\":{\"uri\":\"");
    ccb_str_append(&chg, uri.buf);
    ccb_str_append(&chg,
      "\"},\"contentChanges\":[{\"text\":\"");
    {
      // Reuse the same Extra text: rebuild the escaped form from
      // the open document (orig + Extra) — identical content means
      // the tree JSON is unchanged and nothing is published.
      ccb_str extra;
      const char *p = NULL;
      ccb_str_init(&extra);
      ccb_str_append(&extra, orig);
      ccb_str_append(&extra,
        "\nfun Extra() {\n  Column {\n    Text(\"Extra screen\")\n"
        "    Button(\"Back\") { nav(\"Home\") }\n  }\n}\n");
      for (p = extra.buf; *p != '\0'; p++) {
        if (*p == '"') {
          ccb_str_append(&chg, "\\\"");
        } else if (*p == '\n') {
          ccb_str_append(&chg, "\\n");
        } else {
          ccb_str_append_n(&chg, p, 1);
        }
      }
      ccb_str_free(&extra);
    }
    ccb_str_append(&chg, "\"}]}}");
    send_frame(fd, chg.buf);
    ccb_str_free(&chg);
  }
  m = sock_next(&r, 300);
  CHECK(m == NULL);
  free(m);
  m = NULL;
  // Unknown method with id: -32601.
  m = json_msg_id(9, "\"ccb/nope\"", "{}");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":9"));
    CHECK(contains(m, "\"code\":-32601"));
  }
  free(m);
  m = NULL;
  // Unparseable JSON body: -32700 with a null id.
  send_frame(fd, "{not json");
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":null"));
    CHECK(contains(m, "\"code\":-32700"));
  }
  free(m);
  m = NULL;
  // Unknown screen: -32602.
  m = json_msg_id(10, "\"ccb/screenInfo\"", "{\"screen\":\"Nope\"}");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":10"));
    CHECK(contains(m, "\"code\":-32602"));
  }
  free(m);
  m = NULL;
  // shutdown: null result; then a request: -32600; then exit: 0.
  m = json_msg_id(11, "\"shutdown\"", "null");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":11"));
    CHECK(contains(m, "\"result\":null"));
  }
  free(m);
  m = NULL;
  m = json_msg_id(12, "\"ccb/logicTree\"", "{}");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":12"));
    CHECK(contains(m, "\"code\":-32600"));
  }
  free(m);
  m = NULL;
  m = json_notif("\"exit\"", "null");
  send_frame(fd, m);
  free(m);
  close(fd);
  CHECK(child_exit(pid) == 0);
  ccb_str_free(&uri);
  free(orig);
  free(home);
  free(flow_dir);
  free(toml);
  free(proj);
}

static void test_socket_eof(void) {  char *proj = scratch_sample();
  char *toml = NULL;
  pid_t pid = 0;
  int fd = -1;
  sock_reader r;
  ccb_str init;
  char *m = NULL;
  signal(SIGPIPE, SIG_IGN);
  CHECK(proj != NULL);
  if (proj == NULL) {
    return;
  }
  toml = join2(proj, "ccb.toml");
  fd = spawn_server(toml, &pid);
  CHECK(fd >= 0);
  if (fd < 0) {
    free(toml);
    free(proj);
    return;
  }
  memset(&r, 0, sizeof(r));
  r.fd = fd;
  ccb_str_init(&init);
  ccb_str_append(&init,
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
    "{\"rootUri\":\"file://");
  ccb_str_append(&init, proj);
  ccb_str_append(&init, "\"}}");
  send_frame(fd, init.buf);
  ccb_str_free(&init);
  // Drain initialize result + publishTree, then close: EOF without
  // shutdown must exit 1.
  m = sock_next(&r, 5000);
  free(m);
  m = sock_next(&r, 5000);
  free(m);
  close(fd);
  CHECK(child_exit(pid) == 1);
  free(toml);
  free(proj);
}

static void test_socket_reroot(void) {
  char *proj_a = scratch_sample();
  char *toml_a = NULL;
  char *proj_b = NULL;
  char *toml_b = NULL;
  char *flow_b = NULL;
  char *styles_b = NULL;
  char *solo = NULL;
  pid_t pid = 0;
  int fd = -1;
  sock_reader r;
  ccb_str init;
  char *m = NULL;
  char tmpl[] = "/tmp/ccb-lsp-reroot-XXXXXX";
  char *scratch = mkdtemp(tmpl);
  signal(SIGPIPE, SIG_IGN);
  CHECK(proj_a != NULL && scratch != NULL);
  if (proj_a == NULL || scratch == NULL) {
    free(proj_a);
    return;
  }
  toml_a = join2(proj_a, "ccb.toml");
  proj_b = join2(scratch, "projb");
  mkdir(proj_b, 0777);
  flow_b = join2(proj_b, "flow");
  styles_b = join2(proj_b, "styles");
  mkdir(flow_b, 0777);
  mkdir(styles_b, 0777);
  toml_b = join2(proj_b, "ccb.toml");
  CHECK(write_all(toml_b,
    "ccb_version = 1\n\n[project]\nname = \"tiny\"\n"
    "package = \"com.example.tiny\"\nversion = \"1.0\"\nversion_code = 1\n"
    "min_sdk = 26\ntarget_sdk = 35\ncompile_sdk = 35\n\n[preview]\n"
    "styles = \"styles\"\nflow = \"flow\"\nphone_width = 390\n\n[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n",
    strlen("ccb_version = 1\n\n[project]\nname = \"tiny\"\n"
    "package = \"com.example.tiny\"\nversion = \"1.0\"\nversion_code = 1\n"
    "min_sdk = 26\ntarget_sdk = 35\ncompile_sdk = 35\n\n[preview]\n"
    "styles = \"styles\"\nflow = \"flow\"\nphone_width = 390\n\n[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n")) == 0);
  {
    // Config load requires the manifest file to exist.
    char *src_dir = join2(proj_b, "src");
    char *src_main = join2(proj_b, "src/main");
    char *manifest = NULL;
    mkdir(src_dir, 0777);
    mkdir(src_main, 0777);
    manifest = join2(src_main, "AndroidManifest.xml");
    CHECK(write_all(manifest,
      "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"com.example.tiny\">\n</manifest>\n",
      strlen("<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"com.example.tiny\">\n</manifest>\n")) == 0);
    free(manifest);
    free(src_main);
    free(src_dir);
  }
  solo = join2(flow_b, "Solo.kt");
  CHECK(write_all(solo,
    "fun Solo() {\n  Column {\n    Text(\"Hi\")\n  }\n}\n",
    strlen("fun Solo() {\n  Column {\n    Text(\"Hi\")\n  }\n}\n")) == 0);
  {
    char *common = join2(styles_b, "common.css");
    CHECK(write_all(common, "screen { background-color: #ffffff; }\n",
      strlen("screen { background-color: #ffffff; }\n")) == 0);
    free(common);
  }
  fd = spawn_server(toml_a, &pid);
  CHECK(fd >= 0);
  if (fd < 0) {
    free(solo);
    free(toml_b);
    free(styles_b);
    free(flow_b);
    free(proj_b);
    free(toml_a);
    free(proj_a);
    return;
  }
  memset(&r, 0, sizeof(r));
  r.fd = fd;
  ccb_str_init(&init);
  ccb_str_append(&init,
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
    "{\"rootUri\":\"file://");
  ccb_str_append(&init, proj_a);
  ccb_str_append(&init, "\"}}");
  send_frame(fd, init.buf);
  ccb_str_free(&init);
  m = sock_next(&r, 5000);
  free(m);
  m = sock_next(&r, 5000);
  free(m);
  m = json_msg_id(2, "\"ccb/logicTree\"", "{}");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"name\":\"Home\""));
  }
  free(m);
  m = NULL;
  // Second initialize with a different root: the server re-roots.
  ccb_str_init(&init);
  ccb_str_append(&init,
    "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"initialize\",\"params\":"
    "{\"rootUri\":\"file://");
  ccb_str_append(&init, proj_b);
  ccb_str_append(&init, "\"}}");
  send_frame(fd, init.buf);
  ccb_str_free(&init);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"id\":20"));
  }
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"method\":\"ccb/publishTree\""));
    CHECK(contains(m, "\"name\":\"Solo\""));
  }
  free(m);
  m = NULL;
  m = json_msg_id(21, "\"ccb/logicTree\"", "{}");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  CHECK(m != NULL);
  if (m != NULL) {
    CHECK(contains(m, "\"name\":\"Solo\""));
    CHECK(!contains(m, "\"name\":\"Home\""));
  }
  free(m);
  m = NULL;
  m = json_msg_id(22, "\"shutdown\"", "null");
  send_frame(fd, m);
  free(m);
  m = sock_next(&r, 5000);
  free(m);
  m = json_notif("\"exit\"", "null");
  send_frame(fd, m);
  free(m);
  close(fd);
  CHECK(child_exit(pid) == 0);
  free(solo);
  free(toml_b);
  free(styles_b);
  free(flow_b);
  free(proj_b);
  free(toml_a);
  free(proj_a);
}

// Regression: a first initialize whose rootUri spells the same
// directory with a trailing slash must still yield result +
// publishTree. Before the fix, the spelling mismatch counted as a
// re-root, the handler skipped the publish (rebuild queued), and
// the rebuild reported the identical tree as unchanged, so the
// initial publish never arrived.
static void test_socket_trailing_slash(void) {
  char *proj = scratch_sample();
  char *toml = NULL;
  pid_t pid = 0;
  int fd = -1;
  sock_reader r;
  ccb_str init;
  char *m1 = NULL;
  char *m2 = NULL;
  signal(SIGPIPE, SIG_IGN);
  CHECK(proj != NULL);
  if (proj == NULL) {
    return;
  }
  toml = join2(proj, "ccb.toml");
  fd = spawn_server(toml, &pid);
  CHECK(fd >= 0);
  if (fd < 0) {
    free(toml);
    free(proj);
    return;
  }
  memset(&r, 0, sizeof(r));
  r.fd = fd;
  ccb_str_init(&init);
  ccb_str_append(&init,
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
    "{\"rootUri\":\"file://");
  ccb_str_append(&init, proj);
  ccb_str_append(&init, "/\"}}");
  send_frame(fd, init.buf);
  ccb_str_free(&init);
  m1 = sock_next(&r, 5000);
  m2 = sock_next(&r, 5000);
  CHECK(m1 != NULL && m2 != NULL);
  if (m1 != NULL && m2 != NULL) {
    int has_result = contains(m1, "\"id\":1") ||
      contains(m2, "\"id\":1");
    int has_pub = contains(m1, "\"method\":\"ccb/publishTree\"") ||
      contains(m2, "\"method\":\"ccb/publishTree\"");
    CHECK(has_result);
    CHECK(has_pub);
  }
  free(m1);
  free(m2);
  m1 = json_msg_id(2, "\"shutdown\"", "null");
  send_frame(fd, m1);
  free(m1);
  m1 = sock_next(&r, 5000);
  free(m1);
  m1 = json_notif("\"exit\"", "null");
  send_frame(fd, m1);
  free(m1);
  close(fd);
  CHECK(child_exit(pid) == 0);
  free(toml);
  free(proj);
}

int main(void) {
  test_equivalence();
  test_incremental();
  test_open_errors();
  test_css_merge();
  test_snapshot_diff();
  test_socket_session();
  test_socket_eof();
  test_socket_reroot();
  test_socket_trailing_slash();
  TEST_MAIN_END();
}
