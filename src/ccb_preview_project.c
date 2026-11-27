// ccb_preview_project: scenario load, free, and the diagnostics pass.
// Split from ccb_preview.c.

#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_preview_scenario.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include "ccb_json.h"
#include "ccb_preview_internal.h"

ccb_status pv_project_load_scenario(const ccb_config *cfg,
    long frame_width, int ascii, int strict, ccb_diag_list *diags,
    const ccb_scenario *scenario, pv_project **out) {
  ccb_diag_list own_diags;
  ccb_diag_list *dl = NULL;
  pv_project *p = NULL;
  ccb_arena *arena = NULL;
  char **css_files = NULL;
  size_t ncss = 0;
  char **flow_files = NULL;
  size_t nflow = 0;
  size_t i = 0;
  pv_screen **screens = NULL;
  size_t nscreens = 0;
  pv_pending_edge **pending = NULL;
  size_t npending = 0;
  // Chunk A: graph values are function-scoped (not scan-block
  // scoped) so pv_project_assemble can create nodes for graph
  // destinations with no flow screen. The arrays are arena-owned
  // by the scan, so they outlive the block either way.
  char **graph_vals = NULL;
  size_t ngraph = 0;
  // Phase 2: the manifest launcher activity, as a stripped screen
  // name ("Main" for MainActivity). Set by the real-code load
  // below; after assemble, the launcher moves to index 0 when no
  // screen is named "Home".
  char *launcher = NULL;
  if (out == NULL) {
    return CCB_ERR_CONFIG;
  }
  *out = NULL;
  if (cfg == NULL || !pv_preview_applicable(cfg)) {
    return CCB_ERR_CONFIG;
  }
  if (diags != NULL) {
    dl = diags;
  } else {
    ccb_diag_list_init(&own_diags);
    dl = &own_diags;
  }
  arena = ccb_arena_create();
  p = ccb_arena_alloc(arena, sizeof(*p));
  memset(p, 0, sizeof(*p));
  p->arena = arena;
  p->ascii = ascii;
  p->strict = strict || cfg->preview_strict;
  p->phone_dp_width = cfg->preview_phone_width > 0 ?
    cfg->preview_phone_width : PV_DEFAULT_PHONE_DP_WIDTH;
  if (frame_width == 0) {
    frame_width = PV_DEFAULT_FRAME_WIDTH;
  }
  if (frame_width < PV_MIN_FRAME_WIDTH) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_appendf(&msg, "frame width %ld clamped to minimum %d",
      frame_width, PV_MIN_FRAME_WIDTH);
    pv_diag(dl, "CCB-P1013", msg.buf != NULL ? msg.buf : "", NULL, -1);
    ccb_str_free(&msg);
    frame_width = PV_MIN_FRAME_WIDTH;
  }
  p->frame_width = frame_width;
  p->interior_cols = frame_width - 2;
  {
    char **graph_keys = NULL;
    char **onclick_methods = NULL;
    char **onclick_labels = NULL;
    size_t nonclick = 0;
    pv_preview_scan_xml(cfg, arena, &graph_keys, &graph_vals,
      &ngraph, &onclick_methods, &onclick_labels, &nonclick, dl);
    // Flow first: screens must exist before CSS files can be scoped.
    // Chunk B: all flow texts are read first so the whole-project
    // function index can be built before any file is parsed; the
    // texts outlive every parse that uses the index. The kept
    // arrays borrow their paths from flow_files (still owned by
    // it) and own their texts.
    if (cfg->preview_flow != NULL) {
      if (ccb_collect_files(cfg->preview_flow, ".kt", &flow_files,
          &nflow) == CCB_OK) {
        char **kept_files = ccb_xcalloc(nflow == 0 ? 1 : nflow,
          sizeof(*kept_files));
        char **kept_texts = ccb_xcalloc(nflow == 0 ? 1 : nflow,
          sizeof(*kept_texts));
        size_t nkept = 0;
        pv_fun_index *index = NULL;
        for (i = 0; i < nflow; i++) {
          char *text = NULL;
          size_t len = 0;
          if (ccb_read_file(flow_files[i], &text, &len) != CCB_OK) {
            continue;
          }
          kept_files[nkept] = flow_files[i];
          kept_texts[nkept] = text;
          nkept++;
        }
        index = pv_fun_index_build(arena, kept_files, kept_texts, nkept);
        for (i = 0; i < nkept; i++) {
          pv_flow_parse_indexed(arena, kept_texts[i], kept_files[i], dl,
            &screens, &nscreens, cfg->preview_binding_keys,
            cfg->preview_binding_values, cfg->preview_binding_count,
            onclick_methods, onclick_labels, nonclick,
            graph_keys, graph_vals, ngraph, &pending, &npending, index);
        }
        // Chunk C: warn for every nav call no parse attributed to
        // an edge, before the index texts are freed.
        pv_sweep_unattributed_navs(index, dl);
        for (i = 0; i < nkept; i++) {
          free(kept_texts[i]);
        }
        free(kept_files);
        free(kept_texts);
        pv_fun_index_free(index);
        ccb_free_str_array(flow_files, nflow);
      }
    }
    // Phase 2: real-code screens/edges merge into the sketch
    // screens (or stand alone when there is no flow/). Runs after
    // the sketch parse so the merge can dedup against it, and
    // before assemble so pending onClick edges distribute with the
    // sketch's.
    {
      pv_screen **real_screens = NULL;
      size_t nreal = 0;
      pv_pending_edge **real_pending = NULL;
      size_t nreal_pending = 0;
      pv_real_load(cfg, arena, dl, graph_keys, graph_vals, ngraph,
        onclick_methods, onclick_labels, nonclick,
        &real_screens, &nreal, &real_pending, &nreal_pending,
        &launcher);
      pv_merge_real_screens(arena, &screens, &nscreens, &pending,
        &npending, real_screens, nreal, real_pending, nreal_pending,
        launcher, cfg->package, dl);
      // real_screens / real_pending are arena-grown by the real
      // parse (see pv_screen_add / pv_pending_add): never freed.
      // Phase 2: the nav graph's "Home" destination IS the
      // launcher screen — the same launcher↔Home equivalence as
      // the merge above. Without this, assemble would create an
      // empty graph-only "Home" node beside the real launcher
      // screen, and the entry rule would pick that empty node.
      // Only when no sketch screen is named "Home": with a
      // sketch, the graph destination already matches it.
      if (launcher != NULL) {
        size_t g = 0;
        int has_home = 0;
        for (i = 0; i < nscreens; i++) {
          if (strcmp(screens[i]->name, "Home") == 0) {
            has_home = 1;
            break;
          }
        }
        if (!has_home) {
          for (g = 0; g < ngraph; g++) {
            if (strcmp(graph_vals[g], "Home") == 0) {
              graph_vals[g] = launcher;
            }
          }
        }
      }
    }
  }
  // CSS entries: every .css file parsed once, in sorted order,
  // into its own rules + diagnostics. Assembly merges them in
  // load order (common first, then per-screen) and replays the
  // diagnostics of merged files only, so a file assembly skips
  // (CCB-P1015) contributes nothing, exactly as before.
  {
    pv_css_entry *css_entries = NULL;
    size_t ncss_entries = 0;
    ccb_diag_list *css_lists = NULL;
    size_t ci = 0;
    if (cfg->preview_styles != NULL) {
      if (ccb_collect_files(cfg->preview_styles, ".css", &css_files,
          &ncss) == CCB_OK) {
        css_entries = ccb_xcalloc(ncss == 0 ? 1 : ncss,
          sizeof(*css_entries));
        css_lists = ccb_xcalloc(ncss == 0 ? 1 : ncss,
          sizeof(*css_lists));
        ncss_entries = ncss;
        for (ci = 0; ci < ncss; ci++) {
          char *text = NULL;
          size_t len = 0;
          ccb_diag_list_init(&css_lists[ci]);
          if (ccb_read_file(css_files[ci], &text, &len) != CCB_OK) {
            continue;
          }
          css_entries[ci].path = css_files[ci];
          pv_css_parse(arena, text, css_files[ci], p->strict,
            &css_lists[ci], &css_entries[ci].rules,
            &css_entries[ci].nrules);
          css_entries[ci].diags = &css_lists[ci];
          free(text);
        }
      }
    }
    {
      // Chunk A: manifest activities for the P1005 scope, parsed
      // from the configured manifest (none when unconfigured or
      // unreadable — preview never fails on the manifest).
      char **manifest_acts = NULL;
      size_t nmanifest_acts = 0;
      pv_manifest_activities(arena, cfg->manifest, cfg->package,
        &manifest_acts, &nmanifest_acts);
      pv_project_assemble(p, screens, nscreens, pending, npending,
        css_entries, ncss_entries, cfg->preview_resources, scenario,
        dl, graph_vals, ngraph, manifest_acts, nmanifest_acts,
        cfg->package);
    }
    for (ci = 0; ci < ncss_entries; ci++) {
      ccb_diag_list_free(&css_lists[ci]);
    }
    free(css_lists);
    free(css_entries);
    ccb_free_str_array(css_files, ncss);
  }
  // Phase 2: the launcher is the entry screen when no sketch names
  // a screen "Home" — the entry rule is Home else screens[0], so
  // the launcher moves to index 0 (assemble appends graph-only
  // nodes after the real screens, so the move sees them all).
  if (launcher != NULL) {
    size_t hi = 0;
    int has_home = 0;
    for (hi = 0; hi < p->nscreens; hi++) {
      if (strcmp(p->screens[hi]->name, "Home") == 0) {
        has_home = 1;
        break;
      }
    }
    if (!has_home) {
      size_t li = 0;
      for (li = 0; li < p->nscreens; li++) {
        if (pv_names_match(cfg->package, p->screens[li]->name,
            launcher)) {
          pv_screen *tmp = p->screens[li];
          memmove(&p->screens[1], &p->screens[0],
            li * sizeof(*p->screens));
          p->screens[0] = tmp;
          break;
        }
      }
    }
    free(launcher);
  }
  *out = p;
  return CCB_OK;
}

void pv_project_free(pv_project *p) {
  if (p == NULL) {
    return;
  }
  ccb_diag_list_free(&p->diags);
  if (p->arena != NULL) {
    ccb_arena_destroy(p->arena);
  }
}

int pv_diag_is_error(const char *code, int strict) {
  if (code == NULL) {
    return 0;
  }
  if (strcmp(code, "CCB-P1005") == 0 ||
      strcmp(code, "CCB-P1008") == 0) {
    return 1;
  }
  if (strict && (strcmp(code, "CCB-P1001") == 0 ||
      strcmp(code, "CCB-P1014") == 0)) {
    return 1;
  }
  return 0;
}

long pv_project_diagnose(pv_project *p) {
  pv_screen *entry = NULL;
  size_t entry_idx = 0;
  size_t i = 0;
  size_t j = 0;
  long errors = 0;
  int changed = 0;
  if (p == NULL) {
    return 0;
  }
  if (p->nscreens == 0) {
    pv_diag(&p->diags, "CCB-P1011", "no screens found", NULL, -1);
    return 0;
  }
  entry = p->screens[0];
  entry_idx = 0;
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, "Home") == 0) {
      entry = p->screens[i];
      entry_idx = i;
      break;
    }
  }
  for (i = 0; i < p->nscreens; i++) {
    p->screens[i]->reachable = 0;
  }
  entry->reachable = 1;
  // BFS from the entry over known (non-unknown) edges, by screen
  // name, to a fixed point. Unknown edges never traverse. Chunk A:
  // the target resolves through pv_screen_find, so an edge to
  // DetailActivity marks the Detail screen reachable.
  do {
    changed = 0;
    for (i = 0; i < p->nscreens; i++) {
      pv_screen *screen = p->screens[i];
      size_t e = 0;
      if (!screen->reachable) {
        continue;
      }
      for (e = 0; e < screen->nedges; e++) {
        pv_edge *edge = &screen->edges[e];
        pv_screen *target = NULL;
        if (edge->unknown || edge->target == NULL) {
          continue;
        }
        target = pv_screen_find(p, edge->target);
        if (target != NULL && !target->reachable) {
          target->reachable = 1;
          changed = 1;
        }
      }
    }
  } while (changed);
  // Missing-target edges: one error per edge. Chunk A: a target is
  // known when it matches a screen (normalized, suffix-aware), a
  // nav-graph destination (now a node, so matched above), or a
  // manifest activity. Anything else is CCB-P1005.
  for (i = 0; i < p->nscreens; i++) {
    pv_screen *screen = p->screens[i];
    size_t e = 0;
    for (e = 0; e < screen->nedges; e++) {
      pv_edge *edge = &screen->edges[e];
      ccb_str msg;
      if (edge->unknown || edge->target == NULL) {
        continue;
      }
      if (pv_screen_find(p, edge->target) != NULL ||
          pv_manifest_has(p, edge->target)) {
        continue;
      }
      ccb_str_init(&msg);
      ccb_str_append(&msg, "navigation to unknown screen '");
      ccb_str_append(&msg, edge->target);
      ccb_str_append(&msg, "'");
      pv_diag(&p->diags, "CCB-P1005", msg.buf != NULL ? msg.buf : "",
        NULL, -1);
      ccb_str_free(&msg);
    }
  }
  // Duplicate screen names: the 2nd and later occurrences are errors.
  for (i = 0; i < p->nscreens; i++) {
    int dup = 0;
    ccb_str msg;
    if (p->screens[i]->name == NULL) {
      continue;
    }
    for (j = 0; j < i; j++) {
      if (p->screens[j]->name != NULL &&
          strcmp(p->screens[j]->name, p->screens[i]->name) == 0) {
        dup = 1;
        break;
      }
    }
    if (!dup) {
      continue;
    }
    ccb_str_init(&msg);
    ccb_str_append(&msg, "duplicate screen '");
    ccb_str_append(&msg, p->screens[i]->name);
    ccb_str_append(&msg, "'");
    pv_diag(&p->diags, "CCB-P1008", msg.buf != NULL ? msg.buf : "",
      NULL, -1);
    ccb_str_free(&msg);
  }
  // Unreachable non-entry screens, and empty screens.
  for (i = 0; i < p->nscreens; i++) {
    pv_screen *screen = p->screens[i];
    ccb_str msg;
    if (!screen->reachable && i != entry_idx) {
      ccb_str_init(&msg);
      ccb_str_append(&msg, "unreachable screen '");
      ccb_str_append(&msg, screen->name != NULL ? screen->name : "?");
      ccb_str_append(&msg, "'");
      pv_diag(&p->diags, "CCB-P1006", msg.buf != NULL ? msg.buf : "",
        NULL, -1);
      ccb_str_free(&msg);
    }
    if (!screen->graph_only &&
        (screen->root == NULL || screen->root->nchildren == 0)) {
      ccb_str_init(&msg);
      ccb_str_append(&msg, "empty screen '");
      ccb_str_append(&msg, screen->name != NULL ? screen->name : "?");
      ccb_str_append(&msg, "'");
      pv_diag(&p->diags, "CCB-P1007", msg.buf != NULL ? msg.buf : "",
        NULL, -1);
      ccb_str_free(&msg);
    }
  }
  // Error count: missing-target and duplicate-screen are errors; under
  // strict, the subset warnings CCB-P1001 and CCB-P1014 count as errors
  // too. Everything else in the list is a warning.
  for (i = 0; i < p->diags.count; i++) {
    const char *code = p->diags.items[i].code;
    if (code == NULL) {
      continue;
    }
    if (strcmp(code, "CCB-P1005") == 0 ||
        strcmp(code, "CCB-P1008") == 0) {
      errors++;
    } else if (p->strict && (strcmp(code, "CCB-P1001") == 0 ||
        strcmp(code, "CCB-P1014") == 0)) {
      errors++;
    }
  }
  return errors;
}
