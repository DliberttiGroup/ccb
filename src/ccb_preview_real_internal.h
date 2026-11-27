// ccb_preview_real_internal.h: shared internals for the ccb_preview_real.c split:
// file-local types, macros, and the helpers called across the split
// files. Not public API: only the split files include this header.
#ifndef CCB_PREVIEW_REAL_INTERNAL_H
#define CCB_PREVIEW_REAL_INTERNAL_H

#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include <ctype.h>

typedef struct {
  char *name;
  char *value;
} rv_str_entry;

typedef struct {
  rv_str_entry *items;
  size_t n, cap;
} rv_strings;

typedef struct {
  char *id; // view id, e.g. note1_button
  char *label; // resolved label (or the id when no android:text)
  char *method; // android:onClick method, or NULL
} rv_button;

typedef struct {
  char *name; // layout name, e.g. activity_main
  char *path; // first file found with that name (malloc'd)
  rv_button *buttons;
  size_t nbuttons, capbuttons;
} rv_layout;

#define RV_WIRE_MAX_DEPTH 32

typedef struct {
  const char *text;
  size_t len;
  size_t pos;
  ccb_arena *arena;
} rv_wire_cursor;

typedef struct {
  char *name; // member fun name
  char *params; // raw parameter list text (may be "")
  size_t brace, close; // body brace range in the file text
  long line;
} rv_fun;

typedef struct {
  char *id; // literal view id
  char *method; // ::method name, or NULL for a lambda
  size_t body_start, body_end; // lambda body range, or 0 when a method ref
  long line;
  int is_onclick_prop; // `.onClick = ...` instead of setOnClickListener
} rv_listener;

typedef struct {
  char *cls; // class name as written
  char *screen; // stripped screen name
  char *layout; // layout name, or NULL
  const char *file; // borrowed .kt path
  const char *text; // borrowed file text
  size_t text_len;
  size_t class_brace, class_close;
  size_t oncreate_brace, oncreate_close; // (size_t)-1 when absent
  rv_fun *funs;
  size_t nfuns, capfuns;
  rv_listener *listeners;
  size_t nlisteners, caplisteners;
  // [start, end) ranges of consumed findViewById attachments, so a
  // later scan can tell an unrecognized setOnClickListener apart.
  size_t *consumed_starts, *consumed_ends;
  size_t nconsumed, capconsumed;
} rv_activity;

typedef struct {
  char *var;
  char *cls;
} rv_intent_var;

int rv_is_id_start(char c);
int rv_is_id_char(char c);
long rv_line_of(const char *text, size_t off);
size_t rv_skip_ws(const char *s, size_t len, size_t i);
size_t rv_skip_raw_string(const char *s, size_t len, size_t i);
size_t rv_skip_quoted(const char *s, size_t len, size_t i);
size_t rv_skip_block_comment(const char *s, size_t len,
    size_t i);
size_t rv_match(const char *s, size_t len, size_t open, char oc,
    char cc);
int rv_at_word(const char *s, size_t len, size_t off,
    const char *word);
size_t rv_find_word(const char *s, size_t len, size_t from,
    const char *word);
size_t rv_ident_end(const char *s, size_t len, size_t i);
void rv_diag(ccb_diag_list *diags, const char *code,
    const char *message, const char *file, long line);
const char *rv_strings_find(rv_strings *t, const char *name);
void rv_strings_parse_file(rv_strings *t, const char *path);
rv_layout *rv_layout_find(rv_layout **layouts, size_t nlayouts,
    const char *name);
void rv_scan_layout_file(const char *text, size_t len,
    const char *layout_name, const char *layout_path,
    rv_layout ***layouts_io, size_t *nlayouts_io,
    rv_strings *strings);
pv_box *rv_layout_wireframe(ccb_arena *arena,
    const char *layout_path);
char *rv_strip_suffix(const char *name);
char *rv_transform_body(const char *body, size_t len);
int rv_body_has_nav_direct(rv_activity *a, size_t start,
    size_t end);
int rv_body_has_nav(rv_activity *a, size_t start, size_t end);
void rv_collect_funs(rv_activity *a);
void rv_warn_unresolved_listeners(rv_activity *a,
    ccb_diag_list *diags);
char **rv_emitted_funs(rv_activity *a, char **onclick_methods,
    size_t nonclick, ccb_diag_list *diags, size_t *nemitted_io);
char *rv_synth_activity(rv_activity *a, rv_layout *layout,
    char **emitted, size_t nemitted);
void rv_activity_free(rv_activity *a);
void rv_extract_activities(const char *file, const char *text,
    rv_activity **acts_io, size_t *nacts_io);

#endif // CCB_PREVIEW_REAL_INTERNAL_H
