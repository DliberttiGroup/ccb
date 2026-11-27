// ccb_preview_flow_chunkc: Chunk C index-wide screen candidacy +
// the flow parser itself (dense logic, kept whole). Split from
// ccb_preview_flow.c: one mutually-recursive parser; its candidate
// scan, handler scan, and component parse call each other, so
// splitting it further would scatter one algorithm across files.

#include "ccb_common.h"
#include "ccb_preview.h"
#include <ctype.h>
#include "ccb_preview_flow_internal.h"

// ---------- Chunk C: index-wide screen candidacy ----------
//
// A top-level fun referenced as a handler or helper is not a screen
// candidate. The per-file pre-pass above only sees
// setOnClickListener(::name) in the same file; this pass looks
// through the whole-project index, so cross-file handlers, helper
// calls from listener lambdas, variable-held references, and
// helpers called from handler bodies all disqualify their funs.
// Three reference shapes count, all conservative:
//   (a) any `::name` anywhere in a fun body (direct attachment,
//       property assignment, val-held reference);
//   (b) a bare `name(` call inside a setOnClickListener/onClick
//       lambda block in any fun body (the P3 helper shape);
//   (c) a bare `name(` call anywhere in a handler fun's body
//       (helper chains), iterated to a fixed point.
// A bare call directly in a screen's UI body does NOT count: that
// is screen composition (`Detail()` as a component), and the
// composed screen must stay a screen. A reference from a body to
// its own (file, name) never qualifies on its own, so
// self-recursion cannot make a fun its own handler. An ambiguous
// name (defined in several files) disqualifies every definition:
// the reference is never guessed, and the sweep warns for the
// funs' navs instead of leaving them silent.

// One index file's token stream + collected funs, for the candidacy
// pass. The parser struct is scratch: only its token and fun arrays
// are kept, and the caller frees them.

// (file index, fun name) pairs: the global handler set. Names point
// at arena token text and are never freed here; the arrays are
// xmalloc'd and freed by the caller.

static int pv_cand_has(pv_cand_pairs *set, size_t file,
    const char *name) {
  size_t i = 0;
  for (i = 0; i < set->n; i++) {
    if (set->files[i] == file && strcmp(set->names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

// Add one pair; returns 1 when the pair is new.
static int pv_cand_add(pv_cand_pairs *set, size_t file, char *name) {
  if (pv_cand_has(set, file, name)) {
    return 0;
  }
  if (set->n == set->cap) {
    size_t ncap = set->cap == 0 ? 16 : set->cap * 2;
    set->files = ccb_xrealloc(set->files, ncap * sizeof(*set->files));
    set->names = ccb_xrealloc(set->names, ncap * sizeof(*set->names));
    set->cap = ncap;
  }
  set->files[set->n] = file;
  set->names[set->n] = name;
  set->n++;
  return 1;
}

// True when token k starts a bare call `name(`: an identifier
// followed by '(', not dotted (not a method call) and not a `fun`
// declaration.
static int pv_cand_is_bare_call(pv_token *toks, size_t ntoks, size_t k) {
  if (k + 1 >= ntoks || toks[k].kind != PV_TK_IDENT ||
      toks[k].text == NULL) {
    return 0;
  }
  if (toks[k + 1].kind != PV_TK_SYM || toks[k + 1].sym != '(') {
    return 0;
  }
  if (k > 0 && toks[k - 1].kind == PV_TK_SYM &&
      toks[k - 1].sym == '.') {
    return 0;
  }
  if (k > 0 && toks[k - 1].kind == PV_TK_IDENT &&
      toks[k - 1].text != NULL && strcmp(toks[k - 1].text, "fun") == 0) {
    return 0;
  }
  return 1;
}

// Block range (brace, close) of the lambda attached to the listener
// at token k: trailing `setOnClickListener { ... }`, parenthesized
// `setOnClickListener({ ... })`, or `onClick = { ... }` (receiver
// optional). A direct :: reference or a variable argument has no
// block. Returns 1 and sets the range, else 0.
static int pv_cand_lambda_block(pv_parser *q, size_t k, size_t end,
    size_t *brace_out, size_t *close_out) {
  size_t close = 0;
  if (pv_is_ident(&q->toks[k], "setOnClickListener")) {
    if (k + 1 < end && pv_is_sym(&q->toks[k + 1], '{')) {
      close = pv_match_brace(q, k + 1);
      if (close == (size_t)-1) {
        return 0;
      }
      *brace_out = k + 1;
      *close_out = close;
      return 1;
    }
    if (k + 1 < end && pv_is_sym(&q->toks[k + 1], '(')) {
      size_t pc = pv_match_paren(q, k + 1);
      if (pc == (size_t)-1 || pc + 1 >= end ||
          !pv_is_sym(&q->toks[pc + 1], '{')) {
        return 0;
      }
      close = pv_match_brace(q, pc + 1);
      if (close == (size_t)-1) {
        return 0;
      }
      *brace_out = pc + 1;
      *close_out = close;
      return 1;
    }
    return 0;
  }
  if (pv_is_ident(&q->toks[k], "onClick")) {
    if (k + 1 < end && pv_is_sym(&q->toks[k + 1], '=') &&
        k + 2 < end && pv_is_sym(&q->toks[k + 2], '{')) {
      close = pv_match_brace(q, k + 2);
      if (close == (size_t)-1) {
        return 0;
      }
      *brace_out = k + 2;
      *close_out = close;
      return 1;
    }
    return 0;
  }
  return 0;
}

// Add handler pairs for every file defining `name`, except the
// referencing body's own (file, name): a body never qualifies
// itself. Returns 1 when a pair was added.
static int pv_cand_add_name(pv_fun_index *index, pv_cand_pairs *set,
    char *name, size_t ref_file, const char *ref_fun) {
  size_t f = 0;
  size_t i = 0;
  int added = 0;
  for (f = 0; f < index->nfiles; f++) {
    int defines = 0;
    if (f == ref_file && strcmp(name, ref_fun) == 0) {
      continue;
    }
    for (i = 0; i < index->nfuns; i++) {
      if (index->fun_files[i] == f &&
          strcmp(index->fun_names[i], name) == 0) {
        defines = 1;
        break;
      }
    }
    if (defines && pv_cand_add(set, f, name)) {
      added = 1;
    }
  }
  return added;
}

// Scan one fun body for references, adding handler pairs. Mode 0 is
// the seed scan: :: refs anywhere plus bare calls inside listener
// lambda blocks. Mode 1 is the handler-body scan: bare calls
// anywhere (plus :: refs, as in mode 0). Returns 1 when a pair was
// added.
// The bare calls inside one listener lambda body: each names a
// candidate handler for this file's fun.
static int pv_cand_scan_lambda(pv_parser *q, size_t brace, size_t close,
    size_t file, char *fun, pv_fun_index *index, pv_cand_pairs *set) {
  int added = 0;
  for (size_t j = brace + 1; j < close; j++) {
    if (pv_cand_is_bare_call(q->toks, q->ntoks, j) &&
        pv_cand_add_name(index, set, q->toks[j].text, file, fun)) {
      added = 1;
    }
  }
  return added;
}

static int pv_cand_scan_body(pv_parser *q, size_t file, char *fun,
    size_t start, size_t end, pv_fun_index *index, pv_cand_pairs *set,
    int mode) {
  size_t k = 0;
  int added = 0;
  for (k = start; k < end; k++) {
    if (k + 2 < end && pv_is_sym(&q->toks[k], ':') &&
        pv_is_sym(&q->toks[k + 1], ':') &&
        q->toks[k + 2].kind == PV_TK_IDENT &&
        q->toks[k + 2].text != NULL) {
      if (pv_cand_add_name(index, set, q->toks[k + 2].text, file,
          fun)) {
        added = 1;
      }
      continue;
    }
    if (mode == 1) {
      if (pv_cand_is_bare_call(q->toks, q->ntoks, k) &&
          pv_cand_add_name(index, set, q->toks[k].text, file, fun)) {
        added = 1;
      }
      continue;
    }
    if (pv_is_ident(&q->toks[k], "setOnClickListener") ||
        pv_is_ident(&q->toks[k], "onClick")) {
      size_t brace = 0;
      size_t close = 0;
      if (!pv_cand_lambda_block(q, k, end, &brace, &close)) {
        continue;
      }
      if (pv_cand_scan_lambda(q, brace, close, file, fun, index, set)) {
        added = 1;
      }
    }
  }
  return added;
}

// Extend this file's handler set with funs referenced through the
// whole-project index (see above). The global handler set is built
// from scratch views of every index file, seeded from every fun
// body, then closed under handler-body bare calls; finally, this
// file's funs in the set join the parser's handler names, so the
// main loop skips their bodies instead of creating screens. A NULL
// index keeps the exact per-file behavior.
// Close one candidate pair under handler-body bare calls:
// scanning the named fun's body in mode 1 may add new pairs.
// Returns 1 when it added any.
static int pv_cand_close_one(pv_parser *q, size_t file, pv_fun_index *index,
    pv_cand_pairs *set, size_t s) {
  int changed = 0;
  for (size_t g = 0; g < q->nfuns; g++) {
    if (strcmp(q->fun_names[g], set->names[s]) != 0) {
      continue;
    }
    if (pv_cand_scan_body(q, file, q->fun_names[g],
        q->fun_braces[g] + 1, q->fun_closes[g], index, set, 1)) {
      changed = 1;
    }
  }
  return changed;
}

static void pv_collect_index_handlers(pv_parser *p) {
  pv_fun_index *index = p->index;
  pv_cand_file *files = NULL;
  pv_cand_pairs set;
  size_t f = 0;
  size_t i = 0;
  size_t self = (size_t)-1;
  int changed = 0;
  if (index == NULL) {
    return;
  }
  memset(&set, 0, sizeof(set));
  files = ccb_xcalloc(index->nfiles == 0 ? 1 : index->nfiles,
    sizeof(*files));
  for (f = 0; f < index->nfiles; f++) {
    pv_parser *q = &files[f].q;
    memset(q, 0, sizeof(*q));
    q->arena = p->arena;
    q->text = index->texts[f] != NULL ? index->texts[f] : "";
    q->text_len = strlen(q->text);
    q->file = index->labels[f];
    pv_tokenize(q);
    pv_collect_funs(q);
    if (index->labels[f] != NULL && p->file != NULL &&
        strcmp(index->labels[f], p->file) == 0) {
      self = f;
    }
  }
  for (f = 0; f < index->nfiles; f++) {
    pv_parser *q = &files[f].q;
    for (i = 0; i < q->nfuns; i++) {
      (void)pv_cand_scan_body(q, f, q->fun_names[i],
        q->fun_braces[i] + 1, q->fun_closes[i], index, &set, 0);
    }
  }
  changed = 1;
  while (changed) {
    size_t s = 0;
    changed = 0;
    for (s = 0; s < set.n; s++) {
      if (pv_cand_close_one(&files[set.files[s]].q, set.files[s],
          index, &set, s)) {
        changed = 1;
      }
    }
  }
  if (self != (size_t)-1) {
    for (i = 0; i < p->nfuns; i++) {
      if (pv_cand_has(&set, self, p->fun_names[i])) {
        pv_handler_add(p, p->fun_names[i]);
      }
    }
  }
  for (f = 0; f < index->nfiles; f++) {
    free(files[f].q.toks);
    free(files[f].q.fun_names);
    free(files[f].q.fun_braces);
    free(files[f].q.fun_closes);
  }
  free(files);
  free(set.files);
  free(set.names);
}

// True when file f defines name.
static int pv_file_defines(pv_fun_index *index, size_t f, const char *name) {
  for (size_t i = 0; i < index->nfuns; i++) {
    if (index->fun_files[i] == f &&
        strcmp(index->fun_names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

// Count distinct files defining name; return the first file index
// (or (size_t)-1 when absent). Count goes in *count_out.
static size_t pv_index_lookup(pv_fun_index *index, const char *name,
    size_t *count_out) {
  size_t first = (size_t)-1;
  size_t count = 0;
  for (size_t f = 0; f < index->nfiles; f++) {
    if (!pv_file_defines(index, f, name)) {
      continue;
    }
    if (first == (size_t)-1) {
      first = f;
    }
    count++;
  }
  if (count_out != NULL) {
    *count_out = count;
  }
  return first;
}

// Warn CCB-P1024 once per (referencing file, name): an ambiguous
// reference records nothing and is never guessed.
static void pv_warn_ambiguous(pv_parser *p, const char *name,
    size_t count, long line) {
  size_t i = 0;
  ccb_str msg;
  const char *file = p->file != NULL ? p->file : "";
  if (p->index == NULL) {
    return;
  }
  for (i = 0; i < p->index->nwarned; i++) {
    if (strcmp(p->index->warned_files[i], file) == 0 &&
        strcmp(p->index->warned_names[i], name) == 0) {
      return;
    }
  }
  if (p->index->nwarned == p->index->capwarned) {
    size_t ncap = p->index->capwarned == 0 ? 8 : p->index->capwarned * 2;
    char **nf = ccb_arena_alloc(p->arena, ncap * sizeof(*nf));
    char **nn = ccb_arena_alloc(p->arena, ncap * sizeof(*nn));
    for (i = 0; i < p->index->nwarned; i++) {
      nf[i] = p->index->warned_files[i];
      nn[i] = p->index->warned_names[i];
    }
    p->index->warned_files = nf;
    p->index->warned_names = nn;
    p->index->capwarned = ncap;
  }
  p->index->warned_files[p->index->nwarned] =
    ccb_arena_strdup(p->arena, file);
  p->index->warned_names[p->index->nwarned] =
    ccb_arena_strdup(p->arena, name);
  p->index->nwarned++;
  ccb_str_init(&msg);
  ccb_str_append(&msg, "ambiguous reference to '");
  ccb_str_append(&msg, name);
  ccb_str_append(&msg, "': defined in ");
  ccb_str_appendf(&msg, "%zu", count);
  ccb_str_append(&msg, " flow files; no edge recorded");
  pv_flow_diag(p, "CCB-P1024", msg.buf != NULL ? msg.buf : "", line);
  ccb_str_free(&msg);
}

static void pv_warn_unbound(pv_parser *p, const char *receiver) {
  const char *screen = p->screen != NULL && p->screen->name != NULL ?
    p->screen->name : "";
  size_t i = 0;
  ccb_str msg;
  for (i = 0; i < p->nunbound; i++) {
    if (strcmp(p->unbound_screens[i], screen) == 0 &&
        strcmp(p->unbound_receivers[i], receiver) == 0) {
      return;
    }
  }
  if (p->nunbound == p->capunbound) {
    size_t ncap = p->capunbound == 0 ? 8 : p->capunbound * 2;
    p->unbound_screens = ccb_xrealloc(p->unbound_screens,
      ncap * sizeof(*p->unbound_screens));
    p->unbound_receivers = ccb_xrealloc(p->unbound_receivers,
      ncap * sizeof(*p->unbound_receivers));
    p->capunbound = ncap;
  }
  p->unbound_screens[p->nunbound] = ccb_arena_strdup(p->arena, screen);
  p->unbound_receivers[p->nunbound] = ccb_arena_strdup(p->arena, receiver);
  p->nunbound++;
  ccb_str_init(&msg);
  ccb_str_append(&msg, "handler found, button not bound: click handler for '");
  ccb_str_append(&msg, receiver);
  ccb_str_append(&msg, "' has no [preview.bindings] entry; no edge recorded");
  pv_flow_diag(p, "CCB-P1019", msg.buf != NULL ? msg.buf : "", pv_cur_line(p));
  ccb_str_free(&msg);
}

// Skip one statement starting at i, for braceless conditional
// bodies (e.g. `if (c) nav("X")`) and for statements the branch
// scanner decides are not nav calls. A call chain includes a
// trailing lambda block; a nested if/when is skipped whole,
// including its else chain, so a nav inside a nested braceless
// conditional can never leak out as an unconditional edge. A '{'
// is left for the caller's depth counting (its contents are
// nested, hence excluded). Returns the index just past the skipped
// statement, or end.
static size_t pv_skip_stmt(pv_parser *p, size_t i, size_t end) {
  if (i >= end) {
    return end;
  }
  if (pv_is_sym(&p->toks[i], '{')) {
    return i;
  }
  if (pv_is_ident(&p->toks[i], "if")) {
    i++;
    i = pv_skip_paren(p, i, end);
    i = pv_skip_stmt(p, i, end);
    if (i < end && pv_is_ident(&p->toks[i], "else")) {
      i++;
      i = pv_skip_stmt(p, i, end);
    }
    return i;
  }
  if (pv_is_ident(&p->toks[i], "when")) {
    i++;
    i = pv_skip_paren(p, i, end);
    return pv_skip_brace(p, i, end);
  }
  if (p->toks[i].kind == PV_TK_IDENT) {
    while (i < end && p->toks[i].kind == PV_TK_IDENT) {
      i++;
      i = pv_skip_paren(p, i, end);
      if (i < end && pv_is_sym(&p->toks[i], '.')) {
        i++;
        continue;
      }
      break;
    }
    return pv_skip_brace(p, i, end);
  }
  return i + 1;
}

// Build a branch annotation for one entry of a conditional:
// "<base>: <entry>", prefixed with the enclosing branch and ", "
// when this conditional is itself nested inside a branch body
// (outer is NULL at the handler top level). Labels only, never
// evaluated; e.g. "if ready: then" or
// "if ready: then, if other: else".
static char *pv_branch_entry(pv_parser *p, const char *outer,
    const char *base, const char *entry) {
  ccb_str s;
  char *out = NULL;
  ccb_str_init(&s);
  if (outer != NULL) {
    ccb_str_append(&s, outer);
    ccb_str_append(&s, ", ");
  }
  ccb_str_append(&s, base);
  ccb_str_append(&s, ": ");
  ccb_str_append(&s, entry);
  out = ccb_arena_strdup(p->arena, s.buf != NULL ? s.buf : "");
  ccb_str_free(&s);
  return out;
}

// The first comma segment of a navigate() call: a plain string
// literal is the target; an R.id.<name> resolves through the nav
// graph (an unlisted id stays unknown, never guessed).
static void pv_nav_first_segment(pv_parser *p, size_t seg_start, size_t i,
    char **target, int *unknown) {
  if (i - seg_start == 1 &&
      p->toks[seg_start].kind == PV_TK_STRING &&
      strchr(p->toks[seg_start].text, '$') == NULL) {
    *target = p->toks[seg_start].text;
    *unknown = 0;
    return;
  }
  if (i - seg_start != 5 ||
      !pv_is_ident(&p->toks[seg_start], "R") ||
      !pv_is_sym(&p->toks[seg_start + 1], '.') ||
      !pv_is_ident(&p->toks[seg_start + 2], "id") ||
      !pv_is_sym(&p->toks[seg_start + 3], '.') ||
      p->toks[seg_start + 4].kind != PV_TK_IDENT) {
    return;
  }
  for (size_t g = 0; g < p->ngraph; g++) {
    if (strcmp(p->graph_keys[g], p->toks[seg_start + 4].text) == 0) {
      *target = p->graph_vals[g];
      *unknown = 0;
      break;
    }
  }
}

// Append one edge argument (NULL for an empty segment, i.e. a
// trailing comma), growing the arena arrays.
void pv_nav_arg_push(pv_parser *p, char ***args, size_t *nargs,
    int **args_named, char *arg, int named) {
  char **next = ccb_arena_alloc(p->arena, (*nargs + 1) * sizeof(*next));
  int *next_named = ccb_arena_alloc(p->arena,
    (*nargs + 1) * sizeof(*next_named));
  for (size_t k = 0; k < *nargs; k++) {
    next[k] = (*args)[k];
    next_named[k] = (*args_named)[k];
  }
  next[*nargs] = arg;
  next_named[*nargs] = named;
  *args = next;
  *args_named = next_named;
  (*nargs)++;
}

// Parse a NavController navigate() call at token idx (ident + '(')
// into a derived edge from from_label, annotated with branch.
// The destination is a string literal (the "..." form), an
// `R.id.<name>` reference resolved through the nav graph, or
// anything else, which yields an unknown edge, never a guess: a
// destination id with no graph entry, and a navigate() call with no
// graph file at all, are both unknown. Extra arguments reuse the
// nav-args classification (literal / "name = value" / unknown).
// Returns the index past the call, (size_t)-1 on breakage.
static size_t pv_handler_navigate(pv_parser *p, size_t idx, char *from_label,
    char *branch) {
  size_t close = pv_match_paren(p, idx + 1);
  char *target = NULL;
  int unknown = 1;
  char **args = NULL;
  size_t nargs = 0;
  int *args_named = NULL;
  size_t seg_start = 0;
  size_t i = 0;
  long paren_depth = 0;
  long brace_depth = 0;
  int first = 1;
  if (close == (size_t)-1) {
    pv_broken(p, p->toks[idx].line);
    return (size_t)-1;
  }
  seg_start = idx + 2;
  paren_depth = 1;
  brace_depth = 0;
  for (i = idx + 2; i <= close; i++) {
    int is_split = 0;
    int is_end = i == close;
    if (!is_end) {
      if (pv_is_sym(&p->toks[i], '(')) {
        paren_depth++;
      } else if (pv_is_sym(&p->toks[i], ')')) {
        paren_depth--;
      } else if (pv_is_sym(&p->toks[i], '{')) {
        brace_depth++;
      } else if (pv_is_sym(&p->toks[i], '}')) {
        brace_depth--;
      } else if (pv_is_sym(&p->toks[i], ',') && paren_depth == 1 &&
          brace_depth == 0) {
        is_split = 1;
      }
    }
    if (!is_split && !is_end) {
      continue;
    }
    if (seg_start < i) {
      if (first) {
        first = 0;
        pv_nav_first_segment(p, seg_start, i, &target, &unknown);
      } else {
        int named = 0;
        char *arg = pv_arg_classify(p, seg_start, i, &named);
        pv_nav_arg_push(p, &args, &nargs, &args_named, arg, named);
      }
    } else if (!first) {
      // An empty later segment (e.g. a trailing comma) is an
      // unknown argument.
      pv_nav_arg_push(p, &args, &nargs, &args_named, NULL, 0);
    } else {
      first = 0;
    }
    seg_start = i + 1;
  }
  pv_edge_add(p, from_label, target, unknown, args, nargs, args_named, 1,
    branch);
  return close + 1;
}

// Try to consume one nav-ish call at token i: nav/navigateTo (with
// literal arguments, via the shared parser), startActivity (class
// literal or unknown), or NavController navigate() (string literal,
// R.id.x via the graph, or unknown). Returns the index past the
// call, 0 when token i is not a nav-ish call, (size_t)-1 on
// breakage (the caller stops scanning).
static size_t pv_handler_nav_call(pv_parser *p, size_t i, size_t end,
    char *from_label, char *branch) {
  pv_token *t = &p->toks[i];
  if (pv_is_nav(p, i)) {
    char *target = NULL;
    int unknown = 1;
    char **args = NULL;
    size_t nargs = 0;
    int *args_named = NULL;
    size_t next = pv_nav_parse(p, i, &target, &unknown, &args,
      &nargs, &args_named);
    if (next == (size_t)-1 || next <= i) {
      return (size_t)-1;
    }
    pv_index_mark_nav(p->index, p->file, p->toks[i].start);
    pv_edge_add(p, from_label, target, unknown, args, nargs,
      args_named, 1, branch);
    return next > end ? end : next;
  }
  if (pv_is_ident(t, "startActivity") && i + 1 < end &&
      pv_is_sym(&p->toks[i + 1], '(')) {
    size_t close = pv_match_paren(p, i + 1);
    size_t j = 0;
    char *target = NULL;
    int found = 0;
    if (close == (size_t)-1) {
      return (size_t)-1;
    }
    pv_index_mark_nav(p->index, p->file, p->toks[i].start);
    // `Name::class.java`: ':' ':' are two single-char tokens.
    for (j = i + 2; j + 5 < close; j++) {
      if (p->toks[j].kind == PV_TK_IDENT &&
          pv_is_sym(&p->toks[j + 1], ':') &&
          pv_is_sym(&p->toks[j + 2], ':') &&
          pv_is_ident(&p->toks[j + 3], "class") &&
          pv_is_sym(&p->toks[j + 4], '.') &&
          pv_is_ident(&p->toks[j + 5], "java")) {
        target = p->toks[j].text;
        found = 1;
        break;
      }
    }
    pv_edge_add(p, from_label, target, found ? 0 : 1, NULL, 0,
      NULL, 1, branch);
    return close + 1 > end ? end : close + 1;
  }
  if (pv_is_ident(t, "navigate") && i + 1 < end &&
      pv_is_sym(&p->toks[i + 1], '(')) {
    size_t next = pv_handler_navigate(p, i, from_label, branch);
    if (next == (size_t)-1) {
      return (size_t)-1;
    }
    pv_index_mark_nav(p->index, p->file, p->toks[i].start);
    return next > end ? end : next;
  }
  return 0;
}
// Chunk B: try to consume a bare helper call at token i (defined
// before pv_scan_handler_branch); returns the index past the call,
// or 0 when token i is not a followed helper call.
// Every definition of an ambiguous name gets its navs marked: all
// of them are reported by P1024, so none is sweep territory.
static void pv_mark_ambiguous_navs(pv_parser *p, const char *name) {
  for (size_t f = 0; f < p->index->nfiles; f++) {
    for (size_t g = 0; g < p->index->nfuns; g++) {
      if (p->index->fun_files[g] == f &&
          strcmp(p->index->fun_names[g], name) == 0) {
        pv_mark_foreign_navs(p, f, name);
        break;
      }
    }
  }
}

// One if/else body: a braced block scanned as a handler branch, or
// a single nav call / statement. Returns the index past the body,
// or (size_t)-1 on breakage.
static size_t pv_scan_if_body(pv_parser *p, size_t i, size_t end,
    char *from_label, char *label) {
  if (i < end && pv_is_sym(&p->toks[i], '{')) {
    size_t close = pv_match_brace(p, i);
    if (close == (size_t)-1) {
      return (size_t)-1;
    }
    pv_scan_handler_branch(p, i + 1, close, from_label, label);
    return close + 1;
  }
  if (i >= end) {
    return i;
  }
  size_t next = pv_handler_nav_call(p, i, end, from_label, label);
  if (next == (size_t)-1) {
    return (size_t)-1;
  }
  return next != 0 ? next : pv_skip_stmt(p, i, end);
}

// One statement at i that is a nav call or a helper call. Returns
// 1 when consumed (*i_io advanced), 0 when neither applies, -1
// on breakage.
static int pv_step_call(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *branch) {
  size_t next = pv_handler_nav_call(p, *i_io, end, from_label, branch);
  if (next == (size_t)-1) {
    return -1;
  }
  if (next != 0) {
    *i_io = next;
    return 1;
  }
  next = pv_try_helper_call(p, *i_io, end, from_label, branch);
  if (next != 0) {
    *i_io = next;
    return 1;
  }
  return 0;
}

// Scan one if/else-if statement whose `if` is at *i_io: the then
// body (braced or a single braceless statement) and the else chain,
// each annotated with its branch label. Advances *i_io past the
// whole statement, or to end on breakage.
void pv_scan_if(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *outer, int is_else_if) {
  size_t i = *i_io;
  char *cond = ccb_arena_strdup(p->arena, "");
  i++;
  if (i < end && pv_is_sym(&p->toks[i], '(')) {
    // A group closing at or past end is the same as a broken one
    // here: either way the if has no body to scan.
    size_t after = pv_skip_paren(p, i, end);
    if (after >= end) {
      *i_io = end;
      return;
    }
    cond = pv_paren_text(p, i);
    i = after;
  }
  char *base = pv_branch_label(p, is_else_if ? "else if" : "if", cond);
  char *then_label = pv_branch_entry(p, outer, base, "then");
  i = pv_scan_if_body(p, i, end, from_label, then_label);
  if (i == (size_t)-1) {
    *i_io = end;
    return;
  }
  if (i < end && pv_is_ident(&p->toks[i], "else")) {
    i++;
    if (i < end && pv_is_ident(&p->toks[i], "if")) {
      pv_scan_if(p, &i, end, from_label, outer, 1);
    } else {
      char *else_label = pv_branch_entry(p, outer, base, "else");
      i = pv_scan_if_body(p, i, end, from_label, else_label);
      if (i == (size_t)-1) {
        *i_io = end;
        return;
      }
    }
  }
  *i_io = i;
}

// Scan one when statement whose `when` is at *i_io: each entry's
// body (braced, or a single braceless statement) annotated with
// "when <cond>: <entry>". Advances *i_io past the whole statement,
// or to end on breakage. A subjectless `when { ... }` labels
// entries under just "when".
// One when entry's body after its '->': a braced block scanned as
// a handler branch, or a single nav call / statement. Returns the
// index past the body, or close when the body is broken.
static size_t pv_scan_when_entry(pv_parser *p, size_t pos, size_t close,
    char *from_label, char *branch) {
  if (pos < close && pv_is_sym(&p->toks[pos], '{')) {
    size_t bclose = pv_match_brace(p, pos);
    if (bclose == (size_t)-1 || bclose > close) {
      return close;
    }
    pv_scan_handler_branch(p, pos + 1, bclose, from_label, branch);
    return bclose + 1;
  }
  if (pos < close && !pv_is_sym(&p->toks[pos], '}')) {
    size_t next = pv_handler_nav_call(p, pos, close, from_label, branch);
    if (next == (size_t)-1) {
      return close;
    }
    return next != 0 ? next : pv_skip_stmt(p, pos, close);
  }
  return pos;
}

// The entries of a when body, from pos to the body's closing brace.
static void pv_scan_when_entries(pv_parser *p, size_t pos, size_t close,
    char *from_label, char *outer, char *base) {
  while (pos < close) {
    size_t start = pos;
    size_t arrow = pv_find_arrow(p, start, close);
    if (arrow == (size_t)-1) {
      break;
    }
    char *entry_label = pv_trim_arena(p->arena, p->text,
      p->toks[start].start, p->toks[arrow].start);
    char *branch = pv_branch_entry(p, outer, base, entry_label);
    pos = pv_scan_when_entry(p, arrow + 1, close, from_label, branch);
  }
}

void pv_scan_when(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *outer) {
  size_t i = *i_io;
  char *cond = ccb_arena_strdup(p->arena, "");
  i++;
  if (i < end && pv_is_sym(&p->toks[i], '(')) {
    // A group closing at or past end is the same as a broken one
    // here: either way the when has no body to scan.
    size_t after = pv_skip_paren(p, i, end);
    if (after >= end) {
      *i_io = end;
      return;
    }
    cond = pv_paren_text(p, i);
    i = after;
  }
  char *base = pv_branch_label(p, "when", cond);
  if (i < end && pv_is_sym(&p->toks[i], '{')) {
    size_t close = pv_match_brace(p, i);
    if (close == (size_t)-1) {
      *i_io = end;
      return;
    }
    pv_scan_when_entries(p, i + 1, close, from_label, outer, base);
    i = close + 1;
  } else if (i < end) {
    i = pv_skip_stmt(p, i, end);
  }
  *i_io = i > end ? end : i;
}

// Classify the argument list of the call at token i (ident + '(')
// into the edge-args shape: each top-level comma segment becomes
// one entry (a literal, a "name = value" raw text, or NULL for
// anything else). Used to carry a helper call's literal arguments
// onto the edges its body produces (Phase 2: a helper like
// openDetail("note-1") whose body only startActivitys).
static void pv_call_site_args(pv_parser *p, size_t i, char ***args_out,
    size_t *nargs_out, int **args_named_out) {
  size_t close = 0;
  char **args = NULL;
  size_t nargs = 0;
  int *args_named = NULL;
  size_t seg_start = 0;
  size_t k = 0;
  long paren_depth = 0;
  long brace_depth = 0;
  if (args_out != NULL) {
    *args_out = NULL;
  }
  if (nargs_out != NULL) {
    *nargs_out = 0;
  }
  if (args_named_out != NULL) {
    *args_named_out = NULL;
  }
  if (i + 1 >= p->ntoks || !pv_is_sym(&p->toks[i + 1], '(')) {
    return;
  }
  close = pv_match_paren(p, i + 1);
  if (close == (size_t)-1 || close == i + 2) {
    return;
  }
  seg_start = i + 2;
  paren_depth = 1;
  brace_depth = 0;
  for (k = i + 2; k <= close; k++) {
    int is_split = 0;
    int is_end = k == close;
    if (!is_end) {
      if (pv_is_sym(&p->toks[k], '(')) {
        paren_depth++;
      } else if (pv_is_sym(&p->toks[k], ')')) {
        paren_depth--;
      } else if (pv_is_sym(&p->toks[k], '{')) {
        brace_depth++;
      } else if (pv_is_sym(&p->toks[k], '}')) {
        brace_depth--;
      } else if (pv_is_sym(&p->toks[k], ',') && paren_depth == 1 &&
          brace_depth == 0) {
        is_split = 1;
      }
    }
    if (!is_split && !is_end) {
      continue;
    }
    {
      int named = 0;
      char *arg = NULL;
      char **next = NULL;
      int *next_named = NULL;
      size_t m = 0;
      if (seg_start < k) {
        arg = pv_arg_classify(p, seg_start, k, &named);
      }
      next = ccb_arena_alloc(p->arena, (nargs + 1) * sizeof(*next));
      next_named = ccb_arena_alloc(p->arena,
        (nargs + 1) * sizeof(*next_named));
      for (m = 0; m < nargs; m++) {
        next[m] = args[m];
        next_named[m] = args_named[m];
      }
      next[nargs] = arg;
      next_named[nargs] = named;
      args = next;
      args_named = next_named;
      nargs++;
    }
    seg_start = k + 1;
  }
  if (args_out != NULL) {
    *args_out = args;
  }
  if (nargs_out != NULL) {
    *nargs_out = nargs;
  }
  if (args_named_out != NULL) {
    *args_named_out = args_named;
  }
}

// Carry a helper call's arguments onto the edges its body just
// produced: every edge added since `before` that carries no
// arguments of its own takes the call-site arguments. An edge whose
// nav call already has arguments keeps its own — the helper's
// literal is more specific than the caller's. A call with no
// arguments changes nothing.
static void pv_apply_helper_args(pv_parser *p, size_t call_idx,
    size_t before) {
  char **args = NULL;
  size_t nargs = 0;
  int *args_named = NULL;
  size_t e = 0;
  if (p->screen == NULL || before >= p->screen->nedges) {
    return;
  }
  pv_call_site_args(p, call_idx, &args, &nargs, &args_named);
  if (nargs == 0) {
    return;
  }
  for (e = before; e < p->screen->nedges; e++) {
    pv_edge *edge = &p->screen->edges[e];
    if (edge->nargs != 0) {
      continue;
    }
    edge->args = args;
    edge->nargs = nargs;
    edge->args_named = args_named;
  }
}

// Chunk B: try to consume a bare helper call at token i in a
// handler-body scan: `helper(...)` where helper is a top-level fun
// (same file first, else the whole-project index when unique).
// The helper's body is scanned at helper depth 1 with the current
// branch annotation, so its navs become edges from from_label;
// calls inside the helper body are NOT followed (depth cap), so
// every helper chain terminates, including self-recursion. Dotted
// calls (`x.helper()`), nav-ish calls, and calls to unknown names
// are not helpers: return 0 and let the caller move on. An
// ambiguous index match warns CCB-P1024 and consumes the call.
// Returns the index past the call's closing paren, or 0.
size_t pv_try_helper_call(pv_parser *p, size_t i, size_t end,
    char *from_label, char *branch) {
  char *name = NULL;
  size_t hs = 0;
  size_t he = 0;
  size_t close = 0;
  if (p->helper_depth != 0) {
    return 0;
  }
  if (p->toks[i].kind != PV_TK_IDENT) {
    return 0;
  }
  if (i > 0 && pv_is_sym(&p->toks[i - 1], '.')) {
    return 0;
  }
  if (i + 1 >= end || !pv_is_sym(&p->toks[i + 1], '(')) {
    return 0;
  }
  name = p->toks[i].text;
  close = pv_match_paren(p, i + 1);
  if (close == (size_t)-1) {
    return 0;
  }
  if (pv_handler_range(p, name, &hs, &he)) {
    size_t before = p->screen != NULL ? p->screen->nedges : 0;
    int saved = p->helper_depth;
    p->helper_depth = 1;
    pv_scan_handler_branch(p, hs, he, from_label, branch);
    p->helper_depth = saved;
    pv_apply_helper_args(p, i, before);
    return close + 1;
  }
  if (p->index != NULL) {
    size_t count = 0;
    size_t file = pv_index_lookup(p->index, name, &count);
    if (count == 1) {
      size_t before = p->screen != NULL ? p->screen->nedges : 0;
      pv_scan_foreign_body(p, file, name, from_label, branch, 1);
      pv_apply_helper_args(p, i, before);
      return close + 1;
    }
    if (count > 1) {
      pv_warn_ambiguous(p, name, count, p->toks[i].line);
      pv_mark_ambiguous_navs(p, name);
      return close + 1;
    }
  }
  return 0;
}

// Scan a handler body range [start, end) for navigation at brace
// depth 0 only, recording derived edges from from_label, annotated
// with branch (NULL at the handler top level). Slice 2: a nav or
// startActivity inside an if/when branch body — braced or a single
// braceless statement — becomes an edge annotated with that branch
// ("if ready: then", "when state: is Loaded"); the condition is
// labeled, never evaluated. A nav inside any other nested brace
// block (a nested lambda/callback, a coroutine, any inner block)
// still produces NO edge at all, not even an unknown edge. Chunk B:
// a bare helper call at depth 0 is followed one level (see
// pv_try_helper_call); its navs become edges from from_label with
// the current branch annotation. A startActivity whose Intent carries a `Name::class.java` literal
// yields that simple name as the target; any other startActivity
// argument shape yields an unknown edge, never a guessed
// destination. NavController navigate() calls in the same positions
// resolve through the nav graph (Slice 3).
void pv_scan_handler_branch(pv_parser *p, size_t start, size_t end,
    char *from_label, char *branch) {
  size_t i = start;
  long depth = 0;
  while (i < end) {
    pv_token *t = &p->toks[i];
    if (pv_is_sym(t, '{')) {
      depth++;
      i++;
      continue;
    }
    if (pv_is_sym(t, '}')) {
      if (depth > 0) {
        depth--;
      }
      i++;
      continue;
    }
    if (depth == 0) {
      if (pv_is_ident(t, "if")) {
        pv_scan_if(p, &i, end, from_label, branch, 0);
        continue;
      }
      if (pv_is_ident(t, "when")) {
        pv_scan_when(p, &i, end, from_label, branch);
        continue;
      }
      if (pv_is_ident(t, "else")) {
        // A stray else (its if was already consumed, or malformed
        // input): skip one statement, never an edge on its own.
        i = pv_skip_stmt(p, i + 1, end);
        continue;
      }
      int stepped = pv_step_call(p, &i, end, from_label, branch);
      if (stepped < 0) {
        return;
      }
      if (stepped == 1) {
        continue;
      }
    }
    i++;
  }
}

static void pv_scan_handler_body(pv_parser *p, size_t start, size_t end,
    char *from_label) {
  pv_scan_handler_branch(p, start, end, from_label, NULL);
}

// Chunk B: scan a foreign file's top-level fun body with a scratch
// parser over the index's retained text, sharing arena, diagnostics,
// bindings, graph, and index with the caller, and attributing edges
// to the caller's screen (p->screen). helper_depth 0 scans a
// handler body (helper calls inside it are followed); 1 scans a
// followed helper body (calls inside it are not). The scratch
// parser's token and fun arrays are freed here; token text is
// arena-owned and lives on. A missing fun (index raced ahead of
// the text, or an unbalanced body) scans nothing, never crashes.
void pv_scan_foreign_body(pv_parser *p, size_t file_idx,
    const char *name, char *from_label, char *branch, int helper_depth) {
  pv_parser q;
  size_t i = 0;
  if (p->index == NULL || file_idx >= p->index->nfiles ||
      p->index->texts[file_idx] == NULL || p->screen == NULL) {
    return;
  }
  memset(&q, 0, sizeof(q));
  q.arena = p->arena;
  q.text = p->index->texts[file_idx];
  q.text_len = strlen(q.text);
  q.file = p->index->labels[file_idx];
  q.diags = p->diags;
  q.screen = p->screen;
  q.bind_keys = p->bind_keys;
  q.bind_vals = p->bind_vals;
  q.nbind = p->nbind;
  q.graph_keys = p->graph_keys;
  q.graph_vals = p->graph_vals;
  q.ngraph = p->ngraph;
  q.index = p->index;
  q.helper_depth = helper_depth;
  pv_tokenize(&q);
  // No P1010 here: the foreign file's own parse reports its
  // malformation; the scratch scan only reuses its tokens.
  pv_collect_funs(&q);
  for (i = 0; i < q.nfuns; i++) {
    if (strcmp(q.fun_names[i], name) == 0) {
      pv_scan_handler_branch(&q, q.fun_braces[i] + 1, q.fun_closes[i],
        from_label, branch);
      break;
    }
  }
  free(q.toks);
  free(q.fun_names);
  free(q.fun_braces);
  free(q.fun_closes);
}

// Chunk B: resolve a named handler to a body and attribute it to
// its button. Same-file fun range first (today's behavior); else
// the whole-project index when the name is unique across files.
// An ambiguous index match warns CCB-P1024 and records nothing.
// No match at all records nothing and warns nothing (today's
// cross-file behavior, kept for index-less parses).
static void pv_handle_named(pv_parser *p, const char *receiver,
    const char *hname, long line) {
  size_t hs = 0;
  size_t he = 0;
  if (pv_handler_range(p, hname, &hs, &he)) {
    pv_handle_body(p, receiver, hs, he);
    return;
  }
  if (p->index != NULL) {
    size_t count = 0;
    size_t file = pv_index_lookup(p->index, hname, &count);
    if (count == 1) {
      const char *label = pv_binding_label(p, receiver);
      if (label == NULL) {
        pv_warn_unbound(p, receiver);
        // Chunk C: the body's navs are reported by P1019, not
        // silent, so the sweep must not repeat them as P1025.
        pv_mark_foreign_navs(p, file, hname);
        return;
      }
      pv_scan_foreign_body(p, file, hname,
        ccb_arena_strdup(p->arena, label), NULL, 0);
      return;
    }
    if (count > 1) {
      pv_warn_ambiguous(p, hname, count, line);
      pv_mark_ambiguous_navs(p, hname);
      return;
    }
  }
}

// Attribute one handler body to its button: bound receivers get
// derived edges, unbound receivers get one CCB-P1019 per
// (screen, receiver) and no edge.
void pv_handle_body(pv_parser *p, const char *receiver, size_t start,
    size_t end) {
  const char *label = pv_binding_label(p, receiver);
  if (label == NULL) {
    pv_warn_unbound(p, receiver);
    // Chunk C: the body's navs are reported by P1019, not silent,
    // so the sweep must not repeat them as P1025.
    pv_mark_range_navs(p, start, end);
    return;
  }
  pv_scan_handler_body(p, start, end, ccb_arena_strdup(p->arena, label));
}

// Try to consume a click-handler attachment statement at p->pos.
// Returns 1 when a recognized shape was consumed (edges/warnings
// already handled), 0 when the statement is not a recognized shape
// (the caller falls through to the generic statement handling, and
// p->pos is unchanged). Unrecognized setOnClickListener argument
// shapes (a non-lambda, non-:: argument) fall through the same way.
static int pv_try_handler_attachment(pv_parser *p) {
  size_t k = p->pos;
  char *receiver = NULL;
  char *method = NULL;
  size_t method_idx = 0;
  if (k >= p->ntoks || p->toks[k].kind != PV_TK_IDENT) {
    return 0;
  }
  receiver = p->toks[k].text;
  k++;
  while (k < p->ntoks && pv_is_sym(&p->toks[k], '.')) {
    k++;
    if (k >= p->ntoks || p->toks[k].kind != PV_TK_IDENT) {
      return 0;
    }
    if (pv_is_ident(&p->toks[k], "setOnClickListener") ||
        pv_is_ident(&p->toks[k], "onClick")) {
      method = p->toks[k].text;
      method_idx = k;
      break;
    }
    receiver = p->toks[k].text;
    k++;
  }
  if (method == NULL) {
    return 0;
  }
  if (strcmp(method, "onClick") == 0) {
    size_t brace = 0;
    size_t close = 0;
    if (method_idx + 2 >= p->ntoks ||
        !pv_is_sym(&p->toks[method_idx + 1], '=')) {
      return 0;
    }
    // Chunk B: `onClick = h` where `val h = ::handler` (or `var`)
    // appeared earlier in this screen body. An unrecorded
    // identifier falls through, as today.
    if (p->toks[method_idx + 2].kind == PV_TK_IDENT) {
      char *hname = pv_val_lookup(p, p->toks[method_idx + 2].text);
      if (hname == NULL) {
        return 0;
      }
      p->pos = method_idx + 3;
      pv_handle_named(p, receiver, hname, p->toks[method_idx + 2].line);
      return 1;
    }
    if (!pv_is_sym(&p->toks[method_idx + 2], '{')) {
      return 0;
    }
    brace = method_idx + 2;
    close = pv_match_brace(p, brace);
    if (close == (size_t)-1) {
      pv_broken(p, p->toks[brace].line);
      p->pos = p->ntoks;
      return 1;
    }
    p->pos = close + 1;
    pv_handle_body(p, receiver, brace + 1, close);
    return 1;
  }
  // setOnClickListener: trailing lambda, parenthesized lambda, or a
  // direct method reference.
  if (method_idx + 1 < p->ntoks && pv_is_sym(&p->toks[method_idx + 1], '{')) {
    size_t brace = method_idx + 1;
    size_t close = pv_match_brace(p, brace);
    if (close == (size_t)-1) {
      pv_broken(p, p->toks[brace].line);
      p->pos = p->ntoks;
      return 1;
    }
    p->pos = close + 1;
    pv_handle_body(p, receiver, brace + 1, close);
    return 1;
  }
  if (method_idx + 1 < p->ntoks && pv_is_sym(&p->toks[method_idx + 1], '(')) {
    size_t open = method_idx + 1;
    size_t close = pv_match_paren(p, open);
    if (close == (size_t)-1) {
      pv_broken(p, p->toks[open].line);
      p->pos = p->ntoks;
      return 1;
    }
    if (open + 1 < close && pv_is_sym(&p->toks[open + 1], '{')) {
      size_t brace = open + 1;
      size_t bclose = pv_match_brace(p, brace);
      if (bclose != (size_t)-1 && bclose + 1 == close) {
        p->pos = close + 1;
        pv_handle_body(p, receiver, brace + 1, bclose);
        return 1;
      }
      return 0;
    }
    if (open + 4 == close && pv_is_sym(&p->toks[open + 1], ':') &&
        pv_is_sym(&p->toks[open + 2], ':') &&
        p->toks[open + 3].kind == PV_TK_IDENT) {
      char *hname = p->toks[open + 3].text;
      p->pos = close + 1;
      pv_handle_named(p, receiver, hname, p->toks[open + 3].line);
      return 1;
    }
    // Chunk B: identifier argument, `setOnClickListener(h)` where
    // `val h = ::handler` (or `var`) appeared earlier in this
    // screen body. An unrecorded identifier falls through, as
    // today: it may be a property or a lambda value, not a
    // handler reference.
    if (open + 2 == close && p->toks[open + 1].kind == PV_TK_IDENT) {
      char *hname = pv_val_lookup(p, p->toks[open + 1].text);
      if (hname == NULL) {
        return 0;
      }
      p->pos = close + 1;
      pv_handle_named(p, receiver, hname, p->toks[open + 1].line);
      return 1;
    }
    return 0;
  }
  return 0;
}

// Skip a '{' block without recording anything; used for nested fun
// bodies, which are not UI flow.
static void pv_skip_block(pv_parser *p) {
  if (!pv_at_sym(p, '{')) {
    return;
  }
  size_t close = pv_match_brace(p, p->pos);
  if (close == (size_t)-1) {
    pv_broken(p, pv_cur_line(p));
    p->pos = p->ntoks;
    return;
  }
  p->pos = close + 1;
}

// Scan a '{' block for navs only, without adding boxes.
static void pv_scan_block(pv_parser *p) {
  if (!pv_at_sym(p, '{')) {
    return;
  }
  size_t close = pv_match_brace(p, p->pos);
  if (close == (size_t)-1) {
    pv_broken(p, pv_cur_line(p));
    p->pos = p->ntoks;
    return;
  }
  pv_scan_navs(p, p->pos + 1, close);
  p->pos = close + 1;
}

// Parse the argument list at p->pos ('('): scan it for navs. The first
// top-level string literal is returned as the label, or NULL. When
// owner is not NULL, the label is assigned to it before navs are
// scanned, so edges recorded from the arguments see the label.
static char *pv_parse_args(pv_parser *p, pv_box *owner) {
  if (!pv_at_sym(p, '(')) {
    return NULL;
  }
  size_t open = p->pos;
  size_t close = pv_match_paren(p, open);
  if (close == (size_t)-1) {
    pv_broken(p, pv_cur_line(p));
    p->pos = p->ntoks;
    return NULL;
  }
  char *label = NULL;
  long paren_depth = 1;
  long brace_depth = 0;
  for (size_t i = open + 1; i < close; i++) {
    pv_token *t = &p->toks[i];
    if (pv_is_sym(t, '(')) {
      paren_depth++;
    } else if (pv_is_sym(t, ')')) {
      paren_depth--;
    } else if (pv_is_sym(t, '{')) {
      brace_depth++;
    } else if (pv_is_sym(t, '}')) {
      brace_depth--;
    } else if (t->kind == PV_TK_STRING && paren_depth == 1 &&
        brace_depth == 0 && label == NULL) {
      label = t->text;
    }
  }
  if (owner != NULL) {
    owner->label = label;
  }
  pv_scan_navs(p, open + 1, close);
  p->pos = close + 1;
  return label;
}

// After a call, consume ".name(...)" / ".name { ... }" chains, scanning
// them for navs but adding no boxes.
static void pv_parse_chain(pv_parser *p) {
  while (pv_at_sym(p, '.')) {
    p->pos++;
    if (p->pos < p->ntoks && p->toks[p->pos].kind == PV_TK_IDENT) {
      p->pos++;
    } else {
      break;
    }
    if (pv_at_sym(p, '(')) {
      (void)pv_parse_args(p, NULL);
    }
    if (pv_at_sym(p, '{')) {
      pv_scan_block(p);
    }
  }
}

static void pv_warn_generic(pv_parser *p, const char *name, long line) {
  for (size_t i = 0; i < p->nwarned; i++) {
    if (strcmp(p->warned[i], name) == 0) {
      return;
    }
  }
  if (p->nwarned == p->capwarned) {
    size_t ncap = p->capwarned == 0 ? 8 : p->capwarned * 2;
    p->warned = ccb_xrealloc(p->warned, ncap * sizeof(*p->warned));
    p->capwarned = ncap;
  }
  p->warned[p->nwarned] = ccb_arena_strdup(p->arena, name);
  p->nwarned++;
  ccb_str msg;
  ccb_str_init(&msg);
  ccb_str_appendf(&msg, "unknown Kotlin construct '%s', kept as a generic box", name);
  pv_flow_diag(p, "CCB-P1012", msg.buf != NULL ? msg.buf : "", line);
  ccb_str_free(&msg);
}

// Parse one component/generic call whose name is at p->pos, adding a
// box under parent. Handles args, chains, and an optional trailing
// block. At the depth guard, the call is scanned for navs only.
static void pv_parse_component(pv_parser *p, pv_box *parent) {
  pv_token *name_tok = &p->toks[p->pos];
  char *name = name_tok->text;
  long line = name_tok->line;
  int known = pv_is_known(name);
  int capitalized = name[0] != '\0' && isupper((unsigned char)name[0]);
  char *type = pv_lower_arena(p->arena, name);
  if (!known && capitalized) {
    pv_warn_generic(p, name, line);
  }
  p->pos++;
  if (p->depth >= PV_MAX_DEPTH) {
    pv_broken(p, line);
    if (pv_at_sym(p, '(')) {
      (void)pv_parse_args(p, NULL);
    }
    pv_parse_chain(p);
    if (pv_at_sym(p, '{')) {
      pv_scan_block(p);
    }
    return;
  }
  pv_box *box = pv_box_new(p->arena, type, NULL);
  pv_box_add_child(p->arena, parent, box);
  pv_stack_push(p, box);
  if (pv_at_sym(p, '(')) {
    box->label = pv_parse_args(p, box);
  }
  pv_parse_chain(p);
  if (pv_at_sym(p, '{')) {
    p->depth++;
    pv_parse_block(p, box);
    p->depth--;
  }
  pv_stack_pop(p);
}

// Parse an ignored lowercase call: no box, but navs inside its args,
// chains, and block are still edges.
static void pv_parse_ignored_call(pv_parser *p) {
  p->pos++;
  if (pv_at_sym(p, '(')) {
    (void)pv_parse_args(p, NULL);
  }
  pv_parse_chain(p);
  if (pv_at_sym(p, '{')) {
    if (p->depth >= PV_MAX_DEPTH) {
      pv_scan_block(p);
    } else {
      p->depth++;
      // Block contents are scanned for navs only, not added as boxes.
      // pv_scan_block walks the whole block, including nested calls.
      pv_scan_block(p);
      p->depth--;
    }
  }
}

// Parse one statement under parent (a component call, a branch, a nav,
// an ignored call, or a single skipped token). Bare '{' blocks are
// parsed into the same parent.
void pv_parse_statement(pv_parser *p, pv_box *parent) {
  if (p->pos >= p->ntoks) {
    return;
  }
  pv_token *t = &p->toks[p->pos];
  if (pv_is_sym(t, '{')) {
    if (p->depth >= PV_MAX_DEPTH) {
      pv_scan_block(p);
    } else {
      p->depth++;
      pv_parse_block(p, parent);
      p->depth--;
    }
    return;
  }
  if (t->kind != PV_TK_IDENT) {
    p->pos++;
    return;
  }
  if (pv_is_ident(t, "when") || pv_is_ident(t, "if")) {
    pv_parse_branch(p, parent);
    return;
  }
  if (pv_is_ident(t, "fun")) {
    // A nested function definition is not UI flow: skip its body.
    p->pos++;
    if (p->pos < p->ntoks && p->toks[p->pos].kind == PV_TK_IDENT) {
      p->pos++;
    }
    if (pv_at_sym(p, '(')) {
      size_t close = pv_match_paren(p, p->pos);
      if (close == (size_t)-1) {
        pv_broken(p, pv_cur_line(p));
        p->pos = p->ntoks;
        return;
      }
      p->pos = close + 1;
    }
    if (pv_at_sym(p, '{')) {
      pv_skip_block(p);
    }
    return;
  }
  if (pv_is_nav(p, p->pos)) {
    size_t next = pv_nav_at(p, p->pos);
    p->pos = next;
    if (pv_at_sym(p, '{')) {
      pv_scan_block(p);
    }
    return;
  }
  // Slice 1: a recognized click-handler attachment consumes the
  // whole statement (so the generic scan below cannot also record
  // its nav as an edge from the enclosing component or screen).
  if (pv_try_handler_attachment(p)) {
    return;
  }
  // Chunk B: a local `val`/`var` binding of the plain shape
  // `val NAME = ::HANDLER` records NAME -> HANDLER for later
  // identifier-argument attachments in this screen body. Any
  // other shape (typed form, non-:: right-hand side) falls
  // through to generic handling, unchanged.
  if (pv_is_ident(t, "val") || pv_is_ident(t, "var")) {
    if (p->pos + 5 < p->ntoks &&
        p->toks[p->pos + 1].kind == PV_TK_IDENT &&
        pv_is_sym(&p->toks[p->pos + 2], '=') &&
        pv_is_sym(&p->toks[p->pos + 3], ':') &&
        pv_is_sym(&p->toks[p->pos + 4], ':') &&
        p->toks[p->pos + 5].kind == PV_TK_IDENT) {
      pv_val_record(p, p->toks[p->pos + 1].text,
        p->toks[p->pos + 5].text);
      p->pos += 6;
      return;
    }
  }
  int is_call = p->pos + 1 < p->ntoks &&
    (pv_is_sym(&p->toks[p->pos + 1], '(') || pv_is_sym(&p->toks[p->pos + 1], '{'));
  if (!is_call) {
    p->pos++;
    return;
  }
  if (pv_is_known(t->text) ||
      (t->text[0] != '\0' && isupper((unsigned char)t->text[0]))) {
    pv_parse_component(p, parent);
  } else {
    pv_parse_ignored_call(p);
  }
}

// Parse the block at p->pos ('{') into parent, consuming the closing
// brace. EOF inside the block is unbalanced-brace breakage.
void pv_parse_block(pv_parser *p, pv_box *parent) {
  if (!pv_at_sym(p, '{')) {
    return;
  }
  p->pos++;
  while (p->pos < p->ntoks) {
    if (pv_at_sym(p, '}')) {
      p->pos++;
      return;
    }
    pv_parse_statement(p, parent);
  }
  pv_broken(p, pv_cur_line(p));
}

// Build a branch label: "when <cond>" / "if <cond>", or just the
// keyword when the condition is empty.
char *pv_branch_label(pv_parser *p, const char *keyword, const char *cond) {
  if (cond[0] == '\0') {
    return ccb_arena_strdup(p->arena, keyword);
  }
  size_t n = strlen(keyword) + 1 + strlen(cond);
  char *out = ccb_arena_alloc(p->arena, n + 1);
  memcpy(out, keyword, strlen(keyword));
  out[strlen(keyword)] = ' ';
  memcpy(out + strlen(keyword) + 1, cond, strlen(cond) + 1);
  return out;
}

// Parse a when/if statement whose keyword is at p->pos, adding a
// branch box under parent.
// Condition text in parens at the cursor, or "" when there is none.
// Advances past the closing paren (or to the end on breakage).
static char *pv_take_cond(pv_parser *p) {
  char *cond = ccb_arena_strdup(p->arena, "");
  if (!pv_at_sym(p, '(')) {
    return cond;
  }
  size_t open = p->pos;
  cond = pv_paren_text(p, open);
  size_t close = pv_match_paren(p, open);
  p->pos = close == (size_t)-1 ? p->ntoks : close + 1;
  return cond;
}

// Skip to (and past) the when body's closing '}', after a broken entry.
static void pv_skip_to_close_brace(pv_parser *p) {
  while (p->pos < p->ntoks && !pv_at_sym(p, '}')) {
    p->pos++;
  }
  if (pv_at_sym(p, '}')) {
    p->pos++;
  }
}

// Parse one branch entry's body: a block, or a single statement.
static void pv_parse_entry_body(pv_parser *p, pv_box *entry) {
  if (pv_at_sym(p, '{')) {
    pv_parse_block(p, entry);
  } else if (p->pos < p->ntoks && !pv_at_sym(p, '}')) {
    pv_parse_statement(p, entry);
  }
}

// One when body: entries of "<label> -> <body>" up to the closing '}'.
// The cursor is at the '{' on entry; depth is held for the body.
static void pv_parse_when_body(pv_parser *p, pv_box *branch) {
  p->pos++;
  p->depth++;
  while (p->pos < p->ntoks) {
    if (pv_at_sym(p, '}')) {
      p->pos++;
      break;
    }
    size_t start = p->pos;
    size_t arrow = pv_find_arrow(p, start, p->ntoks);
    if (arrow == (size_t)-1) {
      pv_broken(p, pv_cur_line(p));
      pv_skip_to_close_brace(p);
      break;
    }
    char *entry_label = pv_trim_arena(p->arena, p->text,
      p->toks[start].start, p->toks[arrow].start);
    pv_box *entry = pv_box_new(p->arena,
      ccb_arena_strdup(p->arena, "branch-entry"), entry_label);
    pv_box_add_child(p->arena, branch, entry);
    pv_stack_push(p, entry);
    p->pos = arrow + 1;
    pv_parse_entry_body(p, entry);
    pv_stack_pop(p);
  }
  if (p->pos >= p->ntoks) {
    pv_broken(p, pv_cur_line(p));
  }
  p->depth--;
}

void pv_parse_branch(pv_parser *p, pv_box *parent) {
  char *keyword = p->toks[p->pos].text;
  int is_when = strcmp(keyword, "when") == 0;
  p->pos++;
  char *cond = pv_take_cond(p);
  if (p->depth >= PV_MAX_DEPTH) {
    pv_broken(p, pv_cur_line(p));
    if (pv_at_sym(p, '{')) {
      pv_scan_block(p);
    }
    return;
  }
  pv_box *branch = pv_box_new(p->arena, ccb_arena_strdup(p->arena, "branch"),
    pv_branch_label(p, keyword, cond));
  branch->is_branch = 1;
  pv_box_add_child(p->arena, parent, branch);
  pv_stack_push(p, branch);
  p->depth++;
  if (is_when) {
    if (pv_at_sym(p, '{')) {
      pv_parse_when_body(p, branch);
    }
  } else {
    // if: one "then" entry, then an else chain.
    pv_box *then_entry = pv_box_new(p->arena,
      ccb_arena_strdup(p->arena, "branch-entry"),
      ccb_arena_strdup(p->arena, "then"));
    pv_box_add_child(p->arena, branch, then_entry);
    pv_stack_push(p, then_entry);
    pv_parse_entry_body(p, then_entry);
    pv_stack_pop(p);
    while (pv_at_ident(p, "else")) {
      p->pos++;
      char *else_label = ccb_arena_strdup(p->arena, "else");
      if (pv_at_ident(p, "if")) {
        p->pos++;
        else_label = pv_branch_label(p, "else if", pv_take_cond(p));
      }
      pv_box *else_entry = pv_box_new(p->arena,
        ccb_arena_strdup(p->arena, "branch-entry"), else_label);
      pv_box_add_child(p->arena, branch, else_entry);
      pv_stack_push(p, else_entry);
      pv_parse_entry_body(p, else_entry);
      pv_stack_pop(p);
    }
  }
  p->depth--;
  pv_stack_pop(p);
}

// Append one onClick edge (copied from a scratch screen's edge) to
// the caller's pending list, in the caller's arena.
static void pv_pending_add(pv_parser *p, char *method, pv_edge *e) {
  size_t n = 0;
  pv_pending_edge **next = NULL;
  pv_pending_edge *entry = NULL;
  size_t k = 0;
  if (p->pending_io == NULL || p->npending_io == NULL) {
    return;
  }
  n = *p->npending_io;
  next = ccb_arena_alloc(p->arena, (n + 1) * sizeof(*next));
  for (k = 0; k < n; k++) {
    next[k] = (*p->pending_io)[k];
  }
  entry = ccb_arena_alloc(p->arena, sizeof(*entry));
  memset(entry, 0, sizeof(*entry));
  entry->method = method;
  entry->from = e->from;
  entry->target = e->target;
  entry->unknown = e->unknown;
  entry->args = e->args;
  entry->nargs = e->nargs;
  entry->args_named = e->args_named;
  entry->branch = e->branch;
  next[n] = entry;
  *p->pending_io = next;
  *p->npending_io = n + 1;
}

ccb_status pv_flow_parse(ccb_arena *arena, const char *text, const char *file,
    ccb_diag_list *diags, pv_screen ***screens_io, size_t *nscreens_io) {
  return pv_flow_parse_bindings(arena, text, file, diags, screens_io,
    nscreens_io, NULL, NULL, 0);
}

ccb_status pv_flow_parse_bindings(ccb_arena *arena, const char *text,
    const char *file, ccb_diag_list *diags, pv_screen ***screens_io,
    size_t *nscreens_io, char **bind_keys, char **bind_vals, size_t nbind) {
  return pv_flow_parse_full(arena, text, file, diags, screens_io,
    nscreens_io, bind_keys, bind_vals, nbind, NULL, NULL, 0, NULL, NULL,
    0, NULL, NULL);
}

// Scan an android:onClick method fun's body into the pending list:
// one pending edge per bound label for the method. The cursor is at
// the fun's opening brace; the body runs to close.
static void pv_scan_onclick_handler(pv_parser *p, char *name, size_t close) {
  if (p->pending_io == NULL) {
    return;
  }
  for (size_t k = 0; k < p->nonclick; k++) {
    if (strcmp(p->onclick_methods[k], name) != 0) {
      continue;
    }
    pv_screen *scratch = ccb_arena_alloc(p->arena, sizeof(*scratch));
    pv_screen *saved = p->screen;
    memset(scratch, 0, sizeof(*scratch));
    scratch->name = name;
    p->screen = scratch;
    pv_scan_handler_body(p, p->pos + 1, close,
      ccb_arena_strdup(p->arena, p->onclick_labels[k]));
    p->screen = saved;
    for (size_t e = 0; e < scratch->nedges; e++) {
      pv_pending_add(p, p->onclick_methods[k], &scratch->edges[e]);
    }
  }
}

// A top-level fun that is a handler, not a screen: skip its body
// (scanning onClick bodies into the pending list). Returns 1 when
// the fun was a handler. On a broken body the cursor is left at
// the end, so the caller exits the loop either way.
static int pv_skip_handler_fun(pv_parser *p, char *name, long line) {
  if (!pv_is_handler(p, name)) {
    return 0;
  }
  size_t close = pv_match_brace(p, p->pos);
  if (close == (size_t)-1) {
    pv_broken(p, line);
    p->pos = p->ntoks;
    return 1;
  }
  pv_scan_onclick_handler(p, name, close);
  p->pos = close + 1;
  return 1;
}

// A top-level fun that is a screen: parse its body into a new screen.
static void pv_parse_screen_fun(pv_parser *p, ccb_arena *arena,
    pv_screen ***screens_io, size_t *nscreens_io, char *name) {
  pv_screen *screen = ccb_arena_alloc(arena, sizeof(*screen));
  memset(screen, 0, sizeof(*screen));
  screen->name = name;
  screen->root = pv_box_new(arena, ccb_arena_strdup(arena, "screen"), NULL);
  pv_screen_add(arena, screens_io, nscreens_io, screen);
  p->screen = screen;
  pv_stack_push(p, screen->root);
  // Chunk B: a new screen fun starts with an empty val map;
  // bindings never leak across screens.
  p->nvals = 0;
  pv_parse_block(p, screen->root);
  pv_stack_pop(p);
  p->screen = NULL;
}

// One top-level fun at the cursor: parse it as a screen or skip it
// as a handler. Returns 1 when the cursor was at a fun (handled),
// 0 when it was not a fun at top level. A broken fun leaves the
// cursor at the end (or one past a nameless fun), so the caller
// exits the loop either way.
static int pv_try_top_fun(pv_parser *p, ccb_arena *arena,
    pv_screen ***screens_io, size_t *nscreens_io, long top_depth) {
  if (top_depth != 0 || !pv_is_ident(&p->toks[p->pos], "fun")) {
    return 0;
  }
  if (p->pos + 1 >= p->ntoks || p->toks[p->pos + 1].kind != PV_TK_IDENT) {
    pv_broken(p, pv_cur_line(p));
    p->pos++;
    return 1;
  }
  char *name = p->toks[p->pos + 1].text;
  long line = p->toks[p->pos + 1].line;
  p->pos += 2;
  if (pv_at_sym(p, '(')) {
    // A valid group ending exactly at the end is the same as a
    // broken one here: either way there is no '{' after it, so
    // the fun is broken and the loop ends.
    size_t after = pv_skip_paren(p, p->pos, p->ntoks);
    if (after >= p->ntoks) {
      pv_broken(p, line);
      p->pos = p->ntoks;
      return 1;
    }
    p->pos = after;
  }
  if (!pv_at_sym(p, '{')) {
    pv_broken(p, line);
    return 1;
  }
  // Slice 1: a top-level fun referenced via :: from a screen
  // body is a handler, not a screen. Skip its body; it is
  // scanned only when an attachment references it.
  // Slice 3: the same skip applies to android:onClick method
  // funs, whose bodies are scanned here into the pending list
  // (one pending edge per bound label for the method) for the
  // caller to attach to the screens containing that label.
  if (pv_skip_handler_fun(p, name, line)) {
    return 1;
  }
  pv_parse_screen_fun(p, arena, screens_io, nscreens_io, name);
  return 1;
}

static ccb_status pv_flow_parse_full_inner(ccb_arena *arena,
    const char *text, const char *file, ccb_diag_list *diags,
    pv_screen ***screens_io, size_t *nscreens_io, char **bind_keys,
    char **bind_vals, size_t nbind, char **onclick_methods,
    char **onclick_labels, size_t nonclick, char **graph_keys,
    char **graph_vals, size_t ngraph, pv_pending_edge ***pending_io,
    size_t *npending_io, pv_fun_index *index) {
  if (arena == NULL || screens_io == NULL || nscreens_io == NULL ||
      diags == NULL) {
    return CCB_OK;
  }
  if (*screens_io == NULL) {
    *nscreens_io = 0;
  }
  pv_parser p;
  memset(&p, 0, sizeof(p));
  p.arena = arena;
  p.text = text != NULL ? text : "";
  p.text_len = strlen(p.text);
  p.file = file;
  p.diags = diags;
  p.screens_io = screens_io;
  p.nscreens_io = nscreens_io;
  p.bind_keys = bind_keys;
  p.bind_vals = bind_vals;
  p.nbind = nbind;
  p.onclick_methods = onclick_methods;
  p.onclick_labels = onclick_labels;
  p.nonclick = nonclick;
  p.graph_keys = graph_keys;
  p.graph_vals = graph_vals;
  p.ngraph = ngraph;
  p.index = index;
  p.pending_io = pending_io;
  p.npending_io = npending_io;
  if (p.pending_io != NULL && p.npending_io == NULL) {
    p.pending_io = NULL;
  }
  if (p.pending_io != NULL && *p.pending_io == NULL) {
    *p.npending_io = 0;
  }
  pv_tokenize(&p);
  if (p.unterminated_string) {
    pv_broken(&p, 1);
  }
  // Slice 1 pre-pass: named handlers are funs, not screens.
  pv_collect_funs(&p);
  pv_collect_handlers(&p);
  // Slice 3: a top-level fun named by an android:onClick method is
  // a handler, not a screen, whether or not any screen references
  // it via ::. Added after the :: pruning, so it is never pruned.
  {
    size_t k = 0;
    for (k = 0; k < p.nonclick; k++) {
      pv_handler_add(&p, p.onclick_methods[k]);
    }
  }
  // Chunk C: a top-level fun referenced through the whole-project
  // index — via ::, from a listener lambda, or from a handler body
  // — is a handler, not a screen, in every file that defines it.
  // Added after the per-file pruning, so it is never pruned.
  pv_collect_index_handlers(&p);
  long top_depth = 0;
  while (p.pos < p.ntoks) {
    if (pv_is_sym(&p.toks[p.pos], '{')) {
      top_depth++;
      p.pos++;
      continue;
    }
    if (pv_is_sym(&p.toks[p.pos], '}')) {
      if (top_depth == 0) {
        pv_broken(&p, p.toks[p.pos].line);
      } else {
        top_depth--;
      }
      p.pos++;
      continue;
    }
    if (pv_try_top_fun(&p, arena, screens_io, nscreens_io, top_depth)) {
      continue;
    }
    p.pos++;
  }
  if (top_depth > 0) {
    pv_broken(&p, pv_cur_line(&p));
  }
  free(p.toks);
  free(p.stack);
  free(p.warned);
  free(p.fun_names);
  free(p.fun_braces);
  free(p.fun_closes);
  free(p.handler_names);
  free(p.unbound_screens);
  free(p.unbound_receivers);
  free(p.val_names);
  free(p.val_targets);
  return CCB_OK;
}

ccb_status pv_flow_parse_full(ccb_arena *arena, const char *text,
    const char *file, ccb_diag_list *diags, pv_screen ***screens_io,
    size_t *nscreens_io, char **bind_keys, char **bind_vals, size_t nbind,
    char **onclick_methods, char **onclick_labels, size_t nonclick,
    char **graph_keys, char **graph_vals, size_t ngraph,
    pv_pending_edge ***pending_io, size_t *npending_io) {
  return pv_flow_parse_full_inner(arena, text, file, diags, screens_io,
    nscreens_io, bind_keys, bind_vals, nbind, onclick_methods,
    onclick_labels, nonclick, graph_keys, graph_vals, ngraph,
    pending_io, npending_io, NULL);
}

ccb_status pv_flow_parse_indexed(ccb_arena *arena, const char *text,
    const char *file, ccb_diag_list *diags, pv_screen ***screens_io,
    size_t *nscreens_io, char **bind_keys, char **bind_vals, size_t nbind,
    char **onclick_methods, char **onclick_labels, size_t nonclick,
    char **graph_keys, char **graph_vals, size_t ngraph,
    pv_pending_edge ***pending_io, size_t *npending_io,
    pv_fun_index *index) {
  return pv_flow_parse_full_inner(arena, text, file, diags, screens_io,
    nscreens_io, bind_keys, bind_vals, nbind, onclick_methods,
    onclick_labels, nonclick, graph_keys, graph_vals, ngraph,
    pending_io, npending_io, index);
}
