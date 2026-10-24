// ccb_preview_diff: compare a live preview render against a snapshot
// directory. No external diff binary, no system()/popen: the unified
// diff is a small line-based LCS diff built in this module. The
// current render mirrors --update-goldens exactly (frames with no
// color, logic tree, JSON with no trailing newline), after the same
// pv_project_diagnose pass the CLI runs before regenerating goldens,
// so a golden snapshot diffs identical.
#include "ccb_common.h"
#include "ccb_preview_diff.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_json.h"
#include <dirent.h>
#include <sys/stat.h>

#define PV_DIFF_CTX 3
// LCS cell cap: frames and logic trees are small, and preview.json
// is a single line, so real inputs stay far below this. Above it the
// diff falls back to a whole-file replace instead of allocating a
// quadratic table for a hostile snapshot file.
#define PV_DIFF_LCS_CELLS 1000000

// ---------- compared-file list ----------

enum { PV_FILE_IDENTICAL, PV_FILE_DIFFERENT, PV_FILE_MISSING,
  PV_FILE_STALE };

typedef struct {
  char *name; // malloc'd; "<Screen>.txt", "logic-tree.txt", "preview.json"
  char *cur; // malloc'd current render bytes, NULL for stale files
  size_t cur_len;
  int status;
  char *diff; // malloc'd unified diff, only for "different" files
} diff_file;

typedef struct {
  diff_file *items;
  size_t count, cap;
} diff_list;

static void diff_list_free(diff_list *list) {
  size_t i = 0;
  if (list == NULL) {
    return;
  }
  for (i = 0; i < list->count; i++) {
    free(list->items[i].name);
    free(list->items[i].cur);
    free(list->items[i].diff);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->cap = 0;
}

static diff_file *diff_list_find(diff_list *list, const char *name) {
  size_t i = 0;
  for (i = 0; i < list->count; i++) {
    if (strcmp(list->items[i].name, name) == 0) {
      return &list->items[i];
    }
  }
  return NULL;
}

// Add a file, or replace the current render when the name already
// exists. This mirrors --update-goldens, which writes screens first
// and then overwrites logic-tree.txt and preview.json: the last
// writer for a name wins (e.g. a screen literally named
// "logic-tree" loses to the logic tree itself).
static void diff_list_add(diff_list *list, char *name, char *cur,
    size_t cur_len) {
  diff_file *slot = diff_list_find(list, name);
  if (slot != NULL) {
    free(slot->cur);
    free(name);
    slot->cur = cur;
    slot->cur_len = cur_len;
    slot->status = PV_FILE_IDENTICAL;
    free(slot->diff);
    slot->diff = NULL;
    return;
  }
  if (list->count == list->cap) {
    size_t ncap = list->cap == 0 ? 16 : list->cap * 2;
    list->items = ccb_xrealloc(list->items, ncap * sizeof(*list->items));
    list->cap = ncap;
  }
  slot = &list->items[list->count++];
  slot->name = name;
  slot->cur = cur;
  slot->cur_len = cur_len;
  slot->status = PV_FILE_IDENTICAL;
  slot->diff = NULL;
}

// Rendered text as a malloc'd buffer plus its byte length. A render
// that produces nothing still yields an empty (non-NULL) buffer.
static char *render_steal(ccb_str *s, size_t *len_out) {
  *len_out = s->len;
  return ccb_str_steal(s);
}

static char *screen_file_name(const char *screen_name) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, screen_name != NULL ? screen_name : "?");
  ccb_str_append(&s, ".txt");
  return ccb_str_steal(&s);
}

// ---------- line-based LCS unified diff ----------

typedef struct {
  const char *ptr;
  size_t len;
  int no_nl; // last line of a file that does not end with '\n'
} diff_line;

// Split text into lines (content without the '\n'). An empty file has
// no lines; a file ending in '\n' has no phantom trailing line.
static diff_line *split_lines(const char *text, size_t len, size_t *n_out) {
  diff_line *lines = NULL;
  size_t count = 0;
  size_t cap = 0;
  size_t start = 0;
  size_t i = 0;
  if (len == 0) {
    *n_out = 0;
    return NULL;
  }
  for (i = 0; i < len; i++) {
    if (text[i] != '\n') {
      continue;
    }
    if (count == cap) {
      size_t ncap = cap == 0 ? 32 : cap * 2;
      lines = ccb_xrealloc(lines, ncap * sizeof(*lines));
      cap = ncap;
    }
    lines[count].ptr = text + start;
    lines[count].len = i - start;
    lines[count].no_nl = 0;
    count++;
    start = i + 1;
  }
  if (start < len) {
    if (count == cap) {
      size_t ncap = cap == 0 ? 32 : cap * 2;
      lines = ccb_xrealloc(lines, ncap * sizeof(*lines));
      cap = ncap;
    }
    lines[count].ptr = text + start;
    lines[count].len = len - start;
    lines[count].no_nl = 1;
    count++;
  }
  *n_out = count;
  return lines;
}

static int lines_equal(const diff_line *a, const diff_line *b) {
  return a->len == b->len &&
    (a->len == 0 || memcmp(a->ptr, b->ptr, a->len) == 0);
}

enum { OP_EQ, OP_DEL, OP_INS };

typedef struct {
  int kind;
  size_t old_idx; // valid for EQ and DEL
  size_t new_idx; // valid for EQ and INS
} diff_op;

// Emit one hunk's header and lines. consumed counts are the numbers
// of old/new lines before the hunk; counts are the lines in the hunk.
static void emit_hunk(ccb_str *out, const diff_line *old_lines,
    const diff_line *new_lines, const diff_op *ops, size_t from,
    size_t to, size_t old_consumed, size_t new_consumed) {
  size_t old_count = 0;
  size_t new_count = 0;
  size_t old_start = 0;
  size_t new_start = 0;
  size_t i = 0;
  for (i = from; i <= to; i++) {
    if (ops[i].kind != OP_INS) {
      old_count++;
    }
    if (ops[i].kind != OP_DEL) {
      new_count++;
    }
  }
  old_start = old_count == 0 ? old_consumed : old_consumed + 1;
  new_start = new_count == 0 ? new_consumed : new_consumed + 1;
  ccb_str_append(out, "@@ -");
  ccb_str_appendf(out, "%zu", old_start);
  if (old_count != 1) {
    ccb_str_appendf(out, ",%zu", old_count);
  }
  ccb_str_append(out, " +");
  ccb_str_appendf(out, "%zu", new_start);
  if (new_count != 1) {
    ccb_str_appendf(out, ",%zu", new_count);
  }
  ccb_str_append(out, " @@\n");
  for (i = from; i <= to; i++) {
    const diff_line *line = NULL;
    if (ops[i].kind == OP_EQ) {
      line = &old_lines[ops[i].old_idx];
      ccb_str_append_c(out, ' ');
    } else if (ops[i].kind == OP_DEL) {
      line = &old_lines[ops[i].old_idx];
      ccb_str_append_c(out, '-');
    } else {
      line = &new_lines[ops[i].new_idx];
      ccb_str_append_c(out, '+');
    }
    ccb_str_append_n(out, line->ptr, line->len);
    ccb_str_append_c(out, '\n');
    if (line->no_nl) {
      ccb_str_append(out, "\\ No newline at end of file\n");
    }
  }
}

// Group ops into hunks: a hunk covers a run of changes whose gaps of
// unchanged lines are at most 2 * context, padded with context lines
// on both sides.
static void emit_unified(ccb_str *out, const diff_line *old_lines,
    const diff_line *new_lines, const diff_op *ops, size_t nops) {
  size_t idx = 0;
  while (idx < nops) {
    size_t first = idx;
    size_t last = 0;
    size_t hunk_from = 0;
    size_t hunk_to = 0;
    size_t old_consumed = 0;
    size_t new_consumed = 0;
    size_t i = 0;
    while (first < nops && ops[first].kind == OP_EQ) {
      first++;
    }
    if (first >= nops) {
      break;
    }
    last = first;
    for (;;) {
      size_t next = last + 1;
      while (next < nops && ops[next].kind == OP_EQ) {
        next++;
      }
      if (next >= nops || next - last - 1 > 2 * PV_DIFF_CTX) {
        break;
      }
      last = next;
    }
    hunk_from = first >= PV_DIFF_CTX ? first - PV_DIFF_CTX : 0;
    hunk_to = last + PV_DIFF_CTX < nops ? last + PV_DIFF_CTX : nops - 1;
    for (i = 0; i < hunk_from; i++) {
      if (ops[i].kind != OP_INS) {
        old_consumed++;
      }
      if (ops[i].kind != OP_DEL) {
        new_consumed++;
      }
    }
    emit_hunk(out, old_lines, new_lines, ops, hunk_from, hunk_to,
      old_consumed, new_consumed);
    idx = hunk_to + 1;
  }
}

// Build the unified diff body for old_text -> new_text, including the
// ---/+++ header lines. The caller only calls this for differing
// files, so at least one hunk is always emitted.
static char *build_unified_diff(const char *name, const char *old_text,
    size_t old_len, const char *new_text, size_t new_len) {
  diff_line *old_lines = NULL;
  diff_line *new_lines = NULL;
  size_t nold = 0;
  size_t nnew = 0;
  diff_op *ops = NULL;
  size_t nops = 0;
  size_t capops = 0;
  ccb_str out;
  ccb_str_init(&out);
  old_lines = split_lines(old_text, old_len, &nold);
  new_lines = split_lines(new_text, new_len, &nnew);
  ccb_str_append(&out, "--- snapshot/");
  ccb_str_append(&out, name);
  ccb_str_append(&out, "\n+++ current/");
  ccb_str_append(&out, name);
  ccb_str_append(&out, "\n");
  if (nold == 0 || nnew == 0 || nold > PV_DIFF_LCS_CELLS / (nnew == 0 ?
      1 : nnew)) {
    // Degenerate or oversized input: whole-file replace, one hunk.
    size_t i = 0;
    for (i = 0; i < nold; i++) {
      if (nops == capops) {
        size_t ncap = capops == 0 ? 32 : capops * 2;
        ops = ccb_xrealloc(ops, ncap * sizeof(*ops));
        capops = ncap;
      }
      ops[nops].kind = OP_DEL;
      ops[nops].old_idx = i;
      ops[nops].new_idx = 0;
      nops++;
    }
    for (i = 0; i < nnew; i++) {
      if (nops == capops) {
        size_t ncap = capops == 0 ? 32 : capops * 2;
        ops = ccb_xrealloc(ops, ncap * sizeof(*ops));
        capops = ncap;
      }
      ops[nops].kind = OP_INS;
      ops[nops].old_idx = 0;
      ops[nops].new_idx = i;
      nops++;
    }
  } else {
    // Prefix LCS table: dp[i][j] is the LCS length of the first i old
    // lines and the first j new lines.
    size_t stride = nnew + 1;
    size_t *dp = ccb_xcalloc((nold + 1) * stride, sizeof(*dp));
    size_t i = 0;
    size_t j = 0;
    for (i = 1; i <= nold; i++) {
      for (j = 1; j <= nnew; j++) {
        if (lines_equal(&old_lines[i - 1], &new_lines[j - 1])) {
          dp[i * stride + j] = dp[(i - 1) * stride + (j - 1)] + 1;
        } else if (dp[(i - 1) * stride + j] >= dp[i * stride + (j - 1)]) {
          dp[i * stride + j] = dp[(i - 1) * stride + j];
        } else {
          dp[i * stride + j] = dp[i * stride + (j - 1)];
        }
      }
    }
    // Backtrack from the end; ties prefer insertions so the final
    // (reversed) order shows deletions before insertions.
    i = nold;
    j = nnew;
    while (i > 0 || j > 0) {
      diff_op op;
      if (i > 0 && j > 0 &&
          lines_equal(&old_lines[i - 1], &new_lines[j - 1])) {
        op.kind = OP_EQ;
        op.old_idx = i - 1;
        op.new_idx = j - 1;
        i--;
        j--;
      } else if (j > 0 && (i == 0 ||
          dp[i * stride + (j - 1)] >= dp[(i - 1) * stride + j])) {
        op.kind = OP_INS;
        op.old_idx = 0;
        op.new_idx = j - 1;
        j--;
      } else {
        op.kind = OP_DEL;
        op.old_idx = i - 1;
        op.new_idx = 0;
        i--;
      }
      if (nops == capops) {
        size_t ncap = capops == 0 ? 32 : capops * 2;
        ops = ccb_xrealloc(ops, ncap * sizeof(*ops));
        capops = ncap;
      }
      ops[nops++] = op;
    }
    free(dp);
    // The backtrack produced ops in reverse order; flip them.
    {
      size_t lo = 0;
      size_t hi = nops == 0 ? 0 : nops - 1;
      while (lo < hi) {
        diff_op tmp = ops[lo];
        ops[lo] = ops[hi];
        ops[hi] = tmp;
        lo++;
        hi--;
      }
    }
  }
  emit_unified(&out, old_lines, new_lines, ops, nops);
  free(ops);
  free(old_lines);
  free(new_lines);
  return ccb_str_steal(&out);
}

// ---------- report emission ----------

static const char *status_name(int status) {
  switch (status) {
    case PV_FILE_DIFFERENT: return "different";
    case PV_FILE_MISSING: return "missing";
    case PV_FILE_STALE: return "stale";
    default: return "identical";
  }
}

static void emit_human(ccb_str *out, const char *snapshot_dir,
    const diff_list *list, int identical) {
  size_t i = 0;
  size_t ndiff = 0;
  if (identical) {
    ccb_str_append(out, "preview identical to snapshot in ");
    ccb_str_append(out, snapshot_dir);
    ccb_str_append_c(out, '\n');
    return;
  }
  for (i = 0; i < list->count; i++) {
    if (list->items[i].status == PV_FILE_IDENTICAL) {
      continue;
    }
    ndiff++;
    if (list->items[i].status == PV_FILE_DIFFERENT) {
      ccb_str_append(out, "diff: ");
      ccb_str_append(out, list->items[i].name);
      ccb_str_append_c(out, '\n');
      ccb_str_append(out, list->items[i].diff != NULL ?
        list->items[i].diff : "");
    } else if (list->items[i].status == PV_FILE_MISSING) {
      ccb_str_append(out, "missing: ");
      ccb_str_append(out, list->items[i].name);
      ccb_str_append(out, " (not in snapshot)\n");
    } else {
      ccb_str_append(out, "stale: ");
      ccb_str_append(out, list->items[i].name);
      ccb_str_append(out, " (no matching current screen)\n");
    }
  }
  ccb_str_appendf(out, "differing files (%zu): ", ndiff);
  {
    int first = 1;
    for (i = 0; i < list->count; i++) {
      if (list->items[i].status == PV_FILE_IDENTICAL) {
        continue;
      }
      if (!first) {
        ccb_str_append(out, ", ");
      }
      first = 0;
      ccb_str_append(out, list->items[i].name);
    }
  }
  ccb_str_append_c(out, '\n');
}

static void emit_json(ccb_str *out, const diff_list *list, int identical) {
  size_t i = 0;
  ccb_str_append(out, "{\"event\":\"preview-diff\",\"identical\":");
  ccb_str_append(out, identical ? "true" : "false");
  ccb_str_append(out, ",\"files\":[");
  for (i = 0; i < list->count; i++) {
    if (i > 0) {
      ccb_str_append(out, ",");
    }
    ccb_str_append(out, "{\"name\":");
    ccb_json_escape(out, list->items[i].name);
    ccb_str_append(out, ",\"status\":");
    ccb_json_escape(out, status_name(list->items[i].status));
    ccb_str_append(out, ",\"diff\":");
    if (list->items[i].diff != NULL) {
      ccb_json_escape(out, list->items[i].diff);
    } else {
      ccb_str_append(out, "null");
    }
    ccb_str_append(out, "}");
  }
  ccb_str_append(out, "]}");
}

// ---------- snapshot directory enumeration ----------

static int name_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// Append a stale entry for every snapshot-dir entry whose name is not
// in the current list. Entries are sorted, so stale files report in a
// deterministic order after the current files.
static void collect_stale(diff_list *list, const char *snapshot_dir) {
  DIR *d = opendir(snapshot_dir);
  char **names = NULL;
  size_t count = 0;
  size_t cap = 0;
  struct dirent *e = NULL;
  size_t i = 0;
  if (d == NULL) {
    return;
  }
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    if (count == cap) {
      size_t ncap = cap == 0 ? 32 : cap * 2;
      names = ccb_xrealloc(names, ncap * sizeof(*names));
      cap = ncap;
    }
    names[count++] = ccb_xstrdup(e->d_name);
  }
  closedir(d);
  if (count > 1) {
    qsort(names, count, sizeof(*names), name_cmp);
  }
  for (i = 0; i < count; i++) {
    if (diff_list_find(list, names[i]) != NULL) {
      free(names[i]);
    } else {
      diff_file *slot = NULL;
      if (list->count == list->cap) {
        size_t ncap = list->cap == 0 ? 16 : list->cap * 2;
        list->items = ccb_xrealloc(list->items,
          ncap * sizeof(*list->items));
        list->cap = ncap;
      }
      slot = &list->items[list->count++];
      slot->name = names[i];
      slot->cur = NULL;
      slot->cur_len = 0;
      slot->status = PV_FILE_STALE;
      slot->diff = NULL;
    }
  }
  free(names);
}

// ---------- entry point ----------

int pv_diff_run(const ccb_config *cfg, const char *snapshot_dir,
    const char *screen_or_null, long frame_width, int ascii, int strict,
    int json, ccb_str *out, ccb_str *err_out) {
  pv_project *proj = NULL;
  ccb_status lrc = CCB_OK;
  diff_list list;
  size_t i = 0;
  int identical = 1;
  int rc = 0;
  struct stat st;
  if (cfg == NULL || snapshot_dir == NULL || snapshot_dir[0] == '\0' ||
      out == NULL || err_out == NULL) {
    if (err_out != NULL) {
      ccb_str_append(err_out, "error: invalid arguments to preview diff\n");
    }
    return 2;
  }
  {
    DIR *probe = NULL;
    if (stat(snapshot_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
      probe = NULL;
    } else {
      probe = opendir(snapshot_dir);
    }
    if (probe == NULL) {
      ccb_str_append(err_out, "error: cannot read snapshot directory '");
      ccb_str_append(err_out, snapshot_dir);
      ccb_str_append(err_out, "'\n");
      return 2;
    }
    closedir(probe);
  }
  lrc = pv_project_load(cfg, frame_width, ascii, strict, NULL, &proj);
  if (lrc == CCB_ERR_CONFIG || proj == NULL) {
    ccb_str_append(err_out, "error: preview not applicable to this "
      "project; --diff needs a preview project to compare against\n");
    return 2;
  }
  if (lrc != CCB_OK) {
    ccb_str_append(err_out, "error: cannot load preview project\n");
    return 2;
  }
  // Same diagnostics pass the CLI runs before regenerating goldens:
  // reachability marks and diagnostics feed the logic tree and the
  // JSON, so skipping it would diff-differ from a golden snapshot.
  (void)pv_project_diagnose(proj);
  memset(&list, 0, sizeof(list));
  if (screen_or_null != NULL) {
    pv_screen *screen = NULL;
    ccb_str rendered;
    for (i = 0; i < proj->nscreens; i++) {
      if (proj->screens[i]->name != NULL &&
          strcmp(proj->screens[i]->name, screen_or_null) == 0) {
        screen = proj->screens[i];
        break;
      }
    }
    if (screen == NULL) {
      ccb_str_append(err_out, "error: no screen named '");
      ccb_str_append(err_out, screen_or_null);
      ccb_str_append(err_out, "'\n");
      pv_project_free(proj);
      return 2;
    }
    ccb_str_init(&rendered);
    pv_render_screen(&rendered, screen, proj->frame_width, ascii, 0);
    {
      size_t len = 0;
      char *buf = render_steal(&rendered, &len);
      diff_list_add(&list, screen_file_name(screen->name), buf, len);
    }
  } else {
    for (i = 0; i < proj->nscreens; i++) {
      ccb_str rendered;
      size_t len = 0;
      char *buf = NULL;
      ccb_str_init(&rendered);
      pv_render_screen(&rendered, proj->screens[i], proj->frame_width,
        ascii, 0);
      buf = render_steal(&rendered, &len);
      diff_list_add(&list,
        screen_file_name(proj->screens[i]->name), buf, len);
    }
    {
      ccb_str rendered;
      size_t len = 0;
      char *buf = NULL;
      ccb_str_init(&rendered);
      pv_render_logic_tree(&rendered, proj);
      buf = render_steal(&rendered, &len);
      diff_list_add(&list, ccb_xstrdup("logic-tree.txt"), buf, len);
    }
    {
      ccb_str rendered;
      size_t len = 0;
      char *buf = NULL;
      ccb_str_init(&rendered);
      pv_project_json(&rendered, proj);
      buf = render_steal(&rendered, &len);
      diff_list_add(&list, ccb_xstrdup("preview.json"), buf, len);
    }
  }
  pv_project_free(proj);
  // Compare each current file against its snapshot twin.
  for (i = 0; i < list.count; i++) {
    char *path = ccb_path_join(snapshot_dir, list.items[i].name);
    char *snap = NULL;
    size_t snap_len = 0;
    if (stat(path, &st) != 0) {
      list.items[i].status = PV_FILE_MISSING;
    } else if (ccb_read_file(path, &snap, &snap_len) != CCB_OK) {
      // A snapshot entry that exists but cannot be read is a clean
      // diagnostic, never a crash: report it as missing and keep
      // comparing the rest.
      ccb_str_append(err_out, "warning: cannot read snapshot file '");
      ccb_str_append(err_out, list.items[i].name);
      ccb_str_append(err_out, "'\n");
      list.items[i].status = PV_FILE_MISSING;
    } else if (snap_len == list.items[i].cur_len &&
        (snap_len == 0 ||
        memcmp(snap, list.items[i].cur, snap_len) == 0)) {
      list.items[i].status = PV_FILE_IDENTICAL;
    } else {
      list.items[i].status = PV_FILE_DIFFERENT;
      list.items[i].diff = build_unified_diff(list.items[i].name,
        snap, snap_len, list.items[i].cur, list.items[i].cur_len);
    }
    free(snap);
    free(path);
  }
  if (screen_or_null == NULL) {
    collect_stale(&list, snapshot_dir);
  }
  for (i = 0; i < list.count; i++) {
    if (list.items[i].status != PV_FILE_IDENTICAL) {
      identical = 0;
      break;
    }
  }
  if (json) {
    emit_json(out, &list, identical);
  } else {
    emit_human(out, snapshot_dir, &list, identical);
  }
  rc = identical ? 0 : 1;
  diff_list_free(&list);
  return rc;
}
