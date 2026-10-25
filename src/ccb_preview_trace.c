// ccb_preview_trace: Chunk 2 trace overlay. See the header for the
// contract: a trace is a data input, applied through chunk 1's
// shared merge primitives, marked with "trace" provenance.
#include "ccb_preview_trace.h"
#include "ccb_preview_scenario.h"
#include "ccb_json.h"

// ---------- parse helpers ----------

static char *tr_err(const char *msg) {
  return ccb_xstrdup(msg != NULL ? msg : "invalid trace");
}

// Append one parsed step, copying its strings (the JSON tree is
// freed after parsing, so the trace must own its copies).
static int tr_step_add(pv_trace *t, const char *screen,
    const char *edge, const char *branch, const char **args,
    size_t nargs) {
  pv_trace_step *step = NULL;
  size_t k = 0;
  if (t->nsteps == t->capsteps) {
    size_t ncap = t->capsteps == 0 ? 8 : t->capsteps * 2;
    pv_trace_step *next = ccb_xrealloc(t->steps,
      ncap * sizeof(*next));
    if (next == NULL) {
      return 0;
    }
    t->steps = next;
    t->capsteps = ncap;
  }
  step = &t->steps[t->nsteps];
  step->screen = ccb_xstrdup(screen);
  step->edge = edge != NULL ? ccb_xstrdup(edge) : NULL;
  step->branch = branch != NULL ? ccb_xstrdup(branch) : NULL;
  step->args = NULL;
  step->nargs = 0;
  if (nargs > 0) {
    step->args = ccb_xmalloc(nargs * sizeof(*step->args));
    if (step->args == NULL) {
      free((void *)step->screen);
      free((void *)step->edge);
      free((void *)step->branch);
      return 0;
    }
    for (k = 0; k < nargs; k++) {
      step->args[k] = args[k] != NULL ? ccb_xstrdup(args[k]) : NULL;
    }
    step->nargs = nargs;
  }
  t->nsteps++;
  return 1;
}

// One string-or-null field of a step object: returns 1 and sets *out
// (NULL for JSON null or a missing field), or 0 with *err set when
// the field has the wrong type.
static int tr_opt_str(const ccb_json_value *obj, const char *key,
    const char **out, char **err) {
  const ccb_json_value *v = ccb_json_obj_get(obj, key);
  if (v == NULL || v->type == CCB_JSON_NULL) {
    *out = NULL;
    return 1;
  }
  if (v->type != CCB_JSON_STR) {
    if (err != NULL) {
      *err = tr_err("trace step field must be a string or null");
    }
    return 0;
  }
  *out = v->str;
  return 1;
}

pv_trace *pv_trace_parse(const char *text, char **err) {
  ccb_json_value *doc = NULL;
  const ccb_json_value *steps = NULL;
  const ccb_json_value *version = NULL;
  pv_trace *t = NULL;
  size_t i = 0;
  char *parse_err = NULL;
  if (err != NULL) {
    *err = NULL;
  }
  if (text == NULL) {
    if (err != NULL) {
      *err = tr_err("trace is empty");
    }
    return NULL;
  }
  doc = ccb_json_parse(ccb_slice_from(text), &parse_err);
  if (doc == NULL) {
    if (err != NULL) {
      *err = parse_err != NULL ? parse_err : tr_err("invalid JSON");
    } else {
      free(parse_err);
    }
    return NULL;
  }
  if (doc->type != CCB_JSON_OBJ) {
    if (err != NULL) {
      *err = tr_err("trace must be a JSON object");
    }
    ccb_json_free(doc);
    return NULL;
  }
  version = ccb_json_obj_get(doc, "version");
  if (version != NULL) {
    if (version->type != CCB_JSON_NUM || version->num != 1) {
      if (err != NULL) {
        *err = tr_err("trace version must be 1");
      }
      ccb_json_free(doc);
      return NULL;
    }
  }
  steps = ccb_json_obj_get(doc, "steps");
  if (steps == NULL || steps->type != CCB_JSON_ARR) {
    if (err != NULL) {
      *err = tr_err("trace must carry a \"steps\" array");
    }
    ccb_json_free(doc);
    return NULL;
  }
  t = ccb_xmalloc(sizeof(*t));
  if (t == NULL) {
    ccb_json_free(doc);
    return NULL;
  }
  memset(t, 0, sizeof(*t));
  for (i = 0; i < steps->count; i++) {
    const ccb_json_value *s = steps->items[i];
    const ccb_json_value *screen = NULL;
    const ccb_json_value *args_v = NULL;
    const char *edge = NULL;
    const char *branch = NULL;
    const char **args = NULL;
    size_t nargs = 0;
    size_t k = 0;
    int ok = 1;
    if (s->type != CCB_JSON_OBJ) {
      if (err != NULL) {
        *err = tr_err("trace steps must be objects");
      }
      ok = 0;
    }
    if (ok) {
      screen = ccb_json_obj_get(s, "screen");
      if (screen == NULL || screen->type != CCB_JSON_STR) {
        if (err != NULL) {
          *err = tr_err("trace step must carry a \"screen\" string");
        }
        ok = 0;
      }
    }
    if (ok && !tr_opt_str(s, "edge", &edge, err)) {
      ok = 0;
    }
    if (ok && !tr_opt_str(s, "branch", &branch, err)) {
      ok = 0;
    }
    if (ok) {
      args_v = ccb_json_obj_get(s, "args");
      if (args_v != NULL && args_v->type != CCB_JSON_ARR) {
        if (err != NULL) {
          *err = tr_err("trace step \"args\" must be an array");
        }
        ok = 0;
      } else if (args_v != NULL) {
        nargs = args_v->count;
        if (nargs > 0) {
          args = ccb_xmalloc(nargs * sizeof(*args));
          if (args == NULL) {
            ok = 0;
          } else {
            for (k = 0; k < nargs; k++) {
              const ccb_json_value *a = args_v->items[k];
              if (a->type == CCB_JSON_NULL) {
                args[k] = NULL;
              } else if (a->type == CCB_JSON_STR) {
                args[k] = a->str;
              } else {
                if (err != NULL) {
                  *err = tr_err("trace step args must be "
                    "strings or null");
                }
                ok = 0;
                break;
              }
            }
          }
        }
      }
    }
    if (ok) {
      ok = tr_step_add(t, screen->str, edge, branch, args, nargs);
      if (!ok && err != NULL) {
        *err = tr_err("out of memory");
      }
    }
    free(args);
    if (!ok) {
      pv_trace_free(t);
      ccb_json_free(doc);
      return NULL;
    }
  }
  ccb_json_free(doc);
  return t;
}

void pv_trace_free(pv_trace *t) {
  size_t i = 0;
  size_t k = 0;
  if (t == NULL) {
    return;
  }
  for (i = 0; i < t->nsteps; i++) {
    free((void *)t->steps[i].screen);
    free((void *)t->steps[i].edge);
    free((void *)t->steps[i].branch);
    for (k = 0; k < t->steps[i].nargs; k++) {
      free((void *)t->steps[i].args[k]);
    }
    free(t->steps[i].args);
  }
  free(t->steps);
  free(t);
}

// ---------- write ----------

void pv_trace_write(ccb_str *out, const pv_trace_step *steps,
    size_t nsteps) {
  size_t i = 0;
  size_t k = 0;
  if (out == NULL) {
    return;
  }
  ccb_str_append(out, "{\"version\":1,\"steps\":[");
  for (i = 0; i < nsteps; i++) {
    const pv_trace_step *s = &steps[i];
    if (i > 0) {
      ccb_str_append(out, ",");
    }
    ccb_str_append(out, "{\"screen\":");
    if (s->screen != NULL) {
      ccb_json_escape(out, s->screen);
    } else {
      ccb_str_append(out, "null");
    }
    ccb_str_append(out, ",\"edge\":");
    if (s->edge != NULL) {
      ccb_json_escape(out, s->edge);
    } else {
      ccb_str_append(out, "null");
    }
    ccb_str_append(out, ",\"branch\":");
    if (s->branch != NULL) {
      ccb_json_escape(out, s->branch);
    } else {
      ccb_str_append(out, "null");
    }
    ccb_str_append(out, ",\"args\":[");
    for (k = 0; k < s->nargs; k++) {
      if (k > 0) {
        ccb_str_append(out, ",");
      }
      if (s->args[k] != NULL) {
        ccb_json_escape(out, s->args[k]);
      } else {
        ccb_str_append(out, "null");
      }
    }
    ccb_str_append(out, "]}");
  }
  ccb_str_append(out, "]}");
}

// ---------- apply ----------

static pv_screen *tr_find_screen(pv_project *p, const char *name) {
  size_t i = 0;
  if (name == NULL) {
    return NULL;
  }
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, name) == 0) {
      return p->screens[i];
    }
  }
  return NULL;
}

// True when any box in the subtree has the given label. This is the
// walk's branch-entry rule (branch_entry_edge): the only static link
// between a branch entry and an outgoing edge is that the edge's
// source label names a component inside the entry.
static int tr_subtree_has_label(pv_box *box, const char *label) {
  size_t i = 0;
  if (box == NULL || label == NULL) {
    return 0;
  }
  if (box->label != NULL && strcmp(box->label, label) == 0) {
    return 1;
  }
  for (i = 0; i < box->nchildren; i++) {
    if (tr_subtree_has_label(box->children[i], label)) {
      return 1;
    }
  }
  return 0;
}

// True when the subtree rooted at box contains a branch-entry box
// labeled entry whose subtree names from.
static int tr_box_in_branch(pv_box *box, const char *entry,
    const char *from) {
  size_t i = 0;
  if (box == NULL) {
    return 0;
  }
  if (box->type != NULL && strcmp(box->type, "branch-entry") == 0 &&
      box->label != NULL && strcmp(box->label, entry) == 0 &&
      tr_subtree_has_label(box, from)) {
    return 1;
  }
  for (i = 0; i < box->nchildren; i++) {
    if (tr_box_in_branch(box->children[i], entry, from)) {
      return 1;
    }
  }
  return 0;
}

// True when the edge's source label names a component inside a
// branch-entry box labeled entry somewhere in screen's tree
// (entries nest under branch nodes, so the whole tree is searched).
static int tr_edge_in_branch(pv_screen *screen, const char *entry,
    const char *from) {
  if (screen == NULL || entry == NULL || from == NULL) {
    return 0;
  }
  return tr_box_in_branch(screen->root, entry, from);
}

void pv_trace_apply(pv_project *p, const pv_trace *t,
    ccb_diag_list *diags) {
  size_t si = 0;
  if (p == NULL || t == NULL) {
    return;
  }
  p->trace_applied = 1;
  p->trace_step_count = (long)t->nsteps;
  for (si = 0; si < t->nsteps; si++) {
    const pv_trace_step *step = &t->steps[si];
    pv_screen *screen = tr_find_screen(p, step->screen);
    ccb_str detail;
    if (screen == NULL) {
      ccb_str_init(&detail);
      ccb_str_append(&detail, "step for unknown screen '");
      ccb_str_append(&detail, step->screen != NULL ?
        step->screen : "?");
      ccb_str_append(&detail, "'; step skipped");
      pv_rt_warn(diags, "CCB-P1023", "trace",
        detail.buf != NULL ? detail.buf : "");
      ccb_str_free(&detail);
      continue;
    }
    if (step->edge == NULL) {
      // Pick-only step: a branch picked without navigating.
      if (step->branch == NULL) {
        ccb_str_init(&detail);
        ccb_str_append(&detail, "step for screen '");
        ccb_str_append(&detail, screen->name != NULL ?
          screen->name : "?");
        ccb_str_append(&detail, "' names neither an edge nor a "
          "branch; step skipped");
        pv_rt_warn(diags, "CCB-P1023", "trace",
          detail.buf != NULL ? detail.buf : "");
        ccb_str_free(&detail);
        continue;
      }
      if (pv_rt_mark_branch(p, screen, step->branch,
          PV_PROV_TRACE) == 0) {
        ccb_str_init(&detail);
        ccb_str_append(&detail, "branch entry '");
        ccb_str_append(&detail, step->branch);
        ccb_str_append(&detail, "' not found in screen '");
        ccb_str_append(&detail, screen->name != NULL ?
          screen->name : "?");
        ccb_str_append(&detail, "'; pick skipped");
        pv_rt_warn(diags, "CCB-P1023", "trace",
          detail.buf != NULL ? detail.buf : "");
        ccb_str_free(&detail);
      }
      continue;
    }
    {
      size_t ei = 0;
      size_t nfrom = 0;
      size_t nbranch = 0;
      for (ei = 0; ei < screen->nedges; ei++) {
        if (screen->edges[ei].from != NULL &&
            strcmp(screen->edges[ei].from, step->edge) == 0) {
          nfrom++;
          if (step->branch == NULL ||
              tr_edge_in_branch(screen, step->branch,
                screen->edges[ei].from)) {
            nbranch++;
          }
        }
      }
      if (nfrom == 0) {
        ccb_str_init(&detail);
        ccb_str_append(&detail, "edge '");
        ccb_str_append(&detail, step->edge);
        ccb_str_append(&detail, "' matches no edge in screen '");
        ccb_str_append(&detail, screen->name != NULL ?
          screen->name : "?");
        ccb_str_append(&detail, "'; step skipped");
        pv_rt_warn(diags, "CCB-P1023", "trace",
          detail.buf != NULL ? detail.buf : "");
        ccb_str_free(&detail);
        continue;
      }
      if (step->branch != NULL) {
        // The pick is a fact even when the edge match is loose: a
        // step's branch names the entry that ran.
        if (pv_rt_mark_branch(p, screen, step->branch,
            PV_PROV_TRACE) == 0) {
          ccb_str_init(&detail);
          ccb_str_append(&detail, "branch entry '");
          ccb_str_append(&detail, step->branch);
          ccb_str_append(&detail, "' not found in screen '");
          ccb_str_append(&detail, screen->name != NULL ?
            screen->name : "?");
          ccb_str_append(&detail, "'; edge still marked taken");
          pv_rt_warn(diags, "CCB-P1023", "trace",
            detail.buf != NULL ? detail.buf : "");
          ccb_str_free(&detail);
        } else if (nbranch == 0) {
          // The entry exists but names no component with this
          // button's label: the step's branch context does not
          // contain the button. The press itself still matched,
          // so the edge is marked taken anyway.
          ccb_str_init(&detail);
          ccb_str_append(&detail, "edge '");
          ccb_str_append(&detail, step->edge);
          ccb_str_append(&detail, "' is not under branch entry '");
          ccb_str_append(&detail, step->branch);
          ccb_str_append(&detail, "' in screen '");
          ccb_str_append(&detail, screen->name != NULL ?
            screen->name : "?");
          ccb_str_append(&detail, "'; edge still marked taken");
          pv_rt_warn(diags, "CCB-P1023", "trace",
            detail.buf != NULL ? detail.buf : "");
          ccb_str_free(&detail);
        }
      }
      for (ei = 0; ei < screen->nedges; ei++) {
        pv_edge *e = &screen->edges[ei];
        size_t k = 0;
        size_t limit = 0;
        if (e->from == NULL || strcmp(e->from, step->edge) != 0) {
          continue;
        }
        // Prefer edges under the step's branch; when none is, the
        // button press itself is still certain, so every edge with
        // this label is marked taken.
        if (step->branch != NULL && nbranch > 0 &&
            !tr_edge_in_branch(screen, step->branch, e->from)) {
          continue;
        }
        e->trace_taken = 1;
        // Positional fills: step arg k fills edge arg k when that
        // slot is unknown. Static literals never change, and a
        // NULL step arg leaves its slot unknown.
        limit = step->nargs < e->nargs ? step->nargs : e->nargs;
        for (k = 0; k < limit; k++) {
          if (step->args[k] != NULL) {
            (void)pv_rt_fill_arg(p, e, k, step->args[k],
              PV_PROV_TRACE);
          }
        }
      }
    }
  }
}
