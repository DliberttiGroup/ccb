// ccb_preview_flow_tree: logic-tree rendering.
// Split from ccb_preview_flow.c.

#include "ccb_common.h"
#include "ccb_preview.h"
#include <ctype.h>
#include "ccb_preview_flow_internal.h"

// ---------- logic tree ----------

static void pv_render_branches(ccb_str *out, pv_box *box) {
  if (box->is_branch) {
    int has_trace_pick = 0;
    size_t i = 0;
    // Chunk 2: a trace pick greys the entries it did not take —
    // in plain text, "greyed" is the [not taken] mark. The picked
    // entry keeps its [trace] mark and every entry still renders,
    // so the pick never hides structure (same rule as chunk 1).
    for (i = 0; i < box->nchildren; i++) {
      if (box->children[i]->trace_picked) {
        has_trace_pick = 1;
        break;
      }
    }
    ccb_str_append(out, "  branches: ");
    ccb_str_append(out, box->label != NULL ? box->label : "");
    ccb_str_append(out, " [");
    int first = 1;
    for (size_t i2 = 0; i2 < box->nchildren; i2++) {
      pv_box *child = box->children[i2];
      if (child->label == NULL) {
        continue;
      }
      if (!first) {
        ccb_str_append(out, ", ");
      }
      ccb_str_append(out, child->label);
      // Chunk 1: the scenario-picked entry is marked; the other
      // entries still render, so the pick never hides structure.
      if (child->scenario_picked) {
        ccb_str_append(out, " [scenario]");
      }
      if (child->trace_picked) {
        ccb_str_append(out, " [trace]");
      } else if (has_trace_pick) {
        ccb_str_append(out, " [not taken]");
      }
      first = 0;
    }
    ccb_str_append(out, "]\n");
  }
  for (size_t i = 0; i < box->nchildren; i++) {
    pv_render_branches(out, box->children[i]);
  }
}

void pv_render_edge_args(ccb_str *out, const pv_edge *e) {
  size_t k = 0;
  if (out == NULL || e == NULL || e->nargs == 0) {
    return;
  }
  ccb_str_append(out, "(");
  for (k = 0; k < e->nargs; k++) {
    if (k > 0) {
      ccb_str_append(out, ", ");
    }
    if (e->args[k] != NULL) {
      if (e->args_named != NULL && e->args_named[k]) {
        // Named argument: shown as written, never evaluated.
        ccb_str_append(out, e->args[k]);
      } else {
        ccb_str_append(out, "\"");
        ccb_str_append(out, e->args[k]);
        ccb_str_append(out, "\"");
      }
      // Chunk 1: a scenario-filled argument keeps its literal text
      // but carries its provenance, so a filled value is never
      // mistaken for a static one. Chunk 2: same for trace-filled
      // arguments; a slot carries at most one source's mark.
      if (e->args_scenario != NULL && e->args_scenario[k]) {
        ccb_str_append(out, " [scenario]");
      }
      if (e->args_trace != NULL && e->args_trace[k]) {
        ccb_str_append(out, " [trace]");
      }
    } else {
      ccb_str_append(out, "unknown");
    }
  }
  ccb_str_append(out, ")");
}

// Chunk 1: per-screen scenario effects in the tree. List rows and
// field content are values the wireframe shows as content, so the
// tree marks their provenance here: one line per affected list or
// field, in tree-walk order. Branch picks are marked inline in the
// branches line and filled args inline in the edge lines, so this
// covers only the two value classes that have no tree line of their
// own. Nothing is emitted for screens a scenario does not touch.
// One list box's scenario rows: `scenario: list "label" = [...]`.
// Nothing renders when the list has no scenario rows.
static void pv_render_scenario_list(ccb_str *out, pv_box *box) {
  size_t nrows = 0;
  for (size_t i = 0; i < box->nchildren; i++) {
    if (box->children[i]->from_scenario) {
      nrows++;
    }
  }
  if (nrows == 0) {
    return;
  }
  int first = 1;
  ccb_str_append(out, "  scenario: list \"");
  ccb_str_append(out, box->label != NULL ? box->label : "");
  ccb_str_append(out, "\" = [");
  for (size_t i = 0; i < box->nchildren; i++) {
    if (!box->children[i]->from_scenario) {
      continue;
    }
    if (!first) {
      ccb_str_append(out, ", ");
    }
    ccb_str_append(out, "\"");
    ccb_str_append(out, box->children[i]->label != NULL ?
      box->children[i]->label : "");
    ccb_str_append(out, "\"");
    first = 0;
  }
  ccb_str_append(out, "] [scenario]\n");
}

static void pv_render_scenario_lines(ccb_str *out, pv_box *box) {
  size_t i = 0;
  if (out == NULL || box == NULL) {
    return;
  }
  if (box->type != NULL && strcmp(box->type, "list") == 0) {
    pv_render_scenario_list(out, box);
  } else if (box->type != NULL && strcmp(box->type, "textfield") == 0 &&
      box->scenario_text != NULL) {
    ccb_str_append(out, "  scenario: field \"");
    ccb_str_append(out, box->label != NULL ? box->label : "");
    ccb_str_append(out, "\" = \"");
    ccb_str_append(out, box->scenario_text);
    ccb_str_append(out, "\" [scenario]\n");
  }
  for (i = 0; i < box->nchildren; i++) {
    pv_render_scenario_lines(out, box->children[i]);
  }
}

void pv_render_logic_tree(ccb_str *out, pv_project *p) {
  if (out == NULL || p == NULL || p->nscreens == 0) {
    return;
  }
  pv_screen *entry = p->screens[0];
  for (size_t i = 0; i < p->nscreens; i++) {
    if (strcmp(p->screens[i]->name, "Home") == 0) {
      entry = p->screens[i];
      break;
    }
  }
  for (size_t i = 0; i < p->nscreens; i++) {
    pv_screen *screen = p->screens[i];
    ccb_str_append(out, screen->name);
    // Chunk A: graph-only nodes carry unknown layout, marked so
    // the tree never presents one as a sketched screen.
    if (screen->graph_only) {
      ccb_str_append(out, " [graph]");
    }
    if (screen == entry) {
      ccb_str_append(out, " (entry)");
    }
    ccb_str_append_c(out, '\n');
    if (screen->root != NULL) {
      pv_render_branches(out, screen->root);
    }
    for (size_t j = 0; j < screen->nedges; j++) {
      pv_edge *e = &screen->edges[j];
      ccb_str_append(out, "  -> ");
      if (e->target != NULL) {
        ccb_str_append(out, e->target);
      } else {
        ccb_str_append(out, "unknown");
      }
      pv_render_edge_args(out, e);
      ccb_str_append(out, " via \"");
      ccb_str_append(out, e->from != NULL ? e->from : "");
      ccb_str_append(out, "\"");
      if (e->branch != NULL) {
        ccb_str_append(out, " [");
        ccb_str_append(out, e->branch);
        ccb_str_append(out, "]");
      }
      // Chunk 2: the taken path is highlighted with the trace mark.
      if (e->trace_taken) {
        ccb_str_append(out, " [trace]");
      }
      ccb_str_append(out, "\n");
    }
    if (screen->root != NULL) {
      pv_render_scenario_lines(out, screen->root);
    }
  }
}
