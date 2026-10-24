// ccb_diag: structured diagnostics. Every error carries a stable code
// (see docs/diagnostics.md), the phase that failed, a message, optional
// source position, the exact command that failed, and one concrete
// suggestion. Ownership: all string fields are malloc'd; free with
// ccb_diag_clear. A diag moved into a list must not be cleared by the
// caller afterwards.
#ifndef CCB_DIAG_H
#define CCB_DIAG_H

#include "ccb_common.h"

typedef struct {
  char *code;
  char *phase;
  char *message;
  char *file;
  long line;
  long col;
  char *command;
  char *workdir;
  char *suggestion;
} ccb_diag;

void ccb_diag_init(ccb_diag *d);
void ccb_diag_clear(ccb_diag *d);
// Build a diagnostic from scratch (all fields owned by the result).
ccb_diag ccb_diag_make(const char *code, const char *phase, const char *message);
void ccb_diag_set(ccb_diag *d, const char *code, const char *phase, const char *message);
void ccb_diag_set_full(ccb_diag *d, const char *code, const char *phase,
  const char *message, const char *file, long line, long col,
  const char *command, const char *workdir, const char *suggestion);
void ccb_diag_emit_human(const ccb_diag *d, FILE *f);
void ccb_diag_emit_json(const ccb_diag *d, FILE *f);

typedef struct {
  ccb_diag *items;
  size_t count;
  size_t cap;
} ccb_diag_list;

void ccb_diag_list_init(ccb_diag_list *l);
void ccb_diag_list_free(ccb_diag_list *l);
// Moves ownership of *d into the list; the caller must not clear *d after.
void ccb_diag_list_push(ccb_diag_list *l, ccb_diag *d);
void ccb_diag_list_emit_human(const ccb_diag_list *l, FILE *f);
void ccb_diag_list_emit_json(const ccb_diag_list *l, FILE *f);

#endif
