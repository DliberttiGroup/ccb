// ccb_preview_flow_slice1: Slice 1 click-handler edges.
// Split from ccb_preview_flow.c.

#include "ccb_common.h"
#include "ccb_preview.h"
#include <ctype.h>
#include "ccb_preview_flow_internal.h"

// ---------- Slice 1: click-handler edges ----------
//
// Recognized shapes, all inside a screen fun body (a top-level `fun`
// body that is a screen; attachments elsewhere are not recognized):
//   recv.setOnClickListener { ... }          (trailing lambda)
//   recv.setOnClickListener({ ... })         (parenthesized lambda)
//   recv.onClick = { ... }                   (property assignment)
//   recv.setOnClickListener(::handler)       (direct method reference)
//   recv.setOnClickListener(h)               (variable-held reference,
//     where `val h = ::handler` appeared earlier in the same screen
//     body; the plain shape only, no typed form)
//   recv.onClick = h                         (same, property form)
// where recv is the receiver identifier (the last identifier before
// the method in a dotted chain, e.g. `openButton` in
// `binding.openButton.setOnClickListener`). The handler body's nav
// calls become derived edges from the bound component label (via
// [preview.bindings]); nav/startActivity calls nested inside any
// brace block of the handler body, inside a conditional, or inside a
// helper the handler merely calls are NOT edges in Slice 1 — except
// for Chunk B's one-level helper follow (below). A named handler
// must be a top-level fun; same-file first, else the whole-project
// index when the name is unique across files. An ambiguous index
// match warns CCB-P1024 and records nothing, never a guess.
//
// Chunk B: a handler body may call a helper by bare name
// (`openDetail()`), and the helper's body is scanned one level
// deep (calls inside the helper body are not followed, so every
// chain terminates, including self-recursion) with the current
// branch annotation, attributing the helper's navs to the button.
// Dotted calls, nested-block calls, and calls to unknown names are
// not followed. A helper is any top-level fun found by bare name;
// a screen fun scanned this way yields only its depth-0 navs.
//
// A top-level fun whose name is referenced via :: from a screen body
// is a handler, not a screen: the pre-pass below collects those
// names (pruned to references from non-handler funs, so two handlers
// referencing each other do not keep each other alive) and the main
// loop skips their bodies instead of creating screens for them.
// Chunk C extends this with the whole-project index
// (pv_collect_index_handlers): a fun referenced via ::, from a
// listener lambda, or from a handler body is a handler in every
// file that defines it, so cross-file handlers and helpers no
// longer appear as phantom screens.

const char *pv_binding_label(pv_parser *p, const char *receiver) {
  size_t i = 0;
  if (receiver == NULL) {
    return NULL;
  }
  for (i = 0; i < p->nbind; i++) {
    if (p->bind_keys[i] != NULL && strcmp(p->bind_keys[i], receiver) == 0) {
      return p->bind_vals[i];
    }
  }
  return NULL;
}

// Chunk B: local `val`/`var` bindings in the current screen fun
// body. Record `val NAME = ::HANDLER` (plain shape only); look a
// variable's handler name up, or NULL when unrecorded.
void pv_val_record(pv_parser *p, char *name, char *target) {
  size_t i = 0;
  for (i = 0; i < p->nvals; i++) {
    if (strcmp(p->val_names[i], name) == 0) {
      p->val_targets[i] = target;
      return;
    }
  }
  if (p->nvals == p->capvals) {
    size_t ncap = p->capvals == 0 ? 8 : p->capvals * 2;
    p->val_names = ccb_xrealloc(p->val_names, ncap * sizeof(*p->val_names));
    p->val_targets = ccb_xrealloc(p->val_targets,
      ncap * sizeof(*p->val_targets));
    p->capvals = ncap;
  }
  p->val_names[p->nvals] = name;
  p->val_targets[p->nvals] = target;
  p->nvals++;
}

char *pv_val_lookup(pv_parser *p, const char *name) {
  size_t i = 0;
  for (i = 0; i < p->nvals; i++) {
    if (strcmp(p->val_names[i], name) == 0) {
      return p->val_targets[i];
    }
  }
  return NULL;
}

int pv_is_handler(pv_parser *p, const char *name) {
  size_t i = 0;
  for (i = 0; i < p->nhandlers; i++) {
    if (strcmp(p->handler_names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void pv_fun_add(pv_parser *p, char *name, size_t brace, size_t close) {
  if (p->nfuns == p->capfuns) {
    size_t ncap = p->capfuns == 0 ? 8 : p->capfuns * 2;
    p->fun_names = ccb_xrealloc(p->fun_names, ncap * sizeof(*p->fun_names));
    p->fun_braces = ccb_xrealloc(p->fun_braces, ncap * sizeof(*p->fun_braces));
    p->fun_closes = ccb_xrealloc(p->fun_closes, ncap * sizeof(*p->fun_closes));
    p->capfuns = ncap;
  }
  p->fun_names[p->nfuns] = name;
  p->fun_braces[p->nfuns] = brace;
  p->fun_closes[p->nfuns] = close;
  p->nfuns++;
}

void pv_handler_add(pv_parser *p, char *name) {
  if (pv_is_handler(p, name)) {
    return;
  }
  if (p->nhandlers == p->caphandlers) {
    size_t ncap = p->caphandlers == 0 ? 8 : p->caphandlers * 2;
    p->handler_names = ccb_xrealloc(p->handler_names,
      ncap * sizeof(*p->handler_names));
    p->caphandlers = ncap;
  }
  p->handler_names[p->nhandlers] = name;
  p->nhandlers++;
}

// Collect every top-level fun (same shape the main loop accepts:
// `fun Name(` ... `{` body) with its body brace range.
// When tokens at j start a fun's parameter list, register the fun
// (name, brace, close) and return the index past its body, or
// (size_t)-1 when this is not a well-formed fun.
static size_t pv_try_fun_add(pv_parser *p, char *name, size_t j) {
  if (j >= p->ntoks || !pv_is_sym(&p->toks[j], '(')) {
    return (size_t)-1;
  }
  size_t pc = pv_match_paren(p, j);
  if (pc == (size_t)-1 || pc + 1 >= p->ntoks ||
      !pv_is_sym(&p->toks[pc + 1], '{')) {
    return (size_t)-1;
  }
  size_t brace = pc + 1;
  size_t bc = pv_match_brace(p, brace);
  if (bc == (size_t)-1) {
    return (size_t)-1;
  }
  pv_fun_add(p, name, brace, bc);
  return bc + 1;
}

void pv_collect_funs(pv_parser *p) {
  size_t i = 0;
  long top_depth = 0;
  while (i < p->ntoks) {
    if (pv_is_sym(&p->toks[i], '{')) {
      top_depth++;
      i++;
      continue;
    }
    if (pv_is_sym(&p->toks[i], '}')) {
      if (top_depth > 0) {
        top_depth--;
      }
      i++;
      continue;
    }
    if (top_depth == 0 && pv_is_ident(&p->toks[i], "fun") &&
        i + 1 < p->ntoks && p->toks[i + 1].kind == PV_TK_IDENT) {
      size_t after = pv_try_fun_add(p, p->toks[i + 1].text, i + 2);
      if (after != (size_t)-1) {
        i = after;
        continue;
      }
    }
    i++;
  }
}

// True when tokens [start, end) contain `recv.setOnClickListener(
// ::name)` for the given name. The receiver and the reference must
// both lie inside the range (the range is a fun body).
static int pv_refers_to(pv_parser *p, size_t start, size_t end,
    const char *name) {
  size_t i = start;
  while (i < end) {
    if (i >= start + 2 && i + 4 < end &&
        pv_is_ident(&p->toks[i], "setOnClickListener") &&
        pv_is_sym(&p->toks[i - 1], '.') &&
        p->toks[i - 2].kind == PV_TK_IDENT &&
        pv_is_sym(&p->toks[i + 1], '(') &&
        pv_is_sym(&p->toks[i + 2], ':') &&
        pv_is_sym(&p->toks[i + 3], ':') &&
        p->toks[i + 4].kind == PV_TK_IDENT &&
        strcmp(p->toks[i + 4].text, name) == 0) {
      return 1;
    }
    i++;
  }
  return 0;
}

// Handler names = funs referenced via :: from a fun body, pruned to
// references from non-handler funs (iterated to a fixed point, so a
// handler referencing another handler does not by itself keep that
// handler alive, and mutual references collapse to nothing).
// True when handler h is referenced via :: from a non-handler fun.
static int pv_handler_kept(pv_parser *p, size_t h) {
  for (size_t f = 0; f < p->nfuns; f++) {
    if (pv_is_handler(p, p->fun_names[f])) {
      continue;
    }
    if (pv_refers_to(p, p->fun_braces[f] + 1, p->fun_closes[f],
        p->handler_names[h])) {
      return 1;
    }
  }
  return 0;
}

// Drop handler h, shifting the names after it down.
static void pv_handler_drop(pv_parser *p, size_t h) {
  for (size_t k = h + 1; k < p->nhandlers; k++) {
    p->handler_names[k - 1] = p->handler_names[k];
  }
  p->nhandlers--;
}

void pv_collect_handlers(pv_parser *p) {
  size_t f = 0;
  size_t g = 0;
  int changed = 0;
  for (f = 0; f < p->nfuns; f++) {
    for (g = 0; g < p->nfuns; g++) {
      if (pv_refers_to(p, p->fun_braces[f] + 1, p->fun_closes[f],
          p->fun_names[g])) {
        pv_handler_add(p, p->fun_names[g]);
      }
    }
  }
  changed = 1;
  while (changed) {
    size_t h = 0;
    changed = 0;
    while (h < p->nhandlers) {
      if (pv_handler_kept(p, h)) {
        h++;
      } else {
        pv_handler_drop(p, h);
        changed = 1;
      }
    }
  }
}

// Body token range (brace+1, close) of the top-level fun named name,
// or 0 when there is none.
int pv_handler_range(pv_parser *p, const char *name, size_t *start_out,
    size_t *end_out) {
  size_t i = 0;
  for (i = 0; i < p->nfuns; i++) {
    if (strcmp(p->fun_names[i], name) == 0) {
      *start_out = p->fun_braces[i] + 1;
      *end_out = p->fun_closes[i];
      return 1;
    }
  }
  return 0;
}
