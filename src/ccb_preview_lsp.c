// ccb_preview_lsp: `ccb lsp` server. See ccb_preview_lsp.h for the
// contract. Single-threaded: the run loop polls stdin and the watch
// context's inotify wakeup; nothing here spawns a thread.
#include "ccb_preview_lsp.h"
#include "ccb_preview_trace.h"
#include "ccb_preview_watch.h"
#include "ccb_json.h"
#include "ccb_fs.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// The phone default frame width and phone DP width, mirroring
// PV_DEFAULT_FRAME_WIDTH / PV_DEFAULT_PHONE_DP_WIDTH in
// ccb_preview.c (file-local there). The server has no --width flag;
// it serves the phone-default frame.
#define PV_LSP_FRAME_WIDTH 40
#define PV_LSP_PHONE_DP_WIDTH 390

// One open editor document: malloc'd path + text, owned by the list.
typedef struct {
  char *path;
  char *text;
} pv_lsp_doc;

// One extra watched file (nav/layout XML, resources values): stat
// identity for the disk-change diff. The set itself is re-enumerated
// on every scan, so added/removed files are detected too.
typedef struct {
  char *path;
  long long mtime_sec;
  long mtime_nsec;
  long long size;
} pv_lsp_stat;

struct pv_lsp_server {
  char *toml_path; // malloc'd absolute
  char *config_dir; // malloc'd (dirname of toml_path)
  ccb_config *cfg; // owned
  char *scenario_name; // malloc'd or NULL
  pv_trace *trace; // owned or NULL
  // XML inputs, rescanned fresh on every build (small files; the
  // scan's P1019 warnings regenerate in load order each build).
  ccb_arena *xml_arena;
  char **graph_keys;
  char **graph_vals;
  size_t ngraph;
  char **onclick_methods;
  char **onclick_labels;
  size_t nonclick;
  // Staging caches, sorted by path.
  pv_lsp_flow_entry *flow;
  size_t nflow, capflow;
  pv_lsp_css_entry *css;
  size_t ncss, capcss;
  // Open editor documents.
  pv_lsp_doc *docs;
  size_t ndocs, capdocs;
  // Disk watching: the watch context (inotify wakeup) plus this
  // server's own snapshot for path-level diffs.
  pv_watch_ctx *watch_ctx;
  pv_watch_snapshot *watch_snap;
  pv_watch_debounce deb;
  // Extra-file stats (nav/layout/resources), sorted by path.
  pv_lsp_stat *extra;
  size_t nextra, capextra;
  // Served state.
  pv_project *project; // owned, NULL before the first good build
  char *tree_json; // malloc'd pv_project_json output, or NULL
  char *last_published; // malloc'd tree JSON of the last publish
  int shutdown; // shutdown request received
  int editor_dirty; // an editor update needs an immediate rebuild
  int config_dirty; // ccb.toml changed: reload config at rebuild
  int xml_dirty; // nav/layout changed: invalidate the flow cache
  int resources_dirty; // resources changed: rebuild (re-read in build)
  int first_build; // no build has run yet
};

// ---------- small helpers ----------

static int lsp_has_suffix(const char *path, const char *suffix) {
  size_t pl = 0;
  size_t sl = 0;
  if (path == NULL || suffix == NULL) {
    return 0;
  }
  pl = strlen(path);
  sl = strlen(suffix);
  return pl >= sl && strcmp(path + pl - sl, suffix) == 0;
}

static int lsp_path_under(const char *path, const char *dir) {
  size_t dl = 0;
  if (path == NULL || dir == NULL || dir[0] == '\0') {
    return 0;
  }
  dl = strlen(dir);
  if (strncmp(path, dir, dl) != 0) {
    return 0;
  }
  // dir itself is not "under" it; a child needs the separator (or
  // dir already ends in one).
  if (path[dl] == '\0') {
    return 0;
  }
  if (dir[dl - 1] == '/') {
    return 1;
  }
  return path[dl] == '/';
}

// Input kinds for a path, for didOpen/didChange/didClose and disk
// diffs. IGNORE covers everything that is not a preview input.
typedef enum {
  PV_LSP_IGNORE,
  PV_LSP_FLOW,
  PV_LSP_CSS,
  PV_LSP_TOML,
  PV_LSP_XML,
  PV_LSP_RESOURCES
} pv_lsp_kind;

static pv_lsp_kind lsp_classify(pv_lsp_server *s, const char *path) {
  if (path == NULL) {
    return PV_LSP_IGNORE;
  }
  if (strcmp(path, s->toml_path) == 0) {
    return PV_LSP_TOML;
  }
  if (s->cfg != NULL && s->cfg->preview_flow != NULL &&
      lsp_path_under(path, s->cfg->preview_flow) &&
      lsp_has_suffix(path, ".kt")) {
    return PV_LSP_FLOW;
  }
  if (s->cfg != NULL && s->cfg->preview_styles != NULL &&
      lsp_path_under(path, s->cfg->preview_styles) &&
      lsp_has_suffix(path, ".css")) {
    return PV_LSP_CSS;
  }
  if (s->cfg != NULL && s->cfg->preview_resources != NULL) {
    char *sp = ccb_path_join(s->cfg->preview_resources, "strings.xml");
    char *cp = ccb_path_join(s->cfg->preview_resources, "colors.xml");
    int hit = strcmp(path, sp) == 0 || strcmp(path, cp) == 0;
    free(sp);
    free(cp);
    if (hit) {
      return PV_LSP_RESOURCES;
    }
  }
  if (s->cfg != NULL) {
    // Nav/layout inputs: a file, or any .xml under a directory.
    const char *dirs[2];
    size_t d = 0;
    dirs[0] = s->cfg->preview_nav_graph;
    dirs[1] = s->cfg->preview_layout;
    for (d = 0; d < 2; d++) {
      const char *base = dirs[d];
      if (base == NULL) {
        continue;
      }
      // An exact match counts regardless of extension: a configured
      // single file is an input even without a .xml suffix (load
      // includes it unconditionally); under a directory only .xml
      // files are inputs, matching the enumeration filter.
      if (strcmp(path, base) == 0) {
        return PV_LSP_XML;
      }
      if (lsp_path_under(path, base) && lsp_has_suffix(path, ".xml")) {
        return PV_LSP_XML;
      }
    }
    // Fallbacks from pv_nav_path/pv_layout_path: resources or [paths]
    // res subdirectories. These mirror load's resolution so an XML
    // change under a fallback dir is classified, not ignored.
    {
      const char *roots[2];
      const char *subs[2];
      size_t r = 0;
      roots[0] = s->cfg->preview_resources;
      roots[1] = s->cfg->res;
      subs[0] = "navigation";
      subs[1] = "layout";
      // navigation lives under resources/res; layout too. Check both
      // subdirs under both roots (cheap string compares).
      for (r = 0; r < 2; r++) {
        size_t k = 0;
        if (roots[r] == NULL) {
          continue;
        }
        for (k = 0; k < 2; k++) {
          char *dir = ccb_path_join(roots[r], subs[k]);
          int hit = lsp_path_under(path, dir) &&
            lsp_has_suffix(path, ".xml");
          free(dir);
          if (hit) {
            return PV_LSP_XML;
          }
        }
      }
    }
  }
  return PV_LSP_IGNORE;
}

// ---------- open documents ----------

static pv_lsp_doc *lsp_doc_find(pv_lsp_server *s, const char *path) {
  size_t i = 0;
  for (i = 0; i < s->ndocs; i++) {
    if (strcmp(s->docs[i].path, path) == 0) {
      return &s->docs[i];
    }
  }
  return NULL;
}

static void lsp_doc_set(pv_lsp_server *s, const char *path,
    const char *text) {
  pv_lsp_doc *d = lsp_doc_find(s, path);
  if (d != NULL) {
    free(d->text);
    d->text = ccb_xstrdup(text != NULL ? text : "");
    return;
  }
  if (s->ndocs == s->capdocs) {
    size_t ncap = s->capdocs == 0 ? 8 : s->capdocs * 2;
    s->docs = ccb_xrealloc(s->docs, ncap * sizeof(*s->docs));
    s->capdocs = ncap;
  }
  s->docs[s->ndocs].path = ccb_xstrdup(path);
  s->docs[s->ndocs].text = ccb_xstrdup(text != NULL ? text : "");
  s->ndocs++;
}

static void lsp_doc_drop(pv_lsp_server *s, const char *path) {
  size_t i = 0;
  for (i = 0; i < s->ndocs; i++) {
    if (strcmp(s->docs[i].path, path) == 0) {
      free(s->docs[i].path);
      free(s->docs[i].text);
      memmove(&s->docs[i], &s->docs[i + 1],
        (s->ndocs - i - 1) * sizeof(*s->docs));
      s->ndocs--;
      return;
    }
  }
}

// ---------- staging cache entries ----------

static void lsp_flow_entry_free(pv_lsp_flow_entry *e) {
  if (e == NULL) {
    return;
  }
  free(e->path);
  if (e->arena != NULL) {
    ccb_arena_destroy(e->arena);
  }
  ccb_diag_list_free(&e->diags);
  memset(e, 0, sizeof(*e));
}

static void lsp_css_entry_free(pv_lsp_css_entry *e) {
  if (e == NULL) {
    return;
  }
  free(e->path);
  if (e->arena != NULL) {
    ccb_arena_destroy(e->arena);
  }
  ccb_diag_list_free(&e->diags);
  memset(e, 0, sizeof(*e));
}

// Sorted-insert position for path in the flow cache (or the existing
// entry's index when found). Returns 1 when found, 0 when not.
static int lsp_flow_lookup(pv_lsp_server *s, const char *path,
    size_t *idx_io) {
  size_t lo = 0;
  size_t hi = s->nflow;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(path, s->flow[mid].path);
    if (cmp == 0) {
      *idx_io = mid;
      return 1;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  *idx_io = lo;
  return 0;
}

static int lsp_css_lookup(pv_lsp_server *s, const char *path,
    size_t *idx_io) {
  size_t lo = 0;
  size_t hi = s->ncss;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(path, s->css[mid].path);
    if (cmp == 0) {
      *idx_io = mid;
      return 1;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  *idx_io = lo;
  return 0;
}

static pv_lsp_flow_entry *lsp_flow_ensure(pv_lsp_server *s,
    const char *path) {
  size_t idx = 0;
  if (lsp_flow_lookup(s, path, &idx)) {
    return &s->flow[idx];
  }
  if (s->nflow == s->capflow) {
    size_t ncap = s->capflow == 0 ? 16 : s->capflow * 2;
    s->flow = ccb_xrealloc(s->flow, ncap * sizeof(*s->flow));
    s->capflow = ncap;
  }
  memmove(&s->flow[idx + 1], &s->flow[idx],
    (s->nflow - idx) * sizeof(*s->flow));
  memset(&s->flow[idx], 0, sizeof(*s->flow));
  s->flow[idx].path = ccb_xstrdup(path);
  s->flow[idx].dirty = 1;
  ccb_diag_list_init(&s->flow[idx].diags);
  s->nflow++;
  return &s->flow[idx];
}

static pv_lsp_css_entry *lsp_css_ensure(pv_lsp_server *s,
    const char *path) {
  size_t idx = 0;
  if (lsp_css_lookup(s, path, &idx)) {
    return &s->css[idx];
  }
  if (s->ncss == s->capcss) {
    size_t ncap = s->capcss == 0 ? 16 : s->capcss * 2;
    s->css = ccb_xrealloc(s->css, ncap * sizeof(*s->css));
    s->capcss = ncap;
  }
  memmove(&s->css[idx + 1], &s->css[idx],
    (s->ncss - idx) * sizeof(*s->css));
  memset(&s->css[idx], 0, sizeof(*s->css));
  s->css[idx].path = ccb_xstrdup(path);
  s->css[idx].dirty = 1;
  ccb_diag_list_init(&s->css[idx].diags);
  s->ncss++;
  return &s->css[idx];
}

// Drop a flow entry (freeing its staging arena + diagnostics).
static void lsp_flow_drop(pv_lsp_server *s, size_t idx) {
  lsp_flow_entry_free(&s->flow[idx]);
  memmove(&s->flow[idx], &s->flow[idx + 1],
    (s->nflow - idx - 1) * sizeof(*s->flow));
  s->nflow--;
}

static void lsp_css_drop(pv_lsp_server *s, size_t idx) {
  lsp_css_entry_free(&s->css[idx]);
  memmove(&s->css[idx], &s->css[idx + 1],
    (s->ncss - idx - 1) * sizeof(*s->css));
  s->ncss--;
}

// ---------- deep copy (staging -> build) ----------

// Copy one box tree into the build arena. Strings are shared with
// the staging arena (never mutated; staging outlives the build).
// Layout fields and overlay flags start at zero — fresh-parse
// state — because layout and the overlays re-run on every build.
static pv_box *lsp_copy_box(ccb_arena *arena, const pv_box *src) {
  pv_box *dst = NULL;
  size_t i = 0;
  if (src == NULL) {
    return NULL;
  }
  dst = ccb_arena_alloc(arena, sizeof(*dst));
  memset(dst, 0, sizeof(*dst));
  dst->type = src->type;
  dst->label = src->label;
  dst->is_branch = src->is_branch;
  if (src->nchildren > 0) {
    dst->children = ccb_arena_alloc(arena,
      src->nchildren * sizeof(*dst->children));
    dst->capchildren = src->nchildren;
    for (i = 0; i < src->nchildren; i++) {
      dst->children[i] = lsp_copy_box(arena, src->children[i]);
    }
    dst->nchildren = src->nchildren;
  }
  return dst;
}

static pv_screen *lsp_copy_screen(ccb_arena *arena,
    const pv_screen *src) {
  pv_screen *dst = NULL;
  size_t i = 0;
  size_t k = 0;
  if (src == NULL) {
    return NULL;
  }
  dst = ccb_arena_alloc(arena, sizeof(*dst));
  memset(dst, 0, sizeof(*dst));
  dst->name = src->name;
  dst->root = lsp_copy_box(arena, src->root);
  // reachable is recomputed by pv_project_diagnose; starts 0.
  if (src->nedges > 0) {
    dst->edges = ccb_arena_alloc(arena,
      src->nedges * sizeof(*dst->edges));
    dst->capedges = src->nedges;
    for (i = 0; i < src->nedges; i++) {
      const pv_edge *se = &src->edges[i];
      pv_edge *de = &dst->edges[i];
      memset(de, 0, sizeof(*de));
      de->from = se->from;
      de->target = se->target;
      de->unknown = se->unknown;
      de->branch = se->branch;
      de->derived = se->derived;
      if (se->nargs > 0) {
        de->args = ccb_arena_alloc(arena,
          se->nargs * sizeof(*de->args));
        de->args_named = ccb_arena_alloc(arena,
          se->nargs * sizeof(int));
        for (k = 0; k < se->nargs; k++) {
          de->args[k] = se->args[k];
          de->args_named[k] = se->args_named != NULL ?
            se->args_named[k] : 0;
        }
        de->nargs = se->nargs;
      }
      // args_scenario/args_trace NULL, trace_taken 0: fresh state.
    }
    dst->nedges = src->nedges;
  }
  return dst;
}

// ---------- diag replay (staging -> build) ----------

// Copy every diagnostic from src into dl (push moves ownership, so
// each item is copied field by field; src stays usable). Mirrors
// pv_diag_replay in ccb_preview.c, which assembly uses for CSS
// entries; this one serves the server's flow entries.
static void lsp_diag_replay(ccb_diag_list *dl,
    const ccb_diag_list *src) {
  size_t i = 0;
  if (dl == NULL || src == NULL) {
    return;
  }
  for (i = 0; i < src->count; i++) {
    ccb_diag d;
    const ccb_diag *s = &src->items[i];
    ccb_diag_init(&d);
    ccb_diag_set_full(&d, s->code, s->phase, s->message, s->file,
      s->line, s->col, s->command, s->workdir, s->suggestion);
    ccb_diag_list_push(dl, &d);
  }
}

// ---------- per-file parsing (the incremental step) ----------

static void lsp_parse_flow(pv_lsp_server *s, pv_lsp_flow_entry *e) {
  pv_lsp_doc *doc = lsp_doc_find(s, e->path);
  char *text = NULL;
  size_t len = 0;
  int from_disk = 0;
  if (doc != NULL) {
    text = doc->text;
  } else {
    if (ccb_read_file(e->path, &text, &len) != CCB_OK) {
      // Race: listed but unreadable. Keep the old parse; clear
      // dirty so the next list resync (which drops the entry when
      // the file is really gone) heals this.
      e->dirty = 0;
      return;
    }
    from_disk = 1;
  }
  if (e->arena != NULL) {
    ccb_arena_destroy(e->arena);
  }
  e->arena = ccb_arena_create();
  e->screens = NULL;
  e->nscreens = 0;
  e->pending = NULL;
  e->npending = 0;
  ccb_diag_list_free(&e->diags);
  ccb_diag_list_init(&e->diags);
  pv_flow_parse_full(e->arena, text, e->path, &e->diags,
    &e->screens, &e->nscreens, s->cfg->preview_binding_keys,
    s->cfg->preview_binding_values, s->cfg->preview_binding_count,
    s->onclick_methods, s->onclick_labels, s->nonclick,
    s->graph_keys, s->graph_vals, s->ngraph,
    &e->pending, &e->npending);
  if (from_disk) {
    free(text);
  }
  e->parse_count++;
  e->dirty = 0;
}

static void lsp_parse_css(pv_lsp_server *s, pv_lsp_css_entry *e) {
  pv_lsp_doc *doc = lsp_doc_find(s, e->path);
  char *text = NULL;
  size_t len = 0;
  int from_disk = 0;
  if (doc != NULL) {
    text = doc->text;
  } else {
    if (ccb_read_file(e->path, &text, &len) != CCB_OK) {
      e->dirty = 0;
      return;
    }
    from_disk = 1;
  }
  if (e->arena != NULL) {
    ccb_arena_destroy(e->arena);
  }
  e->arena = ccb_arena_create();
  e->rules = NULL;
  e->nrules = 0;
  ccb_diag_list_free(&e->diags);
  ccb_diag_list_init(&e->diags);
  pv_css_parse(e->arena, text, e->path,
    s->cfg != NULL && s->cfg->preview_strict, &e->diags,
    &e->rules, &e->nrules);
  if (from_disk) {
    free(text);
  }
  e->parse_count++;
  e->dirty = 0;
}

// ---------- list resync ----------

// Sorted string array helpers for the resync candidate sets.
static void lsp_str_array_add(char ***arr_io, size_t *n_io,
    size_t *cap_io, const char *path) {
  size_t i = 0;
  if (arr_io == NULL || n_io == NULL || cap_io == NULL ||
      path == NULL) {
    return;
  }
  for (i = 0; i < *n_io; i++) {
    if (strcmp((*arr_io)[i], path) == 0) {
      return;
    }
  }
  if (*n_io == *cap_io) {
    size_t ncap = *cap_io == 0 ? 16 : *cap_io * 2;
    *arr_io = ccb_xrealloc(*arr_io, ncap * sizeof(**arr_io));
    *cap_io = ncap;
  }
  (*arr_io)[*n_io] = ccb_xstrdup(path);
  (*n_io)++;
}

static int lsp_str_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static int lsp_str_sorted_has(char **arr, size_t n, const char *path) {
  size_t lo = 0;
  size_t hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(path, arr[mid]);
    if (cmp == 0) {
      return 1;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return 0;
}

// Resync one cache with its candidate set: drop entries whose file
// is gone (and no open doc covers it), add new files as dirty
// entries. The candidate set is the disk file list unioned with open
// docs of that kind, so an unsaved new file still parses.
static void lsp_resync_flow(pv_lsp_server *s) {
  char **cand = NULL;
  size_t ncand = 0;
  size_t capcand = 0;
  size_t i = 0;
  if (s->cfg != NULL && s->cfg->preview_flow != NULL) {
    char **files = NULL;
    size_t nfiles = 0;
    if (ccb_collect_files(s->cfg->preview_flow, ".kt", &files,
        &nfiles) == CCB_OK) {
      for (i = 0; i < nfiles; i++) {
        lsp_str_array_add(&cand, &ncand, &capcand, files[i]);
      }
      ccb_free_str_array(files, nfiles);
    }
  }
  for (i = 0; i < s->ndocs; i++) {
    if (lsp_classify(s, s->docs[i].path) == PV_LSP_FLOW) {
      lsp_str_array_add(&cand, &ncand, &capcand, s->docs[i].path);
    }
  }
  if (ncand > 1) {
    qsort(cand, ncand, sizeof(*cand), lsp_str_cmp);
  }
  // Drop (backwards, so memmove stays valid).
  for (i = s->nflow; i > 0; i--) {
    if (!lsp_str_sorted_has(cand, ncand, s->flow[i - 1].path)) {
      lsp_flow_drop(s, i - 1);
    }
  }
  for (i = 0; i < ncand; i++) {
    (void)lsp_flow_ensure(s, cand[i]);
  }
  ccb_free_str_array(cand, ncand);
}

static void lsp_resync_css(pv_lsp_server *s) {
  char **cand = NULL;
  size_t ncand = 0;
  size_t capcand = 0;
  size_t i = 0;
  if (s->cfg != NULL && s->cfg->preview_styles != NULL) {
    char **files = NULL;
    size_t nfiles = 0;
    if (ccb_collect_files(s->cfg->preview_styles, ".css", &files,
        &nfiles) == CCB_OK) {
      for (i = 0; i < nfiles; i++) {
        lsp_str_array_add(&cand, &ncand, &capcand, files[i]);
      }
      ccb_free_str_array(files, nfiles);
    }
  }
  for (i = 0; i < s->ndocs; i++) {
    if (lsp_classify(s, s->docs[i].path) == PV_LSP_CSS) {
      lsp_str_array_add(&cand, &ncand, &capcand, s->docs[i].path);
    }
  }
  if (ncand > 1) {
    qsort(cand, ncand, sizeof(*cand), lsp_str_cmp);
  }
  for (i = s->ncss; i > 0; i--) {
    if (!lsp_str_sorted_has(cand, ncand, s->css[i - 1].path)) {
      lsp_css_drop(s, i - 1);
    }
  }
  for (i = 0; i < ncand; i++) {
    (void)lsp_css_ensure(s, cand[i]);
  }
  ccb_free_str_array(cand, ncand);
}

// ---------- config reload ----------

static int lsp_str_eq(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

static int lsp_bindings_equal(const ccb_config *a, const ccb_config *b) {
  size_t i = 0;
  if (a == NULL || b == NULL) {
    return a == b;
  }
  if (a->preview_binding_count != b->preview_binding_count) {
    return 0;
  }
  for (i = 0; i < a->preview_binding_count; i++) {
    if (!lsp_str_eq(a->preview_binding_keys[i],
        b->preview_binding_keys[i])) {
      return 0;
    }
    if (!lsp_str_eq(a->preview_binding_values[i],
        b->preview_binding_values[i])) {
      return 0;
    }
  }
  return 1;
}

// Reload the config after a ccb.toml change. Returns 0 on success
// (s->cfg replaced; sets the flow-invalidate and watch-rebuild
// consequences), -1 when the new config is unloadable or has no
// usable [preview] section — the old config is kept and the caller
// keeps the last good project, like watch mode keeps the last good
// render on a broken save.
static int lsp_reload_config(pv_lsp_server *s) {
  ccb_config *ncfg = NULL;
  ccb_diag_list dl;
  int bindings_changed = 0;
  int dirs_changed = 0;
  int xml_paths_changed = 0;
  ccb_diag_list_init(&dl);
  if (ccb_config_load(s->toml_path, NULL, &ncfg, &dl) != CCB_OK ||
      ncfg == NULL || !ncfg->has_preview ||
      (ncfg->preview_styles == NULL && ncfg->preview_flow == NULL)) {
    ccb_diag_list_free(&dl);
    if (ncfg != NULL) {
      ccb_config_free(ncfg);
    }
    return -1;
  }
  ccb_diag_list_free(&dl);
  bindings_changed = !lsp_bindings_equal(s->cfg, ncfg);
  dirs_changed = !lsp_str_eq(s->cfg->preview_styles,
      ncfg->preview_styles) ||
    !lsp_str_eq(s->cfg->preview_flow, ncfg->preview_flow);
  xml_paths_changed = !lsp_str_eq(s->cfg->preview_nav_graph,
      ncfg->preview_nav_graph) ||
    !lsp_str_eq(s->cfg->preview_layout, ncfg->preview_layout) ||
    !lsp_str_eq(s->cfg->preview_resources, ncfg->preview_resources);
  ccb_config_free(s->cfg);
  s->cfg = ncfg;
  if (bindings_changed || xml_paths_changed) {
    // Bindings and the graph/layout mapping are flow-parse inputs:
    // every cached flow parse is stale.
    size_t i = 0;
    for (i = 0; i < s->nflow; i++) {
      s->flow[i].dirty = 1;
    }
  }
  if (dirs_changed) {
    // The watch context and snapshot are tied to the old dirs.
    int used = 0;
    pv_watch_end(s->watch_ctx);
    s->watch_ctx = NULL;
    pv_watch_snapshot_free(s->watch_snap);
    s->watch_snap = NULL;
    pv_watch_begin(s->config_dir, s->cfg->preview_styles,
      s->cfg->preview_flow, s->toml_path, &s->watch_ctx, &used);
    CCB_UNUSED(used);
    pv_watch_snapshot_take(s->config_dir, s->cfg->preview_styles,
      s->cfg->preview_flow, s->toml_path, &s->watch_snap);
  }
  return 0;
}

// ---------- build ----------

// Build the served project from the staging caches. The old project
// is freed by the caller before this runs, so staging arenas may be
// destroyed by re-parses without dangling the previous build.
static pv_project *lsp_build(pv_lsp_server *s, ccb_diag_list *dl) {
  ccb_arena *arena = NULL;
  pv_project *p = NULL;
  pv_screen **screens = NULL;
  size_t nscreens = 0;
  size_t i = 0;
  size_t j = 0;
  pv_pending_edge **pending = NULL;
  size_t npending = 0;
  pv_css_entry *entries = NULL;
  const ccb_scenario *scenario = NULL;
  // XML inputs rescan fresh on every build: small files, and the
  // scan's P1019 warnings regenerate in load order each build.
  if (s->xml_arena != NULL) {
    ccb_arena_destroy(s->xml_arena);
  }
  s->xml_arena = ccb_arena_create();
  s->graph_keys = NULL;
  s->graph_vals = NULL;
  s->ngraph = 0;
  s->onclick_methods = NULL;
  s->onclick_labels = NULL;
  s->nonclick = 0;
  pv_preview_scan_xml(s->cfg, s->xml_arena, &s->graph_keys,
    &s->graph_vals, &s->ngraph, &s->onclick_methods,
    &s->onclick_labels, &s->nonclick, dl);
  // List resync, then parse only dirty entries.
  lsp_resync_flow(s);
  lsp_resync_css(s);
  for (i = 0; i < s->nflow; i++) {
    if (s->flow[i].dirty) {
      lsp_parse_flow(s, &s->flow[i]);
    }
  }
  for (i = 0; i < s->ncss; i++) {
    if (s->css[i].dirty) {
      lsp_parse_css(s, &s->css[i]);
    }
  }
  // Flow parse diagnostics replay in file order (cache order is
  // sorted path order, matching load's sorted enumeration).
  for (i = 0; i < s->nflow; i++) {
    lsp_diag_replay(dl, &s->flow[i].diags);
  }
  // Deep-copy staging screens into the build arena.
  for (i = 0; i < s->nflow; i++) {
    nscreens += s->flow[i].nscreens;
    npending += s->flow[i].npending;
  }
  arena = ccb_arena_create();
  screens = ccb_arena_alloc(arena,
    (nscreens == 0 ? 1 : nscreens) * sizeof(*screens));
  nscreens = 0;
  for (i = 0; i < s->nflow; i++) {
    for (j = 0; j < s->flow[i].nscreens; j++) {
      screens[nscreens++] = lsp_copy_screen(arena,
        s->flow[i].screens[j]);
    }
  }
  pending = ccb_arena_alloc(arena,
    (npending == 0 ? 1 : npending) * sizeof(*pending));
  npending = 0;
  for (i = 0; i < s->nflow; i++) {
    for (j = 0; j < s->flow[i].npending; j++) {
      pending[npending++] = s->flow[i].pending[j];
    }
  }
  // CSS entries borrow staging rules + diagnostics.
  entries = ccb_xcalloc(s->ncss == 0 ? 1 : s->ncss,
    sizeof(*entries));
  for (i = 0; i < s->ncss; i++) {
    entries[i].path = s->css[i].path;
    entries[i].rules = s->css[i].rules;
    entries[i].nrules = s->css[i].nrules;
    entries[i].diags = &s->css[i].diags;
  }
  if (s->scenario_name != NULL) {
    // A scenario that vanished in a config reload falls back to no
    // scenario (startup still rejects an unknown name, like the
    // CLI); the tree JSON just lacks the scenario field.
    scenario = ccb_config_scenario(s->cfg, s->scenario_name);
  }
  p = ccb_arena_alloc(arena, sizeof(*p));
  memset(p, 0, sizeof(*p));
  p->arena = arena;
  p->ascii = 0;
  p->strict = s->cfg->preview_strict;
  p->phone_dp_width = s->cfg->preview_phone_width > 0 ?
    s->cfg->preview_phone_width : PV_LSP_PHONE_DP_WIDTH;
  p->frame_width = PV_LSP_FRAME_WIDTH;
  p->interior_cols = PV_LSP_FRAME_WIDTH - 2;
  {
    // Chunk A: same graph-node + manifest inputs as the load path.
    // Manifest activities live in the build arena (not the XML
    // arena, which the next rebuild destroys). Manifest changes do
    // not yet trigger a rebuild on their own; the next flow, CSS,
    // config, or XML change picks them up.
    char **manifest_acts = NULL;
    size_t nmanifest_acts = 0;
    pv_manifest_activities(arena, s->cfg->manifest, s->cfg->package,
      &manifest_acts, &nmanifest_acts);
    pv_project_assemble(p, screens, nscreens, pending, npending,
      entries, s->ncss, s->cfg->preview_resources, scenario, dl,
      s->graph_vals, s->ngraph, manifest_acts, nmanifest_acts,
      s->cfg->package);
  }
  free(entries);
  // Trace overlay after assembly, before diagnose: the CLI's order.
  if (s->trace != NULL) {
    pv_trace_apply(p, s->trace, &p->diags);
  }
  pv_project_diagnose(p);
  return p;
}

int pv_lsp_rebuild(pv_lsp_server *s) {
  int any_dirty = 0;
  size_t i = 0;
  pv_project *p = NULL;
  ccb_diag_list dl;
  ccb_str out;
  char *new_json = NULL;
  int changed = 0;
  if (s == NULL) {
    return -1;
  }
  if (!s->first_build) {
    any_dirty = s->editor_dirty || s->config_dirty || s->xml_dirty ||
      s->resources_dirty;
    if (!any_dirty) {
      for (i = 0; i < s->nflow && !any_dirty; i++) {
        any_dirty = s->flow[i].dirty;
      }
    }
    if (!any_dirty) {
      for (i = 0; i < s->ncss && !any_dirty; i++) {
        any_dirty = s->css[i].dirty;
      }
    }
    if (!any_dirty) {
      return 0;
    }
  }
  // Config reload first: it may invalidate the flow cache and
  // rebuild the watch context. On failure the last good project is
  // kept and every dirty flag stays set for a retry.
  if (s->config_dirty) {
    if (lsp_reload_config(s) != 0) {
      return -1;
    }
    s->config_dirty = 0;
  }
  if (s->xml_dirty) {
    for (i = 0; i < s->nflow; i++) {
      s->flow[i].dirty = 1;
    }
    s->xml_dirty = 0;
  }
  // Free the old project BEFORE re-parsing: its copied strings
  // point into staging arenas that re-parses may destroy.
  pv_project_free(s->project);
  s->project = NULL;
  ccb_diag_list_init(&dl);
  p = lsp_build(s, &dl);
  ccb_str_init(&out);
  pv_project_json(&out, p);
  new_json = out.buf != NULL ? out.buf : ccb_xstrdup("");
  // The ccb_str buffer is adopted as the tree JSON (do not free out).
  changed = s->tree_json == NULL || strcmp(s->tree_json, new_json) != 0;
  free(s->tree_json);
  s->tree_json = new_json;
  s->project = p;
  s->editor_dirty = 0;
  s->resources_dirty = 0;
  pv_watch_debounce_reset(&s->deb);
  s->first_build = 0;
  return changed ? 1 : 0;
}

// ---------- accessors ----------

const char *pv_lsp_tree_json(pv_lsp_server *s) {
  return s != NULL ? s->tree_json : NULL;
}

pv_project *pv_lsp_project(pv_lsp_server *s) {
  return s != NULL ? s->project : NULL;
}

long pv_lsp_parse_count(pv_lsp_server *s, const char *path) {
  size_t idx = 0;
  if (s == NULL || path == NULL) {
    return -1;
  }
  if (lsp_flow_lookup(s, path, &idx)) {
    return s->flow[idx].parse_count;
  }
  if (lsp_css_lookup(s, path, &idx)) {
    return s->css[idx].parse_count;
  }
  return -1;
}

// ---------- editor updates ----------

static void lsp_mark_dirty(pv_lsp_server *s, const char *path,
    pv_lsp_kind kind) {
  size_t idx = 0;
  switch (kind) {
    case PV_LSP_FLOW:
      if (lsp_flow_lookup(s, path, &idx)) {
        s->flow[idx].dirty = 1;
      }
      s->editor_dirty = 1;
      break;
    case PV_LSP_CSS:
      if (lsp_css_lookup(s, path, &idx)) {
        s->css[idx].dirty = 1;
      }
      s->editor_dirty = 1;
      break;
    case PV_LSP_TOML:
      s->config_dirty = 1;
      s->editor_dirty = 1;
      break;
    case PV_LSP_XML:
      s->xml_dirty = 1;
      s->editor_dirty = 1;
      break;
    case PV_LSP_RESOURCES:
      s->resources_dirty = 1;
      s->editor_dirty = 1;
      break;
    default:
      break;
  }
}

void pv_lsp_did_open(pv_lsp_server *s, const char *path,
    const char *text) {
  pv_lsp_kind kind = PV_LSP_IGNORE;
  if (s == NULL || path == NULL) {
    return;
  }
  kind = lsp_classify(s, path);
  if (kind == PV_LSP_IGNORE) {
    return;
  }
  lsp_doc_set(s, path, text);
  lsp_mark_dirty(s, path, kind);
}

void pv_lsp_did_change(pv_lsp_server *s, const char *path,
    const char *text) {
  pv_lsp_did_open(s, path, text);
}

void pv_lsp_did_close(pv_lsp_server *s, const char *path) {
  pv_lsp_kind kind = PV_LSP_IGNORE;
  if (s == NULL || path == NULL) {
    return;
  }
  kind = lsp_classify(s, path);
  if (kind == PV_LSP_IGNORE) {
    return;
  }
  lsp_doc_drop(s, path);
  lsp_mark_dirty(s, path, kind);
}

// ---------- disk scan ----------

// Enumerate one nav/layout input path (a file, or a directory's
// *.xml files) into the path list. Mirrors pv_xml_inputs in
// ccb_preview.c, which load uses for the same inputs.
static void lsp_extra_add_inputs(char ***paths_io, size_t *n_io,
    size_t *cap_io, const char *base) {
  if (base == NULL || !ccb_path_exists(base)) {
    return;
  }
  if (ccb_path_is_dir(base)) {
    char **files = NULL;
    size_t nfiles = 0;
    size_t i = 0;
    if (ccb_collect_files(base, ".xml", &files, &nfiles) != CCB_OK) {
      return;
    }
    for (i = 0; i < nfiles; i++) {
      lsp_str_array_add(paths_io, n_io, cap_io, files[i]);
    }
    // Strings moved into the list; free the array only.
    free(files);
  } else {
    lsp_str_array_add(paths_io, n_io, cap_io, base);
  }
}

// Diff the extra-file stats against the stored set. Returns 1 when
// any nav/layout/resources file was added, removed, or modified
// (and marks the corresponding dirty flags).
static int lsp_scan_extra(pv_lsp_server *s) {
  char **paths = NULL;
  size_t npaths = 0;
  size_t cappaths = 0;
  pv_lsp_stat *fresh = NULL;
  size_t nfresh = 0;
  size_t i = 0;
  size_t j = 0;
  int dirty = 0;
  char *nav = NULL;
  char *layout = NULL;
  if (s->cfg != NULL) {
    nav = pv_preview_nav_path(s->cfg);
    layout = pv_preview_layout_path(s->cfg);
    lsp_extra_add_inputs(&paths, &npaths, &cappaths, nav);
    lsp_extra_add_inputs(&paths, &npaths, &cappaths, layout);
    free(nav);
    free(layout);
    if (s->cfg->preview_resources != NULL) {
      char *sp = ccb_path_join(s->cfg->preview_resources,
        "strings.xml");
      char *cp = ccb_path_join(s->cfg->preview_resources,
        "colors.xml");
      lsp_str_array_add(&paths, &npaths, &cappaths, sp);
      lsp_str_array_add(&paths, &npaths, &cappaths, cp);
      free(sp);
      free(cp);
    }
  }
  if (npaths > 1) {
    qsort(paths, npaths, sizeof(*paths), lsp_str_cmp);
  }
  // Stat the existing files only: a missing file drops out of the
  // fresh set, so its removal diffs below.
  fresh = ccb_xcalloc(npaths == 0 ? 1 : npaths, sizeof(*fresh));
  for (i = 0; i < npaths; i++) {
    struct stat st;
    if (stat(paths[i], &st) == 0 && S_ISREG(st.st_mode)) {
      fresh[nfresh].path = paths[i];
      fresh[nfresh].mtime_sec = (long long)st.st_mtim.tv_sec;
      fresh[nfresh].mtime_nsec = (long)st.st_mtim.tv_nsec;
      fresh[nfresh].size = (long long)st.st_size;
      nfresh++;
    } else {
      free(paths[i]);
    }
  }
  free(paths);
  // Merge-walk stored vs fresh (both sorted by path).
  i = 0;
  j = 0;
  while (i < s->nextra && j < nfresh) {
    int cmp = strcmp(s->extra[i].path, fresh[j].path);
    if (cmp == 0) {
      if (s->extra[i].mtime_sec != fresh[j].mtime_sec ||
          s->extra[i].mtime_nsec != fresh[j].mtime_nsec ||
          s->extra[i].size != fresh[j].size) {
        pv_lsp_kind kind = lsp_classify(s, fresh[j].path);
        if (kind == PV_LSP_XML) {
          s->xml_dirty = 1;
        } else if (kind == PV_LSP_RESOURCES) {
          s->resources_dirty = 1;
        }
        dirty = 1;
      }
      i++;
      j++;
    } else if (cmp < 0) {
      // Removed.
      pv_lsp_kind kind = lsp_classify(s, s->extra[i].path);
      if (kind == PV_LSP_XML) {
        s->xml_dirty = 1;
      } else if (kind == PV_LSP_RESOURCES) {
        s->resources_dirty = 1;
      }
      dirty = 1;
      i++;
    } else {
      // Added.
      pv_lsp_kind kind = lsp_classify(s, fresh[j].path);
      if (kind == PV_LSP_XML) {
        s->xml_dirty = 1;
      } else if (kind == PV_LSP_RESOURCES) {
        s->resources_dirty = 1;
      }
      dirty = 1;
      j++;
    }
  }
  for (; i < s->nextra; i++) {
    pv_lsp_kind kind = lsp_classify(s, s->extra[i].path);
    if (kind == PV_LSP_XML) {
      s->xml_dirty = 1;
    } else if (kind == PV_LSP_RESOURCES) {
      s->resources_dirty = 1;
    }
    dirty = 1;
  }
  for (; j < nfresh; j++) {
    pv_lsp_kind kind = lsp_classify(s, fresh[j].path);
    if (kind == PV_LSP_XML) {
      s->xml_dirty = 1;
    } else if (kind == PV_LSP_RESOURCES) {
      s->resources_dirty = 1;
    }
    dirty = 1;
  }
  for (i = 0; i < s->nextra; i++) {
    free(s->extra[i].path);
  }
  free(s->extra);
  s->extra = fresh;
  s->nextra = nfresh;
  s->capextra = npaths == 0 ? 1 : npaths;
  return dirty;
}

int pv_lsp_scan_disk(pv_lsp_server *s) {
  pv_watch_snapshot *fresh = NULL;
  char **changed = NULL;
  size_t nchanged = 0;
  size_t i = 0;
  int dirty = 0;
  if (s == NULL) {
    return 0;
  }
  // The main set: snapshot + path-level diff. (The watch context's
  // wait is only the run loop's wakeup; this diff is authoritative
  // for which files changed.)
  if (pv_watch_snapshot_take(s->config_dir, s->cfg->preview_styles,
      s->cfg->preview_flow, s->toml_path, &fresh) == CCB_OK) {
    pv_watch_snapshot_diff(s->watch_snap, fresh, &changed, &nchanged);
    for (i = 0; i < nchanged; i++) {
      pv_lsp_kind kind = lsp_classify(s, changed[i]);
      if (kind == PV_LSP_IGNORE) {
        continue;
      }
      // An open document wins over disk content: the disk change
      // is invisible until the document closes.
      if (lsp_doc_find(s, changed[i]) != NULL) {
        continue;
      }
      switch (kind) {
        case PV_LSP_FLOW: {
          size_t idx = 0;
          if (lsp_flow_lookup(s, changed[i], &idx)) {
            s->flow[idx].dirty = 1;
          }
          break;
        }
        case PV_LSP_CSS: {
          size_t idx = 0;
          if (lsp_css_lookup(s, changed[i], &idx)) {
            s->css[idx].dirty = 1;
          }
          break;
        }
        case PV_LSP_TOML:
          s->config_dirty = 1;
          break;
        case PV_LSP_XML:
          s->xml_dirty = 1;
          break;
        case PV_LSP_RESOURCES:
          s->resources_dirty = 1;
          break;
        default:
          break;
      }
      dirty = 1;
    }
    pv_watch_snapshot_free(s->watch_snap);
    s->watch_snap = fresh;
    ccb_free_str_array(changed, nchanged);
  }
  if (lsp_scan_extra(s)) {
    dirty = 1;
  }
  if (dirty) {
    pv_watch_debounce_event(&s->deb, pv_watch_now_ms());
  }
  return dirty;
}

// ---------- open / close ----------

pv_lsp_server *pv_lsp_open(const char *toml_path,
    const char *scenario_name, const char *trace_path, char **err) {
  pv_lsp_server *s = NULL;
  ccb_diag_list dl;
  int used = 0;
  if (err != NULL) {
    *err = NULL;
  }
  if (toml_path == NULL) {
    if (err != NULL) {
      *err = ccb_xstrdup("no ccb.toml path given");
    }
    return NULL;
  }
  s = ccb_xcalloc(1, sizeof(*s));
  s->toml_path = ccb_xstrdup(toml_path);
  s->config_dir = ccb_dirname(toml_path);
  s->first_build = 1;
  ccb_diag_list_init(&dl);
  if (ccb_config_load(toml_path, NULL, &s->cfg, &dl) != CCB_OK ||
      s->cfg == NULL) {
    ccb_diag_list_free(&dl);
    if (err != NULL) {
      *err = ccb_xstrdup("cannot load ccb.toml");
    }
    free(s->toml_path);
    free(s->config_dir);
    free(s);
    return NULL;
  }
  ccb_diag_list_free(&dl);
  if (!s->cfg->has_preview || (s->cfg->preview_styles == NULL &&
      s->cfg->preview_flow == NULL)) {
    if (err != NULL) {
      *err = ccb_xstrdup("preview not applicable to this project");
    }
    ccb_config_free(s->cfg);
    free(s->toml_path);
    free(s->config_dir);
    free(s);
    return NULL;
  }
  if (scenario_name != NULL) {
    if (ccb_config_scenario(s->cfg, scenario_name) == NULL) {
      ccb_str msg;
      if (err != NULL) {
        ccb_str_init(&msg);
        ccb_str_append(&msg, "unknown scenario '");
        ccb_str_append(&msg, scenario_name);
        ccb_str_append(&msg, "'");
        *err = msg.buf != NULL ? msg.buf : ccb_xstrdup("unknown scenario");
      }
      ccb_config_free(s->cfg);
      free(s->toml_path);
      free(s->config_dir);
      free(s);
      return NULL;
    }
    s->scenario_name = ccb_xstrdup(scenario_name);
  }
  if (trace_path != NULL) {
    char *ttext = NULL;
    size_t tlen = 0;
    char *terr = NULL;
    if (ccb_read_file(trace_path, &ttext, &tlen) != CCB_OK) {
      if (err != NULL) {
        *err = ccb_xstrdup("cannot read trace file");
      }
      ccb_config_free(s->cfg);
      free(s->toml_path);
      free(s->config_dir);
      free(s->scenario_name);
      free(s);
      return NULL;
    }
    s->trace = pv_trace_parse(ttext, &terr);
    free(ttext);
    if (s->trace == NULL) {
      if (err != NULL) {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg, "invalid trace file: ");
        ccb_str_append(&msg, terr != NULL ? terr : "parse error");
        *err = msg.buf != NULL ? msg.buf :
          ccb_xstrdup("invalid trace file");
      }
      free(terr);
      ccb_config_free(s->cfg);
      free(s->toml_path);
      free(s->config_dir);
      free(s->scenario_name);
      free(s);
      return NULL;
    }
    free(terr);
  }
  pv_watch_begin(s->config_dir, s->cfg->preview_styles,
    s->cfg->preview_flow, s->toml_path, &s->watch_ctx, &used);
  CCB_UNUSED(used);
  pv_watch_snapshot_take(s->config_dir, s->cfg->preview_styles,
    s->cfg->preview_flow, s->toml_path, &s->watch_snap);
  memset(&s->deb, 0, sizeof(s->deb));
  if (pv_lsp_rebuild(s) < 0) {
    // Unreachable in practice (the first build never reloads the
    // config), but never hand out a server with no tree.
    if (err != NULL) {
      *err = ccb_xstrdup("initial preview build failed");
    }
    pv_lsp_close(s);
    return NULL;
  }
  return s;
}

void pv_lsp_close(pv_lsp_server *s) {
  size_t i = 0;
  if (s == NULL) {
    return;
  }
  pv_project_free(s->project);
  free(s->tree_json);
  free(s->last_published);
  if (s->cfg != NULL) {
    ccb_config_free(s->cfg);
  }
  if (s->trace != NULL) {
    pv_trace_free(s->trace);
  }
  if (s->xml_arena != NULL) {
    ccb_arena_destroy(s->xml_arena);
  }
  for (i = 0; i < s->nflow; i++) {
    lsp_flow_entry_free(&s->flow[i]);
  }
  free(s->flow);
  for (i = 0; i < s->ncss; i++) {
    lsp_css_entry_free(&s->css[i]);
  }
  free(s->css);
  for (i = 0; i < s->ndocs; i++) {
    free(s->docs[i].path);
    free(s->docs[i].text);
  }
  free(s->docs);
  pv_watch_end(s->watch_ctx);
  pv_watch_snapshot_free(s->watch_snap);
  for (i = 0; i < s->nextra; i++) {
    free(s->extra[i].path);
  }
  free(s->extra);
  free(s->toml_path);
  free(s->config_dir);
  free(s->scenario_name);
  free(s);
}

// ---------- JSON-RPC framing ----------

// Buffered stdin reader: partial headers and bodies survive across
// loop iterations (a client may split a message across writes).
typedef struct {
  int fd;
  char *buf;
  size_t len;
  size_t cap;
  size_t pos; // consumed offset
  int eof;
} lsp_reader;

static void lsp_reader_compact(lsp_reader *r) {
  if (r->pos == 0) {
    return;
  }
  if (r->pos < r->len) {
    memmove(r->buf, r->buf + r->pos, r->len - r->pos);
  }
  r->len -= r->pos;
  r->pos = 0;
}

// Read more input (non-blocking fd). Returns 1 when new bytes
// arrived, 0 on EAGAIN/EINTR/EOF/error (eof set on EOF/error).
static int lsp_reader_more(lsp_reader *r) {
  ssize_t n = 0;
  lsp_reader_compact(r);
  if (r->len == r->cap) {
    size_t ncap = r->cap == 0 ? 8192 : r->cap * 2;
    r->buf = ccb_xrealloc(r->buf, ncap);
    r->cap = ncap;
  }
  n = read(r->fd, r->buf + r->len, r->cap - r->len);
  if (n > 0) {
    r->len += (size_t)n;
    return 1;
  }
  if (n == 0) {
    r->eof = 1;
    return 0;
  }
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    return 0;
  }
  r->eof = 1;
  return 0;
}

// Try to extract one complete message body. Returns 1 with a
// malloc'd NUL-terminated body in *body_io, 0 when more input is
// needed, 2 when a header block without Content-Length was skipped
// (caller retries immediately for a following message).
static int lsp_reader_next(lsp_reader *r, char **body_io) {
  size_t i = 0;
  size_t hdr_end = 0;
  size_t body_start = 0;
  long content_length = -1;
  *body_io = NULL;
  // Find the header terminator.
  for (i = r->pos; i + 3 < r->len; i++) {
    if (r->buf[i] == '\r' && r->buf[i + 1] == '\n' &&
        r->buf[i + 2] == '\r' && r->buf[i + 3] == '\n') {
      hdr_end = i;
      body_start = i + 4;
      break;
    }
  }
  if (body_start == 0) {
    for (i = r->pos; i + 1 < r->len; i++) {
      if (r->buf[i] == '\n' && r->buf[i + 1] == '\n') {
        hdr_end = i;
        body_start = i + 2;
        break;
      }
    }
  }
  if (body_start == 0) {
    return 0;
  }
  // Parse header lines for Content-Length (case-insensitive).
  {
    size_t p = r->pos;
    while (p < hdr_end) {
      size_t line_end = p;
      while (line_end < hdr_end && r->buf[line_end] != '\n') {
        line_end++;
      }
      {
        size_t ll = line_end - p;
        if (ll > 0 && r->buf[p + ll - 1] == '\r') {
          ll--;
        }
        if (ll >= 15 && (r->buf[p] == 'C' || r->buf[p] == 'c') &&
            strncasecmp(r->buf + p, "content-length:", 15) == 0) {
          const char *v = r->buf + p + 15;
          char *end = NULL;
          long n = 0;
          while (*v == ' ' || *v == '\t') {
            v++;
          }
          n = strtol(v, &end, 10);
          if (end != v && n >= 0) {
            content_length = n;
          }
        }
      }
      p = line_end + 1;
    }
  }
  if (content_length < 0) {
    // No usable length: skip this block and resync on the next.
    r->pos = body_start;
    return 2;
  }
  if (r->len - body_start < (size_t)content_length) {
    return 0;
  }
  *body_io = ccb_xmalloc((size_t)content_length + 1);
  memcpy(*body_io, r->buf + body_start, (size_t)content_length);
  (*body_io)[content_length] = '\0';
  r->pos = body_start + (size_t)content_length;
  return 1;
}

static int lsp_write_all(int fd, const char *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
  return 0;
}

static void lsp_send_raw(int out_fd, const char *json, size_t len) {
  ccb_str frame;
  ccb_str_init(&frame);
  ccb_str_appendf(&frame, "Content-Length: %zu\r\n\r\n", len);
  if (lsp_write_all(out_fd, frame.buf != NULL ? frame.buf : "",
      strlen(frame.buf != NULL ? frame.buf : "")) != 0) {
    ccb_str_free(&frame);
    return;
  }
  ccb_str_free(&frame);
  (void)lsp_write_all(out_fd, json, len);
}

static void lsp_send_json(int out_fd, const ccb_str *json) {
  lsp_send_raw(out_fd, json->buf != NULL ? json->buf : "",
    strlen(json->buf != NULL ? json->buf : ""));
}

// Serialize a request id verbatim: integral numbers as integers
// (ids are small), other numbers as-is, strings escaped, anything
// else as null.
static void lsp_write_id(ccb_str *out, const ccb_json_value *id) {
  if (id == NULL) {
    ccb_str_append(out, "null");
    return;
  }
  if (id->type == CCB_JSON_NUM) {
    double d = id->num;
    if (d == (double)(long long)d) {
      ccb_str_appendf(out, "%lld", (long long)d);
    } else {
      ccb_str_appendf(out, "%.17g", d);
    }
    return;
  }
  if (id->type == CCB_JSON_STR && id->str != NULL) {
    ccb_json_escape(out, id->str);
    return;
  }
  ccb_str_append(out, "null");
}

static void lsp_send_result(int out_fd, const ccb_json_value *id,
    const char *result_json) {
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
  lsp_write_id(&out, id);
  ccb_str_append(&out, ",\"result\":");
  ccb_str_append(&out, result_json != NULL ? result_json : "null");
  ccb_str_append(&out, "}");
  lsp_send_json(out_fd, &out);
  ccb_str_free(&out);
}

static void lsp_send_error(int out_fd, const ccb_json_value *id,
    long code, const char *message) {
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
  lsp_write_id(&out, id);
  ccb_str_appendf(&out, ",\"error\":{\"code\":%ld,\"message\":", code);
  ccb_json_escape(&out, message != NULL ? message : "error");
  ccb_str_append(&out, "}}");
  lsp_send_json(out_fd, &out);
  ccb_str_free(&out);
}

// Publish the current tree when it differs from the last publish
// (the first publish always sends: last_published starts NULL).
static void lsp_maybe_publish(pv_lsp_server *s, int out_fd) {
  ccb_str out;
  const char *tree = NULL;
  if (s == NULL) {
    return;
  }
  tree = pv_lsp_tree_json(s);
  if (tree == NULL) {
    return;
  }
  if (s->last_published != NULL && strcmp(s->last_published, tree) == 0) {
    return;
  }
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"jsonrpc\":\"2.0\",\"method\":\"ccb/publishTree\",\"params\":");
  ccb_str_append(&out, tree);
  ccb_str_append(&out, "}");
  lsp_send_json(out_fd, &out);
  ccb_str_free(&out);
  free(s->last_published);
  s->last_published = ccb_xstrdup(tree);
}

// ---------- method helpers ----------

// file:// URI to path, with %XX decoding. Returns a malloc'd path or
// NULL for a non-file URI.
static char *lsp_uri_to_path(const char *uri) {
  const char *p = NULL;
  ccb_str out;
  if (uri == NULL || strncmp(uri, "file://", 7) != 0) {
    return NULL;
  }
  p = uri + 7;
  if (strncmp(p, "localhost/", 10) == 0) {
    p += 9; // keep the leading slash of the path
  }
  ccb_str_init(&out);
  while (*p != '\0') {
    if (*p == '%' && p[1] != '\0' && p[2] != '\0' &&
        ((p[1] >= '0' && p[1] <= '9') ||
         (p[1] >= 'a' && p[1] <= 'f') ||
         (p[1] >= 'A' && p[1] <= 'F')) &&
        ((p[2] >= '0' && p[2] <= '9') ||
         (p[2] >= 'a' && p[2] <= 'f') ||
         (p[2] >= 'A' && p[2] <= 'F'))) {
      int hi = (p[1] <= '9') ? p[1] - '0' :
        ((p[1] <= 'F' && p[1] >= 'A') ? p[1] - 'A' + 10 : p[1] - 'a' + 10);
      int lo = (p[2] <= '9') ? p[2] - '0' :
        ((p[2] <= 'F' && p[2] >= 'A') ? p[2] - 'A' + 10 : p[2] - 'a' + 10);
      char c = (char)(hi * 16 + lo);
      ccb_str_append_n(&out, &c, 1);
      p += 3;
    } else {
      ccb_str_append_n(&out, p, 1);
      p++;
    }
  }
  if (out.buf == NULL) {
    return ccb_xstrdup("");
  }
  return out.buf;
}

// Re-root the server on a new workspace directory when it holds a
// usable ccb.toml (validated by a throwaway load first, so a bad
// root never disturbs the current project). Drops staging, docs,
// and extra stats; the next rebuild reloads the new config.
static void lsp_reroot(pv_lsp_server *s, const char *dir) {
  char *toml = NULL;
  ccb_config *probe = NULL;
  ccb_diag_list dl;
  size_t i = 0;
  size_t dlen = 0;
  size_t clen = 0;
  if (s == NULL || dir == NULL) {
    return;
  }
  // Same directory under a different spelling (notably a trailing
  // slash from the client) is not a re-root: comparing raw strings
  // would drop every cache and, worse, suppress the initial publish
  // (the handler skips it when a rebuild is queued, and the rebuild
  // then reports the identical tree as unchanged).
  dlen = strlen(dir);
  while (dlen > 1 && dir[dlen - 1] == '/') {
    dlen--;
  }
  clen = strlen(s->config_dir);
  while (clen > 1 && s->config_dir[clen - 1] == '/') {
    clen--;
  }
  if (dlen == clen && memcmp(dir, s->config_dir, dlen) == 0) {
    return;
  }
  toml = ccb_path_join(dir, "ccb.toml");
  if (!ccb_path_exists(toml)) {
    free(toml);
    return;
  }
  ccb_diag_list_init(&dl);
  if (ccb_config_load(toml, NULL, &probe, &dl) != CCB_OK ||
      probe == NULL || !probe->has_preview ||
      (probe->preview_styles == NULL && probe->preview_flow == NULL)) {
    ccb_diag_list_free(&dl);
    if (probe != NULL) {
      ccb_config_free(probe);
    }
    free(toml);
    return;
  }
  ccb_diag_list_free(&dl);
  ccb_config_free(probe);
  // Commit: drop everything tied to the old root.
  for (i = 0; i < s->nflow; i++) {
    lsp_flow_entry_free(&s->flow[i]);
  }
  free(s->flow);
  s->flow = NULL;
  s->nflow = 0;
  s->capflow = 0;
  for (i = 0; i < s->ncss; i++) {
    lsp_css_entry_free(&s->css[i]);
  }
  free(s->css);
  s->css = NULL;
  s->ncss = 0;
  s->capcss = 0;
  for (i = 0; i < s->ndocs; i++) {
    free(s->docs[i].path);
    free(s->docs[i].text);
  }
  free(s->docs);
  s->docs = NULL;
  s->ndocs = 0;
  s->capdocs = 0;
  for (i = 0; i < s->nextra; i++) {
    free(s->extra[i].path);
  }
  free(s->extra);
  s->extra = NULL;
  s->nextra = 0;
  s->capextra = 0;
  free(s->toml_path);
  free(s->config_dir);
  s->toml_path = toml;
  s->config_dir = ccb_dirname(toml);
  s->config_dirty = 1;
  s->editor_dirty = 1;
}

// Pick the initialize root directory: rootUri wins, then the first
// workspace folder's uri, then rootPath. Returns a malloc'd path or
// NULL when none was given or usable.
static char *lsp_init_root(const ccb_json_value *params) {
  const ccb_json_value *v = NULL;
  const ccb_json_value *folders = NULL;
  if (params == NULL || params->type != CCB_JSON_OBJ) {
    return NULL;
  }
  v = ccb_json_obj_get(params, "rootUri");
  if (v != NULL && v->type == CCB_JSON_STR && v->str != NULL) {
    char *p = lsp_uri_to_path(v->str);
    if (p != NULL && p[0] != '\0') {
      return p;
    }
    free(p);
  }
  folders = ccb_json_obj_get(params, "workspaceFolders");
  if (folders != NULL && folders->type == CCB_JSON_ARR &&
      folders->count > 0) {
    const ccb_json_value *f0 = folders->items[0];
    if (f0 != NULL && f0->type == CCB_JSON_OBJ) {
      v = ccb_json_obj_get(f0, "uri");
      if (v != NULL && v->type == CCB_JSON_STR && v->str != NULL) {
        char *p = lsp_uri_to_path(v->str);
        if (p != NULL && p[0] != '\0') {
          return p;
        }
        free(p);
      }
    }
  }
  v = ccb_json_obj_get(params, "rootPath");
  if (v != NULL && v->type == CCB_JSON_STR && v->str != NULL &&
      v->str[0] != '\0') {
    return ccb_xstrdup(v->str);
  }
  return NULL;
}

// Diagnostics array JSON, mirroring pv_project_json's diagnostics
// section exactly (code/phase/message default "", file null when
// absent, line as-is).
static void lsp_json_diagnostics(ccb_str *out, pv_project *p) {
  size_t i = 0;
  ccb_str_append(out, "{\"diagnostics\":[");
  if (p != NULL) {
    for (i = 0; i < p->diags.count; i++) {
      ccb_diag *d = &p->diags.items[i];
      if (i > 0) {
        ccb_str_append(out, ",");
      }
      ccb_str_append(out, "{\"code\":");
      if (d->code != NULL) {
        ccb_json_escape(out, d->code);
      } else {
        ccb_str_append(out, "\"\"");
      }
      ccb_str_append(out, ",\"phase\":");
      if (d->phase != NULL) {
        ccb_json_escape(out, d->phase);
      } else {
        ccb_str_append(out, "\"\"");
      }
      ccb_str_append(out, ",\"message\":");
      if (d->message != NULL) {
        ccb_json_escape(out, d->message);
      } else {
        ccb_str_append(out, "\"\"");
      }
      ccb_str_append(out, ",\"file\":");
      if (d->file != NULL) {
        ccb_json_escape(out, d->file);
      } else {
        ccb_str_append(out, "null");
      }
      ccb_str_appendf(out, ",\"line\":%ld}", d->line);
    }
  }
  ccb_str_append(out, "]}");
}

// Incoming edges for a screen: every edge in every screen whose
// target is this screen's name, including self-edges.
static void lsp_json_incoming(ccb_str *out, pv_project *p,
    const char *name) {
  size_t si = 0;
  size_t ei = 0;
  size_t k = 0;
  int first = 1;
  ccb_str_append(out, "[");
  if (p == NULL || name == NULL) {
    ccb_str_append(out, "]");
    return;
  }
  for (si = 0; si < p->nscreens; si++) {
    pv_screen *screen = p->screens[si];
    for (ei = 0; ei < screen->nedges; ei++) {
      pv_edge *e = &screen->edges[ei];
      if (e->target == NULL || strcmp(e->target, name) != 0) {
        continue;
      }
      if (!first) {
        ccb_str_append(out, ",");
      }
      first = 0;
      ccb_str_append(out, "{\"from_screen\":");
      if (screen->name != NULL) {
        ccb_json_escape(out, screen->name);
      } else {
        ccb_str_append(out, "null");
      }
      ccb_str_append(out, ",\"from\":");
      if (e->from != NULL) {
        ccb_json_escape(out, e->from);
      } else {
        ccb_str_append(out, "null");
      }
      ccb_str_append(out, ",\"target\":");
      ccb_json_escape(out, e->target);
      ccb_str_appendf(out, ",\"unknown\":%s", e->unknown ? "true" : "false");
      ccb_str_append(out, ",\"args\":[");
      for (k = 0; k < e->nargs; k++) {
        if (k > 0) {
          ccb_str_append(out, ",");
        }
        if (e->args[k] != NULL) {
          ccb_json_escape(out, e->args[k]);
        } else {
          ccb_str_append(out, "null");
        }
      }
      ccb_str_append(out, "],\"branch\":");
      if (e->branch != NULL) {
        ccb_json_escape(out, e->branch);
      } else {
        ccb_str_append(out, "null");
      }
      ccb_str_append(out, "}");
    }
  }
  ccb_str_append(out, "]");
}

// ---------- message dispatch ----------

// Handle one parsed message body. Returns 1 when the loop should
// exit (exit notification), 0 otherwise. Never rebuilds: handlers
// only update state; the loop rebuilds when editor_dirty is set or
// the disk debounce fires, so one rebuild covers a burst.
static int lsp_handle(pv_lsp_server *s, const char *body, int out_fd) {
  ccb_json_value *msg = NULL;
  char *err = NULL;
  const ccb_json_value *method_v = NULL;
  const ccb_json_value *id_v = NULL;
  const ccb_json_value *params = NULL;
  const char *method = NULL;
  int is_request = 0;
  int rc = 0;
  msg = ccb_json_parse((ccb_slice){ body, strlen(body) }, &err);
  if (msg == NULL) {
    free(err);
    lsp_send_error(out_fd, NULL, -32700, "Parse error");
    return 0;
  }
  if (msg->type != CCB_JSON_OBJ) {
    ccb_json_free(msg);
    return 0;
  }
  method_v = ccb_json_obj_get(msg, "method");
  if (method_v == NULL || method_v->type != CCB_JSON_STR ||
      method_v->str == NULL) {
    // A client response (or a malformed message): the server never
    // sends requests, so there is nothing to answer.
    ccb_json_free(msg);
    return 0;
  }
  method = method_v->str;
  id_v = ccb_json_obj_get(msg, "id");
  is_request = id_v != NULL;
  params = ccb_json_obj_get(msg, "params");
  if (s->shutdown && is_request) {
    lsp_send_error(out_fd, id_v, -32600, "server shut down");
    ccb_json_free(msg);
    return 0;
  }
  if (s->shutdown && !is_request) {
    // Notifications after shutdown are ignored, except exit.
    if (strcmp(method, "exit") == 0) {
      ccb_json_free(msg);
      return 1;
    }
    ccb_json_free(msg);
    return 0;
  }
  if (!is_request) {
    // Notifications.
    if (strcmp(method, "exit") == 0) {
      ccb_json_free(msg);
      return 1;
    }
    if (strcmp(method, "textDocument/didOpen") == 0 ||
        strcmp(method, "textDocument/didChange") == 0 ||
        strcmp(method, "textDocument/didClose") == 0) {
      const ccb_json_value *doc = NULL;
      const ccb_json_value *uri_v = NULL;
      const ccb_json_value *text_v = NULL;
      const char *uri = NULL;
      char *path = NULL;
      int is_close = strcmp(method, "textDocument/didClose") == 0;
      if (params != NULL && params->type == CCB_JSON_OBJ) {
        doc = ccb_json_obj_get(params, "textDocument");
        if (doc != NULL && doc->type == CCB_JSON_OBJ) {
          uri_v = ccb_json_obj_get(doc, "uri");
          if (uri_v != NULL && uri_v->type == CCB_JSON_STR) {
            uri = uri_v->str;
          }
        }
        if (!is_close) {
          if (strcmp(method, "textDocument/didOpen") == 0) {
            // didOpen carries the text on the document itself.
            if (doc != NULL) {
              text_v = ccb_json_obj_get(doc, "text");
            }
          } else {
            const ccb_json_value *changes =
              ccb_json_obj_get(params, "contentChanges");
            if (changes != NULL && changes->type == CCB_JSON_ARR &&
                changes->count > 0) {
              // Full sync: the last change carries the whole text.
              const ccb_json_value *last =
                changes->items[changes->count - 1];
              if (last != NULL && last->type == CCB_JSON_OBJ) {
                text_v = ccb_json_obj_get(last, "text");
              }
            }
          }
        }
      }
      if (uri != NULL) {
        path = lsp_uri_to_path(uri);
      }
      if (path != NULL) {
        if (is_close) {
          pv_lsp_did_close(s, path);
        } else if (text_v != NULL &&
            text_v->type == CCB_JSON_STR &&
            text_v->str != NULL) {
          if (strcmp(method, "textDocument/didOpen") == 0) {
            pv_lsp_did_open(s, path, text_v->str);
          } else {
            pv_lsp_did_change(s, path, text_v->str);
          }
        }
        free(path);
      }
      ccb_json_free(msg);
      return 0;
    }
    // initialized, didSave, $/cancelRequest, and anything else:
    // no action (didSave needs none: the text already synced).
    ccb_json_free(msg);
    return 0;
  }
  // Requests.
  if (strcmp(method, "initialize") == 0) {
    char *root = lsp_init_root(params);
    ccb_str result;
    if (root != NULL) {
      lsp_reroot(s, root);
      free(root);
    }
    ccb_str_init(&result);
    ccb_str_append(&result, "{\"capabilities\":{\"textDocumentSync\":1},");
    ccb_str_append(&result, "\"serverInfo\":{\"name\":\"ccb lsp\",\"version\":\"");
    ccb_str_append(&result, CCB_VERSION);
    ccb_str_append(&result, "\"}}");
    lsp_send_result(out_fd, id_v, result.buf != NULL ? result.buf : "{}");
    ccb_str_free(&result);
    // First publish, unless a re-root queued a rebuild that will
    // publish the new tree right after.
    if (!s->editor_dirty) {
      lsp_maybe_publish(s, out_fd);
    }
    ccb_json_free(msg);
    return 0;
  }
  if (strcmp(method, "shutdown") == 0) {
    s->shutdown = 1;
    lsp_send_result(out_fd, id_v, "null");
    ccb_json_free(msg);
    return 0;
  }
  if (strcmp(method, "ccb/logicTree") == 0) {
    const char *tree = pv_lsp_tree_json(s);
    if (tree == NULL) {
      lsp_send_error(out_fd, id_v, -32603, "no project loaded");
    } else {
      lsp_send_result(out_fd, id_v, tree);
    }
    ccb_json_free(msg);
    return 0;
  }
  if (strcmp(method, "ccb/screenInfo") == 0) {
    const ccb_json_value *name_v = NULL;
    pv_project *p = pv_lsp_project(s);
    pv_screen *screen = NULL;
    size_t i = 0;
    if (params != NULL && params->type == CCB_JSON_OBJ) {
      name_v = ccb_json_obj_get(params, "screen");
    }
    if (name_v != NULL && name_v->type == CCB_JSON_STR &&
        name_v->str != NULL && p != NULL) {
      for (i = 0; i < p->nscreens; i++) {
        if (p->screens[i]->name != NULL &&
            strcmp(p->screens[i]->name, name_v->str) == 0) {
          screen = p->screens[i];
          break;
        }
      }
    }
    if (screen == NULL) {
      lsp_send_error(out_fd, id_v, -32602, "unknown screen");
    } else {
      ccb_str result;
      ccb_str_init(&result);
      ccb_str_append(&result, "{\"screen\":");
      pv_project_json_screen(&result, p, screen);
      ccb_str_append(&result, ",\"incoming\":");
      lsp_json_incoming(&result, p, screen->name);
      ccb_str_append(&result, "}");
      lsp_send_result(out_fd, id_v,
        result.buf != NULL ? result.buf : "{}");
      ccb_str_free(&result);
    }
    ccb_json_free(msg);
    return 0;
  }
  if (strcmp(method, "ccb/diagnostics") == 0) {
    ccb_str result;
    ccb_str_init(&result);
    lsp_json_diagnostics(&result, pv_lsp_project(s));
    lsp_send_result(out_fd, id_v,
      result.buf != NULL ? result.buf : "{}");
    ccb_str_free(&result);
    ccb_json_free(msg);
    return 0;
  }
  lsp_send_error(out_fd, id_v, -32601, "Method not found");
  ccb_json_free(msg);
  return rc;
}

// ---------- run loop ----------

int pv_lsp_run(pv_lsp_server *s, int in_fd, int out_fd) {
  lsp_reader rd;
  int exit_requested = 0;
  int flags = 0;
  memset(&rd, 0, sizeof(rd));
  rd.fd = in_fd;
  if (s == NULL) {
    return 1;
  }
  flags = fcntl(in_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(in_fd, F_SETFL, flags | O_NONBLOCK);
  }
  pv_watch_install_signals();
  for (;;) {
    // 1. Drain stdin messages (non-blocking reads).
    for (;;) {
      char *body = NULL;
      int pr = lsp_reader_next(&rd, &body);
      if (pr == 0) {
        if (!lsp_reader_more(&rd)) {
          break;
        }
        continue;
      }
      if (pr == 2) {
        continue; // skipped a header block without a length
      }
      if (lsp_handle(s, body, out_fd)) {
        exit_requested = 1;
      }
      free(body);
      if (exit_requested) {
        break;
      }
    }
    if (exit_requested || rd.eof || pv_watch_stop) {
      break;
    }
    // 2. Editor updates rebuild immediately (a burst still costs
    // one rebuild per message; preview projects are small). Publish
    // when the tree changed, or when nothing was ever published:
    // a re-root that rebuilds to an identical tree (e.g. a root
    // spelling that differs from the discovered directory) must
    // not swallow the first publish.
    if (s->editor_dirty) {
      int rrc = pv_lsp_rebuild(s);
      if (rrc == 1 || s->last_published == NULL) {
        lsp_maybe_publish(s, out_fd);
      }
    }
    if (rd.eof || pv_watch_stop) {
      break;
    }
    // 3. Sleep on the inotify wakeup (bounded 50ms so disk changes
    // surface promptly even without an event), then scan + debounce.
    if (s->watch_ctx != NULL) {
      (void)pv_watch_wait(s->watch_ctx, 50);
    } else {
      poll(NULL, 0, 50);
    }
    if (pv_watch_stop) {
      break;
    }
    (void)pv_lsp_scan_disk(s);
    if (pv_watch_debounce_due(&s->deb, pv_watch_now_ms())) {
      pv_watch_debounce_reset(&s->deb);
      if (pv_lsp_rebuild(s) == 1) {
        lsp_maybe_publish(s, out_fd);
      }
    }
    if (rd.eof || pv_watch_stop) {
      break;
    }
  }
  free(rd.buf);
  return s->shutdown ? 0 : 1;
}
