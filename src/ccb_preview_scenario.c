// ccb_preview_scenario: Chunk 1 scenario merge. See the header for
// the contract: fill unknowns, pick branches, never override a
// static literal, warn (CCB-P1022) on anything that matches nothing.
#include "ccb_preview_scenario.h"

// ---------- small helpers ----------

void pv_rt_warn(ccb_diag_list *diags, const char *code,
    const char *label, const char *detail) {
  ccb_diag d;
  ccb_str msg;
  if (diags == NULL) {
    return;
  }
  ccb_str_init(&msg);
  ccb_str_append(&msg, label != NULL ? label : "?");
  ccb_str_append(&msg, ": ");
  ccb_str_append(&msg, detail != NULL ? detail : "");
  ccb_diag_init(&d);
  ccb_diag_set_full(&d, code != NULL ? code : "CCB-P1022", "preview",
    msg.buf != NULL ? msg.buf : "", NULL, -1, -1, NULL, NULL, NULL);
  ccb_diag_list_push(diags, &d);
  ccb_str_free(&msg);
}

// Warning message for one scenario entry: "scenario '<name>': <detail>".
static void sc_warn(ccb_diag_list *diags, const ccb_scenario *sc,
    const char *detail) {
  ccb_str label;
  ccb_str_init(&label);
  ccb_str_append(&label, "scenario '");
  ccb_str_append(&label, sc->name != NULL ? sc->name : "?");
  ccb_str_append(&label, "'");
  pv_rt_warn(diags, "CCB-P1022", label.buf != NULL ? label.buf : "",
    detail);
  ccb_str_free(&label);
}

static int sc_type_eq(const pv_box *box, const char *type) {
  return box != NULL && box->type != NULL &&
    strcmp(box->type, type) == 0;
}

static int sc_label_eq(const pv_box *box, const char *label) {
  return box != NULL && label != NULL && box->label != NULL &&
    strcmp(box->label, label) == 0;
}

static pv_box *sc_box_new(ccb_arena *arena, const char *type,
    const char *label) {
  pv_box *box = ccb_arena_alloc(arena, sizeof(*box));
  memset(box, 0, sizeof(*box));
  box->type = ccb_arena_strdup(arena, type);
  box->label = label != NULL ? ccb_arena_strdup(arena, label) : NULL;
  return box;
}

static void sc_box_add_child(ccb_arena *arena, pv_box *parent,
    pv_box *child) {
  if (parent->nchildren == parent->capchildren) {
    size_t ncap = parent->capchildren == 0 ? 4 :
      parent->capchildren * 2;
    size_t i = 0;
    pv_box **next = ccb_arena_alloc(arena, ncap * sizeof(*next));
    for (i = 0; i < parent->nchildren; i++) {
      next[i] = parent->children[i];
    }
    parent->children = next;
    parent->capchildren = ncap;
  }
  parent->children[parent->nchildren] = child;
  parent->nchildren++;
}

// Collect boxes matching (type, label) under root, depth-first,
// into an arena-grown array, each with its owning screen name for
// diagnostics. Collection never mutates, so callers can safely
// append children to matches afterwards.
typedef struct {
  pv_box *box;
  const char *screen; // borrowed screen name
} sc_match;

typedef struct {
  sc_match *items;
  size_t count, cap;
} sc_match_list;

static void sc_collect(pv_box *box, const char *type,
    const char *label, const char *screen, ccb_arena *arena,
    sc_match_list *out) {
  size_t i = 0;
  if (box == NULL) {
    return;
  }
  if (sc_type_eq(box, type) && sc_label_eq(box, label)) {
    if (out->count == out->cap) {
      size_t ncap = out->cap == 0 ? 4 : out->cap * 2;
      size_t k = 0;
      sc_match *next = ccb_arena_alloc(arena, ncap * sizeof(*next));
      for (k = 0; k < out->count; k++) {
        next[k] = out->items[k];
      }
      out->items = next;
      out->cap = ncap;
    }
    out->items[out->count].box = box;
    out->items[out->count].screen = screen;
    out->count++;
  }
  for (i = 0; i < box->nchildren; i++) {
    sc_collect(box->children[i], type, label, screen, arena, out);
  }
}

static pv_screen *sc_find_screen(pv_project *p, const char *name) {
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

// ---------- shared merge primitives (Chunks 1 + 2) ----------

// Mark every branch-entry box with this label in screen. Shared by
// the scenario branch step and the trace branch step: a pick marks,
// it never hides, and it never touches any other entry.
size_t pv_rt_mark_branch(pv_project *p, pv_screen *screen,
    const char *entry, pv_prov prov) {
  sc_match_list found;
  size_t i = 0;
  if (p == NULL || screen == NULL || entry == NULL) {
    return 0;
  }
  memset(&found, 0, sizeof(found));
  if (screen->root != NULL) {
    sc_collect(screen->root, "branch-entry", entry, screen->name,
      p->arena, &found);
  }
  for (i = 0; i < found.count; i++) {
    if (prov == PV_PROV_TRACE) {
      found.items[i].box->trace_picked = 1;
    } else {
      found.items[i].box->scenario_picked = 1;
    }
  }
  return found.count;
}

// Fill one unknown argument slot. Shared by the scenario args step
// (which walks unknown slots left-to-right) and the trace args step
// (which maps step args positionally): the rule is identical — a
// NULL slot takes the value, a static literal never changes, and
// the filled slot carries its source's provenance.
int pv_rt_fill_arg(pv_project *p, pv_edge *e, size_t k,
    const char *value, pv_prov prov) {
  int **prov_array = NULL;
  size_t z = 0;
  if (p == NULL || e == NULL || value == NULL || k >= e->nargs ||
      e->args[k] != NULL) {
    return 0;
  }
  prov_array = prov == PV_PROV_TRACE ? &e->args_trace :
    &e->args_scenario;
  if (*prov_array == NULL) {
    *prov_array = ccb_arena_alloc(p->arena,
      (e->nargs == 0 ? 1 : e->nargs) * sizeof(int));
    for (z = 0; z < e->nargs; z++) {
      (*prov_array)[z] = 0;
    }
  }
  e->args[k] = ccb_arena_strdup(p->arena, value);
  (*prov_array)[k] = 1;
  return 1;
}

// ---------- merge steps ----------

// Branches: mark every branch-entry box with the picked label in the
// named screen. Other entries still render; a pick marks, it never
// hides, so the static structure stays visible.
static void sc_apply_branches(pv_project *p, const ccb_scenario *sc,
    ccb_diag_list *diags) {
  size_t bi = 0;
  for (bi = 0; bi < sc->nbranches; bi++) {
    pv_screen *screen = sc_find_screen(p, sc->branches[bi].screen);
    ccb_str detail;
    if (screen == NULL) {
      ccb_str_init(&detail);
      ccb_str_append(&detail, "branch for unknown screen '");
      ccb_str_append(&detail, sc->branches[bi].screen != NULL ?
        sc->branches[bi].screen : "?");
      ccb_str_append(&detail, "'; entry '");
      ccb_str_append(&detail, sc->branches[bi].entry != NULL ?
        sc->branches[bi].entry : "?");
      ccb_str_append(&detail, "' skipped");
      sc_warn(diags, sc, detail.buf != NULL ? detail.buf : "");
      ccb_str_free(&detail);
      continue;
    }
    if (pv_rt_mark_branch(p, screen, sc->branches[bi].entry,
        PV_PROV_SCENARIO) == 0) {
      ccb_str_init(&detail);
      ccb_str_append(&detail, "branch entry '");
      ccb_str_append(&detail, sc->branches[bi].entry != NULL ?
        sc->branches[bi].entry : "?");
      ccb_str_append(&detail, "' not found in screen '");
      ccb_str_append(&detail, screen->name != NULL ?
        screen->name : "?");
      ccb_str_append(&detail, "'; pick skipped");
      sc_warn(diags, sc, detail.buf != NULL ? detail.buf : "");
      ccb_str_free(&detail);
      continue;
    }
  }
}

// Lists: a childless list's 4 placeholder rows are the unknown, so
// scenario rows replace them (as real child boxes, which layout
// then sizes one row per child). A list that already has static
// children keeps them and warns: the static rows win.
static void sc_apply_lists(pv_project *p, const ccb_scenario *sc,
    ccb_diag_list *diags) {
  size_t li = 0;
  for (li = 0; li < sc->nlists; li++) {
    sc_match_list found;
    size_t si = 0;
    size_t i = 0;
    memset(&found, 0, sizeof(found));
    for (si = 0; si < p->nscreens; si++) {
      if (p->screens[si]->root != NULL) {
        sc_collect(p->screens[si]->root, "list", sc->lists[li].label,
          p->screens[si]->name, p->arena, &found);
      }
    }
    if (found.count == 0) {
      ccb_str detail;
      ccb_str_init(&detail);
      ccb_str_append(&detail, "list '");
      ccb_str_append(&detail, sc->lists[li].label != NULL ?
        sc->lists[li].label : "?");
      ccb_str_append(&detail, "' matches no list in any screen; "
        "rows skipped");
      sc_warn(diags, sc, detail.buf != NULL ? detail.buf : "");
      ccb_str_free(&detail);
      continue;
    }
    for (i = 0; i < found.count; i++) {
      pv_box *list = found.items[i].box;
      size_t r = 0;
      if (list->nchildren > 0) {
        ccb_str detail;
        ccb_str_init(&detail);
        ccb_str_append(&detail, "list '");
        ccb_str_append(&detail, sc->lists[li].label != NULL ?
          sc->lists[li].label : "?");
        ccb_str_append(&detail, "' in screen '");
        ccb_str_append(&detail, found.items[i].screen != NULL ?
          found.items[i].screen : "?");
        ccb_str_append(&detail, "' already has static rows; "
          "scenario rows ignored");
        sc_warn(diags, sc, detail.buf != NULL ? detail.buf : "");
        ccb_str_free(&detail);
        continue;
      }
      for (r = 0; r < sc->lists[li].nrows; r++) {
        pv_box *row = sc_box_new(p->arena, "text",
          sc->lists[li].rows[r]);
        row->from_scenario = 1;
        sc_box_add_child(p->arena, list, row);
      }
    }
  }
}

// Fields: content is additional, never a replacement — the static
// label stays and the wireframe shows "[label: text]".
static void sc_apply_fields(pv_project *p, const ccb_scenario *sc,
    ccb_diag_list *diags) {
  size_t fi = 0;
  for (fi = 0; fi < sc->nfields; fi++) {
    sc_match_list found;
    size_t si = 0;
    size_t i = 0;
    memset(&found, 0, sizeof(found));
    for (si = 0; si < p->nscreens; si++) {
      if (p->screens[si]->root != NULL) {
        sc_collect(p->screens[si]->root, "textfield",
          sc->fields[fi].label, p->screens[si]->name, p->arena,
          &found);
      }
    }
    if (found.count == 0) {
      ccb_str detail;
      ccb_str_init(&detail);
      ccb_str_append(&detail, "field '");
      ccb_str_append(&detail, sc->fields[fi].label != NULL ?
        sc->fields[fi].label : "?");
      ccb_str_append(&detail, "' matches no textfield in any screen; "
        "text skipped");
      sc_warn(diags, sc, detail.buf != NULL ? detail.buf : "");
      ccb_str_free(&detail);
      continue;
    }
    for (i = 0; i < found.count; i++) {
      found.items[i].box->scenario_text =
        ccb_arena_strdup(p->arena, sc->fields[fi].text);
    }
  }
}

// Args: fill unknown (NULL) argument slots left-to-right, per
// matching edge independently — the same button label on two
// screens fills each screen's edge from the same values. A static
// literal is never touched, and a dynamic target is never resolved:
// only NULL slots change. Values beyond every edge's slots are
// ignored (documented; arities legitimately differ per screen).
static void sc_apply_args(pv_project *p, const ccb_scenario *sc,
    ccb_diag_list *diags) {
  size_t ai = 0;
  for (ai = 0; ai < sc->nargs; ai++) {
    int matched_edge = 0;
    int filled_any = 0;
    size_t si = 0;
    size_t ei = 0;
    for (si = 0; si < p->nscreens; si++) {
      pv_screen *screen = p->screens[si];
      for (ei = 0; ei < screen->nedges; ei++) {
        pv_edge *e = &screen->edges[ei];
        size_t vi = 0;
        size_t k = 0;
        if (e->from == NULL ||
            strcmp(e->from, sc->args[ai].label) != 0) {
          continue;
        }
        matched_edge = 1;
        for (k = 0; k < e->nargs; k++) {
          if (e->args[k] != NULL || vi >= sc->args[ai].nvalues) {
            continue;
          }
          if (pv_rt_fill_arg(p, e, k, sc->args[ai].values[vi],
              PV_PROV_SCENARIO)) {
            vi++;
            filled_any = 1;
          }
        }
      }
    }
    if (!matched_edge) {
      ccb_str detail;
      ccb_str_init(&detail);
      ccb_str_append(&detail, "args for unknown button '");
      ccb_str_append(&detail, sc->args[ai].label != NULL ?
        sc->args[ai].label : "?");
      ccb_str_append(&detail, "'; values skipped");
      sc_warn(diags, sc, detail.buf != NULL ? detail.buf : "");
      ccb_str_free(&detail);
    } else if (!filled_any) {
      ccb_str detail;
      ccb_str_init(&detail);
      ccb_str_append(&detail, "args for button '");
      ccb_str_append(&detail, sc->args[ai].label != NULL ?
        sc->args[ai].label : "?");
      ccb_str_append(&detail, "' match no unknown argument to fill; "
        "values skipped");
      sc_warn(diags, sc, detail.buf != NULL ? detail.buf : "");
      ccb_str_free(&detail);
    }
  }
}

// ---------- entry point ----------

void pv_scenario_apply(pv_project *p, const ccb_scenario *sc,
    ccb_diag_list *diags) {
  if (p == NULL || sc == NULL) {
    return;
  }
  p->scenario_name = ccb_arena_strdup(p->arena, sc->name);
  sc_apply_branches(p, sc, diags);
  sc_apply_lists(p, sc, diags);
  sc_apply_fields(p, sc, diags);
  sc_apply_args(p, sc, diags);
}
