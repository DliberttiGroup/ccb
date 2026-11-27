#include "ccb_common.h"
#include "ccb_preview.h"
#include <ctype.h>
#include "ccb_preview_flow_internal.h"

// ccb_preview_flow: Kotlin structure parser + logic tree renderer.
// Static structure only: it never executes Kotlin, never evaluates
// conditions, and never crashes on malformed input. Structural
// breakage emits CCB-P1010 and keeps every screen parsed so far.

void pv_flow_diag(pv_parser *p, const char *code, const char *message, long line) {
  ccb_diag d;
  ccb_diag_init(&d);
  ccb_diag_set_full(&d, code, "preview", message, p->file, line, 1,
    NULL, NULL, NULL);
  ccb_diag_list_push(p->diags, &d);
}

void pv_broken(pv_parser *p, long line) {
  if (p->diag_emitted) {
    return;
  }
  p->diag_emitted = 1;
  pv_flow_diag(p, "CCB-P1010", "flow parse error, partial parse kept", line);
}

long pv_cur_line(pv_parser *p) {
  if (p->pos < p->ntoks) {
    return p->toks[p->pos].line;
  }
  if (p->ntoks > 0) {
    return p->toks[p->ntoks - 1].line;
  }
  return 1;
}

// ---------- small helpers ----------

int pv_is_ident(const pv_token *t, const char *name) {
  return t->kind == PV_TK_IDENT && t->text != NULL && strcmp(t->text, name) == 0;
}

int pv_is_sym(const pv_token *t, char c) {
  return t->kind == PV_TK_SYM && t->sym == c;
}

int pv_at_ident(pv_parser *p, const char *name) {
  return p->pos < p->ntoks && pv_is_ident(&p->toks[p->pos], name);
}

int pv_at_sym(pv_parser *p, char c) {
  return p->pos < p->ntoks && pv_is_sym(&p->toks[p->pos], c);
}

static int pv_eq_ci(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
      return 0;
    }
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static const char *pv_known_types[] = {
  "column", "row", "text", "button", "list", "textfield", "image", "box",
  "spacer"
};

int pv_is_known(const char *name) {
  for (size_t i = 0; i < CCB_ARRAY_LEN(pv_known_types); i++) {
    if (pv_eq_ci(name, pv_known_types[i])) {
      return 1;
    }
  }
  return 0;
}

char *pv_lower_arena(ccb_arena *arena, const char *s) {
  size_t n = strlen(s);
  char *out = ccb_arena_alloc(arena, n + 1);
  for (size_t i = 0; i < n; i++) {
    out[i] = (char)tolower((unsigned char)s[i]);
  }
  out[n] = '\0';
  return out;
}

// Trimmed arena copy of source[start..end).
char *pv_trim_arena(ccb_arena *arena, const char *text, size_t start, size_t end) {
  while (start < end && isspace((unsigned char)text[start])) {
    start++;
  }
  while (end > start && isspace((unsigned char)text[end - 1])) {
    end--;
  }
  return ccb_arena_strndup(arena, text + start, end - start);
}

// ---------- arena tree growth ----------

pv_box *pv_box_new(ccb_arena *arena, char *type, char *label) {
  pv_box *box = ccb_arena_alloc(arena, sizeof(*box));
  memset(box, 0, sizeof(*box));
  box->type = type;
  box->label = label;
  return box;
}

void pv_box_add_child(ccb_arena *arena, pv_box *parent, pv_box *child) {
  if (parent->nchildren == parent->capchildren) {
    size_t ncap = parent->capchildren == 0 ? 4 : parent->capchildren * 2;
    pv_box **next = ccb_arena_alloc(arena, ncap * sizeof(*next));
    for (size_t i = 0; i < parent->nchildren; i++) {
      next[i] = parent->children[i];
    }
    parent->children = next;
    parent->capchildren = ncap;
  }
  parent->children[parent->nchildren] = child;
  parent->nchildren++;
}

void pv_screen_add(ccb_arena *arena, pv_screen ***screens_io,
    size_t *nscreens_io, pv_screen *screen) {
  size_t n = *nscreens_io;
  pv_screen **next = ccb_arena_alloc(arena, (n + 1) * sizeof(*next));
  for (size_t i = 0; i < n; i++) {
    next[i] = (*screens_io)[i];
  }
  next[n] = screen;
  *screens_io = next;
  *nscreens_io = n + 1;
}

// True when two edge arg lists match (same values, same named flags).
static int pv_args_equal(char **a, int *a_named, char **b, int *b_named,
    size_t nargs) {
  for (size_t k = 0; k < nargs; k++) {
    int same = (a[k] == NULL && b[k] == NULL) ||
      (a[k] != NULL && b[k] != NULL &&
       strcmp(a[k], b[k]) == 0 && a_named[k] == b_named[k]);
    if (!same) {
      return 0;
    }
  }
  return 1;
}

void pv_edge_add(pv_parser *p, char *from, char *target, int unknown,
    char **args, size_t nargs, int *args_named, int derived, char *branch) {
  pv_screen *screen = p->screen;
  for (size_t i = 0; i < screen->nedges; i++) {
    pv_edge *e = &screen->edges[i];
    int same_target = (e->target == NULL && target == NULL) ||
      (e->target != NULL && target != NULL && strcmp(e->target, target) == 0);
    int same_branch = (e->branch == NULL && branch == NULL) ||
      (e->branch != NULL && branch != NULL && strcmp(e->branch, branch) == 0);
    int same_args = e->nargs == nargs &&
      pv_args_equal(e->args, e->args_named, args, args_named, nargs);
    if (strcmp(e->from, from) == 0 && same_target && e->unknown == unknown &&
        same_args && same_branch) {
      return;
    }
  }
  if (screen->nedges == screen->capedges) {
    size_t ncap = screen->capedges == 0 ? 4 : screen->capedges * 2;
    pv_edge *next = ccb_arena_alloc(p->arena, ncap * sizeof(*next));
    for (size_t i = 0; i < screen->nedges; i++) {
      next[i] = screen->edges[i];
    }
    screen->edges = next;
    screen->capedges = ncap;
  }
  screen->edges[screen->nedges].from = from;
  screen->edges[screen->nedges].target = target;
  screen->edges[screen->nedges].unknown = unknown;
  screen->edges[screen->nedges].args = args;
  screen->edges[screen->nedges].nargs = nargs;
  screen->edges[screen->nedges].args_named = args_named;
  screen->edges[screen->nedges].derived = derived;
  screen->edges[screen->nedges].branch = branch;
  // Chunk 1: no scenario has filled anything yet; the merge sets
  // this (never the parser) so a stale pointer can never leak in.
  screen->edges[screen->nedges].args_scenario = NULL;
  // Chunk 2: same for the trace fields; only the trace merge sets
  // them, and only on edges an applied trace names.
  screen->edges[screen->nedges].args_trace = NULL;
  screen->edges[screen->nedges].trace_taken = 0;
  screen->nedges++;
}

// Enclosing component for a nav edge: the nearest stack box that is a
// real component (not a branch, entry, or screen root); else the screen.
static char *pv_edge_from(pv_parser *p) {
  for (size_t i = p->nstack; i > 0; i--) {
    pv_box *box = p->stack[i - 1];
    if (strcmp(box->type, "branch") == 0 ||
        strcmp(box->type, "branch-entry") == 0 ||
        strcmp(box->type, "screen") == 0) {
      continue;
    }
    return box->label != NULL ? box->label : box->type;
  }
  return p->screen->name;
}

void pv_stack_push(pv_parser *p, pv_box *box) {
  if (p->nstack == p->capstack) {
    size_t ncap = p->capstack == 0 ? 16 : p->capstack * 2;
    p->stack = ccb_xrealloc(p->stack, ncap * sizeof(*p->stack));
    p->capstack = ncap;
  }
  p->stack[p->nstack] = box;
  p->nstack++;
}

void pv_stack_pop(pv_parser *p) {
  if (p->nstack > 0) {
    p->nstack--;
  }
}

// ---------- token matching ----------

// Index of the token matching the '(' at idx, or (size_t)-1.
size_t pv_match_paren(pv_parser *p, size_t idx) {
  long depth = 0;
  for (size_t i = idx; i < p->ntoks; i++) {
    if (pv_is_sym(&p->toks[i], '(')) {
      depth++;
    } else if (pv_is_sym(&p->toks[i], ')')) {
      depth--;
      if (depth == 0) {
        return i;
      }
    }
  }
  return (size_t)-1;
}

// Index of the token matching the '{' at idx, or (size_t)-1.
size_t pv_match_brace(pv_parser *p, size_t idx) {
  long depth = 0;
  for (size_t i = idx; i < p->ntoks; i++) {
    if (pv_is_sym(&p->toks[i], '{')) {
      depth++;
    } else if (pv_is_sym(&p->toks[i], '}')) {
      depth--;
      if (depth == 0) {
        return i;
      }
    }
  }
  return (size_t)-1;
}

// Index past the paren group at idx, when toks[idx] is '('. A token
// that is not '(' (or past end) is left alone; a broken group, or
// one running past end, yields end.
size_t pv_skip_paren(pv_parser *p, size_t i, size_t end) {
  if (i >= end || !pv_is_sym(&p->toks[i], '(')) {
    return i;
  }
  size_t close = pv_match_paren(p, i);
  if (close == (size_t)-1 || close >= end) {
    return end;
  }
  return close + 1;
}

// Index past the brace block at idx, when toks[idx] is '{'. A token
// that is not '{' (or past end) is left alone; an unterminated
// block yields end.
size_t pv_skip_brace(pv_parser *p, size_t i, size_t end) {
  if (i >= end || !pv_is_sym(&p->toks[i], '{')) {
    return i;
  }
  size_t close = pv_match_brace(p, i);
  if (close == (size_t)-1) {
    return end;
  }
  return close + 1;
}

// Index of the top-level '->' in [start, end), stopping at a '}'
// at the top level. (size_t)-1 when there is none.
size_t pv_find_arrow(pv_parser *p, size_t start, size_t end) {
  long paren_depth = 0;
  long brace_depth = 0;
  for (size_t i = start; i < end; i++) {
    pv_token *t = &p->toks[i];
    if (pv_is_sym(t, '(')) {
      paren_depth++;
    } else if (pv_is_sym(t, ')')) {
      paren_depth--;
    } else if (pv_is_sym(t, '{')) {
      brace_depth++;
    } else if (pv_is_sym(t, '}')) {
      if (paren_depth == 0 && brace_depth == 0) {
        break;
      }
      brace_depth--;
    } else if (t->kind == PV_TK_ARROW && paren_depth == 0 && brace_depth == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

// Raw text between the parens starting at idx ('('), trimmed. The
// caller must have verified idx points at '('; on a missing match the
// result is empty and the parser is marked broken.
char *pv_paren_text(pv_parser *p, size_t idx) {
  size_t close = pv_match_paren(p, idx);
  if (close == (size_t)-1) {
    pv_broken(p, p->toks[idx].line);
    return ccb_arena_strdup(p->arena, "");
  }
  return pv_trim_arena(p->arena, p->text, p->toks[idx].end, p->toks[close].start);
}

// ---------- tokenizer ----------

static void pv_tok_push(pv_parser *p, pv_token *tok) {
  if (p->ntoks == p->captoks) {
    size_t ncap = p->captoks == 0 ? 256 : p->captoks * 2;
    p->toks = ccb_xrealloc(p->toks, ncap * sizeof(*p->toks));
    p->captoks = ncap;
  }
  p->toks[p->ntoks] = *tok;
  p->ntoks++;
}

// Skip a /* ... */ comment starting at i (past the opener).
// Returns the index past the comment; marks the parser when
// unterminated.
static size_t pv_skip_block_comment(pv_parser *p, size_t i, long *line) {
  const char *s = p->text;
  int closed = 0;
  while (i < p->text_len) {
    if (s[i] == '\n') {
      (*line)++;
    }
    if (s[i] == '*' && i + 1 < p->text_len && s[i + 1] == '/') {
      i += 2;
      closed = 1;
      break;
    }
    i++;
  }
  if (!closed) {
    p->unterminated_string = 1;
  }
  return i;
}

// One backslash escape inside a string literal.
static void pv_append_escape(ccb_str *decoded, char e) {
  switch (e) {
    case 'n': ccb_str_append_c(decoded, '\n'); break;
    case 't': ccb_str_append_c(decoded, '\t'); break;
    case 'r': ccb_str_append_c(decoded, '\r'); break;
    default: ccb_str_append_c(decoded, e); break;
  }
}

// Scan a string literal starting at i (the opening quote). Pushes
// the STRING token and returns the index past the literal. Marks
// the parser when unterminated.
static size_t pv_scan_string(pv_parser *p, size_t i, long line) {
  const char *s = p->text;
  size_t start = i;
  i++;
  ccb_str decoded;
  ccb_str_init(&decoded);
  int closed = 0;
  while (i < p->text_len) {
    char d = s[i];
    if (d == '"') {
      i++;
      closed = 1;
      break;
    }
    if (d == '\n') {
      break;
    }
    if (d == '\\' && i + 1 < p->text_len && s[i + 1] != '\n') {
      pv_append_escape(&decoded, s[i + 1]);
      i += 2;
      continue;
    }
    ccb_str_append_c(&decoded, d);
    i++;
  }
  if (!closed) {
    p->unterminated_string = 1;
  }
  pv_token tok;
  memset(&tok, 0, sizeof(tok));
  tok.kind = PV_TK_STRING;
  tok.start = start;
  tok.end = i;
  tok.line = line;
  tok.text = ccb_arena_strdup(p->arena, decoded.buf != NULL ? decoded.buf : "");
  ccb_str_free(&decoded);
  pv_tok_push(p, &tok);
  return i;
}

void pv_tokenize(pv_parser *p) {
  const char *s = p->text;
  size_t i = 0;
  long line = 1;
  while (i < p->text_len) {
    char c = s[i];
    if (c == '\n') {
      line++;
      i++;
      continue;
    }
    if (isspace((unsigned char)c)) {
      i++;
      continue;
    }
    if (c == '/' && i + 1 < p->text_len && s[i + 1] == '/') {
      while (i < p->text_len && s[i] != '\n') {
        i++;
      }
      continue;
    }
    if (c == '/' && i + 1 < p->text_len && s[i + 1] == '*') {
      i = pv_skip_block_comment(p, i + 2, &line);
      continue;
    }
    if (c == '"') {
      i = pv_scan_string(p, i, line);
      continue;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      size_t start = i;
      while (i < p->text_len &&
          (isalnum((unsigned char)s[i]) || s[i] == '_')) {
        i++;
      }
      pv_token tok;
      memset(&tok, 0, sizeof(tok));
      tok.kind = PV_TK_IDENT;
      tok.start = start;
      tok.end = i;
      tok.line = line;
      tok.text = ccb_arena_strndup(p->arena, s + start, i - start);
      pv_tok_push(p, &tok);
      continue;
    }
    if (isdigit((unsigned char)c)) {
      size_t start = i;
      while (i < p->text_len && isdigit((unsigned char)s[i])) {
        i++;
      }
      pv_token tok;
      memset(&tok, 0, sizeof(tok));
      tok.kind = PV_TK_NUMBER;
      tok.start = start;
      tok.end = i;
      tok.line = line;
      pv_tok_push(p, &tok);
      continue;
    }
    if (c == '-' && i + 1 < p->text_len && s[i + 1] == '>') {
      pv_token tok;
      memset(&tok, 0, sizeof(tok));
      tok.kind = PV_TK_ARROW;
      tok.start = i;
      tok.end = i + 2;
      tok.line = line;
      pv_tok_push(p, &tok);
      i += 2;
      continue;
    }
    pv_token tok;
    memset(&tok, 0, sizeof(tok));
    tok.kind = PV_TK_SYM;
    tok.start = i;
    tok.end = i + 1;
    tok.line = line;
    tok.sym = c;
    pv_tok_push(p, &tok);
    i++;
  }
}

// ---------- nav scanning ----------

// Classify one nav argument (tokens [start, end)) for the edge's
// args array: a single string literal without '$' is its literal
// text; "name = <string literal without $ | number>" is the raw
// "name = value" text as written, never evaluated; anything else
// (identifiers, templates with '$', expressions, calls) is NULL
// (= unknown). Returns an arena string or NULL.
char *pv_arg_classify(pv_parser *p, size_t start, size_t end,
    int *named_out) {
  if (named_out != NULL) {
    *named_out = 0;
  }
  if (end - start == 1 && p->toks[start].kind == PV_TK_STRING &&
      strchr(p->toks[start].text, '$') == NULL) {
    return p->toks[start].text;
  }
  if (end - start == 3 && p->toks[start].kind == PV_TK_IDENT &&
      pv_is_sym(&p->toks[start + 1], '=') &&
      ((p->toks[start + 2].kind == PV_TK_STRING &&
        strchr(p->toks[start + 2].text, '$') == NULL) ||
       p->toks[start + 2].kind == PV_TK_NUMBER)) {
    if (named_out != NULL) {
      *named_out = 1;
    }
    return pv_trim_arena(p->arena, p->text, p->toks[start].start,
      p->toks[end - 1].end);
  }
  return NULL;
}

// Parse the nav call at token idx (ident + '(') into its target /
// unknown / args outputs and return the index just past its closing
// ')', or (size_t)-1 on breakage. The first argument is the target (a
// single string literal without '$', else an unknown destination);
// the remaining top-level arguments (split by commas at paren depth
// 1 / brace depth 0) become the edge's args. A dynamic target with
// arguments keeps target NULL and unknown true, and still records
// its args. This is the shared parser for flow-declared navs
// (pv_nav_at, attributed to the enclosing component) and Slice 1
// handler navs (attributed to the bound button label).
// The first segment of a nav call's argument list: a plain string
// literal is the target.
static void pv_nav_string_target(pv_parser *p, size_t seg_start, size_t i,
    char **target, int *unknown) {
  if (i - seg_start == 1 &&
      p->toks[seg_start].kind == PV_TK_STRING &&
      strchr(p->toks[seg_start].text, '$') == NULL) {
    *target = p->toks[seg_start].text;
    *unknown = 0;
  }
}

size_t pv_nav_parse(pv_parser *p, size_t idx, char **target_out,
    int *unknown_out, char ***args_out, size_t *nargs_out,
    int **args_named_out) {
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
  if (close == (size_t)-1) {
    pv_broken(p, p->toks[idx].line);
    return (size_t)-1;
  }
  // Split the argument list into top-level segments. Segment 0 is
  // the target; the rest are the edge's args.
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
      // A non-empty segment.
      if (target == NULL && unknown == 1 && nargs == 0 &&
          seg_start == idx + 2) {
        // First segment: the target.
        pv_nav_string_target(p, seg_start, i, &target, &unknown);
      } else {
        // Later segment: an edge argument. The first-segment test
        // above already consumed segment 0 (whether it resolved
        // or not), so every segment after it is an argument; the
        // nargs == 0 guard is only for the very first segment.
        int named = 0;
        char *arg = pv_arg_classify(p, seg_start, i, &named);
        pv_nav_arg_push(p, &args, &nargs, &args_named, arg, named);
      }
    } else if (seg_start == idx + 2 && i == close) {
      // nav() with no arguments: no target, no args.
    } else {
      // An empty later segment (e.g. a trailing comma) is an
      // unknown argument.
      pv_nav_arg_push(p, &args, &nargs, &args_named, NULL, 0);
    }
    seg_start = i + 1;
  }
  if (target_out != NULL) {
    *target_out = target;
  }
  if (unknown_out != NULL) {
    *unknown_out = unknown;
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
  return close + 1;
}

// Record the nav call at token idx (ident + '(') as a flow-declared
// edge from the enclosing component, and return the index just past
// its closing ')', or ntoks on breakage.
size_t pv_nav_at(pv_parser *p, size_t idx) {
  char *target = NULL;
  int unknown = 1;
  char **args = NULL;
  size_t nargs = 0;
  int *args_named = NULL;
  size_t next = pv_nav_parse(p, idx, &target, &unknown, &args, &nargs,
    &args_named);
  if (next == (size_t)-1) {
    return p->ntoks;
  }
  pv_index_mark_nav(p->index, p->file, p->toks[idx].start);
  pv_edge_add(p, pv_edge_from(p), target, unknown, args, nargs,
    args_named, 0, NULL);
  return next;
}

int pv_is_nav(pv_parser *p, size_t idx) {
  if (idx + 1 >= p->ntoks || p->toks[idx].kind != PV_TK_IDENT) {
    return 0;
  }
  if (!pv_is_ident(&p->toks[idx], "nav") &&
      !pv_is_ident(&p->toks[idx], "navigateTo")) {
    return 0;
  }
  return pv_is_sym(&p->toks[idx + 1], '(');
}

// Scan tokens [start, end) for nav calls, recording edges. Nav argument
// lists are skipped so a nested nav inside an argument is not double
// counted; everything else is walked through, so navs inside lambdas
// and nested calls are still found.
void pv_scan_navs(pv_parser *p, size_t start, size_t end) {
  size_t i = start;
  while (i < end) {
    if (pv_is_nav(p, i)) {
      size_t next = pv_nav_at(p, i);
      if (next <= i) {
        break;
      }
      i = next;
      if (i > end) {
        i = end;
      }
      continue;
    }
    i++;
  }
}
