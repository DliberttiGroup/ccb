// ccb_preview_lsp_build: incremental per-file parsing + rebuild.
// Split from ccb_preview_lsp.c.

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
void lsp_str_array_add(char ***arr_io, size_t *n_io,
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

int lsp_str_cmp(const void *a, const void *b) {
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
