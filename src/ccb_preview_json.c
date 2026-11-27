// ccb_preview_json: project -> JSON rendering.
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

// ---------- JSON ----------

static void pv_json_str(ccb_str *out, const char *s) {
  if (s == NULL) {
    ccb_str_append(out, "null");
  } else {
    ccb_json_escape(out, s);
  }
}

static void pv_json_box(ccb_str *out, pv_box *box, long interior_cols,
    long phone_dp_width) {
  size_t i = 0;
  ccb_str_append(out, "{\"type\":");
  if (box->type != NULL) {
    ccb_json_escape(out, box->type);
  } else {
    ccb_str_append(out, "\"\"");
  }
  ccb_str_append(out, ",\"label\":");
  pv_json_str(out, box != NULL ? box->label : NULL);
  ccb_str_appendf(out, ",\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld",
    box != NULL ? box->x : 0, box != NULL ? box->y : 0,
    box != NULL ? box->w : 0, box != NULL ? box->h : 0);
  // F2: DP size alongside cells, from the project's scale.
  ccb_str_appendf(out, ",\"w_dp\":%ld,\"h_dp\":%ld",
    pv_cols_to_dp(box != NULL ? box->w : 0, interior_cols,
      phone_dp_width),
    pv_rows_to_dp(box != NULL ? box->h : 0));
  ccb_str_append(out, ",\"children\":[");
  if (box != NULL) {
    for (i = 0; i < box->nchildren; i++) {
      if (i > 0) {
        ccb_str_append(out, ",");
      }
      pv_json_box(out, box->children[i], interior_cols, phone_dp_width);
    }
  }
  ccb_str_append(out, "]");
  // Chunk 1: scenario provenance, only when a scenario supplied
  // something for this box — static boxes stay unmarked.
  if (box != NULL && box->from_scenario) {
    ccb_str_append(out, ",\"provenance\":\"scenario\"");
  }
  if (box != NULL && box->scenario_text != NULL) {
    ccb_str_append(out, ",\"value\":");
    pv_json_str(out, box->scenario_text);
    ccb_str_append(out, ",\"value_provenance\":\"scenario\"");
  }
  ccb_str_append(out, "}");
}

static int pv_box_is_branch(pv_box *box) {
  return box != NULL && (box->is_branch ||
    (box->type != NULL && strcmp(box->type, "branch") == 0));
}

static void pv_json_branches(ccb_str *out, pv_box *box, int *first) {
  size_t i = 0;
  if (box == NULL) {
    return;
  }
  if (pv_box_is_branch(box)) {
    if (!*first) {
      ccb_str_append(out, ",");
    }
    *first = 0;
    ccb_str_append(out, "{\"label\":");
    if (box->label != NULL) {
      ccb_json_escape(out, box->label);
    } else {
      ccb_str_append(out, "\"\"");
    }
    ccb_str_append(out, ",\"entries\":[");
    for (i = 0; i < box->nchildren; i++) {
      pv_box *child = box->children[i];
      if (i > 0) {
        ccb_str_append(out, ",");
      }
      if (child->label != NULL) {
        ccb_json_escape(out, child->label);
      } else {
        ccb_str_append(out, "\"\"");
      }
    }
    // Chunk 1: the scenario-picked entry, if any. Entries stay plain
    // strings; only a pick adds a field, so unpicked branches render
    // exactly as before. Chunk 2: a trace pick is the same field
    // with "trace" provenance, and it wins over a scenario pick of
    // a different entry: the trace is the runtime record.
    {
      const char *picked = NULL;
      const char *prov = NULL;
      for (i = 0; i < box->nchildren; i++) {
        if (box->children[i]->trace_picked) {
          picked = box->children[i]->label;
          prov = "trace";
          break;
        }
      }
      if (picked == NULL) {
        for (i = 0; i < box->nchildren; i++) {
          if (box->children[i]->scenario_picked) {
            picked = box->children[i]->label;
            prov = "scenario";
            break;
          }
        }
      }
      if (picked != NULL) {
        ccb_str_append(out, "],\"picked\":{\"entry\":");
        ccb_json_escape(out, picked);
        ccb_str_append(out, ",\"provenance\":\"");
        ccb_str_append(out, prov);
        ccb_str_append(out, "\"}");
      } else {
        ccb_str_append(out, "]");
      }
    }
    ccb_str_append(out, "}");
  }
  for (i = 0; i < box->nchildren; i++) {
    pv_json_branches(out, box->children[i], first);
  }
}

// One screen object, exactly as it appears in the "screens"
// array of pv_project_json_screens. first tracks comma
// separation across calls (*first = 1 for the first screen).
// Extracted from pv_project_json_screens so ccb/screenInfo
// can serve one screen without duplicating the writer.
static void pv_json_screen(ccb_str *out, pv_project *p,
    pv_screen *screen, int *first) {
  size_t j = 0;
  int first_branch = 1;
  pv_screen *entry = NULL;
  size_t i = 0;
  if (out == NULL || p == NULL || screen == NULL ||
      first == NULL) {
    return;
  }
  if (p->nscreens > 0) {
    entry = p->screens[0];
    for (i = 0; i < p->nscreens; i++) {
      if (p->screens[i]->name != NULL &&
          strcmp(p->screens[i]->name, "Home") == 0) {
        entry = p->screens[i];
        break;
      }
    }
  }
  if (!*first) {
    ccb_str_append(out, ",");
  }
  *first = 0;
    ccb_str_append(out, "{\"name\":");
    pv_json_str(out, screen->name);
    ccb_str_appendf(out, ",\"entry\":%s,\"reachable\":%s",
      screen == entry ? "true" : "false",
      screen->reachable ? "true" : "false");
    // Chunk A: only when set, so output without graph nodes is
    // byte-identical to before.
    if (screen->graph_only) {
      ccb_str_append(out, ",\"graph_only\":true");
    }
    ccb_str_append(out, ",\"boxes\":");
    if (screen->root != NULL) {
      pv_json_box(out, screen->root, p->interior_cols,
        p->phone_dp_width);
    } else {
      ccb_str_append(out, "null");
    }
    ccb_str_append(out, ",\"edges\":[");
    for (j = 0; j < screen->nedges; j++) {
      pv_edge *e = &screen->edges[j];
      size_t k = 0;
      if (j > 0) {
        ccb_str_append(out, ",");
      }
      ccb_str_append(out, "{\"from\":");
      pv_json_str(out, e->from);
      ccb_str_append(out, ",\"target\":");
      pv_json_str(out, e->target);
      ccb_str_appendf(out, ",\"unknown\":%s", e->unknown ? "true" : "false");
      ccb_str_append(out, ",\"args\":[");
      for (k = 0; k < e->nargs; k++) {
        if (k > 0) {
          ccb_str_append(out, ",");
        }
        pv_json_str(out, e->args[k]);
      }
      ccb_str_append(out, "],\"branch\":");
      pv_json_str(out, e->branch);
      // Chunk 2: the taken mark, only when an applied trace took
      // this edge.
      if (e->trace_taken) {
        ccb_str_append(out, ",\"taken\":true");
      }
      // Chunk 1: per-argument provenance, only when a scenario
      // filled at least one argument of this edge. Static entries
      // stay null (unmarked); filled ones read "scenario". Chunk 2:
      // trace fills read "trace"; a slot carries at most one.
      if (e->args_scenario != NULL || e->args_trace != NULL) {
        int has_filled = 0;
        for (k = 0; k < e->nargs; k++) {
          if ((e->args_scenario != NULL && e->args_scenario[k]) ||
              (e->args_trace != NULL && e->args_trace[k])) {
            has_filled = 1;
            break;
          }
        }
        if (has_filled) {
          ccb_str_append(out, ",\"args_provenance\":[");
          for (k = 0; k < e->nargs; k++) {
            if (k > 0) {
              ccb_str_append(out, ",");
            }
            if (e->args_trace != NULL && e->args_trace[k]) {
              ccb_str_append(out, "\"trace\"");
            } else if (e->args_scenario != NULL &&
                e->args_scenario[k]) {
              ccb_str_append(out, "\"scenario\"");
            } else {
              ccb_str_append(out, "null");
            }
          }
          ccb_str_append(out, "]");
        }
      }
      ccb_str_append(out, "}");
    }
    ccb_str_append(out, "],\"branches\":[");
    if (screen->root != NULL) {
      pv_json_branches(out, screen->root, &first_branch);
    }
    ccb_str_append(out, "]}");
}

void pv_project_json_screens(ccb_str *out, pv_project *p,
    pv_screen **screens, size_t nscreens) {
  size_t i = 0;
  int first = 1;
  if (out == NULL || p == NULL) {
    return;
  }
  ccb_str_append(out, "{\"event\":\"preview\",\"version\":2");
  // Chunk 1: the applied scenario's name, only when one was applied.
  if (p->scenario_name != NULL) {
    ccb_str_append(out, ",\"scenario\":");
    pv_json_str(out, p->scenario_name);
  }
  // Chunk 2: the applied trace's step count, only when one was
  // applied. Steps in the file, whether or not every step matched.
  if (p->trace_applied) {
    ccb_str_appendf(out, ",\"trace\":{\"steps\":%ld}",
      p->trace_step_count);
  }
  ccb_str_append(out, ",\"screens\":[");
  for (i = 0; i < nscreens; i++) {
    pv_json_screen(out, p, screens[i], &first);
  }
  ccb_str_append(out, "],\"diagnostics\":[");
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
    pv_json_str(out, d->file);
    ccb_str_appendf(out, ",\"line\":%ld}", d->line);
  }
  ccb_str_append(out, "]}");
}

void pv_project_json_screen(ccb_str *out, pv_project *p,
    pv_screen *screen) {
  int first = 1;
  pv_json_screen(out, p, screen, &first);
}

void pv_project_json(ccb_str *out, pv_project *p) {
  if (out == NULL || p == NULL) {
    return;
  }
  pv_project_json_screens(out, p, p->screens, p->nscreens);
}
