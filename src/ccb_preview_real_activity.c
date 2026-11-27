// ccb_preview_real_activity: Activity extraction from real Kotlin.
// Split from ccb_preview_real.c.

#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include <ctype.h>
#include "ccb_preview_real_internal.h"

// ---------- Activity extraction ----------

// Length of name after stripping one trailing "Activity"/"Fragment"
// (a bare "Activity" is a name, not a suffix).
static size_t rv_stripped_len(const char *name, size_t n) {
  if (n > 8 && memcmp(name + n - 8, "Activity", 8) == 0) {
    return n - 8;
  }
  if (n > 8 && memcmp(name + n - 8, "Fragment", 8) == 0) {
    return n - 8;
  }
  return n;
}

// Strip one trailing "Activity" or "Fragment" (a bare "Activity" is
// a name, not a suffix). Malloc'd.
char *rv_strip_suffix(const char *name) {
  size_t n = strlen(name);
  return ccb_xstrndup(name, rv_stripped_len(name, n));
}

// Body transforms, applied to an extracted lambda/member-fun body
// before it is embedded in the synthetic flow text:
//   1. findViewById<...>(R.id.x) / findViewById(R.id.x) -> x
//   2. `val v = Intent(this, C::class.java)` + startActivity(v)
//      -> startActivity(Intent(this, C::class.java))
//   3. CActivity::class.java / CFragment::class.java -> C::class.java
// The result is malloc'd.

// Parse `val|var <name> = Intent(this, <Cls>::class.java)` at use
// (use points at the val/var keyword). Returns 1 on success with
// *var_out / *cls_out owned by the caller and *end_out past
// "::class.java"; 0 otherwise (nothing allocated).
static int rv_parse_intent_var(const char *body, size_t len, size_t use,
    char **var_out, char **cls_out, size_t *end_out) {
  size_t p = use + 3;
  size_t q = rv_skip_ws(body, len, p);
  if (q >= len || !rv_is_id_start(body[q])) {
    return 0;
  }
  size_t ve = rv_ident_end(body, len, q);
  size_t e = rv_skip_ws(body, len, ve);
  if (e >= len || body[e] != '=') {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 1);
  if (!rv_at_word(body, len, e, "Intent")) {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 6);
  if (e >= len || body[e] != '(') {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 1);
  if (!rv_at_word(body, len, e, "this")) {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 4);
  if (e >= len || body[e] != ',') {
    return 0;
  }
  e = rv_skip_ws(body, len, e + 1);
  if (e >= len || !rv_is_id_start(body[e])) {
    return 0;
  }
  size_t cls_start = e;
  size_t cls_end = rv_ident_end(body, len, e);
  if (cls_end + 12 > len || memcmp(body + cls_end, "::class.java", 12) != 0) {
    return 0;
  }
  *var_out = ccb_xstrndup(body + q, ve - q);
  *cls_out = ccb_xstrndup(body + cls_start, cls_end - cls_start);
  *end_out = cls_end + 12;
  return 1;
}

// findViewById<...>(R.id.x) or findViewById(R.id.x) -> x. Returns
// the index past the call, or 0 when this is not such a call.
static size_t rv_rewrite_find_view(ccb_str *out, const char *body,
    size_t len, size_t i) {
  size_t j = rv_skip_ws(body, len, i + 12);
  if (j < len && body[j] == '<') {
    size_t gt = rv_match(body, len, j, '<', '>');
    if (gt == (size_t)-1) {
      return 0;
    }
    j = rv_skip_ws(body, len, gt + 1);
  }
  if (j >= len || body[j] != '(') {
    return 0;
  }
  size_t k = rv_skip_ws(body, len, j + 1);
  if (k + 5 >= len || memcmp(body + k, "R.id.", 5) != 0 ||
      !rv_is_id_start(body[k + 5])) {
    return 0;
  }
  size_t id_start = k + 5;
  size_t id_end = rv_ident_end(body, len, id_start);
  size_t close = rv_match(body, len, j, '(', ')');
  if (close == (size_t)-1) {
    return 0;
  }
  ccb_str_append_n(out, body + id_start, id_end - id_start);
  return close + 1;
}

// startActivity(var) where var held Intent(this, C::class.java):
// rewrite to startActivity(Intent(this, C::class.java)). Returns
// the index past the call, or 0 when this is not such a call.
static size_t rv_rewrite_start_activity(ccb_str *out, const char *body,
    size_t len, size_t i, rv_intent_var *vars, size_t nvars) {
  size_t j = rv_skip_ws(body, len, i + 13);
  if (j >= len || body[j] != '(') {
    return 0;
  }
  size_t v = rv_skip_ws(body, len, j + 1);
  if (v >= len || !rv_is_id_start(body[v])) {
    return 0;
  }
  size_t ve = rv_ident_end(body, len, v);
  size_t after = rv_skip_ws(body, len, ve);
  if (after >= len || body[after] != ')') {
    return 0;
  }
  for (size_t k = 0; k < nvars; k++) {
    if (strlen(vars[k].var) != ve - v ||
        memcmp(vars[k].var, body + v, ve - v) != 0) {
      continue;
    }
    // Strip the Activity/Fragment suffix here too, so the
    // rewritten Intent names the same screen as the direct form
    // below (Detail, not DetailActivity) and dedups against
    // sketch edges.
    char *stripped = rv_strip_suffix(vars[k].cls);
    ccb_str_append(out, "startActivity(Intent(this, ");
    ccb_str_append(out, stripped);
    ccb_str_append(out, "::class.java))");
    free(stripped);
    return after + 1;
  }
  return 0;
}

char *rv_transform_body(const char *body, size_t len) {
  ccb_str out;
  size_t i = 0;
  // Pass 1+2 are fused: scan left to right, rewriting findViewById
  // spans and startActivity(var) spans as they are found. Intent
  // val bindings are collected first (a val precedes its use).
  rv_intent_var *vars = NULL;
  size_t nvars = 0;
  size_t capvars = 0;
  size_t p = 0;
  while (p < len) {
    size_t v = rv_find_word(body, len, p, "val");
    size_t w = rv_find_word(body, len, p, "var");
    size_t use = (size_t)-1;
    if (v != (size_t)-1 && (w == (size_t)-1 || v < w)) {
      use = v;
    } else if (w != (size_t)-1) {
      use = w;
    }
    if (use == (size_t)-1) {
      break;
    }
    char *var = NULL;
    char *cls = NULL;
    size_t end = 0;
    if (rv_parse_intent_var(body, len, use, &var, &cls, &end)) {
      if (nvars == capvars) {
        size_t ncap = capvars == 0 ? 4 : capvars * 2;
        vars = ccb_xrealloc(vars, ncap * sizeof(*vars));
        capvars = ncap;
      }
      vars[nvars].var = var;
      vars[nvars].cls = cls;
      nvars++;
      p = end;
    } else {
      p = use + 3;
    }
  }
  ccb_str_init(&out);
  while (i < len) {
    // findViewById<...>(R.id.x) or findViewById(R.id.x) -> x.
    if (rv_at_word(body, len, i, "findViewById")) {
      size_t after = rv_rewrite_find_view(&out, body, len, i);
      if (after != 0) {
        i = after;
        continue;
      }
    }
    // startActivity(var) where var held Intent(this, C::class.java).
    if (rv_at_word(body, len, i, "startActivity")) {
      size_t after = rv_rewrite_start_activity(&out, body, len, i,
        vars, nvars);
      if (after != 0) {
        i = after;
        continue;
      }
    }
    // CActivity::class.java / CFragment::class.java -> C::class.java.
    if (rv_is_id_start(body[i])) {
      size_t e = rv_ident_end(body, len, i);
      if (e + 12 <= len && memcmp(body + e, "::class.java", 12) == 0) {
        size_t n = e - i;
        size_t keep = rv_stripped_len(body + i, n);
        ccb_str_append_n(&out, body + i, keep);
        ccb_str_append(&out, "::class.java");
        i = e + 12;
        continue;
      }
      ccb_str_append_n(&out, body + i, e - i);
      i = e;
      continue;
    }
    ccb_str_append_c(&out, body[i]);
    i++;
  }
  {
    size_t k = 0;
    for (k = 0; k < nvars; k++) {
      free(vars[k].var);
      free(vars[k].cls);
    }
    free(vars);
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

// True when the body range contains a direct nav-ish call
// (nav/navigateTo/startActivity/navigate followed by '(').
// A warning heuristic only — it never creates an edge.
int rv_body_has_nav_direct(rv_activity *a, size_t start,
    size_t end) {
  static const char *navs[] = {
    "nav", "navigateTo", "startActivity", "navigate",
  };
  size_t n = 0;
  const char *s = a->text;
  size_t len = a->text_len;
  for (n = 0; n < 4; n++) {
    size_t hit = rv_find_word(s, len, start, navs[n]);
    while (hit != (size_t)-1 && hit < end) {
      size_t j = rv_skip_ws(s, len, hit + strlen(navs[n]));
      if (j < end && s[j] == '(') {
        return 1;
      }
      hit = rv_find_word(s, len, hit + 1, navs[n]);
    }
  }
  return 0;
}

// True when the fun named at `hit` is called bare (not `x.name(`)
// and its body contains a direct nav.
static int rv_hit_calls_nav(rv_activity *a, size_t start, size_t end,
    size_t hit, size_t f) {
  size_t j = 0;
  if (hit > start && a->text[hit - 1] == '.') {
    return 0;
  }
  j = rv_skip_ws(a->text, a->text_len,
    hit + strlen(a->funs[f].name));
  if (j >= end || a->text[j] != '(') {
    return 0;
  }
  return rv_body_has_nav_direct(a, a->funs[f].brace + 1,
    a->funs[f].close);
}

// The direct check, plus one level of helper calls: a bare call to
// one of the activity's member funs whose body contains a direct
// nav. One level only, so helper cycles terminate.
int rv_body_has_nav(rv_activity *a, size_t start, size_t end) {
  size_t i = 0;
  const char *s = a->text;
  size_t len = a->text_len;
  if (rv_body_has_nav_direct(a, start, end)) {
    return 1;
  }
  for (i = 0; i < a->nfuns; i++) {
    size_t hit = rv_find_word(s, len, start, a->funs[i].name);
    while (hit != (size_t)-1 && hit < end) {
      if (rv_hit_calls_nav(a, start, end, hit, i)) {
        return 1;
      }
      hit = rv_find_word(s, len, hit + 1, a->funs[i].name);
    }
  }
  return 0;
}

// Collect member funs at class-body depth 1 (direct members only;
// local funs inside onCreate sit deeper). onCreate itself is
// skipped: its attachments are extracted separately.
void rv_collect_funs(rv_activity *a) {
  size_t i = a->class_brace + 1;
  long depth = 1;
  while (i < a->class_close) {
    char c = a->text[i];
    if (c == '"') {
      size_t after = 0;
      if (i + 2 < a->text_len && a->text[i + 1] == '"' &&
          a->text[i + 2] == '"') {
        after = rv_skip_raw_string(a->text, a->text_len, i);
        i = after == (size_t)-1 ? a->text_len : after;
      } else {
        i = rv_skip_quoted(a->text, a->text_len, i);
      }
      continue;
    }
    if (c == '/' && i + 1 < a->text_len && a->text[i + 1] == '/') {
      while (i < a->text_len && a->text[i] != '\n') {
        i++;
      }
      continue;
    }
    if (c == '/' && i + 1 < a->text_len && a->text[i + 1] == '*') {
      size_t after = rv_skip_block_comment(a->text, a->text_len, i);
      i = after == (size_t)-1 ? a->text_len : after;
      continue;
    }
    if (c == '{') {
      depth++;
      i++;
      continue;
    }
    if (c == '}') {
      depth--;
      i++;
      continue;
    }
    if (depth == 1 && rv_at_word(a->text, a->text_len, i, "fun")) {
      size_t j = rv_skip_ws(a->text, a->text_len, i + 3);
      size_t ne = 0;
      char *name = NULL;
      size_t pc = 0;
      size_t open = 0;
      size_t brace = 0;
      size_t close = 0;
      if (j >= a->text_len || !rv_is_id_start(a->text[j])) {
        i++;
        continue;
      }
      ne = rv_ident_end(a->text, a->text_len, j);
      name = ccb_xstrndup(a->text + j, ne - j);
      if (strcmp(name, "onCreate") == 0) {
        free(name);
        i = ne;
        continue;
      }
      open = rv_skip_ws(a->text, a->text_len, ne);
      if (open >= a->text_len || a->text[open] != '(') {
        free(name);
        i = ne;
        continue;
      }
      pc = rv_match(a->text, a->text_len, open, '(', ')');
      if (pc == (size_t)-1) {
        free(name);
        i = ne;
        continue;
      }
      brace = rv_skip_ws(a->text, a->text_len, pc + 1);
      if (brace >= a->text_len || a->text[brace] != '{') {
        free(name);
        i = ne;
        continue;
      }
      close = rv_match(a->text, a->text_len, brace, '{', '}');
      if (close == (size_t)-1) {
        free(name);
        i = ne;
        continue;
      }
      if (a->nfuns == a->capfuns) {
        size_t ncap = a->capfuns == 0 ? 4 : a->capfuns * 2;
        a->funs = ccb_xrealloc(a->funs, ncap * sizeof(*a->funs));
        a->capfuns = ncap;
      }
      a->funs[a->nfuns].name = name;
      a->funs[a->nfuns].params = ccb_xstrndup(a->text + open + 1,
        pc - (open + 1));
      a->funs[a->nfuns].brace = brace;
      a->funs[a->nfuns].close = close;
      a->funs[a->nfuns].line = rv_line_of(a->text, j);
      a->nfuns++;
      i = close + 1;
      continue;
    }
    i++;
  }
}
