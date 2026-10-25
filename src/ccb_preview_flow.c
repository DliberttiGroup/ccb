// ccb_preview_flow: Kotlin structure parser + logic tree renderer.
// Static structure only: it never executes Kotlin, never evaluates
// conditions, and never crashes on malformed input. Structural
// breakage emits CCB-P1010 and keeps every screen parsed so far.
#include "ccb_common.h"
#include "ccb_preview.h"

#include <ctype.h>

#define PV_MAX_DEPTH 128

typedef enum {
  PV_TK_IDENT,
  PV_TK_STRING,
  PV_TK_NUMBER,
  PV_TK_SYM,
  PV_TK_ARROW
} pv_tk_kind;

typedef struct {
  pv_tk_kind kind;
  size_t start, end; // offsets into the source text
  long line;
  char *text; // arena string for identifiers and string literals, else NULL
  char sym; // for PV_TK_SYM
} pv_token;

typedef struct {
  ccb_arena *arena;
  const char *text;
  size_t text_len;
  const char *file;
  ccb_diag_list *diags;
  pv_screen ***screens_io;
  size_t *nscreens_io;
  pv_token *toks;
  size_t ntoks, captoks;
  size_t pos;
  pv_box **stack; // transient, xmalloc'd; boxes themselves are arena-owned
  size_t nstack, capstack;
  pv_screen *screen; // screen currently being parsed, NULL at top level
  int diag_emitted; // a CCB-P1010 diagnostic was already emitted for this file
  int unterminated_string;
  char **warned; // generic names already warned about, this file
  size_t nwarned, capwarned;
  long depth; // recursion guard for nested blocks
  // Slice 1 bindings (borrowed from the caller, never owned here).
  char **bind_keys;
  char **bind_vals;
  size_t nbind;
  // Slice 1 pre-pass: every top-level fun in this file (name +
  // brace-token range of its body), and the subset referenced via
  // setOnClickListener(::name) from a screen body. Handler funs are
  // not screens; their bodies are scanned only when referenced.
  // Names point at token text (arena); the arrays are xmalloc'd
  // and freed at the end of pv_flow_parse.
  char **fun_names;
  size_t *fun_braces, *fun_closes;
  size_t nfuns, capfuns;
  char **handler_names;
  size_t nhandlers, caphandlers;
  // CCB-P1019 dedup: (screen, receiver) pairs already warned about,
  // this file. Strings are arena-owned; arrays are xmalloc'd.
  char **unbound_screens, **unbound_receivers;
  size_t nunbound, capunbound;
  // Slice 3 inputs (borrowed from the caller, never owned here).
  // onclick_methods/labels: android:onClick method name -> bound
  // component label (already resolved through [preview.bindings] by
  // the caller). graph_keys/vals: navigation destination id ->
  // screen name, from the nav-graph file(s).
  char **onclick_methods, **onclick_labels;
  size_t nonclick;
  char **graph_keys, **graph_vals;
  size_t ngraph;
  // Slice 3 output: onClick edges pending screen attachment, grown
  // in the caller's arena (NULL when the caller wants none).
  pv_pending_edge ***pending_io;
  size_t *npending_io;
  // Chunk B: whole-project function index (borrowed, NULL means
  // per-file behavior). Cross-file method references and helper
  // calls resolve through it; see pv_fun_index in ccb_preview.h.
  pv_fun_index *index;
  // Chunk B: helper-follow depth. 0 while scanning a handler body
  // (helper calls are followed one level); 1 while scanning a
  // followed helper's body (calls inside it are NOT followed, so
  // every helper chain terminates).
  int helper_depth;
  // Chunk B: local `val`/`var` bindings in the current screen fun
  // body: `val NAME = ::HANDLER` statements, in order. Reset at
  // each new screen fun. Names and targets point at token text
  // (arena); the arrays are xmalloc'd and freed at the end of the
  // parse. Only the plain shape is recorded; a typed form
  // (`val h: Handler = ::onClick`) is not, and keeps the old
  // behavior (no edge, no warning).
  char **val_names, **val_targets;
  size_t nvals, capvals;
} pv_parser;

static void pv_diag(pv_parser *p, const char *code, const char *message, long line) {
  ccb_diag d;
  ccb_diag_init(&d);
  ccb_diag_set_full(&d, code, "preview", message, p->file, line, 1,
    NULL, NULL, NULL);
  ccb_diag_list_push(p->diags, &d);
}

static void pv_broken(pv_parser *p, long line) {
  if (p->diag_emitted) {
    return;
  }
  p->diag_emitted = 1;
  pv_diag(p, "CCB-P1010", "flow parse error, partial parse kept", line);
}

static long pv_cur_line(pv_parser *p) {
  if (p->pos < p->ntoks) {
    return p->toks[p->pos].line;
  }
  if (p->ntoks > 0) {
    return p->toks[p->ntoks - 1].line;
  }
  return 1;
}

// ---------- small helpers ----------

static int pv_is_ident(const pv_token *t, const char *name) {
  return t->kind == PV_TK_IDENT && t->text != NULL && strcmp(t->text, name) == 0;
}

static int pv_is_sym(const pv_token *t, char c) {
  return t->kind == PV_TK_SYM && t->sym == c;
}

static int pv_at_ident(pv_parser *p, const char *name) {
  return p->pos < p->ntoks && pv_is_ident(&p->toks[p->pos], name);
}

static int pv_at_sym(pv_parser *p, char c) {
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

static int pv_is_known(const char *name) {
  for (size_t i = 0; i < CCB_ARRAY_LEN(pv_known_types); i++) {
    if (pv_eq_ci(name, pv_known_types[i])) {
      return 1;
    }
  }
  return 0;
}

static char *pv_lower_arena(ccb_arena *arena, const char *s) {
  size_t n = strlen(s);
  char *out = ccb_arena_alloc(arena, n + 1);
  for (size_t i = 0; i < n; i++) {
    out[i] = (char)tolower((unsigned char)s[i]);
  }
  out[n] = '\0';
  return out;
}

// Trimmed arena copy of source[start..end).
static char *pv_trim_arena(ccb_arena *arena, const char *text, size_t start, size_t end) {
  while (start < end && isspace((unsigned char)text[start])) {
    start++;
  }
  while (end > start && isspace((unsigned char)text[end - 1])) {
    end--;
  }
  return ccb_arena_strndup(arena, text + start, end - start);
}

// ---------- arena tree growth ----------

static pv_box *pv_box_new(ccb_arena *arena, char *type, char *label) {
  pv_box *box = ccb_arena_alloc(arena, sizeof(*box));
  memset(box, 0, sizeof(*box));
  box->type = type;
  box->label = label;
  return box;
}

static void pv_box_add_child(ccb_arena *arena, pv_box *parent, pv_box *child) {
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

static void pv_screen_add(ccb_arena *arena, pv_screen ***screens_io,
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

static void pv_edge_add(pv_parser *p, char *from, char *target, int unknown,
    char **args, size_t nargs, int *args_named, int derived, char *branch) {
  pv_screen *screen = p->screen;
  for (size_t i = 0; i < screen->nedges; i++) {
    pv_edge *e = &screen->edges[i];
    int same_target = (e->target == NULL && target == NULL) ||
      (e->target != NULL && target != NULL && strcmp(e->target, target) == 0);
    int same_branch = (e->branch == NULL && branch == NULL) ||
      (e->branch != NULL && branch != NULL && strcmp(e->branch, branch) == 0);
    int same_args = e->nargs == nargs;
    size_t k = 0;
    if (same_args) {
      for (k = 0; k < nargs; k++) {
        int same = (e->args[k] == NULL && args[k] == NULL) ||
          (e->args[k] != NULL && args[k] != NULL &&
           strcmp(e->args[k], args[k]) == 0 &&
           e->args_named[k] == args_named[k]);
        if (!same) {
          same_args = 0;
          break;
        }
      }
    }
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

static void pv_stack_push(pv_parser *p, pv_box *box) {
  if (p->nstack == p->capstack) {
    size_t ncap = p->capstack == 0 ? 16 : p->capstack * 2;
    p->stack = ccb_xrealloc(p->stack, ncap * sizeof(*p->stack));
    p->capstack = ncap;
  }
  p->stack[p->nstack] = box;
  p->nstack++;
}

static void pv_stack_pop(pv_parser *p) {
  if (p->nstack > 0) {
    p->nstack--;
  }
}

// ---------- token matching ----------

// Index of the token matching the '(' at idx, or (size_t)-1.
static size_t pv_match_paren(pv_parser *p, size_t idx) {
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
static size_t pv_match_brace(pv_parser *p, size_t idx) {
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

// Raw text between the parens starting at idx ('('), trimmed. The
// caller must have verified idx points at '('; on a missing match the
// result is empty and the parser is marked broken.
static char *pv_paren_text(pv_parser *p, size_t idx) {
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

static void pv_tokenize(pv_parser *p) {
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
      i += 2;
      int closed = 0;
      while (i < p->text_len) {
        if (s[i] == '\n') {
          line++;
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
      continue;
    }
    if (c == '"') {
      size_t start = i;
      long tok_line = line;
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
          char e = s[i + 1];
          switch (e) {
            case 'n': ccb_str_append_c(&decoded, '\n'); break;
            case 't': ccb_str_append_c(&decoded, '\t'); break;
            case 'r': ccb_str_append_c(&decoded, '\r'); break;
            default: ccb_str_append_c(&decoded, e); break;
          }
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
      tok.line = tok_line;
      tok.text = ccb_arena_strdup(p->arena, decoded.buf != NULL ? decoded.buf : "");
      ccb_str_free(&decoded);
      pv_tok_push(p, &tok);
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
static char *pv_arg_classify(pv_parser *p, size_t start, size_t end,
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
static size_t pv_nav_parse(pv_parser *p, size_t idx, char **target_out,
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
        if (i - seg_start == 1 &&
            p->toks[seg_start].kind == PV_TK_STRING &&
            strchr(p->toks[seg_start].text, '$') == NULL) {
          target = p->toks[seg_start].text;
          unknown = 0;
        }
      } else {
        // Later segment: an edge argument. The first-segment test
        // above already consumed segment 0 (whether it resolved
        // or not), so every segment after it is an argument; the
        // nargs == 0 guard is only for the very first segment.
        int named = 0;
        char *arg = pv_arg_classify(p, seg_start, i, &named);
        char **next = NULL;
        int *next_named = NULL;
        next = ccb_arena_alloc(p->arena, (nargs + 1) * sizeof(*next));
        next_named = ccb_arena_alloc(p->arena,
          (nargs + 1) * sizeof(*next_named));
        for (size_t k = 0; k < nargs; k++) {
          next[k] = args[k];
          next_named[k] = args_named[k];
        }
        next[nargs] = arg;
        next_named[nargs] = named;
        args = next;
        args_named = next_named;
        nargs++;
      }
    } else if (seg_start == idx + 2 && i == close) {
      // nav() with no arguments: no target, no args.
    } else {
      // An empty later segment (e.g. a trailing comma) is an
      // unknown argument.
      char **next = ccb_arena_alloc(p->arena, (nargs + 1) * sizeof(*next));
      int *next_named = ccb_arena_alloc(p->arena,
        (nargs + 1) * sizeof(*next_named));
      for (size_t k = 0; k < nargs; k++) {
        next[k] = args[k];
        next_named[k] = args_named[k];
      }
      next[nargs] = NULL;
      next_named[nargs] = 0;
      args = next;
      args_named = next_named;
      nargs++;
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

// Chunk C: record one attributed nav call site (file label + token
// start offset) in the whole-project index, so the post-parse sweep
// can tell attributed navs apart from dropped ones. Defined with the
// index, below; the forward declaration lets the nav consumers mark.
static void pv_index_mark_nav(pv_fun_index *index, const char *file,
  size_t start);

// Record the nav call at token idx (ident + '(') as a flow-declared
// edge from the enclosing component, and return the index just past
// its closing ')', or ntoks on breakage.
static size_t pv_nav_at(pv_parser *p, size_t idx) {
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

static int pv_is_nav(pv_parser *p, size_t idx) {
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
static void pv_scan_navs(pv_parser *p, size_t start, size_t end) {
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

static const char *pv_binding_label(pv_parser *p, const char *receiver) {
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
static void pv_val_record(pv_parser *p, char *name, char *target) {
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

static char *pv_val_lookup(pv_parser *p, const char *name) {
  size_t i = 0;
  for (i = 0; i < p->nvals; i++) {
    if (strcmp(p->val_names[i], name) == 0) {
      return p->val_targets[i];
    }
  }
  return NULL;
}

static int pv_is_handler(pv_parser *p, const char *name) {
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

static void pv_handler_add(pv_parser *p, char *name) {
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
static void pv_collect_funs(pv_parser *p) {
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
      char *name = p->toks[i + 1].text;
      size_t j = i + 2;
      if (j < p->ntoks && pv_is_sym(&p->toks[j], '(')) {
        size_t pc = pv_match_paren(p, j);
        if (pc != (size_t)-1 && pc + 1 < p->ntoks &&
            pv_is_sym(&p->toks[pc + 1], '{')) {
          size_t brace = pc + 1;
          size_t bc = pv_match_brace(p, brace);
          if (bc != (size_t)-1) {
            pv_fun_add(p, name, brace, bc);
            i = bc + 1;
            continue;
          }
        }
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
static void pv_collect_handlers(pv_parser *p) {
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
      int kept = 0;
      for (f = 0; f < p->nfuns; f++) {
        if (pv_is_handler(p, p->fun_names[f])) {
          continue;
        }
        if (pv_refers_to(p, p->fun_braces[f] + 1, p->fun_closes[f],
            p->handler_names[h])) {
          kept = 1;
          break;
        }
      }
      if (kept) {
        h++;
      } else {
        for (size_t k = h + 1; k < p->nhandlers; k++) {
          p->handler_names[k - 1] = p->handler_names[k];
        }
        p->nhandlers--;
        changed = 1;
      }
    }
  }
}

// Body token range (brace+1, close) of the top-level fun named name,
// or 0 when there is none.
static int pv_handler_range(pv_parser *p, const char *name, size_t *start_out,
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

// ---------- Chunk B: whole-project function index ----------
//
// The index maps every top-level fun name to the flow file(s)
// holding it, over borrowed (label, text) pairs. It is built once
// per project load, before any file is parsed; the texts must
// outlive every parse that uses the index. Lookup counts DISTINCT
// files, not definitions: two definitions in one file behave like
// the same-file first match, while one definition in each of two
// files is ambiguous.

struct pv_fun_index {
  ccb_arena *arena;
  char **labels; // borrowed, never freed here
  char **texts; // borrowed, never freed here
  size_t nfiles;
  char **fun_names; // arena-owned (token text)
  size_t *fun_files; // xmalloc'd, freed by pv_fun_index_free
  size_t nfuns, capfuns;
  // CCB-P1024 dedup: (referencing file label, name) pairs already
  // warned about, arena-grown.
  char **warned_files, **warned_names;
  size_t nwarned, capwarned;
  // Chunk C: attributed nav call sites, (file index, token start
  // offset) pairs, xmalloc'd. A parse marks a site when it consumes
  // a nav call as an edge; the post-parse sweep warns for every
  // other nav call in the index files, so no nav stays silent.
  size_t *nav_files, *nav_starts;
  size_t nnav, capnav;
};

pv_fun_index *pv_fun_index_build(ccb_arena *arena, char **labels,
    char **texts, size_t nfiles) {
  pv_fun_index *index = NULL;
  size_t f = 0;
  if (arena == NULL || nfiles == 0) {
    return NULL;
  }
  index = ccb_xmalloc(sizeof(*index));
  memset(index, 0, sizeof(*index));
  index->arena = arena;
  index->labels = labels;
  index->texts = texts;
  index->nfiles = nfiles;
  for (f = 0; f < nfiles; f++) {
    pv_parser q;
    size_t g = 0;
    if (texts[f] == NULL) {
      continue;
    }
    memset(&q, 0, sizeof(q));
    q.arena = arena;
    q.text = texts[f];
    q.text_len = strlen(texts[f]);
    q.file = labels != NULL ? labels[f] : NULL;
    pv_tokenize(&q);
    pv_collect_funs(&q);
    for (g = 0; g < q.nfuns; g++) {
      if (index->nfuns == index->capfuns) {
        size_t ncap = index->capfuns == 0 ? 16 : index->capfuns * 2;
        index->fun_names = ccb_xrealloc(index->fun_names,
          ncap * sizeof(*index->fun_names));
        index->fun_files = ccb_xrealloc(index->fun_files,
          ncap * sizeof(*index->fun_files));
        index->capfuns = ncap;
      }
      index->fun_names[index->nfuns] = q.fun_names[g];
      index->fun_files[index->nfuns] = f;
      index->nfuns++;
    }
    free(q.toks);
    free(q.fun_names);
    free(q.fun_braces);
    free(q.fun_closes);
  }
  return index;
}

void pv_fun_index_free(pv_fun_index *index) {
  if (index == NULL) {
    return;
  }
  free(index->fun_files);
  free(index->fun_names);
  free(index->nav_files);
  free(index->nav_starts);
  free(index);
}

// Chunk C: record one attributed nav call site. The file label
// resolves to an index file by string equality (labels are borrowed
// paths, shared with the parses); an unknown label or a NULL index
// records nothing, never crashes. Duplicate sites are stored once:
// a handler scanned from two buttons attributes the same call twice.
static void pv_index_mark_nav(pv_fun_index *index, const char *file,
    size_t start) {
  size_t f = 0;
  size_t i = 0;
  if (index == NULL || file == NULL) {
    return;
  }
  for (f = 0; f < index->nfiles; f++) {
    if (index->labels[f] != NULL && strcmp(index->labels[f], file) == 0) {
      break;
    }
  }
  if (f >= index->nfiles) {
    return;
  }
  for (i = 0; i < index->nnav; i++) {
    if (index->nav_files[i] == f && index->nav_starts[i] == start) {
      return;
    }
  }
  if (index->nnav == index->capnav) {
    size_t ncap = index->capnav == 0 ? 16 : index->capnav * 2;
    index->nav_files = ccb_xrealloc(index->nav_files,
      ncap * sizeof(*index->nav_files));
    index->nav_starts = ccb_xrealloc(index->nav_starts,
      ncap * sizeof(*index->nav_starts));
    index->capnav = ncap;
  }
  index->nav_files[index->nnav] = f;
  index->nav_starts[index->nnav] = start;
  index->nnav++;
}

// True when the token at i starts a nav-ish call: nav/navigateTo,
// startActivity, or NavController navigate(), each followed by '('.
// The same shape the handler scanner consumes; the sweep uses it to
// find every nav call, attributed or not.
static int pv_is_nav_call(pv_token *toks, size_t ntoks, size_t i) {
  pv_token *t = NULL;
  if (i + 1 >= ntoks || toks[i].kind != PV_TK_IDENT) {
    return 0;
  }
  t = &toks[i];
  if (t->text == NULL) {
    return 0;
  }
  if (strcmp(t->text, "nav") != 0 && strcmp(t->text, "navigateTo") != 0 &&
      strcmp(t->text, "startActivity") != 0 &&
      strcmp(t->text, "navigate") != 0) {
    return 0;
  }
  return toks[i + 1].kind == PV_TK_SYM && toks[i + 1].sym == '(';
}

// Chunk C: mark every nav call in a token range of the current
// parser as accounted for, so the sweep stays quiet for it. Used
// when a handler body is deliberately not scanned: an unbound or
// ambiguous handler's navs are already reported by CCB-P1019 or
// CCB-P1024, so repeating them as CCB-P1025 would be noise, not
// news. The navs are reported, just not as edges.
static void pv_mark_range_navs(pv_parser *p, size_t start, size_t end) {
  size_t k = 0;
  for (k = start; k < end; k++) {
    if (pv_is_nav_call(p->toks, p->ntoks, k)) {
      pv_index_mark_nav(p->index, p->file, p->toks[k].start);
    }
  }
}

// Same, for a foreign fun body: tokenize the index file's text,
// find the fun, mark its body's nav calls with the foreign label.
static void pv_mark_foreign_navs(pv_parser *p, size_t file_idx,
    const char *name) {
  pv_parser q;
  size_t i = 0;
  if (p->index == NULL || file_idx >= p->index->nfiles ||
      p->index->texts[file_idx] == NULL) {
    return;
  }
  memset(&q, 0, sizeof(q));
  q.arena = p->arena;
  q.text = p->index->texts[file_idx];
  q.text_len = strlen(q.text);
  q.file = p->index->labels[file_idx];
  pv_tokenize(&q);
  pv_collect_funs(&q);
  for (i = 0; i < q.nfuns; i++) {
    size_t k = 0;
    if (strcmp(q.fun_names[i], name) != 0) {
      continue;
    }
    for (k = q.fun_braces[i] + 1; k < q.fun_closes[i]; k++) {
      if (pv_is_nav_call(q.toks, q.ntoks, k)) {
        pv_index_mark_nav(p->index, q.file, q.toks[k].start);
      }
    }
    break;
  }
  free(q.toks);
  free(q.fun_names);
  free(q.fun_braces);
  free(q.fun_closes);
}

// Chunk C: warn CCB-P1025 for every nav call in the index files
// that no parse attributed to an edge. The tokenizer skips comments
// and strings, so only real calls are considered; each unattributed
// site warns exactly once, whatever the scan count, because the
// sweep is a single pass over the files and attribution is a set.
// Runs after all files are parsed and before the index is freed;
// a NULL index or diag list is a no-op, so index-less parses keep
// the exact old behavior.
void pv_sweep_unattributed_navs(pv_fun_index *index,
    ccb_diag_list *diags) {
  size_t f = 0;
  if (index == NULL || diags == NULL) {
    return;
  }
  for (f = 0; f < index->nfiles; f++) {
    pv_parser q;
    size_t i = 0;
    if (index->texts[f] == NULL) {
      continue;
    }
    memset(&q, 0, sizeof(q));
    q.arena = index->arena;
    q.text = index->texts[f];
    q.text_len = strlen(q.text);
    q.file = index->labels[f];
    pv_tokenize(&q);
    for (i = 0; i < q.ntoks; i++) {
      size_t k = 0;
      int found = 0;
      if (!pv_is_nav_call(q.toks, q.ntoks, i)) {
        continue;
      }
      for (k = 0; k < index->nnav; k++) {
        if (index->nav_files[k] == f &&
            index->nav_starts[k] == q.toks[i].start) {
          found = 1;
          break;
        }
      }
      if (!found) {
        ccb_diag d;
        ccb_diag_init(&d);
        ccb_diag_set_full(&d, "CCB-P1025", "preview",
          "unattributed navigation: this nav call is not recorded "
          "as an edge from any button (nested block, skipped "
          "statement, or helper never scanned from a button); it is "
          "reported here so it is not silently dropped",
          q.file, q.toks[i].line, 1, NULL, NULL, NULL);
        ccb_diag_list_push(diags, &d);
      }
    }
    free(q.toks);
  }
}

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
typedef struct {
  pv_parser q;
} pv_cand_file;

// (file index, fun name) pairs: the global handler set. Names point
// at arena token text and are never freed here; the arrays are
// xmalloc'd and freed by the caller.
typedef struct {
  size_t *files;
  char **names;
  size_t n, cap;
} pv_cand_pairs;

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
      if (pv_cand_is_bare_call(q->toks, q->ntoks, k)) {
        if (pv_cand_add_name(index, set, q->toks[k].text, file, fun)) {
          added = 1;
        }
      }
      continue;
    }
    if (pv_is_ident(&q->toks[k], "setOnClickListener") ||
        pv_is_ident(&q->toks[k], "onClick")) {
      size_t brace = 0;
      size_t close = 0;
      size_t j = 0;
      if (!pv_cand_lambda_block(q, k, end, &brace, &close)) {
        continue;
      }
      for (j = brace + 1; j < close; j++) {
        if (pv_cand_is_bare_call(q->toks, q->ntoks, j)) {
          if (pv_cand_add_name(index, set, q->toks[j].text, file,
              fun)) {
            added = 1;
          }
        }
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
      pv_parser *q = &files[set.files[s]].q;
      size_t g = 0;
      for (g = 0; g < q->nfuns; g++) {
        if (strcmp(q->fun_names[g], set.names[s]) != 0) {
          continue;
        }
        if (pv_cand_scan_body(q, set.files[s], q->fun_names[g],
            q->fun_braces[g] + 1, q->fun_closes[g], index, &set, 1)) {
          changed = 1;
        }
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

// Count distinct files defining name; return the first file index
// (or (size_t)-1 when absent). Count goes in *count_out.
static size_t pv_index_lookup(pv_fun_index *index, const char *name,
    size_t *count_out) {
  size_t first = (size_t)-1;
  size_t count = 0;
  size_t f = 0;
  size_t i = 0;
  for (f = 0; f < index->nfiles; f++) {
    for (i = 0; i < index->nfuns; i++) {
      if (index->fun_files[i] == f &&
          strcmp(index->fun_names[i], name) == 0) {
        if (first == (size_t)-1) {
          first = f;
        }
        count++;
        break;
      }
    }
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
  pv_diag(p, "CCB-P1024", msg.buf != NULL ? msg.buf : "", line);
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
  pv_diag(p, "CCB-P1019", msg.buf != NULL ? msg.buf : "", pv_cur_line(p));
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
    if (i < end && pv_is_sym(&p->toks[i], '(')) {
      size_t close = pv_match_paren(p, i);
      if (close == (size_t)-1 || close >= end) {
        return end;
      }
      i = close + 1;
    }
    i = pv_skip_stmt(p, i, end);
    if (i < end && pv_is_ident(&p->toks[i], "else")) {
      i++;
      i = pv_skip_stmt(p, i, end);
    }
    return i;
  }
  if (pv_is_ident(&p->toks[i], "when")) {
    i++;
    if (i < end && pv_is_sym(&p->toks[i], '(')) {
      size_t close = pv_match_paren(p, i);
      if (close == (size_t)-1 || close >= end) {
        return end;
      }
      i = close + 1;
    }
    if (i < end && pv_is_sym(&p->toks[i], '{')) {
      size_t close = pv_match_brace(p, i);
      if (close == (size_t)-1) {
        return end;
      }
      return close + 1;
    }
    return i;
  }
  if (p->toks[i].kind == PV_TK_IDENT) {
    while (i < end && p->toks[i].kind == PV_TK_IDENT) {
      i++;
      if (i < end && pv_is_sym(&p->toks[i], '(')) {
        size_t close = pv_match_paren(p, i);
        if (close == (size_t)-1 || close >= end) {
          return end;
        }
        i = close + 1;
      }
      if (i < end && pv_is_sym(&p->toks[i], '.')) {
        i++;
        continue;
      }
      break;
    }
    if (i < end && pv_is_sym(&p->toks[i], '{')) {
      size_t close = pv_match_brace(p, i);
      if (close == (size_t)-1) {
        return end;
      }
      return close + 1;
    }
    return i;
  }
  return i + 1;
}

static char *pv_branch_label(pv_parser *p, const char *keyword,
  const char *cond);

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
        if (i - seg_start == 1 &&
            p->toks[seg_start].kind == PV_TK_STRING &&
            strchr(p->toks[seg_start].text, '$') == NULL) {
          target = p->toks[seg_start].text;
          unknown = 0;
        } else if (i - seg_start == 5 &&
            pv_is_ident(&p->toks[seg_start], "R") &&
            pv_is_sym(&p->toks[seg_start + 1], '.') &&
            pv_is_ident(&p->toks[seg_start + 2], "id") &&
            pv_is_sym(&p->toks[seg_start + 3], '.') &&
            p->toks[seg_start + 4].kind == PV_TK_IDENT) {
          // Graph lookup: an unlisted id is unknown, never guessed.
          size_t g = 0;
          for (g = 0; g < p->ngraph; g++) {
            if (strcmp(p->graph_keys[g], p->toks[seg_start + 4].text) == 0) {
              target = p->graph_vals[g];
              unknown = 0;
              break;
            }
          }
        }
      } else {
        int named = 0;
        char *arg = pv_arg_classify(p, seg_start, i, &named);
        char **next = ccb_arena_alloc(p->arena,
          (nargs + 1) * sizeof(*next));
        int *next_named = ccb_arena_alloc(p->arena,
          (nargs + 1) * sizeof(*next_named));
        for (size_t k = 0; k < nargs; k++) {
          next[k] = args[k];
          next_named[k] = args_named[k];
        }
        next[nargs] = arg;
        next_named[nargs] = named;
        args = next;
        args_named = next_named;
        nargs++;
      }
    } else if (!first) {
      // An empty later segment (e.g. a trailing comma) is an
      // unknown argument.
      char **next = ccb_arena_alloc(p->arena,
        (nargs + 1) * sizeof(*next));
      int *next_named = ccb_arena_alloc(p->arena,
        (nargs + 1) * sizeof(*next_named));
      for (size_t k = 0; k < nargs; k++) {
        next[k] = args[k];
        next_named[k] = args_named[k];
      }
      next[nargs] = NULL;
      next_named[nargs] = 0;
      args = next;
      args_named = next_named;
      nargs++;
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

static void pv_scan_handler_branch(pv_parser *p, size_t start, size_t end,
    char *from_label, char *branch);
static void pv_scan_if(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *outer, int is_else_if);
static void pv_scan_when(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *outer);
// Chunk B: scan a foreign file's fun body with a scratch parser
// (defined after pv_scan_handler_body); helper_depth 0 scans a
// handler body, 1 scans a followed helper body.
static void pv_scan_foreign_body(pv_parser *p, size_t file_idx,
    const char *name, char *from_label, char *branch, int helper_depth);
// Chunk B: attribute one handler body range to a button (defined
// after pv_scan_handler_branch).
static void pv_handle_body(pv_parser *p, const char *receiver,
    size_t start, size_t end);
// Chunk B: try to consume a bare helper call at token i (defined
// before pv_scan_handler_branch); returns the index past the call,
// or 0 when token i is not a followed helper call.
static size_t pv_try_helper_call(pv_parser *p, size_t i, size_t end,
    char *from_label, char *branch);

// Scan one if/else-if statement whose `if` is at *i_io: the then
// body (braced or a single braceless statement) and the else chain,
// each annotated with its branch label. Advances *i_io past the
// whole statement, or to end on breakage.
static void pv_scan_if(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *outer, int is_else_if) {
  size_t i = *i_io;
  char *cond = ccb_arena_strdup(p->arena, "");
  char *base = NULL;
  char *then_label = NULL;
  i++;
  if (i < end && pv_is_sym(&p->toks[i], '(')) {
    size_t close = pv_match_paren(p, i);
    if (close == (size_t)-1) {
      *i_io = end;
      return;
    }
    cond = pv_paren_text(p, i);
    i = close + 1;
  }
  base = pv_branch_label(p, is_else_if ? "else if" : "if", cond);
  then_label = pv_branch_entry(p, outer, base, "then");
  if (i < end && pv_is_sym(&p->toks[i], '{')) {
    size_t close = pv_match_brace(p, i);
    if (close == (size_t)-1) {
      *i_io = end;
      return;
    }
    pv_scan_handler_branch(p, i + 1, close, from_label, then_label);
    i = close + 1;
  } else if (i < end) {
    size_t next = pv_handler_nav_call(p, i, end, from_label, then_label);
    if (next == (size_t)-1) {
      *i_io = end;
      return;
    }
    i = next != 0 ? next : pv_skip_stmt(p, i, end);
  }
  if (i < end && pv_is_ident(&p->toks[i], "else")) {
    i++;
    if (i < end && pv_is_ident(&p->toks[i], "if")) {
      pv_scan_if(p, &i, end, from_label, outer, 1);
    } else {
      char *else_label = pv_branch_entry(p, outer, base, "else");
      if (i < end && pv_is_sym(&p->toks[i], '{')) {
        size_t close = pv_match_brace(p, i);
        if (close == (size_t)-1) {
          *i_io = end;
          return;
        }
        pv_scan_handler_branch(p, i + 1, close, from_label, else_label);
        i = close + 1;
      } else if (i < end) {
        size_t next = pv_handler_nav_call(p, i, end, from_label,
          else_label);
        if (next == (size_t)-1) {
          *i_io = end;
          return;
        }
        i = next != 0 ? next : pv_skip_stmt(p, i, end);
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
static void pv_scan_when(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *outer) {
  size_t i = *i_io;
  char *cond = ccb_arena_strdup(p->arena, "");
  char *base = NULL;
  i++;
  if (i < end && pv_is_sym(&p->toks[i], '(')) {
    size_t close = pv_match_paren(p, i);
    if (close == (size_t)-1) {
      *i_io = end;
      return;
    }
    cond = pv_paren_text(p, i);
    i = close + 1;
  }
  base = pv_branch_label(p, "when", cond);
  if (i < end && pv_is_sym(&p->toks[i], '{')) {
    size_t close = pv_match_brace(p, i);
    size_t pos = 0;
    if (close == (size_t)-1) {
      *i_io = end;
      return;
    }
    pos = i + 1;
    while (pos < close) {
      size_t start = pos;
      size_t arrow = (size_t)-1;
      long paren_depth = 0;
      long brace_depth = 0;
      size_t k = pos;
      char *entry_label = NULL;
      char *branch = NULL;
      while (k < close) {
        pv_token *t = &p->toks[k];
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
        } else if (t->kind == PV_TK_ARROW && paren_depth == 0 &&
            brace_depth == 0) {
          arrow = k;
          break;
        }
        k++;
      }
      if (arrow == (size_t)-1) {
        break;
      }
      entry_label = pv_trim_arena(p->arena, p->text,
        p->toks[start].start, p->toks[arrow].start);
      branch = pv_branch_entry(p, outer, base, entry_label);
      pos = arrow + 1;
      if (pos < close && pv_is_sym(&p->toks[pos], '{')) {
        size_t bclose = pv_match_brace(p, pos);
        if (bclose == (size_t)-1 || bclose > close) {
          pos = close;
          break;
        }
        pv_scan_handler_branch(p, pos + 1, bclose, from_label, branch);
        pos = bclose + 1;
      } else if (pos < close && !pv_is_sym(&p->toks[pos], '}')) {
        size_t next = pv_handler_nav_call(p, pos, close, from_label,
          branch);
        if (next == (size_t)-1) {
          pos = close;
          break;
        }
        pos = next != 0 ? next : pv_skip_stmt(p, pos, close);
      }
    }
    i = close + 1;
  } else if (i < end) {
    i = pv_skip_stmt(p, i, end);
  }
  *i_io = i > end ? end : i;
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
static size_t pv_try_helper_call(pv_parser *p, size_t i, size_t end,
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
    int saved = p->helper_depth;
    p->helper_depth = 1;
    pv_scan_handler_branch(p, hs, he, from_label, branch);
    p->helper_depth = saved;
    return close + 1;
  }
  if (p->index != NULL) {
    size_t count = 0;
    size_t file = pv_index_lookup(p->index, name, &count);
    if (count == 1) {
      pv_scan_foreign_body(p, file, name, from_label, branch, 1);
      return close + 1;
    }
    if (count > 1) {
      size_t f = 0;
      size_t g = 0;
      pv_warn_ambiguous(p, name, count, p->toks[i].line);
      // Chunk C: every definition's navs are reported by P1024,
      // so none of them is sweep territory either.
      for (f = 0; f < p->index->nfiles; f++) {
        for (g = 0; g < p->index->nfuns; g++) {
          if (p->index->fun_files[g] == f &&
              strcmp(p->index->fun_names[g], name) == 0) {
            pv_mark_foreign_navs(p, f, name);
            break;
          }
        }
      }
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
static void pv_scan_handler_branch(pv_parser *p, size_t start, size_t end,
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
      {
        size_t next = pv_handler_nav_call(p, i, end, from_label, branch);
        if (next == (size_t)-1) {
          return;
        }
        if (next != 0) {
          i = next;
          continue;
        }
      }
      {
        size_t next = pv_try_helper_call(p, i, end, from_label, branch);
        if (next != 0) {
          i = next;
          continue;
        }
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
static void pv_scan_foreign_body(pv_parser *p, size_t file_idx,
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
      size_t f = 0;
      size_t i = 0;
      pv_warn_ambiguous(p, hname, count, line);
      // Chunk C: every definition's navs are reported by P1024,
      // so none of them is sweep territory either.
      for (f = 0; f < p->index->nfiles; f++) {
        for (i = 0; i < p->index->nfuns; i++) {
          if (p->index->fun_files[i] == f &&
              strcmp(p->index->fun_names[i], hname) == 0) {
            pv_mark_foreign_navs(p, f, hname);
            break;
          }
        }
      }
      return;
    }
  }
}

// Attribute one handler body to its button: bound receivers get
// derived edges, unbound receivers get one CCB-P1019 per
// (screen, receiver) and no edge.
static void pv_handle_body(pv_parser *p, const char *receiver, size_t start,
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

static void pv_parse_block(pv_parser *p, pv_box *parent);
static void pv_parse_statement(pv_parser *p, pv_box *parent);
static void pv_parse_branch(pv_parser *p, pv_box *parent);

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
  pv_diag(p, "CCB-P1012", msg.buf != NULL ? msg.buf : "", line);
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
static void pv_parse_statement(pv_parser *p, pv_box *parent) {
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
static void pv_parse_block(pv_parser *p, pv_box *parent) {
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
static char *pv_branch_label(pv_parser *p, const char *keyword, const char *cond) {
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
static void pv_parse_branch(pv_parser *p, pv_box *parent) {
  char *keyword = p->toks[p->pos].text;
  int is_when = strcmp(keyword, "when") == 0;
  p->pos++;
  char *cond = ccb_arena_strdup(p->arena, "");
  if (pv_at_sym(p, '(')) {
    size_t open = p->pos;
    cond = pv_paren_text(p, open);
    size_t close = pv_match_paren(p, open);
    p->pos = close == (size_t)-1 ? p->ntoks : close + 1;
  }
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
      p->pos++;
      p->depth++;
      while (p->pos < p->ntoks) {
        if (pv_at_sym(p, '}')) {
          p->pos++;
          break;
        }
        // Find the top-level '->' for this entry.
        size_t start = p->pos;
        size_t arrow = (size_t)-1;
        long paren_depth = 0;
        long brace_depth = 0;
        size_t i = p->pos;
        while (i < p->ntoks) {
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
            arrow = i;
            break;
          }
          i++;
        }
        if (arrow == (size_t)-1) {
          pv_broken(p, pv_cur_line(p));
          // Skip to the closing brace of the when body, if any.
          while (p->pos < p->ntoks && !pv_at_sym(p, '}')) {
            p->pos++;
          }
          if (pv_at_sym(p, '}')) {
            p->pos++;
          }
          break;
        }
        char *entry_label = pv_trim_arena(p->arena, p->text,
          p->toks[start].start, p->toks[arrow].start);
        pv_box *entry = pv_box_new(p->arena,
          ccb_arena_strdup(p->arena, "branch-entry"), entry_label);
        pv_box_add_child(p->arena, branch, entry);
        pv_stack_push(p, entry);
        p->pos = arrow + 1;
        if (pv_at_sym(p, '{')) {
          pv_parse_block(p, entry);
        } else if (p->pos < p->ntoks && !pv_at_sym(p, '}')) {
          pv_parse_statement(p, entry);
        }
        pv_stack_pop(p);
      }
      if (p->pos >= p->ntoks) {
        pv_broken(p, pv_cur_line(p));
      }
      p->depth--;
    }
  } else {
    // if: one "then" entry, then an else chain.
    pv_box *then_entry = pv_box_new(p->arena,
      ccb_arena_strdup(p->arena, "branch-entry"),
      ccb_arena_strdup(p->arena, "then"));
    pv_box_add_child(p->arena, branch, then_entry);
    pv_stack_push(p, then_entry);
    if (pv_at_sym(p, '{')) {
      pv_parse_block(p, then_entry);
    } else if (p->pos < p->ntoks && !pv_at_sym(p, '}')) {
      pv_parse_statement(p, then_entry);
    }
    pv_stack_pop(p);
    while (pv_at_ident(p, "else")) {
      p->pos++;
      char *else_label = ccb_arena_strdup(p->arena, "else");
      if (pv_at_ident(p, "if")) {
        p->pos++;
        char *else_cond = ccb_arena_strdup(p->arena, "");
        if (pv_at_sym(p, '(')) {
          size_t open = p->pos;
          else_cond = pv_paren_text(p, open);
          size_t close = pv_match_paren(p, open);
          p->pos = close == (size_t)-1 ? p->ntoks : close + 1;
        }
        else_label = pv_branch_label(p, "else if", else_cond);
      }
      pv_box *else_entry = pv_box_new(p->arena,
        ccb_arena_strdup(p->arena, "branch-entry"), else_label);
      pv_box_add_child(p->arena, branch, else_entry);
      pv_stack_push(p, else_entry);
      if (pv_at_sym(p, '{')) {
        pv_parse_block(p, else_entry);
      } else if (p->pos < p->ntoks && !pv_at_sym(p, '}')) {
        pv_parse_statement(p, else_entry);
      }
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
    if (top_depth == 0 && pv_is_ident(&p.toks[p.pos], "fun")) {
      if (p.pos + 1 >= p.ntoks || p.toks[p.pos + 1].kind != PV_TK_IDENT) {
        pv_broken(&p, pv_cur_line(&p));
        p.pos++;
        continue;
      }
      char *name = p.toks[p.pos + 1].text;
      long line = p.toks[p.pos + 1].line;
      p.pos += 2;
      if (pv_at_sym(&p, '(')) {
        size_t close = pv_match_paren(&p, p.pos);
        if (close == (size_t)-1) {
          pv_broken(&p, line);
          p.pos = p.ntoks;
          break;
        }
        p.pos = close + 1;
      }
      if (!pv_at_sym(&p, '{')) {
        pv_broken(&p, line);
        continue;
      }
      // Slice 1: a top-level fun referenced via :: from a screen
      // body is a handler, not a screen. Skip its body; it is
      // scanned only when an attachment references it.
      // Slice 3: the same skip applies to android:onClick method
      // funs, whose bodies are scanned here into the pending list
      // (one pending edge per bound label for the method) for the
      // caller to attach to the screens containing that label.
      if (pv_is_handler(&p, name)) {
        size_t close = pv_match_brace(&p, p.pos);
        if (close == (size_t)-1) {
          pv_broken(&p, line);
          p.pos = p.ntoks;
          break;
        }
        if (p.pending_io != NULL) {
          size_t k = 0;
          for (k = 0; k < p.nonclick; k++) {
            if (strcmp(p.onclick_methods[k], name) == 0) {
              pv_screen *scratch = ccb_arena_alloc(arena, sizeof(*scratch));
              pv_screen *saved = p.screen;
              size_t e = 0;
              memset(scratch, 0, sizeof(*scratch));
              scratch->name = name;
              p.screen = scratch;
              pv_scan_handler_body(&p, p.pos + 1, close,
                ccb_arena_strdup(arena, p.onclick_labels[k]));
              p.screen = saved;
              for (e = 0; e < scratch->nedges; e++) {
                pv_pending_add(&p, p.onclick_methods[k],
                  &scratch->edges[e]);
              }
            }
          }
        }
        p.pos = close + 1;
        continue;
      }
      pv_screen *screen = ccb_arena_alloc(arena, sizeof(*screen));
      memset(screen, 0, sizeof(*screen));
      screen->name = name;
      screen->root = pv_box_new(arena, ccb_arena_strdup(arena, "screen"), NULL);
      pv_screen_add(arena, screens_io, nscreens_io, screen);
      p.screen = screen;
      pv_stack_push(&p, screen->root);
      // Chunk B: a new screen fun starts with an empty val map;
      // bindings never leak across screens.
      p.nvals = 0;
      pv_parse_block(&p, screen->root);
      pv_stack_pop(&p);
      p.screen = NULL;
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
static void pv_render_scenario_lines(ccb_str *out, pv_box *box) {
  size_t i = 0;
  if (out == NULL || box == NULL) {
    return;
  }
  if (box->type != NULL && strcmp(box->type, "list") == 0) {
    size_t nrows = 0;
    for (i = 0; i < box->nchildren; i++) {
      if (box->children[i]->from_scenario) {
        nrows++;
      }
    }
    if (nrows > 0) {
      int first = 1;
      ccb_str_append(out, "  scenario: list \"");
      ccb_str_append(out, box->label != NULL ? box->label : "");
      ccb_str_append(out, "\" = [");
      for (i = 0; i < box->nchildren; i++) {
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
