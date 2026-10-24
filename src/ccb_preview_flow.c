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
    char **args, size_t nargs, int *args_named) {
  pv_screen *screen = p->screen;
  for (size_t i = 0; i < screen->nedges; i++) {
    pv_edge *e = &screen->edges[i];
    int same_target = (e->target == NULL && target == NULL) ||
      (e->target != NULL && target != NULL && strcmp(e->target, target) == 0);
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
        same_args) {
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

// Record the nav call at token idx (ident + '(') and return the index
// just past its closing ')', or ntoks on breakage. The first
// argument is the target (a single string literal without '$', else
// an unknown destination); the remaining top-level arguments (split
// by commas at paren depth 1 / brace depth 0) become the edge's
// args. A dynamic target with arguments keeps target NULL and
// unknown true, and still records its args.
static size_t pv_nav_at(pv_parser *p, size_t idx) {
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
    return p->ntoks;
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
  pv_edge_add(p, pv_edge_from(p), target, unknown, args, nargs, args_named);
  return close + 1;
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

ccb_status pv_flow_parse(ccb_arena *arena, const char *text, const char *file,
    ccb_diag_list *diags, pv_screen ***screens_io, size_t *nscreens_io) {
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
  pv_tokenize(&p);
  if (p.unterminated_string) {
    pv_broken(&p, 1);
  }
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
      pv_screen *screen = ccb_arena_alloc(arena, sizeof(*screen));
      memset(screen, 0, sizeof(*screen));
      screen->name = name;
      screen->root = pv_box_new(arena, ccb_arena_strdup(arena, "screen"), NULL);
      pv_screen_add(arena, screens_io, nscreens_io, screen);
      p.screen = screen;
      pv_stack_push(&p, screen->root);
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
  return CCB_OK;
}

// ---------- logic tree ----------

static void pv_render_branches(ccb_str *out, pv_box *box) {
  if (box->is_branch) {
    ccb_str_append(out, "  branches: ");
    ccb_str_append(out, box->label != NULL ? box->label : "");
    ccb_str_append(out, " [");
    int first = 1;
    for (size_t i = 0; i < box->nchildren; i++) {
      pv_box *child = box->children[i];
      if (child->label == NULL) {
        continue;
      }
      if (!first) {
        ccb_str_append(out, ", ");
      }
      ccb_str_append(out, child->label);
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
    } else {
      ccb_str_append(out, "unknown");
    }
  }
  ccb_str_append(out, ")");
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
      ccb_str_append(out, "\"\n");
    }
  }
}
