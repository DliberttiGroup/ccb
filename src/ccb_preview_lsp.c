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
#include "ccb_preview_lsp_internal.h"

// ccb_preview_lsp: `ccb lsp` server. See ccb_preview_lsp.h for the
// contract. Single-threaded: the run loop polls stdin and the watch
// context's inotify wakeup; nothing here spawns a thread.

// The phone default frame width and phone DP width, mirroring
// PV_DEFAULT_FRAME_WIDTH / PV_DEFAULT_PHONE_DP_WIDTH in
// ccb_preview.c (file-local there). The server has no --width flag;
// it serves the phone-default frame.

// One open editor document: malloc'd path + text, owned by the list.

// One extra watched file (nav/layout XML, resources values): stat
// identity for the disk-change diff. The set itself is re-enumerated
// on every scan, so added/removed files are detected too.

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

// True when path is an .xml under a navigation/layout fallback dir
// (resources or [paths] res, subdirs navigation/layout).
static int lsp_under_fallback_xml(const ccb_config *cfg,
    const char *path) {
  const char *roots[2];
  const char *subs[2];
  size_t r = 0;
  roots[0] = cfg->preview_resources;
  roots[1] = cfg->res;
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
        return 1;
      }
    }
  }
  return 0;
}

pv_lsp_kind lsp_classify(pv_lsp_server *s, const char *path) {
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
    if (lsp_under_fallback_xml(s->cfg, path)) {
      return PV_LSP_XML;
    }
  }
  return PV_LSP_IGNORE;
}

// ---------- open documents ----------

pv_lsp_doc *lsp_doc_find(pv_lsp_server *s, const char *path) {
  size_t i = 0;
  for (i = 0; i < s->ndocs; i++) {
    if (strcmp(s->docs[i].path, path) == 0) {
      return &s->docs[i];
    }
  }
  return NULL;
}

void lsp_doc_set(pv_lsp_server *s, const char *path,
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

void lsp_doc_drop(pv_lsp_server *s, const char *path) {
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

void lsp_flow_entry_free(pv_lsp_flow_entry *e) {
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

void lsp_css_entry_free(pv_lsp_css_entry *e) {
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
int lsp_flow_lookup(pv_lsp_server *s, const char *path,
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

int lsp_css_lookup(pv_lsp_server *s, const char *path,
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

pv_lsp_flow_entry *lsp_flow_ensure(pv_lsp_server *s,
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

pv_lsp_css_entry *lsp_css_ensure(pv_lsp_server *s,
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
void lsp_flow_drop(pv_lsp_server *s, size_t idx) {
  lsp_flow_entry_free(&s->flow[idx]);
  memmove(&s->flow[idx], &s->flow[idx + 1],
    (s->nflow - idx - 1) * sizeof(*s->flow));
  s->nflow--;
}

void lsp_css_drop(pv_lsp_server *s, size_t idx) {
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

// Copy one edge's args (and args_named flags) into the arena copy.
static void lsp_copy_args(ccb_arena *arena, pv_edge *de,
    const pv_edge *se) {
  size_t k = 0;
  de->args = ccb_arena_alloc(arena, se->nargs * sizeof(*de->args));
  de->args_named = ccb_arena_alloc(arena, se->nargs * sizeof(int));
  for (k = 0; k < se->nargs; k++) {
    de->args[k] = se->args[k];
    de->args_named[k] = se->args_named != NULL ?
      se->args_named[k] : 0;
  }
  de->nargs = se->nargs;
}

pv_screen *lsp_copy_screen(ccb_arena *arena,
    const pv_screen *src) {
  pv_screen *dst = NULL;
  size_t i = 0;
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
        lsp_copy_args(arena, de, se);
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
void lsp_diag_replay(ccb_diag_list *dl,
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
