// ccb_diag implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_json.h"
#include "ccb_str.h"
#include "ccb_diag.h"

void ccb_diag_init(ccb_diag *d) {
  memset(d, 0, sizeof(*d));
  d->line = -1;
  d->col = -1;
}

void ccb_diag_clear(ccb_diag *d) {
  free(d->code);
  free(d->phase);
  free(d->message);
  free(d->file);
  free(d->command);
  free(d->workdir);
  free(d->suggestion);
  free(d->task_id);
  ccb_diag_init(d);
}

static char *ccb_dup_or_null(const char *s) {
  return s == NULL ? NULL : ccb_xstrdup(s);
}

ccb_diag ccb_diag_make(const char *code, const char *phase, const char *message) {
  ccb_diag d;
  memset(&d, 0, sizeof(d));
  ccb_diag_set(&d, code, phase, message);
  return d;
}

void ccb_diag_set(ccb_diag *d, const char *code, const char *phase, const char *message) {
  ccb_diag_clear(d);
  d->code = ccb_dup_or_null(code);
  d->phase = ccb_dup_or_null(phase);
  d->message = ccb_dup_or_null(message);
}

void ccb_diag_set_full(ccb_diag *d, const char *code, const char *phase,
    const char *message, const char *file, long line, long col,
    const char *command, const char *workdir, const char *suggestion) {
  ccb_diag_clear(d);
  d->code = ccb_dup_or_null(code);
  d->phase = ccb_dup_or_null(phase);
  d->message = ccb_dup_or_null(message);
  d->file = ccb_dup_or_null(file);
  d->line = line;
  d->col = col;
  d->command = ccb_dup_or_null(command);
  d->workdir = ccb_dup_or_null(workdir);
  d->suggestion = ccb_dup_or_null(suggestion);
}

void ccb_diag_emit_human(const ccb_diag *d, FILE *f) {
  fprintf(f, "error[%s] (%s)", d->code != NULL ? d->code : "CCB-E0000",
    d->phase != NULL ? d->phase : "?");
  if (d->task_id != NULL) {
    fprintf(f, " [task %s]", d->task_id);
  }
  fprintf(f, ": %s\n", d->message != NULL ? d->message : "unknown error");
  if (d->file != NULL) {
    fprintf(f, "  at %s:%ld:%ld\n", d->file, d->line, d->col);
  }
  if (d->command != NULL) {
    fprintf(f, "  command: %s\n", d->command);
  }
  if (d->workdir != NULL) {
    fprintf(f, "  workdir: %s\n", d->workdir);
  }
  if (d->suggestion != NULL) {
    fprintf(f, "  suggestion: %s\n", d->suggestion);
  }
}

void ccb_diag_emit_json(const ccb_diag *d, FILE *f) {
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"event\":\"error\",\"code\":");
  ccb_json_escape(&out, d->code != NULL ? d->code : "CCB-E0000");
  ccb_str_append(&out, ",\"phase\":");
  ccb_json_escape(&out, d->phase != NULL ? d->phase : "");
  ccb_str_append(&out, ",\"message\":");
  ccb_json_escape(&out, d->message != NULL ? d->message : "");
  if (d->file != NULL) {
    ccb_str_append(&out, ",\"file\":");
    ccb_json_escape(&out, d->file);
    ccb_str_appendf(&out, ",\"line\":%ld,\"col\":%ld", d->line, d->col);
  }
  if (d->command != NULL) {
    ccb_str_append(&out, ",\"command\":");
    ccb_json_escape(&out, d->command);
  }
  if (d->workdir != NULL) {
    ccb_str_append(&out, ",\"workdir\":");
    ccb_json_escape(&out, d->workdir);
  }
  if (d->suggestion != NULL) {
    ccb_str_append(&out, ",\"suggestion\":");
    ccb_json_escape(&out, d->suggestion);
  }
  if (d->task_id != NULL) {
    ccb_str_append(&out, ",\"task\":");
    ccb_json_escape(&out, d->task_id);
  }
  ccb_str_append(&out, "}\n");
  fputs(out.buf != NULL ? out.buf : "", f);
  ccb_str_free(&out);
}

void ccb_diag_list_init(ccb_diag_list *l) {
  l->items = NULL;
  l->count = 0;
  l->cap = 0;
}

void ccb_diag_list_free(ccb_diag_list *l) {
  for (size_t i = 0; i < l->count; i++) {
    ccb_diag_clear(&l->items[i]);
  }
  free(l->items);
  l->items = NULL;
  l->count = 0;
  l->cap = 0;
}

void ccb_diag_list_push(ccb_diag_list *l, ccb_diag *d) {
  if (l->count == l->cap) {
    size_t ncap = l->cap == 0 ? 8 : l->cap * 2;
    l->items = ccb_xrealloc(l->items, ncap * sizeof(*l->items));
    l->cap = ncap;
  }
  l->items[l->count] = *d;
  l->count++;
  memset(d, 0, sizeof(*d));
  d->line = -1;
  d->col = -1;
}

void ccb_diag_list_emit_human(const ccb_diag_list *l, FILE *f) {
  for (size_t i = 0; i < l->count; i++) {
    ccb_diag_emit_human(&l->items[i], f);
  }
}

void ccb_diag_list_emit_json(const ccb_diag_list *l, FILE *f) {
  for (size_t i = 0; i < l->count; i++) {
    ccb_diag_emit_json(&l->items[i], f);
  }
}
