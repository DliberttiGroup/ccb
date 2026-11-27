// ccb_preview_real_listeners: listener extraction + activity synthesis.
// Split from ccb_preview_real.c.

#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include <ctype.h>
#include "ccb_preview_real_internal.h"

// ---------- listener extraction ----------

static void rv_listener_add(rv_activity *a, const char *id,
    const char *method, size_t body_start, size_t body_end, long line,
    int is_onclick_prop) {
  if (a->nlisteners == a->caplisteners) {
    size_t ncap = a->caplisteners == 0 ? 4 : a->caplisteners * 2;
    a->listeners = ccb_xrealloc(a->listeners,
      ncap * sizeof(*a->listeners));
    a->caplisteners = ncap;
  }
  a->listeners[a->nlisteners].id = ccb_xstrdup(id);
  a->listeners[a->nlisteners].method =
    method != NULL ? ccb_xstrdup(method) : NULL;
  a->listeners[a->nlisteners].body_start = body_start;
  a->listeners[a->nlisteners].body_end = body_end;
  a->listeners[a->nlisteners].line = line;
  a->listeners[a->nlisteners].is_onclick_prop = is_onclick_prop;
  a->nlisteners++;
}

static void rv_consumed_add(rv_activity *a, size_t start, size_t end) {
  if (a->nconsumed == a->capconsumed) {
    size_t ncap = a->capconsumed == 0 ? 8 : a->capconsumed * 2;
    a->consumed_starts = ccb_xrealloc(a->consumed_starts,
      ncap * sizeof(*a->consumed_starts));
    a->consumed_ends = ccb_xrealloc(a->consumed_ends,
      ncap * sizeof(*a->consumed_ends));
    a->capconsumed = ncap;
  }
  a->consumed_starts[a->nconsumed] = start;
  a->consumed_ends[a->nconsumed] = end;
  a->nconsumed++;
}

static int rv_in_consumed(rv_activity *a, size_t off) {
  size_t i = 0;
  for (i = 0; i < a->nconsumed; i++) {
    if (off >= a->consumed_starts[i] && off < a->consumed_ends[i]) {
      return 1;
    }
  }
  return 0;
}

// Parse the attachment after a literal findViewById receiver.
// recv_end is the index just past the findViewById(...) call.
// Records a listener and its consumed range; returns 1 on success.
// True when text[k..ne) names one of the activity's member funs.
static int rv_is_member_fun(rv_activity *a, size_t k, size_t ne) {
  for (size_t f = 0; f < a->nfuns; f++) {
    if (strlen(a->funs[f].name) == ne - k &&
        memcmp(a->funs[f].name, a->text + k, ne - k) == 0) {
      return 1;
    }
  }
  return 0;
}

// A `{ ... }` lambda block at j: 1 with *close_out at its '}'.
static int rv_lambda_block(rv_activity *a, size_t j, size_t *close_out) {
  if (j >= a->text_len || a->text[j] != '{') {
    return 0;
  }
  size_t close = rv_match(a->text, a->text_len, j, '{', '}');
  if (close == (size_t)-1) {
    return 0;
  }
  *close_out = close;
  return 1;
}

// A `::name` method reference at k: the name (malloc'd, NULL when
// there is none) and *end_out past the name.
static char *rv_method_ref(rv_activity *a, size_t k, size_t *end_out) {
  if (k + 1 >= a->text_len || a->text[k] != ':' ||
      a->text[k + 1] != ':' || k + 2 >= a->text_len ||
      !rv_is_id_start(a->text[k + 2])) {
    return NULL;
  }
  size_t ne = rv_ident_end(a->text, a->text_len, k + 2);
  *end_out = ne;
  return ccb_xstrndup(a->text + k + 2, ne - (k + 2));
}

// A bare member-fun name at k: record the attachment. Returns the
// ident end, or 0 when there is no bare member-fun name here. The
// caller records the consumed range.
static size_t rv_add_bare_fun(rv_activity *a, size_t k, const char *id,
    long line, int flag) {
  if (k >= a->text_len || !rv_is_id_start(a->text[k])) {
    return 0;
  }
  size_t fe = rv_ident_end(a->text, a->text_len, k);
  if (!rv_is_member_fun(a, k, fe)) {
    return 0;
  }
  char *fname = ccb_xstrndup(a->text + k, fe - k);
  rv_listener_add(a, id, fname, 0, 0, line, flag);
  free(fname);
  return fe;
}

// setOnClickListener(<form>) at j: a lambda, a ::ref, or a bare
// member-fun name. Returns 1 when an attachment was recorded.
static int rv_parse_listener_paren(rv_activity *a, size_t j,
    const char *id, size_t recv_start, long line) {
  size_t close = rv_match(a->text, a->text_len, j, '(', ')');
  if (close == (size_t)-1) {
    return 0;
  }
  size_t k = rv_skip_ws(a->text, a->text_len, j + 1);
  if (k < a->text_len && a->text[k] == '{') {
    size_t bc = 0;
    if (!rv_lambda_block(a, k, &bc) || bc + 1 != close) {
      return 0;
    }
    rv_listener_add(a, id, NULL, k + 1, bc, line, 0);
    rv_consumed_add(a, recv_start, close + 1);
    return 1;
  }
  size_t ne = 0;
  char *name = rv_method_ref(a, k, &ne);
  if (name != NULL) {
    rv_listener_add(a, id, name, 0, 0, line, 0);
    free(name);
    rv_consumed_add(a, recv_start, close + 1);
    return 1;
  }
  if (rv_add_bare_fun(a, k, id, line, 0) != 0) {
    rv_consumed_add(a, recv_start, close + 1);
    return 1;
  }
  return 0;
}

static int rv_parse_attachment(rv_activity *a, size_t recv_start,
    const char *id, size_t recv_end) {
  size_t i = rv_skip_ws(a->text, a->text_len, recv_end);
  long line = rv_line_of(a->text, recv_start);
  if (i >= a->text_len || a->text[i] != '.') {
    return 0;
  }
  i++;
  if (rv_at_word(a->text, a->text_len, i, "setOnClickListener")) {
    size_t j = rv_skip_ws(a->text, a->text_len, i + 18);
    size_t close = 0;
    if (rv_lambda_block(a, j, &close)) {
      rv_listener_add(a, id, NULL, j + 1, close, line, 0);
      rv_consumed_add(a, recv_start, close + 1);
      return 1;
    }
    if (j < a->text_len && a->text[j] == '(') {
      return rv_parse_listener_paren(a, j, id, recv_start, line);
    }
    return 0;
  }
  if (rv_at_word(a->text, a->text_len, i, "onClick")) {
    size_t j = rv_skip_ws(a->text, a->text_len, i + 7);
    if (j < a->text_len && a->text[j] == '=') {
      size_t close = 0;
      size_t ne = 0;
      char *name = NULL;
      j = rv_skip_ws(a->text, a->text_len, j + 1);
      if (rv_lambda_block(a, j, &close)) {
        rv_listener_add(a, id, NULL, j + 1, close, line, 1);
        rv_consumed_add(a, recv_start, close + 1);
        return 1;
      }
      name = rv_method_ref(a, j, &ne);
      if (name != NULL) {
        rv_listener_add(a, id, name, 0, 0, line, 1);
        free(name);
        rv_consumed_add(a, recv_start, ne);
        return 1;
      }
      size_t fe = rv_add_bare_fun(a, j, id, line, 1);
      if (fe != 0) {
        rv_consumed_add(a, recv_start, fe);
        return 1;
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

// Scan the onCreate body for literal findViewById attachments.
static void rv_extract_listeners(rv_activity *a) {
  size_t start = a->oncreate_brace + 1;
  size_t end = a->oncreate_close;
  size_t pos = start;
  if (a->oncreate_brace == (size_t)-1) {
    return;
  }
  while (pos < end) {
    size_t hit = rv_find_word(a->text, a->text_len, pos,
      "findViewById");
    size_t j = 0;
    size_t id_start = 0;
    size_t id_end = 0;
    size_t close = 0;
    char *id = NULL;
    if (hit == (size_t)-1 || hit >= end) {
      break;
    }
    pos = hit + 12;
    j = rv_skip_ws(a->text, a->text_len, pos);
    if (j < a->text_len && a->text[j] == '<') {
      size_t gt = rv_match(a->text, a->text_len, j, '<', '>');
      if (gt == (size_t)-1) {
        continue;
      }
      j = rv_skip_ws(a->text, a->text_len, gt + 1);
    }
    if (j >= a->text_len || a->text[j] != '(') {
      continue;
    }
    {
      size_t k = rv_skip_ws(a->text, a->text_len, j + 1);
      if (k + 5 >= a->text_len || memcmp(a->text + k, "R.id.", 5) != 0 ||
          !rv_is_id_start(a->text[k + 5])) {
        continue;
      }
      id_start = k + 5;
      id_end = rv_ident_end(a->text, a->text_len, id_start);
      close = rv_match(a->text, a->text_len, j, '(', ')');
      if (close == (size_t)-1) {
        continue;
      }
    }
    id = ccb_xstrndup(a->text + id_start, id_end - id_start);
    (void)rv_parse_attachment(a, hit, id, close + 1);
    free(id);
    pos = close + 1;
  }
}

// Warn CCB-P1025 for listener attachments the extractor did not
// consume (a variable receiver, a val-held view, a loop): the nav
// inside is real but has no button to attribute it to, so it is
// reported here instead of silently dropped.
// Warn for a `{ ... }` listener whose body navigates but whose
// receiver has no literal view id. `start` is the onCreate body
// start, `hit` the setOnClickListener position, `j` the '{'.
static void rv_warn_brace_listener(rv_activity *a, ccb_diag_list *diags,
    size_t start, size_t hit, size_t j, size_t end) {
  size_t close = rv_match(a->text, a->text_len, j, '{', '}');
  if (close == (size_t)-1 || close >= end ||
      !rv_body_has_nav(a, j + 1, close)) {
    return;
  }
  ccb_str msg;
  size_t rs = hit;
  size_t re = hit;
  ccb_str_init(&msg);
  while (rs > start && a->text[rs - 1] != '\n' &&
      a->text[rs - 1] != ';' && a->text[rs - 1] != '{') {
    rs--;
  }
  while (re > rs && (a->text[re - 1] == ' ' ||
      a->text[re - 1] == '\t')) {
    re--;
  }
  ccb_str_append(&msg, "unattributed navigation: click handler for '");
  if (re - rs > 48) {
    ccb_str_append_n(&msg, a->text + rs, 48);
    ccb_str_append(&msg, "...");
  } else {
    ccb_str_append_n(&msg, a->text + rs, re - rs);
  }
  ccb_str_append(&msg, "' has no literal view id, so its navigation "
    "is not recorded as an edge from any button; it is reported "
    "here so it is not silently dropped");
  rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "", a->file,
    rv_line_of(a->text, hit));
  ccb_str_free(&msg);
}

// Warn for a setOnClickListener(<lambda>) whose body navigates but
// whose receiver has no literal view id. `j` is the '('.
static void rv_warn_paren_listener(rv_activity *a, ccb_diag_list *diags,
    size_t hit, size_t j, size_t end) {
  size_t close = rv_match(a->text, a->text_len, j, '(', ')');
  size_t k = 0;
  size_t bc = 0;
  if (close == (size_t)-1) {
    return;
  }
  k = rv_skip_ws(a->text, a->text_len, j + 1);
  if (k >= a->text_len || a->text[k] != '{') {
    return;
  }
  bc = rv_match(a->text, a->text_len, k, '{', '}');
  if (bc == (size_t)-1 || bc >= end ||
      !rv_body_has_nav(a, k + 1, bc)) {
    return;
  }
  ccb_str msg;
  ccb_str_init(&msg);
  ccb_str_append(&msg, "unattributed navigation: click handler with "
    "no literal view id has navigation that is not recorded as an "
    "edge from any button; it is reported here so it is not "
    "silently dropped");
  rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "", a->file,
    rv_line_of(a->text, hit));
  ccb_str_free(&msg);
}

void rv_warn_unresolved_listeners(rv_activity *a,
    ccb_diag_list *diags) {
  size_t start = a->oncreate_brace + 1;
  size_t end = a->oncreate_close;
  size_t pos = start;
  if (a->oncreate_brace == (size_t)-1) {
    return;
  }
  while (pos < end) {
    size_t hit = rv_find_word(a->text, a->text_len, pos,
      "setOnClickListener");
    size_t j = 0;
    if (hit == (size_t)-1 || hit >= end) {
      break;
    }
    pos = hit + 18;
    if (rv_in_consumed(a, hit)) {
      continue;
    }
    j = rv_skip_ws(a->text, a->text_len, hit + 18);
    if (j < a->text_len && a->text[j] == '{') {
      rv_warn_brace_listener(a, diags, start, hit, j, end);
      continue;
    }
    if (j < a->text_len && a->text[j] == '(') {
      rv_warn_paren_listener(a, diags, hit, j, end);
      continue;
    }
  }
}

// True when body range [start, end) references member fun `name`,
// via ::name or a bare name( call.
static int rv_body_refers(rv_activity *a, size_t start, size_t end,
    const char *name) {
  size_t n = strlen(name);
  size_t i = start;
  const char *s = a->text;
  size_t len = a->text_len;
  while (i < end) {
    size_t hit = rv_find_word(s, len, i, name);
    if (hit == (size_t)-1 || hit >= end) {
      return 0;
    }
    if (hit >= start + 2 && s[hit - 1] == ':' &&
        s[hit - 2] == ':') {
      return 1;
    }
    if (!(hit > start && s[hit - 1] == '.')) {
      size_t j = rv_skip_ws(s, len, hit + n);
      if (j < end && s[j] == '(') {
        return 1;
      }
    }
    i = hit + 1;
  }
  return 0;
}

static int rv_name_in(char **names, size_t n, const char *name) {
  size_t i = 0;
  for (i = 0; i < n; i++) {
    if (strcmp(names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

// Append `name` (copied) to the emitted list, growing it as needed.
static void rv_emitted_push(char ***names_io, size_t *n_io,
    size_t *cap_io, const char *name) {
  char **names = *names_io;
  size_t n = *n_io;
  size_t cap = *cap_io;
  if (n == cap) {
    size_t ncap = cap == 0 ? 8 : cap * 2;
    names = ccb_xrealloc(names, ncap * sizeof(*names));
    cap = ncap;
  }
  names[n++] = ccb_xstrdup(name);
  *names_io = names;
  *n_io = n;
  *cap_io = cap;
}

// Add the member funs referenced from one listener lambda body
// (body_start..body_end) to the emitted list.
static void rv_emitted_scan_body(rv_activity *a, char ***names_io,
    size_t *n_io, size_t *cap_io, size_t body_start, size_t body_end) {
  for (size_t f = 0; f < a->nfuns; f++) {
    if (!rv_name_in(*names_io, *n_io, a->funs[f].name) &&
        rv_body_refers(a, body_start, body_end, a->funs[f].name)) {
      rv_emitted_push(names_io, n_io, cap_io, a->funs[f].name);
    }
  }
}

// Add the member funs referenced from fun `f`'s body to the emitted
// list. Returns 1 when anything was added.
static int rv_emitted_scan_fun(rv_activity *a, char ***names_io,
    size_t *n_io, size_t *cap_io, size_t f) {
  int added = 0;
  for (size_t g = 0; g < a->nfuns; g++) {
    if (!rv_name_in(*names_io, *n_io, a->funs[g].name) &&
        rv_body_refers(a, a->funs[f].brace + 1, a->funs[f].close,
          a->funs[g].name)) {
      rv_emitted_push(names_io, n_io, cap_io, a->funs[g].name);
      added = 1;
    }
  }
  return added;
}

// One fixed-point pass over the emitted names: add the funs
// referenced from each emitted fun's body. Returns 1 when anything
// was added.
static int rv_emitted_close_once(rv_activity *a, char ***names_io,
    size_t *n_io, size_t *cap_io) {
  int added = 0;
  for (size_t k = 0; k < *n_io; k++) {
    for (size_t f = 0; f < a->nfuns; f++) {
      if (strcmp(a->funs[f].name, (*names_io)[k]) == 0) {
        added |= rv_emitted_scan_fun(a, names_io, n_io, cap_io, f);
      }
    }
  }
  return added;
}

// The member funs to emit as top-level funs: those named by a layout
// android:onClick method, referenced from a listener (method ref or
// a call inside a lambda body), or referenced from another emitted
// fun (closed to a fixed point, so helper chains count). The rest
// are dropped from the synthetic text; when a dropped fun contains
// a nav, one CCB-P1025 names it, so its navigation is visible
// instead of silent. Returns the emitted name list (malloc'd).
char **rv_emitted_funs(rv_activity *a, char **onclick_methods,
    size_t nonclick, ccb_diag_list *diags, size_t *nemitted_io) {
  char **names = NULL;
  size_t n = 0;
  size_t cap = 0;
  size_t li = 0;
  size_t k = 0;
  int changed = 0;
  for (li = 0; li < a->nlisteners; li++) {
    if (a->listeners[li].method != NULL &&
        !rv_name_in(names, n, a->listeners[li].method)) {
      rv_emitted_push(&names, &n, &cap, a->listeners[li].method);
    }
    if (a->listeners[li].method == NULL) {
      rv_emitted_scan_body(a, &names, &n, &cap,
        a->listeners[li].body_start, a->listeners[li].body_end);
    }
  }
  for (k = 0; k < nonclick; k++) {
    if (!rv_name_in(names, n, onclick_methods[k])) {
      rv_emitted_push(&names, &n, &cap, onclick_methods[k]);
    }
  }
  changed = 1;
  while (changed) {
    changed = rv_emitted_close_once(a, &names, &n, &cap);
  }
  {
    size_t f = 0;
    for (f = 0; f < a->nfuns; f++) {
      if (!rv_name_in(names, n, a->funs[f].name) &&
          rv_body_has_nav_direct(a, a->funs[f].brace + 1,
            a->funs[f].close)) {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg, "unattributed navigation: nav call "
          "in '");
        ccb_str_append(&msg, a->funs[f].name);
        ccb_str_append(&msg, "' is never referenced from a button, "
          "so it is not recorded as an edge; it is reported here "
          "so it is not silently dropped");
        rv_diag(diags, "CCB-P1025", msg.buf != NULL ? msg.buf : "",
          a->file, a->funs[f].line);
        ccb_str_free(&msg);
      }
    }
  }
  *nemitted_io = n;
  return names;
}

// Escape a layout label as a Kotlin string literal body.
static char *rv_escape_label(const char *s) {
  ccb_str out;
  size_t i = 0;
  ccb_str_init(&out);
  for (i = 0; s[i] != '\0'; i++) {
    if (s[i] == '\\' || s[i] == '"') {
      ccb_str_append_c(&out, '\\');
      ccb_str_append_c(&out, s[i]);
    } else if (s[i] == '\n') {
      ccb_str_append(&out, "\\n");
    } else if (s[i] == '\r') {
      ccb_str_append(&out, "\\r");
    } else if (s[i] == '\t') {
      ccb_str_append(&out, "\\t");
    } else {
      ccb_str_append_c(&out, s[i]);
    }
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

// Build the synthetic flow text for one activity. Buttons come from
// its layout (or none when the layout is unknown); listeners become
// attachment statements; emitted member funs become top-level funs.
char *rv_synth_activity(rv_activity *a, rv_layout *layout,
    char **emitted, size_t nemitted) {
  ccb_str out;
  size_t i = 0;
  ccb_str_init(&out);
  ccb_str_append(&out, "fun ");
  ccb_str_append(&out, a->screen);
  ccb_str_append(&out, "() {\n");
  if (layout != NULL) {
    for (i = 0; i < layout->nbuttons; i++) {
      char *esc = rv_escape_label(layout->buttons[i].label);
      ccb_str_append(&out, "  Button(\"");
      ccb_str_append(&out, esc);
      ccb_str_append(&out, "\")\n");
      free(esc);
    }
  }
  for (i = 0; i < a->nlisteners; i++) {
    rv_listener *l = &a->listeners[i];
    ccb_str_append(&out, "  ");
    ccb_str_append(&out, l->id);
    if (l->method != NULL) {
      ccb_str_append(&out, ".setOnClickListener(::");
      ccb_str_append(&out, l->method);
      ccb_str_append(&out, ")\n");
    } else {
      char *body = rv_transform_body(a->text + l->body_start,
        l->body_end - l->body_start);
      ccb_str_append(&out, ".setOnClickListener { ");
      ccb_str_append(&out, body);
      ccb_str_append(&out, " }\n");
      free(body);
    }
  }
  ccb_str_append(&out, "}\n");
  for (i = 0; i < a->nfuns; i++) {
    char *body = NULL;
    if (!rv_name_in(emitted, nemitted, a->funs[i].name)) {
      continue;
    }
    body = rv_transform_body(a->text + a->funs[i].brace + 1,
      a->funs[i].close - (a->funs[i].brace + 1));
    ccb_str_append(&out, "fun ");
    ccb_str_append(&out, a->funs[i].name);
    ccb_str_append(&out, "(");
    ccb_str_append(&out, a->funs[i].params);
    ccb_str_append(&out, ") { ");
    ccb_str_append(&out, body);
    ccb_str_append(&out, " }\n");
    free(body);
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

void rv_activity_free(rv_activity *a) {
  size_t i = 0;
  if (a == NULL) {
    return;
  }
  free(a->cls);
  free(a->screen);
  free(a->layout);
  for (i = 0; i < a->nfuns; i++) {
    free(a->funs[i].name);
    free(a->funs[i].params);
  }
  free(a->funs);
  for (i = 0; i < a->nlisteners; i++) {
    free(a->listeners[i].id);
    free(a->listeners[i].method);
  }
  free(a->listeners);
  free(a->consumed_starts);
  free(a->consumed_ends);
}

// Extract every Activity class in one .kt file: a class with an
// onCreate that calls setContentView. A class without one is not a
// screen (a Compose setContent or a helper class is out of scope,
// and a nav to it stays valid through the manifest exemption).
// Skip the class header (extends/implements/ctor parens) to its
// '{'. Returns the brace position, or (size_t)-1 when there is
// none (a ';' or newline ends the header, an unmatched '(' too).
static size_t rv_class_brace(const char *text, size_t len, size_t ne) {
  size_t brace = ne;
  while (brace < len && text[brace] != '{') {
    if (text[brace] == '(') {
      size_t m = rv_match(text, len, brace, '(', ')');
      if (m == (size_t)-1) {
        return (size_t)-1;
      }
      brace = m + 1;
      continue;
    }
    if (text[brace] == ';' || text[brace] == '\n') {
      return (size_t)-1;
    }
    brace++;
  }
  if (brace >= len || text[brace] != '{') {
    return (size_t)-1;
  }
  return brace;
}

// The onCreate body braces after `onCreate(...)` at fe: 1 with
// *brace_out/*close_out set, or 0 when there is no body.
static int rv_oncreate_braces(const char *text, size_t len, size_t fe,
    size_t *brace_out, size_t *close_out) {
  size_t p = rv_skip_ws(text, len, fe);
  size_t m = 0;
  size_t brace = 0;
  size_t close = 0;
  if (p >= len || text[p] != '(') {
    return 0;
  }
  m = rv_match(text, len, p, '(', ')');
  if (m == (size_t)-1) {
    return 0;
  }
  brace = rv_skip_ws(text, len, m + 1);
  if (brace >= len || text[brace] != '{') {
    return 0;
  }
  close = rv_match(text, len, brace, '{', '}');
  if (close == (size_t)-1) {
    return 0;
  }
  *brace_out = brace;
  *close_out = close;
  return 1;
}

// The layout id of `setContentView(R.layout.<id>)` at sv: malloc'd,
// or NULL when the call is not that form.
static char *rv_content_layout(const char *text, size_t len, size_t sv) {
  size_t p = rv_skip_ws(text, len, sv + 14);
  size_t k = 0;
  if (p >= len || text[p] != '(') {
    return NULL;
  }
  k = rv_skip_ws(text, len, p + 1);
  if (k + 9 >= len || memcmp(text + k, "R.layout.", 9) != 0 ||
      !rv_is_id_start(text[k + 9])) {
    return NULL;
  }
  size_t ls = k + 9;
  size_t le = rv_ident_end(text, len, ls);
  return ccb_xstrndup(text + ls, le - ls);
}

void rv_extract_activities(const char *file, const char *text,
    rv_activity **acts_io, size_t *nacts_io) {
  size_t len = strlen(text);
  size_t pos = 0;
  while (pos < len) {
    size_t hit = rv_find_word(text, len, pos, "class");
    size_t j = 0;
    size_t ne = 0;
    char *cls = NULL;
    size_t brace = 0;
    size_t close = 0;
    rv_activity a;
    size_t oc = 0;
    size_t oc_brace = 0;
    size_t oc_close = 0;
    size_t sv = 0;
    if (hit == (size_t)-1) {
      break;
    }
    pos = hit + 5;
    j = rv_skip_ws(text, len, pos);
    if (j >= len || !rv_is_id_start(text[j])) {
      continue;
    }
    ne = rv_ident_end(text, len, j);
    cls = ccb_xstrndup(text + j, ne - j);
    // Class names are UpperCamelCase by convention; this also keeps
    // `::class.java` (lowercase "class" + "java") from ever parsing
    // as a class named "java".
    if (!isupper((unsigned char)cls[0])) {
      free(cls);
      continue;
    }
    brace = rv_class_brace(text, len, ne);
    if (brace == (size_t)-1) {
      free(cls);
      continue;
    }
    close = rv_match(text, len, brace, '{', '}');
    if (close == (size_t)-1) {
      free(cls);
      break;
    }
    memset(&a, 0, sizeof(a));
    a.cls = cls;
    a.screen = rv_strip_suffix(cls);
    a.file = file;
    a.text = text;
    a.text_len = len;
    a.class_brace = brace;
    a.class_close = close;
    a.oncreate_brace = (size_t)-1;
    a.oncreate_close = (size_t)-1;
    oc = brace + 1;
    while (oc < close) {
      size_t fhit = rv_find_word(text, len, oc, "fun");
      size_t fn = 0;
      size_t fe = 0;
      if (fhit == (size_t)-1 || fhit >= close) {
        break;
      }
      oc = fhit + 3;
      fn = rv_skip_ws(text, len, oc);
      if (fn >= len || !rv_is_id_start(text[fn])) {
        continue;
      }
      fe = rv_ident_end(text, len, fn);
      if (fe - fn != 8 || memcmp(text + fn, "onCreate", 8) != 0) {
        continue;
      }
      if (rv_oncreate_braces(text, len, fe, &oc_brace, &oc_close)) {
        a.oncreate_brace = oc_brace;
        a.oncreate_close = oc_close;
      }
      break;
    }
    if (a.oncreate_brace == (size_t)-1) {
      rv_activity_free(&a);
      pos = close + 1;
      continue;
    }
    sv = rv_find_word(text, len, a.oncreate_brace + 1,
      "setContentView");
    if (sv != (size_t)-1 && sv < a.oncreate_close) {
      a.layout = rv_content_layout(text, len, sv);
    }
    if (a.layout == NULL) {
      rv_activity_free(&a);
      pos = close + 1;
      continue;
    }
    rv_collect_funs(&a);
    rv_extract_listeners(&a);
    *acts_io = ccb_xrealloc(*acts_io,
      (*nacts_io + 1) * sizeof(**acts_io));
    (*acts_io)[*nacts_io] = a;
    (*nacts_io)++;
    pos = close + 1;
  }
}
