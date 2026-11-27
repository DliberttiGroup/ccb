// ccb_preview_assemble: project assembly (scan + merge + applicable).
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

static void pv_diag_replay(ccb_diag_list *dl,
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

// Scan the nav-graph and layout XML inputs (see ccb_preview.h).
// Code motion from pv_project_load_scenario: the scan block
// moved verbatim, with dl renamed to diags and the pending-edge
// declarations left in the caller (pending belongs to flow
// parsing, not to the scan).
void pv_preview_scan_xml(const ccb_config *cfg, ccb_arena *arena,
    char ***graph_keys_io, char ***graph_vals_io, size_t *ngraph_io,
    char ***onclick_methods_io, char ***onclick_labels_io,
    size_t *nonclick_io, ccb_diag_list *diags) {
  char **graph_keys = NULL;
  char **graph_vals = NULL;
  size_t ngraph = 0;
  char **onclick_methods = NULL;
  char **onclick_labels = NULL;
  size_t nonclick = 0;
    char *nav_path = pv_preview_nav_path(cfg);
    char **nav_files = NULL;
    size_t nnav = 0;
    char *layout_path = pv_preview_layout_path(cfg);
    char **layout_files = NULL;
    size_t nlayout = 0;
    char **lay_methods = NULL;
    char **lay_ids = NULL;
    char **lay_texts = NULL;
    size_t nlay = 0;
    char **warned_ids = NULL;
    size_t nwarned_ids = 0;
    size_t li = 0;
    size_t pi = 0;
    pv_xml_inputs(nav_path, &nav_files, &nnav);
    free(nav_path);
    for (li = 0; li < nnav; li++) {
      char *text = NULL;
      size_t len = 0;
      pv_graph_ctx gctx;
      if (ccb_read_file(nav_files[li], &text, &len) != CCB_OK) {
        continue;
      }
      gctx.arena = arena;
      gctx.package = cfg != NULL ? cfg->package : NULL;
      gctx.keys_io = &graph_keys;
      gctx.vals_io = &graph_vals;
      gctx.n_io = &ngraph;
      pv_xml_scan_tags(text, len, pv_graph_tag, &gctx);
      free(text);
    }
    ccb_free_str_array(nav_files, nnav);
    pv_xml_inputs(layout_path, &layout_files, &nlayout);
    free(layout_path);
    for (li = 0; li < nlayout; li++) {
      char *text = NULL;
      size_t len = 0;
      pv_layout_ctx lctx;
      if (ccb_read_file(layout_files[li], &text, &len) != CCB_OK) {
        continue;
      }
      lctx.methods_io = &lay_methods;
      lctx.ids_io = &lay_ids;
      lctx.texts_io = &lay_texts;
      lctx.n_io = &nlay;
      pv_xml_scan_tags(text, len, pv_layout_tag, &lctx);
      free(text);
    }
    ccb_free_str_array(layout_files, nlayout);
    for (pi = 0; pi < nlay; pi++) {
      const char *label = NULL;
      size_t w = 0;
      int warned = 0;
      const char *id_key = lay_ids[pi] != NULL ? lay_ids[pi] : "";
      for (w = 0; w < nwarned_ids; w++) {
        if (strcmp(warned_ids[w], id_key) == 0 &&
            strcmp(warned_ids[w] + strlen(id_key) + 1,
              lay_methods[pi]) == 0) {
          warned = 1;
          break;
        }
      }
      label = lay_ids[pi] != NULL ?
        pv_binding_lookup(cfg, lay_ids[pi]) : NULL;
      // Phase 2 fallback: the layout's own android:text names the
      // button (kept raw here — an "@string/..." reference resolves
      // with the other component labels later in the load). Only a
      // button with neither a binding nor a text warns P1019.
      if (label == NULL) {
        label = lay_texts[pi];
      }
      if (label == NULL) {
        ccb_str msg;
        if (!warned) {
          // Dedup key: "id\0method" in one malloc'd string.
          size_t kl = strlen(id_key) + 1 + strlen(lay_methods[pi]) + 1;
          char *key = ccb_xmalloc(kl);
          memcpy(key, id_key, strlen(id_key) + 1);
          memcpy(key + strlen(id_key) + 1, lay_methods[pi],
            strlen(lay_methods[pi]) + 1);
          warned_ids = ccb_xrealloc(warned_ids,
            (nwarned_ids + 1) * sizeof(*warned_ids));
          warned_ids[nwarned_ids] = key;
          nwarned_ids++;
        }
        if (!warned) {
          ccb_str_init(&msg);
          ccb_str_append(&msg,
            "handler found, button not bound: layout button '");
          ccb_str_append(&msg, id_key);
          ccb_str_append(&msg, "' (android:onClick '");
          ccb_str_append(&msg, lay_methods[pi]);
          ccb_str_append(&msg,
            "') has no [preview.bindings] entry; no edge recorded");
          pv_diag(diags, "CCB-P1019", msg.buf != NULL ? msg.buf : "",
            NULL, -1);
          ccb_str_free(&msg);
        }
        continue;
      }
      {
        // Identical (method, label) pairs (e.g. a button declared
        // in two layout variants) collapse to one; distinct labels
        // for one method are all kept.
        size_t q = 0;
        int dup = 0;
        for (q = 0; q < nonclick; q++) {
          if (strcmp(onclick_methods[q], lay_methods[pi]) == 0 &&
              strcmp(onclick_labels[q], label) == 0) {
            dup = 1;
            break;
          }
        }
        if (!dup) {
          pv_pair_add(arena, &onclick_methods, &onclick_labels,
            &nonclick, ccb_arena_strdup(arena, lay_methods[pi]),
            ccb_arena_strdup(arena, label), 0);
        }
      }
    }
    for (pi = 0; pi < nlay; pi++) {
      free(lay_methods[pi]);
      free(lay_ids[pi]);
      free(lay_texts[pi]);
    }
    free(lay_methods);
    free(lay_ids);
    free(lay_texts);
    for (pi = 0; pi < nwarned_ids; pi++) {
      free(warned_ids[pi]);
    }
    free(warned_ids);
  if (graph_keys_io != NULL) {
    *graph_keys_io = graph_keys;
  }
  if (graph_vals_io != NULL) {
    *graph_vals_io = graph_vals;
  }
  if (ngraph_io != NULL) {
    *ngraph_io = ngraph;
  }
  if (onclick_methods_io != NULL) {
    *onclick_methods_io = onclick_methods;
  }
  if (onclick_labels_io != NULL) {
    *onclick_labels_io = onclick_labels;
  }
  if (nonclick_io != NULL) {
    *nonclick_io = nonclick;
  }
}

// Assemble a project from pre-parsed inputs (see ccb_preview.h).
// Distribution, reconciliation, resources, scenario, diag adopt,
// and layout moved verbatim from pv_project_load_scenario; only
// the CSS loop is new, merging pre-parsed per-file entries
// instead of parsing from disk.
void pv_project_assemble(pv_project *p, pv_screen **screens,
    size_t nscreens, pv_pending_edge **pending, size_t npending,
    pv_css_entry *css_entries, size_t ncss,
    const char *resources_dir, const ccb_scenario *scenario,
    ccb_diag_list *diags, char **graph_vals, size_t ngraph,
    char **manifest_activities, size_t nmanifest_activities,
    const char *package) {
  ccb_arena *arena = NULL;
  size_t i = 0;
  size_t pi = 0;
  pv_screen **all = NULL;
  size_t nall = 0;
  size_t g = 0;
  if (p == NULL) {
    return;
  }
  arena = p->arena;
  // Chunk A: record the package + manifest activities for
  // pv_project_diagnose (P1005 scope) before anything needs them.
  p->package = package != NULL ? ccb_arena_strdup(arena, package) : NULL;
  if (nmanifest_activities > 0) {
    p->manifest_activities = ccb_arena_alloc(arena,
      nmanifest_activities * sizeof(*p->manifest_activities));
    for (i = 0; i < nmanifest_activities; i++) {
      p->manifest_activities[i] =
        ccb_arena_strdup(arena, manifest_activities[i]);
    }
  } else {
    p->manifest_activities = NULL;
  }
  p->nmanifest_activities = nmanifest_activities;
  // Chunk A: one node per nav-graph destination with no matching
  // flow screen, appended after the flow screens so the entry rule
  // (Home, else first screen) never changes for a project that has
  // flow screens. Matching is pv_names_match, so a DetailActivity
  // destination never duplicates a Detail screen, and two graph
  // ids naming the same destination yield one node. Nodes carry no
  // boxes, so distribution (label match), CSS scoping by label,
  // scenario row/field application (root-NULL guards), and layout
  // (root-NULL early return) all skip them naturally.
  all = ccb_arena_alloc(arena,
    (nscreens + ngraph == 0 ? 1 : nscreens + ngraph) * sizeof(*all));
  for (i = 0; i < nscreens; i++) {
    all[nall++] = screens[i];
  }
  for (g = 0; g < ngraph; g++) {
    pv_screen *node = NULL;
    int found = 0;
    if (graph_vals == NULL || graph_vals[g] == NULL) {
      continue;
    }
    for (i = 0; i < nall; i++) {
      if (all[i]->name != NULL &&
          pv_names_match(package, all[i]->name, graph_vals[g])) {
        found = 1;
        break;
      }
    }
    if (found) {
      continue;
    }
    node = ccb_arena_alloc(arena, sizeof(*node));
    memset(node, 0, sizeof(*node));
    node->name = ccb_arena_strdup(arena, graph_vals[g]);
    node->graph_only = 1;
    all[nall++] = node;
  }
  screens = all;
  nscreens = nall;
  // The screens array is a fresh arena array of borrowed screen
  // pointers; the screens themselves stay caller-owned (the
  // load path parses them into this arena, the LSP server
  // deep-copies its staging fragments into it), except the
  // graph-only nodes above, which are arena-owned here.
  p->screens = ccb_arena_alloc(arena,
    (nscreens == 0 ? 1 : nscreens) * sizeof(*p->screens));
  for (i = 0; i < nscreens; i++) {
    p->screens[i] = screens[i];
  }
  p->nscreens = nscreens;
    // Slice 3 distribution: an onClick edge attaches to every screen
    // whose component tree contains a BUTTON with the bound label
    // (the button the layout declares). A label in no screen is
    // CCB-P1021 (warning, no edge): the edge has nowhere to attach,
    // and attaching it to an unrelated screen would be a guess. Distribution runs before
    // reconciliation, so declared edges still beat these derived
    // edges exactly as they beat Slice 1 derived edges. Note the
    // labels compared here are pre-@string-resolution, matching the
    // Slice 1 reconciliation comparison.
    for (pi = 0; pi < npending; pi++) {
      pv_pending_edge *pe = pending[pi];
      int attached = 0;
      size_t si = 0;
      for (si = 0; si < p->nscreens; si++) {
        if (pv_tree_has_label(p->screens[si]->root, pe->from)) {
          pv_screen_edge_add(arena, p->screens[si], pe->from,
            pe->target, pe->unknown, pe->args, pe->nargs,
            pe->args_named, pe->branch);
          attached = 1;
        }
      }
      if (!attached) {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg,
          "onClick handler '");
        ccb_str_append(&msg, pe->method);
        ccb_str_append(&msg, "' for button '");
        ccb_str_append(&msg, pe->from);
        ccb_str_append(&msg,
          "' matches no component in any screen; no edge recorded");
        pv_diag(diags, "CCB-P1021", msg.buf != NULL ? msg.buf : "",
          NULL, -1);
        ccb_str_free(&msg);
      }
    }
  // Declared-beats-derived reconciliation, after all flow files.
  pv_reconcile_derived(p, diags);
  // CSS: common.css first, then per-screen files in sorted order.
  // Entries carry each file's own parsed rules; merging here (not
  // at parse time) is what lets the LSP server reuse per-file
  // cached parses. The merge function reproduces pv_css_parse's
  // cross-file semantics, including the per-screen split, so the
  // merged rules are identical to a direct sequential parse.
  {
    size_t pass = 0;
    size_t ci = 0;
    for (pass = 0; pass < 2; pass++) {
      for (ci = 0; ci < ncss; ci++) {
        const pv_css_entry *entry = &css_entries[ci];
        const char *scope = NULL;
        size_t ri = 0;
        if (pass == 0 && !pv_is_common_css(entry->path)) {
          continue;
        }
        if (pass == 1 && pv_is_common_css(entry->path)) {
          continue;
        }
        if (pass == 1) {
          scope = pv_css_screen(p, entry->path);
          if (scope == NULL) {
            ccb_str msg;
            ccb_str_init(&msg);
            ccb_str_append(&msg, "CSS file ignored: no screen named '");
            ccb_str_append_n(&msg, pv_basename(entry->path),
              strlen(pv_basename(entry->path)) > 4 ?
              strlen(pv_basename(entry->path)) - 4 : 0);
            ccb_str_append(&msg, "'");
            pv_diag(diags, "CCB-P1015",
              msg.buf != NULL ? msg.buf : "", entry->path, -1);
            ccb_str_free(&msg);
            continue;
          }
        }
        // Replay this file's parse diagnostics in load order,
        // then merge its rules.
        if (entry->diags != NULL) {
          pv_diag_replay(diags, entry->diags);
        }
        for (ri = 0; ri < entry->nrules; ri++) {
          pv_css_rule_merge(arena, &p->rules, &p->nrules,
            &entry->rules[ri], entry->path, scope);
        }
      }
    }
  }
  // @string/@color resolution, after flow parsing and CSS parsing
  // but before layout: labels resolve before clip widths are
  // computed, and CSS colors resolve before rules are used.
  if (resources_dir != NULL) {
    pv_res_table strings;
    pv_res_table colors;
    char *spath = NULL;
    char *cpath = NULL;
    size_t si = 0;
    memset(&strings, 0, sizeof(strings));
    memset(&colors, 0, sizeof(colors));
    spath = ccb_path_join(resources_dir, "strings.xml");
    cpath = ccb_path_join(resources_dir, "colors.xml");
    pv_res_parse_file(arena, spath, 0, diags, &strings);
    pv_res_parse_file(arena, cpath, 1, diags, &colors);
    free(spath);
    free(cpath);
    for (si = 0; si < p->nscreens; si++) {
      if (p->screens[si]->root != NULL) {
        pv_res_resolve_labels(arena, p->screens[si]->root, &strings, diags);
      }
    }
    pv_res_resolve_colors(arena, p, &colors, diags);
    free(strings.items);
    free(colors.items);
  }
  // Chunk 1: scenario merge, after resolution (scenario keys match
  // the labels as rendered) and before layout (scenario list rows
  // size the list like static rows do). Warnings go to dl, which is
  // adopted below along with the rest.
  if (scenario != NULL) {
    pv_scenario_apply(p, scenario, diags);
  }
  // Adopt the diag list BEFORE layout: pv_layout_screen pushes its
  // diagnostics directly into p->diags, so the project must own
  // the list by then (adopting it after layout would discard them).
  if (diags != NULL) {
    p->diags = *diags;
    memset(diags, 0, sizeof(*diags));
  } else {
    ccb_diag_list_init(&p->diags);
  }
  // Adopt the diag list BEFORE layout: pv_layout_screen pushes its
  // diagnostics directly into p->diags, so the project must own the
  // list by then (adopting it after layout would discard them).
  for (i = 0; i < p->nscreens; i++) {
    pv_layout_screen(p, p->screens[i]);
  }
  // The project's screen/rule arrays live in the arena; the diag list
  // owns malloc'd strings and is freed by pv_project_free.
  p->capscreens = p->nscreens;
  p->caprules = p->nrules;
}

// Phase 2: preview applies to a config with a [preview] section
// and at least one of styles, flow, or real Kotlin sources. A
// [preview] section with none of the three is "preview not
// applicable", exactly as before Phase 2.
int pv_preview_applicable(const ccb_config *cfg) {
  char *kdir = NULL;
  if (cfg == NULL || !cfg->has_preview) {
    return 0;
  }
  if (cfg->preview_styles != NULL || cfg->preview_flow != NULL) {
    return 1;
  }
  kdir = pv_real_kotlin_dir(cfg);
  if (kdir != NULL) {
    free(kdir);
    return 1;
  }
  return 0;
}

// Phase 2 merge: real-code screens merge into the sketch screens
// (or stand alone when there is no flow/). A real screen merges
// into a sketch screen of the same name (pv_names_match), or a
// launcher real screen merges into a sketch screen named "Home"
// (the launcher IS the entry screen; sketches name it Home while
// real Activities are MainActivity). Edges append via
// pv_screen_edge_add (silent dedup on the same button + target +
// args + branch); a conflicting destination for the same button
// warns CCB-P1020 and the real-code edge stands (PROJECT-FIRST:
// real code over sketch), applied in place so the sketch edge list
// keeps its order. A merged screen keeps the sketch root — the
// real root is synthetic Buttons (Chunk 1), so the sketch wireframe
// wins; a sketch screen with no root at all adopts the real one.
// Real screens that merge nowhere are appended as-is, and the
// real parse's pending onClick edges are appended to the caller's
// pending list, so assemble distributes them by label.
void pv_merge_real_screens(ccb_arena *arena,
    pv_screen ***screens_io, size_t *nscreens_io,
    pv_pending_edge ***pending_io, size_t *npending_io,
    pv_screen **real_screens, size_t nreal,
    pv_pending_edge **real_pending, size_t nreal_pending,
    const char *launcher, const char *package,
    ccb_diag_list *diags) {
  size_t r = 0;
  size_t pi = 0;
  for (r = 0; r < nreal; r++) {
    pv_screen *real = real_screens[r];
    pv_screen *target = NULL;
    size_t i = 0;
    size_t e = 0;
    for (i = 0; i < *nscreens_io; i++) {
      if (pv_names_match(package, (*screens_io)[i]->name,
          real->name)) {
        target = (*screens_io)[i];
        break;
      }
    }
    if (target == NULL && launcher != NULL &&
        pv_names_match(package, real->name, launcher)) {
      for (i = 0; i < *nscreens_io; i++) {
        if (strcmp((*screens_io)[i]->name, "Home") == 0) {
          target = (*screens_io)[i];
          break;
        }
      }
    }
    if (target == NULL) {
      // Real-only screen: keep it (with its synthetic Button
      // roots, for now — Chunk 2 replaces those with a layout
      // wireframe). The screens array is arena-grown (see
      // pv_screen_add), so append the same way, never xrealloc.
      pv_screen **next = ccb_arena_alloc(arena,
        (*nscreens_io + 1) * sizeof(*next));
      size_t k = 0;
      for (k = 0; k < *nscreens_io; k++) {
        next[k] = (*screens_io)[k];
      }
      next[*nscreens_io] = real;
      *screens_io = next;
      (*nscreens_io)++;
      continue;
    }
    for (e = 0; e < real->nedges; e++) {
      pv_edge *edge = &real->edges[e];
      size_t d = 0;
      int handled = 0;
      for (d = 0; d < target->nedges; d++) {
        pv_edge *old = &target->edges[d];
        int same_branch = (old->branch == NULL && edge->branch == NULL) ||
          (old->branch != NULL && edge->branch != NULL &&
           strcmp(old->branch, edge->branch) == 0);
        if (old->from == NULL || edge->from == NULL ||
            strcmp(old->from, edge->from) != 0 || !same_branch) {
          continue;
        }
        if (old->target != NULL && edge->target != NULL &&
            pv_names_match(package, old->target, edge->target)) {
          // Same destination (possibly spelled with/without the
          // Activity suffix, e.g. "Form" vs "FormActivity"): silent
          // dedup, keeping the sketch edge — it may carry args
          // the real call does not.
          handled = 1;
          break;
        }
        if (old->target != NULL && edge->target != NULL) {
          ccb_str msg;
          ccb_str_init(&msg);
          ccb_str_append(&msg,
            "conflicting navigation: button '");
          ccb_str_append(&msg, edge->from);
          ccb_str_append(&msg, "' on screen '");
          ccb_str_append(&msg, target->name);
          ccb_str_append(&msg, "' points to '");
          ccb_str_append(&msg, old->target);
          ccb_str_append(&msg, "' in the flow sketch and to '");
          ccb_str_append(&msg, edge->target);
          ccb_str_append(&msg, "' in the real code; the real-code "
            "destination is used");
          pv_diag(diags, "CCB-P1020", msg.buf != NULL ? msg.buf : "",
            NULL, -1);
          ccb_str_free(&msg);
          old->target = edge->target;
          old->unknown = edge->unknown;
          old->args = edge->args;
          old->nargs = edge->nargs;
          old->args_named = edge->args_named;
          old->branch = edge->branch;
          old->derived = 1;
          handled = 1;
          break;
        }
        // One side unknown: the known destination stands; two
        // unknowns keep the first edge, silently.
        if (edge->target != NULL) {
          old->target = edge->target;
          old->unknown = edge->unknown;
          old->args = edge->args;
          old->nargs = edge->nargs;
          old->args_named = edge->args_named;
          old->branch = edge->branch;
          old->derived = 1;
        }
        handled = 1;
        break;
      }
      if (!handled) {
        pv_screen_edge_add(arena, target, edge->from, edge->target,
          edge->unknown, edge->args, edge->nargs, edge->args_named,
          edge->branch);
      }
    }
    if (target->root == NULL && real->root != NULL) {
      target->root = real->root;
    }
  }
  if (nreal_pending > 0) {
    // Same arena rule as the screens array (see pv_pending_add).
    pv_pending_edge **next = ccb_arena_alloc(arena,
      (*npending_io + nreal_pending) * sizeof(*next));
    for (pi = 0; pi < *npending_io; pi++) {
      next[pi] = (*pending_io)[pi];
    }
    for (pi = 0; pi < nreal_pending; pi++) {
      next[*npending_io + pi] = real_pending[pi];
    }
    *pending_io = next;
    *npending_io += nreal_pending;
  }
}
