// ccb_preview_lsp_server: editor updates, disk scan, open/close.
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
// Mark the dirty flag for one extra input path by kind (XML or
// resources only).
static void lsp_mark_extra(pv_lsp_server *s, const char *path) {
  pv_lsp_kind kind = lsp_classify(s, path);
  if (kind == PV_LSP_XML) {
    s->xml_dirty = 1;
  } else if (kind == PV_LSP_RESOURCES) {
    s->resources_dirty = 1;
  }
}

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
        lsp_mark_extra(s, fresh[j].path);
        dirty = 1;
      }
      i++;
      j++;
    } else if (cmp < 0) {
      // Removed.
      lsp_mark_extra(s, s->extra[i].path);
      dirty = 1;
      i++;
    } else {
      // Added.
      lsp_mark_extra(s, fresh[j].path);
      dirty = 1;
      j++;
    }
  }
  for (; i < s->nextra; i++) {
    lsp_mark_extra(s, s->extra[i].path);
    dirty = 1;
  }
  for (; j < nfresh; j++) {
    lsp_mark_extra(s, fresh[j].path);
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

// Mark one changed file dirty by kind (flow/css/config/xml/
// resources). An open document wins over disk content: the disk
// change is invisible until the document closes. Returns 1 when
// the change marks the project dirty.
static int lsp_mark_changed(pv_lsp_server *s, const char *path) {
  pv_lsp_kind kind = lsp_classify(s, path);
  if (kind == PV_LSP_IGNORE) {
    return 0;
  }
  if (lsp_doc_find(s, path) != NULL) {
    return 0;
  }
  switch (kind) {
    case PV_LSP_FLOW: {
      size_t idx = 0;
      if (lsp_flow_lookup(s, path, &idx)) {
        s->flow[idx].dirty = 1;
      }
      break;
    }
    case PV_LSP_CSS: {
      size_t idx = 0;
      if (lsp_css_lookup(s, path, &idx)) {
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
  return 1;
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
      if (lsp_mark_changed(s, changed[i])) {
        dirty = 1;
      }
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
