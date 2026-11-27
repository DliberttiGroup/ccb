// ccb_preview_flow_internal.h: shared internals for the ccb_preview_flow.c split:
// file-local types, macros, and the helpers called across the split
// files. Not public API: only the split files include this header.
#ifndef CCB_PREVIEW_FLOW_INTERNAL_H
#define CCB_PREVIEW_FLOW_INTERNAL_H

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

typedef struct {
  pv_parser q;
} pv_cand_file;

typedef struct {
  size_t *files;
  char **names;
  size_t n, cap;
} pv_cand_pairs;

void pv_flow_diag(pv_parser *p, const char *code, const char *message, long line);
void pv_broken(pv_parser *p, long line);
long pv_cur_line(pv_parser *p);
int pv_is_ident(const pv_token *t, const char *name);
int pv_is_sym(const pv_token *t, char c);
int pv_at_ident(pv_parser *p, const char *name);
int pv_at_sym(pv_parser *p, char c);
int pv_is_known(const char *name);
char *pv_lower_arena(ccb_arena *arena, const char *s);
char *pv_trim_arena(ccb_arena *arena, const char *text, size_t start, size_t end);
pv_box *pv_box_new(ccb_arena *arena, char *type, char *label);
void pv_box_add_child(ccb_arena *arena, pv_box *parent, pv_box *child);
void pv_screen_add(ccb_arena *arena, pv_screen ***screens_io,
    size_t *nscreens_io, pv_screen *screen);
void pv_edge_add(pv_parser *p, char *from, char *target, int unknown,
    char **args, size_t nargs, int *args_named, int derived, char *branch);
void pv_stack_push(pv_parser *p, pv_box *box);
void pv_stack_pop(pv_parser *p);
size_t pv_match_paren(pv_parser *p, size_t idx);
size_t pv_match_brace(pv_parser *p, size_t idx);
size_t pv_skip_paren(pv_parser *p, size_t i, size_t end);
size_t pv_skip_brace(pv_parser *p, size_t i, size_t end);
size_t pv_find_arrow(pv_parser *p, size_t start, size_t end);
char *pv_paren_text(pv_parser *p, size_t idx);
void pv_tokenize(pv_parser *p);
char *pv_arg_classify(pv_parser *p, size_t start, size_t end,
    int *named_out);
size_t pv_nav_parse(pv_parser *p, size_t idx, char **target_out,
    int *unknown_out, char ***args_out, size_t *nargs_out,
    int **args_named_out);
size_t pv_nav_at(pv_parser *p, size_t idx);
int pv_is_nav(pv_parser *p, size_t idx);
void pv_scan_navs(pv_parser *p, size_t start, size_t end);
const char *pv_binding_label(pv_parser *p, const char *receiver);
void pv_val_record(pv_parser *p, char *name, char *target);
char *pv_val_lookup(pv_parser *p, const char *name);
int pv_is_handler(pv_parser *p, const char *name);
void pv_handler_add(pv_parser *p, char *name);
void pv_collect_funs(pv_parser *p);
void pv_collect_handlers(pv_parser *p);
int pv_handler_range(pv_parser *p, const char *name, size_t *start_out,
    size_t *end_out);
void pv_index_mark_nav(pv_fun_index *index, const char *file,
    size_t start);
void pv_mark_range_navs(pv_parser *p, size_t start, size_t end);
void pv_mark_foreign_navs(pv_parser *p, size_t file_idx,
    const char *name);
void pv_nav_arg_push(pv_parser *p, char ***args, size_t *nargs,
    int **args_named, char *arg, int named);
void pv_scan_if(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *outer, int is_else_if);
void pv_scan_when(pv_parser *p, size_t *i_io, size_t end,
    char *from_label, char *outer);
size_t pv_try_helper_call(pv_parser *p, size_t i, size_t end,
    char *from_label, char *branch);
void pv_scan_handler_branch(pv_parser *p, size_t start, size_t end,
    char *from_label, char *branch);
void pv_scan_foreign_body(pv_parser *p, size_t file_idx,
    const char *name, char *from_label, char *branch, int helper_depth);
void pv_handle_body(pv_parser *p, const char *receiver, size_t start,
    size_t end);
void pv_parse_statement(pv_parser *p, pv_box *parent);
void pv_parse_block(pv_parser *p, pv_box *parent);
char *pv_branch_label(pv_parser *p, const char *keyword, const char *cond);
void pv_parse_branch(pv_parser *p, pv_box *parent);

#endif // CCB_PREVIEW_FLOW_INTERNAL_H
