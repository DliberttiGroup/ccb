// ccb_preview_real_load: public entry points (pv_real_load and helpers).
// Split from ccb_preview_real.c.

#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include <ctype.h>
#include "ccb_preview_real_internal.h"

// ---------- public entry points ----------

// Kotlin source dir for a config: [paths] kotlin, else [paths]
// java, else the kotlin/java dir next to the manifest. Malloc'd,
// or NULL when no candidate exists on disk.
char *pv_real_kotlin_dir(const ccb_config *cfg) {
  char *dir = NULL;
  char *cand = NULL;
  if (cfg == NULL) {
    return NULL;
  }
  if (cfg->kotlin_dir != NULL &&
      ccb_path_is_dir(cfg->kotlin_dir)) {
    return ccb_xstrdup(cfg->kotlin_dir);
  }
  if (cfg->java_dir != NULL && ccb_path_is_dir(cfg->java_dir)) {
    return ccb_xstrdup(cfg->java_dir);
  }
  if (cfg->manifest == NULL) {
    return NULL;
  }
  dir = ccb_dirname(cfg->manifest);
  cand = ccb_path_join(dir, "kotlin");
  if (ccb_path_is_dir(cand)) {
    free(dir);
    return cand;
  }
  free(cand);
  cand = ccb_path_join(dir, "java");
  if (ccb_path_is_dir(cand)) {
    free(dir);
    return cand;
  }
  free(cand);
  free(dir);
  return NULL;
}

// Manifest launcher activity, as a stripped screen name ("Main" for
// MainActivity): the <activity> whose intent-filter carries both
// MAIN and LAUNCHER. Malloc'd, or NULL when there is none.
// The android:name value of the launcher <activity> block starting
// at p and ending at end: malloc'd, or NULL when this block is not
// the launcher or has no name. (The MAIN/LAUNCHER check is
// unbounded, as in the caller: a later block's strings can match.)
static char *rv_activity_name(const char *p, const char *end) {
  const char *n = NULL;
  const char *q = NULL;
  if (strstr(p, "android.intent.action.MAIN") == NULL ||
      strstr(p, "android.intent.category.LAUNCHER") == NULL) {
    return NULL;
  }
  n = strstr(p, "android:name=\"");
  if (n == NULL || n >= end) {
    return NULL;
  }
  n += 14;
  q = strchr(n, '"');
  if (q == NULL || q >= end) {
    return NULL;
  }
  return ccb_xstrndup(n, (size_t)(q - n));
}

static char *rv_manifest_launcher(const char *manifest_path,
    const char *package) {
  char *text = NULL;
  size_t len = 0;
  const char *p = NULL;
  if (manifest_path == NULL ||
      ccb_read_file(manifest_path, &text, &len) != CCB_OK) {
    return NULL;
  }
  p = text;
  while ((p = strstr(p, "<activity")) != NULL) {
    const char *after = p + 9;
    const char *block_end = NULL;
    const char *next_activity = NULL;
    const char *end = NULL;
    char *name = NULL;
    if (*after != '\0' && (rv_is_id_char(*after) || *after == '-')) {
      p = after;
      continue;
    }
    block_end = strstr(p, "</activity>");
    next_activity = strstr(p + 1, "<activity");
    end = block_end;
    if (end == NULL ||
        (next_activity != NULL && next_activity < end)) {
      end = next_activity == NULL ? p + strlen(p) : next_activity;
    } else {
      end = block_end + strlen("</activity>");
    }
    name = rv_activity_name(p, end);
    if (name != NULL) {
      char *norm = pv_normalize_class(package, name);
      free(name);
      free(text);
      if (norm == NULL) {
        return NULL;
      }
      name = rv_strip_suffix(norm);
      free(norm);
      return name;
    }
    p = end;
  }
  free(text);
  return NULL;
}

// Phase 2 load (see ccb_preview.h): parse the app's own Activities
// into screens, appended to *screens_io (with pending onClick edges
// appended to *pending_io), reusing the flow parser over synthesized
// flow text. The launcher screen name is returned in *launcher_io
// (malloc'd, or NULL; the caller frees it). Returns CCB_OK always;
// an absent Kotlin dir or unreadable sources simply yield no
// screens, never an error.
// @string table: [preview] resources, else <res>/values.
static void rv_load_strings(const ccb_config *cfg, rv_strings *strings) {
  char *res_dir = NULL;
  char *spath = NULL;
  if (cfg->preview_resources != NULL) {
    res_dir = ccb_xstrdup(cfg->preview_resources);
  } else if (cfg->res != NULL) {
    res_dir = ccb_path_join(cfg->res, "values");
  }
  if (res_dir != NULL) {
    spath = ccb_path_join(res_dir, "strings.xml");
    rv_strings_parse_file(strings, spath);
    free(spath);
    free(res_dir);
  }
}

// One layout file: scan its buttons into the layouts list.
static void rv_load_one_layout(const char *path, rv_layout ***layouts,
    size_t *nlayouts, rv_strings *strings) {
  char *base = ccb_basename(path);
  size_t bn = strlen(base);
  if (bn <= 4 || strcmp(base + bn - 4, ".xml") != 0) {
    free(base);
    return;
  }
  char *lname = ccb_xstrndup(base, bn - 4);
  free(base);
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &text, &len) == CCB_OK) {
    rv_scan_layout_file(text, len, lname, path, layouts, nlayouts, strings);
    free(text);
  }
  free(lname);
}

// Layout buttons, from the same layout inputs Slice 3 uses.
static void rv_load_layouts(const ccb_config *cfg, rv_layout ***layouts,
    size_t *nlayouts, rv_strings *strings) {
  char *layout_path = pv_preview_layout_path(cfg);
  char **layout_files = NULL;
  size_t nlayout_files = 0;
  if (layout_path != NULL) {
    if (ccb_path_is_dir(layout_path)) {
      (void)ccb_collect_files(layout_path, ".xml", &layout_files,
        &nlayout_files);
    } else if (ccb_path_exists(layout_path)) {
      layout_files = ccb_xmalloc(sizeof(*layout_files));
      layout_files[0] = ccb_xstrdup(layout_path);
      nlayout_files = 1;
    }
    free(layout_path);
  }
  for (size_t li = 0; li < nlayout_files; li++) {
    rv_load_one_layout(layout_files[li], layouts, nlayouts, strings);
  }
  ccb_free_str_array(layout_files, nlayout_files);
}

// Append one emitted fun name (owned copy) to the growing list.
static void rv_emitted_add(char ***all, size_t *n, size_t *cap,
    const char *name) {
  if (*n == *cap) {
    size_t ncap = *cap == 0 ? 8 : *cap * 2;
    *all = ccb_xrealloc(*all, ncap * sizeof(**all));
    *cap = ncap;
  }
  (*all)[(*n)++] = ccb_xstrdup(name);
}

// True when method is already in the emitted list.
static int rv_emitted_has(char **all_emitted, size_t nemitted_all,
    const char *method) {
  for (size_t k = 0; k < nemitted_all; k++) {
    if (strcmp(all_emitted[k], method) == 0) {
      return 1;
    }
  }
  return 0;
}

// Synthesize one flow text for one activity and record it, plus
// its emitted fun names.
static void rv_synth_one_activity(rv_activity *a, char *file,
    rv_layout **layouts, size_t nlayouts, char **onclick_methods,
    size_t nonclick, ccb_diag_list *diags, char ***syn_labels,
    char ***syn_texts, const char ***syn_layouts, size_t *nsyn,
    size_t *capsyn, char ***all_emitted, size_t *nemitted_all,
    size_t *capemitted_all) {
  size_t nemitted = 0;
  char **emitted = NULL;
  rv_layout *layout = NULL;
  rv_warn_unresolved_listeners(a, diags);
  emitted = rv_emitted_funs(a, onclick_methods, nonclick, diags,
    &nemitted);
  if (a->layout != NULL) {
    layout = rv_layout_find(layouts, nlayouts, a->layout);
  }
  if (*nsyn == *capsyn) {
    size_t ncap = *capsyn == 0 ? 4 : *capsyn * 2;
    *syn_labels = ccb_xrealloc(*syn_labels, ncap * sizeof(**syn_labels));
    *syn_texts = ccb_xrealloc(*syn_texts, ncap * sizeof(**syn_texts));
    *syn_layouts = ccb_xrealloc(*syn_layouts, ncap * sizeof(**syn_layouts));
    *capsyn = ncap;
  }
  (*syn_labels)[*nsyn] = file;
  (*syn_texts)[*nsyn] = rv_synth_activity(a, layout, emitted, nemitted);
  (*syn_layouts)[*nsyn] = a->layout;
  (*nsyn)++;
  for (size_t ei = 0; ei < nemitted; ei++) {
    rv_emitted_add(all_emitted, nemitted_all, capemitted_all, emitted[ei]);
    free(emitted[ei]);
  }
  free(emitted);
}

// A listener method reference with no definition in any scanned
// Activity (a top-level fun, or a typo) records no edge: warn
// once per method, so its navigation is visible, not silent.
// (An android:onClick method with no matching fun stays silent,
// like Slice 3: the layout, not a button, names it.)
static void rv_warn_unattributed_listeners(rv_activity *acts, size_t nacts,
    char ***all_emitted, size_t *nemitted_all, size_t *capemitted_all,
    ccb_diag_list *diags) {
  for (size_t ai = 0; ai < nacts; ai++) {
    for (size_t li = 0; li < acts[ai].nlisteners; li++) {
      const char *method = acts[ai].listeners[li].method;
      if (method == NULL) {
        continue;
      }
      if (rv_emitted_has(*all_emitted, *nemitted_all, method)) {
        continue;
      }
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "unattributed navigation: click handler '");
      ccb_str_append(&msg, method);
      ccb_str_append(&msg, "' has no definition in the scanned "
        "Activities, so its navigation is not recorded as an "
        "edge; it is reported here so it is not silently dropped");
      rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "",
        acts[ai].file, acts[ai].listeners[li].line);
      ccb_str_free(&msg);
      rv_emitted_add(all_emitted, nemitted_all, capemitted_all, method);
    }
  }
}

// True when id already has a binding (first wins).
static int rv_bind_has(char **bind_keys, size_t nbind, const char *id) {
  for (size_t k = 0; k < nbind; k++) {
    if (strcmp(bind_keys[k], id) == 0) {
      return 1;
    }
  }
  return 0;
}

// Parse the synthesized flow texts into screens, with bindings:
// the config's [preview.bindings] first (explicit wins), then one
// real binding per layout view id (first wins across layouts, in
// sorted file order).
static void rv_parse_synth(const ccb_config *cfg, ccb_arena *arena,
    ccb_diag_list *diags, char **syn_labels, char **syn_texts,
    const char **syn_layouts, size_t nsyn, rv_layout **layouts,
    size_t nlayouts, char **graph_keys, char **graph_vals, size_t ngraph,
    char **onclick_methods, char **onclick_labels, size_t nonclick,
    pv_screen ***screens_io, size_t *nscreens_io,
    pv_pending_edge ***pending_io, size_t *npending_io) {
  char **bind_keys = NULL;
  char **bind_vals = NULL;
  size_t nbind = 0;
  for (size_t bi = 0; bi < cfg->preview_binding_count; bi++) {
    bind_keys = ccb_xrealloc(bind_keys, (nbind + 1) * sizeof(*bind_keys));
    bind_vals = ccb_xrealloc(bind_vals, (nbind + 1) * sizeof(*bind_vals));
    bind_keys[nbind] = cfg->preview_binding_keys[bi];
    bind_vals[nbind] = cfg->preview_binding_values[bi];
    nbind++;
  }
  for (size_t li = 0; li < nlayouts; li++) {
    for (size_t bi = 0; bi < layouts[li]->nbuttons; bi++) {
      const char *id = layouts[li]->buttons[bi].id;
      const char *label = layouts[li]->buttons[bi].label;
      if (rv_bind_has(bind_keys, nbind, id)) {
        continue;
      }
      bind_keys = ccb_xrealloc(bind_keys, (nbind + 1) * sizeof(*bind_keys));
      bind_vals = ccb_xrealloc(bind_vals, (nbind + 1) * sizeof(*bind_vals));
      bind_keys[nbind] = ccb_arena_strdup(arena, id);
      bind_vals[nbind] = ccb_arena_strdup(arena, label);
      nbind++;
    }
  }
  pv_fun_index *index = pv_fun_index_build(arena, syn_labels, syn_texts, nsyn);
  for (size_t i = 0; i < nsyn; i++) {
    size_t before = *nscreens_io;
    pv_flow_parse_indexed(arena, syn_texts[i], syn_labels[i], diags,
      screens_io, nscreens_io, bind_keys, bind_vals, nbind,
      onclick_methods, onclick_labels, nonclick,
      graph_keys, graph_vals, ngraph, pending_io, npending_io,
      index);
    // Chunk 2: replace the synthetic Button root with the
    // layout-derived wireframe when the Activity's layout file
    // exists; otherwise keep the synthetic root. The parse
    // appends the Activity's screen first (its fun is first in
    // the synthetic text), so index `before` is the fresh
    // screen. The wireframe carries the same button labels, so
    // onClick distribution by label is unaffected.
    if (*nscreens_io > before && syn_layouts[i] != NULL) {
      rv_layout *layout = rv_layout_find(layouts, nlayouts,
        syn_layouts[i]);
      pv_box *wire = NULL;
      if (layout != NULL && layout->path != NULL) {
        wire = rv_layout_wireframe(arena, layout->path);
      }
      if (wire != NULL) {
        (*screens_io)[before]->root = wire;
      }
    }
  }
  // Chunk C, same rule as flow files: a nav no parse attributed
  // warns CCB-P1025 (with the real file path; the line is the
  // synthesized line — see the as-built note).
  pv_sweep_unattributed_navs(index, diags);
  pv_fun_index_free(index);
  free(bind_keys);
  free(bind_vals);
}

ccb_status pv_real_load(const ccb_config *cfg, ccb_arena *arena,
    ccb_diag_list *diags, char **graph_keys, char **graph_vals,
    size_t ngraph, char **onclick_methods, char **onclick_labels,
    size_t nonclick, pv_screen ***screens_io, size_t *nscreens_io,
    pv_pending_edge ***pending_io, size_t *npending_io,
    char **launcher_io) {
  char *kdir = NULL;
  char **kt_files = NULL;
  size_t nkt = 0;
  rv_strings strings;
  rv_layout **layouts = NULL;
  size_t nlayouts = 0;
  rv_activity *acts = NULL;
  size_t nacts = 0;
  char **syn_labels = NULL;
  char **syn_texts = NULL;
  // Layout name per synthetic text (borrowed from the activity;
  // Chunk 2 wireframe lookup). Parallel to syn_labels/syn_texts.
  const char **syn_layouts = NULL;
  size_t nsyn = 0;
  size_t capsyn = 0;
  char **all_emitted = NULL;
  size_t nemitted_all = 0;
  size_t capemitted_all = 0;
  size_t i = 0;
  if (launcher_io != NULL) {
    *launcher_io = NULL;
  }
  if (cfg == NULL || arena == NULL || screens_io == NULL ||
      nscreens_io == NULL) {
    return CCB_OK;
  }
  kdir = pv_real_kotlin_dir(cfg);
  if (kdir == NULL) {
    return CCB_OK;
  }
  memset(&strings, 0, sizeof(strings));
  // @string table: [preview] resources, else <res>/values.
  rv_load_strings(cfg, &strings);
  // Layout buttons, from the same layout inputs Slice 3 uses.
  rv_load_layouts(cfg, &layouts, &nlayouts, &strings);
  if (ccb_collect_files(kdir, ".kt", &kt_files, &nkt) != CCB_OK) {
    nkt = 0;
  }
  free(kdir);
  // Extract activities, warn for unresolved listeners and dropped
  // member funs, and synthesize one flow text per activity.
  for (i = 0; i < nkt; i++) {
    char *text = NULL;
    size_t len = 0;
    size_t before = nacts;
    size_t ai = 0;
    if (ccb_read_file(kt_files[i], &text, &len) != CCB_OK) {
      continue;
    }
    rv_extract_activities(kt_files[i], text, &acts, &nacts);
    for (ai = before; ai < nacts; ai++) {
      rv_synth_one_activity(&acts[ai], kt_files[i], layouts, nlayouts,
        onclick_methods, nonclick, diags, &syn_labels, &syn_texts,
        &syn_layouts, &nsyn, &capsyn, &all_emitted, &nemitted_all,
        &capemitted_all);
    }
    free(text);
  }
  rv_warn_unattributed_listeners(acts, nacts, &all_emitted, &nemitted_all,
    &capemitted_all, diags);
  if (nsyn > 0) {
    rv_parse_synth(cfg, arena, diags, syn_labels, syn_texts, syn_layouts,
      nsyn, layouts, nlayouts, graph_keys, graph_vals, ngraph,
      onclick_methods, onclick_labels, nonclick, screens_io, nscreens_io,
      pending_io, npending_io);
  }
  for (i = 0; i < nsyn; i++) {
    free(syn_texts[i]);
  }
  free(syn_labels);
  free(syn_texts);
  free(syn_layouts);
  for (i = 0; i < nemitted_all; i++) {
    free(all_emitted[i]);
  }
  free(all_emitted);
  ccb_free_str_array(kt_files, nkt);
  for (i = 0; i < nacts; i++) {
    rv_activity_free(&acts[i]);
  }
  free(acts);
  for (i = 0; i < nlayouts; i++) {
    size_t b = 0;
    free(layouts[i]->name);
    free(layouts[i]->path);
    for (b = 0; b < layouts[i]->nbuttons; b++) {
      free(layouts[i]->buttons[b].id);
      free(layouts[i]->buttons[b].label);
      free(layouts[i]->buttons[b].method);
    }
    free(layouts[i]->buttons);
    free(layouts[i]);
  }
  free(layouts);
  for (i = 0; i < strings.n; i++) {
    free(strings.items[i].name);
    free(strings.items[i].value);
  }
  free(strings.items);
  if (launcher_io != NULL) {
    *launcher_io = rv_manifest_launcher(cfg->manifest, cfg->package);
  }
  return CCB_OK;
}
