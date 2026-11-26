// ccb_preview_css: CSS subset parser + box layout for terminal preview.
// Implements pv_css_parse and pv_layout_screen from ccb_preview.h.
#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_diag.h"

// ---------------------------------------------------------------- CSS --

typedef struct {
  const char *text;
  size_t len;
  size_t pos;
  long line;
  const char *file;
  int strict;
  ccb_diag_list *diags;
  ccb_arena *arena;
  pv_rule *rules;
  size_t nrules;
} css_parser;

static int css_is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int css_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int css_is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int css_is_hex(char c) {
  return css_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static char css_lower(char c) {
  if (c >= 'A' && c <= 'Z') {
    return (char)(c - 'A' + 'a');
  }
  return c;
}

// Case-insensitive comparison of a (ptr,len) range against a literal.
static int css_eq_lit(const char *s, size_t n, const char *lit) {
  size_t m = strlen(lit);
  size_t i = 0;
  if (n != m) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (css_lower(s[i]) != lit[i]) {
      return 0;
    }
  }
  return 1;
}

static void css_trim(const char **s, size_t *n) {
  while (*n > 0 && css_is_space((*s)[0])) {
    (*s)++;
    (*n)--;
  }
  while (*n > 0 && css_is_space((*s)[*n - 1])) {
    (*n)--;
  }
}

static void css_diag(css_parser *ps, const char *code, const char *message, long line) {
  ccb_diag d;
  if (ps->diags == NULL) {
    return;
  }
  ccb_diag_init(&d);
  // Note: ccb_diag has no severity field, so strict promotion of a
  // warning to an error is the caller's job (it counts these codes
  // as errors when strict is set); the code emitted is identical.
  CCB_UNUSED(ps->strict);
  ccb_diag_set_full(&d, code, "preview", message, ps->file, line, -1,
    NULL, NULL, NULL);
  ccb_diag_list_push(ps->diags, &d);
}

static void css_diag_prop(css_parser *ps, const char *code, const char *what,
    const char *prop, size_t prop_len, const char *val, size_t val_len, long line) {
  ccb_str msg;
  size_t pn = prop_len > 200 ? 200 : prop_len;
  size_t vn = val_len > 200 ? 200 : val_len;
  ccb_str_init(&msg);
  ccb_str_append(&msg, what);
  ccb_str_append(&msg, " '");
  ccb_str_append_n(&msg, prop, pn);
  ccb_str_append(&msg, "'");
  if (val != NULL) {
    ccb_str_append(&msg, ": '");
    ccb_str_append_n(&msg, val, vn);
    ccb_str_append(&msg, "'");
  }
  css_diag(ps, code, msg.buf != NULL ? msg.buf : "", line);
  ccb_str_free(&msg);
}

static int css_peek(css_parser *ps) {
  if (ps->pos >= ps->len) {
    return -1;
  }
  return (unsigned char)ps->text[ps->pos];
}

static int css_advance(css_parser *ps) {
  char c = '\0';
  if (ps->pos >= ps->len) {
    return -1;
  }
  c = ps->text[ps->pos];
  ps->pos++;
  if (c == '\n') {
    ps->line++;
  }
  return (unsigned char)c;
}

// Skip whitespace and /* */ comments. An unterminated comment is a
// parse error; the rest of the input is treated as comment text.
static void css_skip_ws(css_parser *ps) {
  for (;;) {
    while (ps->pos < ps->len && css_is_space(ps->text[ps->pos])) {
      css_advance(ps);
    }
    if (ps->pos + 1 < ps->len && ps->text[ps->pos] == '/' &&
        ps->text[ps->pos + 1] == '*') {
      int closed = 0;
      css_advance(ps);
      css_advance(ps);
      while (ps->pos < ps->len) {
        if (ps->text[ps->pos] == '*' && ps->pos + 1 < ps->len &&
            ps->text[ps->pos + 1] == '/') {
          css_advance(ps);
          css_advance(ps);
          closed = 1;
          break;
        }
        css_advance(ps);
      }
      if (!closed) {
        css_diag(ps, "CCB-P1002", "unterminated CSS comment", ps->line);
      }
      continue;
    }
    break;
  }
}

// Parse a signed integer token; digits only, capped to avoid overflow.
static int css_parse_int(const char *s, size_t n, long *out) {
  size_t i = 0;
  int neg = 0;
  long long v = 0;
  css_trim(&s, &n);
  if (n == 0) {
    return 0;
  }
  if (s[0] == '-') {
    neg = 1;
    i = 1;
  } else if (s[0] == '+') {
    i = 1;
  }
  if (i >= n) {
    return 0;
  }
  for (; i < n; i++) {
    if (!css_is_digit(s[i])) {
      return 0;
    }
    v = v * 10 + (s[i] - '0');
    if (v > 1000000000LL) {
      v = 1000000000LL;
    }
  }
  if (neg) {
    v = -v;
  }
  *out = (long)v;
  return 1;
}

static int css_ends_px(const char *s, size_t n) {
  return n >= 2 && css_lower(s[n - 2]) == 'p' && css_lower(s[n - 1]) == 'x';
}

// A px value: "12px", or bare "0".
static int css_parse_px(const char *s, size_t n, long *out) {
  css_trim(&s, &n);
  if (n == 1 && s[0] == '0') {
    *out = 0;
    return 1;
  }
  if (!css_ends_px(s, n)) {
    return 0;
  }
  return css_parse_int(s, n - 2, out);
}

// A length value: px or %.
static int css_parse_len(const char *s, size_t n, pv_len *out) {
  long v = 0;
  css_trim(&s, &n);
  if (n == 1 && s[0] == '0') {
    out->kind = PV_LEN_PX;
    out->value = 0;
    return 1;
  }
  if (css_ends_px(s, n)) {
    if (!css_parse_int(s, n - 2, &v)) {
      return 0;
    }
    out->kind = PV_LEN_PX;
    out->value = v;
    return 1;
  }
  if (n >= 1 && s[n - 1] == '%') {
    if (!css_parse_int(s, n - 1, &v) || v < 0) {
      return 0;
    }
    out->kind = PV_LEN_PCT;
    out->value = v;
    return 1;
  }
  return 0;
}

// padding/margin shorthand: 1-4 px values, CSS-style expansion.
static int css_parse_box(const char *s, size_t n, long out[4]) {
  long vals[4];
  size_t count = 0;
  size_t i = 0;
  css_trim(&s, &n);
  while (i < n) {
    size_t start = 0;
    size_t end = 0;
    long v = 0;
    while (i < n && css_is_space(s[i])) {
      i++;
    }
    if (i >= n) {
      break;
    }
    start = i;
    while (i < n && !css_is_space(s[i])) {
      i++;
    }
    end = i;
    if (count >= 4) {
      return 0;
    }
    if (!css_parse_px(s + start, end - start, &v)) {
      return 0;
    }
    vals[count] = v;
    count++;
  }
  if (count == 0) {
    return 0;
  }
  if (count == 1) {
    out[0] = vals[0];
    out[1] = vals[0];
    out[2] = vals[0];
    out[3] = vals[0];
  } else if (count == 2) {
    out[0] = vals[0];
    out[1] = vals[1];
    out[2] = vals[0];
    out[3] = vals[1];
  } else if (count == 3) {
    out[0] = vals[0];
    out[1] = vals[1];
    out[2] = vals[2];
    out[3] = vals[1];
  } else {
    out[0] = vals[0];
    out[1] = vals[1];
    out[2] = vals[2];
    out[3] = vals[3];
  }
  return 1;
}

static int css_valid_color(const char *s, size_t n);

// A color value for color/background-color: hex, a named color, or
// an @color/name reference (kept literally at parse time and
// resolved later by the project loader; see ccb_preview.c).
static int css_valid_color_resolvable(const char *s, size_t n) {
  size_t i = 0;
  css_trim(&s, &n);
  if (n >= 7 && memcmp(s, "@color/", 7) == 0) {
    if (n == 7) {
      return 0;
    }
    for (i = 7; i < n; i++) {
      char c = s[i];
      if (!css_is_alpha(c) && !css_is_digit(c) && c != '_' && c != '-') {
        return 0;
      }
    }
    return 1;
  }
  return css_valid_color(s, n);
}

static int css_valid_color(const char *s, size_t n) {
  size_t i = 0;
  css_trim(&s, &n);
  if (n == 0) {
    return 0;
  }
  if (s[0] == '#') {
    if (n != 4 && n != 7) {
      return 0;
    }
    for (i = 1; i < n; i++) {
      if (!css_is_hex(s[i])) {
        return 0;
      }
    }
    return 1;
  }
  for (i = 0; i < n; i++) {
    if (!css_is_alpha(s[i])) {
      return 0;
    }
  }
  return 1;
}

static pv_rule *css_find_or_create(css_parser *ps, const char *sel, size_t sel_len,
    long sel_line, int *is_new) {
  size_t i = 0;
  pv_rule *grown = NULL;
  pv_rule *rule = NULL;
  size_t base_idx = (size_t)-1;
  const char *base_str = NULL;
  size_t base_len = 0;
  int per_screen = 0;
  *is_new = 0;
  // Scope candidate for this file: the basename minus ".css", for a
  // per-screen file only. common.css (and non-CSS files) have none.
  if (ps->file != NULL) {
    const char *slash = strrchr(ps->file, '/');
    base_str = slash != NULL ? slash + 1 : ps->file;
    base_len = strlen(base_str);
    per_screen = base_len > 4 &&
      strcmp(base_str + base_len - 4, ".css") == 0 &&
      strcmp(base_str, "common.css") != 0;
  }
  for (i = 0; i < ps->nrules; i++) {
    if (ps->rules[i].selector != NULL &&
        strlen(ps->rules[i].selector) == sel_len &&
        memcmp(ps->rules[i].selector, sel, sel_len) == 0) {
      int same_file = ps->rules[i].file != NULL && ps->file != NULL &&
        strcmp(ps->rules[i].file, ps->file) == 0;
      int same_scope = per_screen && ps->rules[i].scope != NULL &&
        strlen(ps->rules[i].scope) == base_len - 4 &&
        memcmp(ps->rules[i].scope, base_str, base_len - 4) == 0;
      if (base_idx == (size_t)-1) {
        base_idx = i;
      }
      // Merge into a rule this file already owns (created by it, or
      // already scoped to its screen): no copy, plain property merge.
      if (same_file || same_scope) {
        return &ps->rules[i];
      }
    }
  }
  // Merge fix: a per-screen file redeclaring a selector first seen
  // elsewhere does NOT mutate that rule's scope. Mutating it would
  // drop the earlier file's properties for every other screen (e.g.
  // Form.css redeclaring common's textfield would steal common's
  // margin from Settings). Instead, split: keep the earlier rule
  // untouched, and append a copy carrying the merged properties
  // (the caller applies this file's declarations to the copy) with
  // this file's scope. layout_find_rule prefers an exact-scope rule
  // over a common one, so the screen gets common + per-screen
  // properties while other screens keep the earlier rule. One
  // merged rule cannot carry two screens' scopes, so a second
  // per-screen file gets its own copy the same way (e.g. Empty.css's
  // column rule survives Settings.css's column redeclaration).
  // Merges from common.css, and merges within one file, keep the
  // plain single-rule path above.
  if (base_idx != (size_t)-1 && per_screen) {
    grown = ccb_arena_alloc(ps->arena, (ps->nrules + 1) * sizeof(pv_rule));
    if (ps->nrules > 0) {
      memcpy(grown, ps->rules, ps->nrules * sizeof(pv_rule));
    }
    rule = &grown[ps->nrules];
    *rule = grown[base_idx];
    rule->file = ccb_arena_strdup(ps->arena, ps->file);
    rule->scope = ccb_arena_strndup(ps->arena, base_str, base_len - 4);
    ps->rules = grown;
    ps->nrules++;
    *is_new = 1;
    return rule;
  }
  if (base_idx != (size_t)-1) {
    return &ps->rules[base_idx];
  }
  // Grow by one: arena arrays cannot be realloc'd, so copy forward.
  // String pointers inside the structs stay valid (arena-owned).
  grown = ccb_arena_alloc(ps->arena, (ps->nrules + 1) * sizeof(pv_rule));
  if (ps->nrules > 0) {
    memcpy(grown, ps->rules, ps->nrules * sizeof(pv_rule));
  }
  memset(&grown[ps->nrules], 0, sizeof(pv_rule));
  rule = &grown[ps->nrules];
  rule->selector = ccb_arena_strndup(ps->arena, sel, sel_len);
  rule->file = ps->file != NULL ? ccb_arena_strdup(ps->arena, ps->file) : NULL;
  rule->line = sel_line;
  ps->rules = grown;
  ps->nrules++;
  *is_new = 1;
  return rule;
}

static void css_apply(css_parser *ps, pv_rule *rule, int is_new,
    int *first_decl_done, const char *prop, size_t prop_len, const char *val,
    size_t val_len, long decl_line) {
  pv_len len;
  long v = 0;
  long boxv[4];
  const char *vtrim = val;
  size_t vn = val_len;
  if (is_new && !*first_decl_done) {
    // Header: line is the line of the first declaration for this selector.
    rule->line = decl_line;
    *first_decl_done = 1;
  }
  css_trim(&vtrim, &vn);
  if (css_eq_lit(prop, prop_len, "display")) {
    if (css_eq_lit(vtrim, vn, "block")) {
      rule->has_display = 1;
      rule->display_flex = 0;
    } else if (css_eq_lit(vtrim, vn, "flex")) {
      rule->has_display = 1;
      rule->display_flex = 1;
    } else {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    }
  } else if (css_eq_lit(prop, prop_len, "flex-direction")) {
    if (css_eq_lit(vtrim, vn, "column")) {
      rule->has_flex_dir = 1;
      rule->flex_row = 0;
    } else if (css_eq_lit(vtrim, vn, "row")) {
      rule->has_flex_dir = 1;
      rule->flex_row = 1;
    } else {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    }
  } else if (css_eq_lit(prop, prop_len, "width") ||
      css_eq_lit(prop, prop_len, "height")) {
    int is_width = css_eq_lit(prop, prop_len, "width");
    if (!css_parse_len(vtrim, vn, &len) ||
        (len.kind == PV_LEN_PX && len.value < 0)) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else if (is_width) {
      rule->has_width = 1;
      rule->width = len;
    } else {
      rule->has_height = 1;
      rule->height = len;
    }
  } else if (css_eq_lit(prop, prop_len, "padding") ||
      css_eq_lit(prop, prop_len, "margin")) {
    int is_padding = css_eq_lit(prop, prop_len, "padding");
    if (!css_parse_box(vtrim, vn, boxv)) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else if (is_padding) {
      rule->has_padding = 1;
      memcpy(rule->padding, boxv, sizeof(boxv));
    } else {
      rule->has_margin = 1;
      memcpy(rule->margin, boxv, sizeof(boxv));
    }
  } else if (css_eq_lit(prop, prop_len, "background-color")) {
    if (!css_valid_color_resolvable(vtrim, vn)) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else {
      rule->bg = ccb_arena_strndup(ps->arena, vtrim, vn);
    }
  } else if (css_eq_lit(prop, prop_len, "color")) {
    if (!css_valid_color_resolvable(vtrim, vn)) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else {
      rule->fg = ccb_arena_strndup(ps->arena, vtrim, vn);
    }
  } else if (css_eq_lit(prop, prop_len, "text-align")) {
    if (css_eq_lit(vtrim, vn, "left")) {
      rule->has_text_align = 1;
      rule->text_align = 0;
    } else if (css_eq_lit(vtrim, vn, "center")) {
      rule->has_text_align = 1;
      rule->text_align = 1;
    } else if (css_eq_lit(vtrim, vn, "right")) {
      rule->has_text_align = 1;
      rule->text_align = 2;
    } else {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    }
  } else if (css_eq_lit(prop, prop_len, "border-width")) {
    if (!css_parse_px(vtrim, vn, &v) || v < 0) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else {
      rule->has_border_width = 1;
      rule->border_width = v;
    }
  } else if (css_eq_lit(prop, prop_len, "border-color")) {
    if (!css_valid_color(vtrim, vn)) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else {
      rule->border_color = ccb_arena_strndup(ps->arena, vtrim, vn);
    }
  } else if (css_eq_lit(prop, prop_len, "flex-wrap")) {
    if (css_eq_lit(vtrim, vn, "nowrap")) {
      rule->has_flex_wrap = 1;
      rule->flex_wrap = 0;
    } else if (css_eq_lit(vtrim, vn, "wrap")) {
      rule->has_flex_wrap = 1;
      rule->flex_wrap = 1;
    } else {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    }
  } else if (css_eq_lit(prop, prop_len, "font-size")) {
    if (!css_parse_px(vtrim, vn, &v) || v < 0) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else {
      rule->has_font_size = 1;
      rule->font_size = v;
    }
  } else if (css_eq_lit(prop, prop_len, "position")) {
    if (css_eq_lit(vtrim, vn, "static")) {
      rule->has_position = 1;
      rule->absolute = 0;
    } else if (css_eq_lit(vtrim, vn, "absolute")) {
      rule->has_position = 1;
      rule->absolute = 1;
    } else {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    }
  } else if (css_eq_lit(prop, prop_len, "top") ||
      css_eq_lit(prop, prop_len, "left")) {
    int is_top = css_eq_lit(prop, prop_len, "top");
    if (!css_parse_px(vtrim, vn, &v)) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else if (is_top) {
      rule->has_top = 1;
      rule->top = v;
    } else {
      rule->has_left = 1;
      rule->left = v;
    }
  } else if (css_eq_lit(prop, prop_len, "gap")) {
    if (!css_parse_px(vtrim, vn, &v) || v < 0) {
      css_diag_prop(ps, "CCB-P1014", "unsupported CSS value for property",
        prop, prop_len, vtrim, vn, decl_line);
    } else {
      rule->has_gap = 1;
      rule->gap = v;
    }
  } else {
    css_diag_prop(ps, "CCB-P1001", "unknown CSS property",
      prop, prop_len, NULL, 0, decl_line);
  }
}

// Skip a block's contents after an empty selector, balancing braces.
static void css_skip_block(css_parser *ps) {
  long depth = 1;
  while (ps->pos < ps->len && depth > 0) {
    int c = css_advance(ps);
    if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
    }
  }
}

// Copy every property the source rule sets onto the target, the
// same property-by-property merge the parser applies declaration by
// declaration. Only has_* properties copy: selector, file, line,
// and scope keep the target's values. Color strings copy as
// pointers (arena-owned by the caller's arena or a staging arena
// that outlives the merge); NULL means unset and never overwrites.
static void css_rule_merge_props(pv_rule *dst, const pv_rule *src) {
  size_t i = 0;
  if (src->has_display) {
    dst->has_display = 1;
    dst->display_flex = src->display_flex;
  }
  if (src->has_flex_dir) {
    dst->has_flex_dir = 1;
    dst->flex_row = src->flex_row;
  }
  if (src->has_width) {
    dst->has_width = 1;
    dst->width = src->width;
  }
  if (src->has_height) {
    dst->has_height = 1;
    dst->height = src->height;
  }
  if (src->has_padding) {
    dst->has_padding = 1;
    for (i = 0; i < 4; i++) {
      dst->padding[i] = src->padding[i];
    }
  }
  if (src->has_margin) {
    dst->has_margin = 1;
    for (i = 0; i < 4; i++) {
      dst->margin[i] = src->margin[i];
    }
  }
  if (src->bg != NULL) {
    dst->bg = src->bg;
  }
  if (src->fg != NULL) {
    dst->fg = src->fg;
  }
  if (src->has_font_size) {
    dst->has_font_size = 1;
    dst->font_size = src->font_size;
  }
  if (src->has_position) {
    dst->has_position = 1;
    dst->absolute = src->absolute;
  }
  if (src->has_top) {
    dst->has_top = 1;
    dst->top = src->top;
  }
  if (src->has_left) {
    dst->has_left = 1;
    dst->left = src->left;
  }
  if (src->has_gap) {
    dst->has_gap = 1;
    dst->gap = src->gap;
  }
  if (src->has_text_align) {
    dst->has_text_align = 1;
    dst->text_align = src->text_align;
  }
  if (src->has_border_width) {
    dst->has_border_width = 1;
    dst->border_width = src->border_width;
  }
  if (src->border_color != NULL) {
    dst->border_color = src->border_color;
  }
  if (src->has_flex_wrap) {
    dst->has_flex_wrap = 1;
    dst->flex_wrap = src->flex_wrap;
  }
}

void pv_css_rule_merge(ccb_arena *arena, pv_rule **rules_io,
    size_t *nrules_io, const pv_rule *src, const char *file,
    const char *scope) {
  size_t i = 0;
  size_t base_idx = (size_t)-1;
  pv_rule *grown = NULL;
  pv_rule *rule = NULL;
  if (arena == NULL || rules_io == NULL || nrules_io == NULL ||
      src == NULL || src->selector == NULL) {
    return;
  }
  for (i = 0; i < *nrules_io; i++) {
    pv_rule *r = &(*rules_io)[i];
    if (r->selector != NULL && strcmp(r->selector, src->selector) == 0) {
      int same_file = r->file != NULL && file != NULL &&
        strcmp(r->file, file) == 0;
      int same_scope = scope != NULL && r->scope != NULL &&
        strcmp(r->scope, scope) == 0;
      if (base_idx == (size_t)-1) {
        base_idx = i;
      }
      // Merge into a rule this file already owns (created by it, or
      // already scoped to its screen): plain property merge, the
      // same branch the parser takes for same_file/same_scope.
      if (same_file || same_scope) {
        css_rule_merge_props(r, src);
        return;
      }
    }
  }
  // The parser's split path, for a per-screen file redeclaring a
  // selector first seen elsewhere: keep the earlier rule untouched
  // and append a copy carrying the merged properties with this
  // file's scope. The copy's line is the new rule's first
  // declaration line, matching the parser: its split copy is
  // returned as new, and the caller stamps the declaration line.
  if (base_idx != (size_t)-1 && scope != NULL) {
    grown = ccb_arena_alloc(arena, (*nrules_io + 1) * sizeof(pv_rule));
    if (*nrules_io > 0) {
      memcpy(grown, *rules_io, *nrules_io * sizeof(pv_rule));
    }
    rule = &grown[*nrules_io];
    *rule = grown[base_idx];
    rule->file = file != NULL ? ccb_arena_strdup(arena, file) : NULL;
    rule->scope = ccb_arena_strdup(arena, scope);
    rule->line = src->line;
    css_rule_merge_props(rule, src);
    *rules_io = grown;
    (*nrules_io)++;
    return;
  }
  // A common file merges into the first matching rule.
  if (base_idx != (size_t)-1) {
    css_rule_merge_props(&(*rules_io)[base_idx], src);
    return;
  }
  // A selector seen for the first time: append it with this file
  // and scope (NULL for common files; the screen name for a
  // per-screen file, matching the scope load used to tag new rules).
  grown = ccb_arena_alloc(arena, (*nrules_io + 1) * sizeof(pv_rule));
  if (*nrules_io > 0) {
    memcpy(grown, *rules_io, *nrules_io * sizeof(pv_rule));
  }
  rule = &grown[*nrules_io];
  memset(rule, 0, sizeof(*rule));
  rule->selector = ccb_arena_strdup(arena, src->selector);
  rule->file = file != NULL ? ccb_arena_strdup(arena, file) : NULL;
  rule->line = src->line;
  rule->scope = scope != NULL ? ccb_arena_strdup(arena, scope) : NULL;
  css_rule_merge_props(rule, src);
  *rules_io = grown;
  (*nrules_io)++;
}

ccb_status pv_css_parse(ccb_arena *arena, const char *text, const char *file,
    int strict, ccb_diag_list *diags, pv_rule **rules_io, size_t *nrules_io) {
  css_parser ps;
  memset(&ps, 0, sizeof(ps));
  ps.text = text != NULL ? text : "";
  ps.len = strlen(ps.text);
  ps.pos = 0;
  ps.line = 1;
  ps.file = file;
  ps.strict = strict;
  ps.diags = diags;
  ps.arena = arena;
  if (rules_io != NULL) {
    ps.rules = *rules_io;
  }
  if (nrules_io != NULL) {
    ps.nrules = *nrules_io;
  }
  if (arena == NULL) {
    // No arena: nothing can be stored; still scan for diagnostics.
    // (Callers always pass an arena per the header contract.)
    if (rules_io != NULL) {
      *rules_io = NULL;
    }
    if (nrules_io != NULL) {
      *nrules_io = 0;
    }
    return CCB_OK;
  }
  for (;;) {
    long sel_line = 0;
    size_t sel_start = 0;
    size_t sel_end = 0;
    const char *sel = NULL;
    size_t sel_len = 0;
    int term = 0;
    int is_new = 0;
    int first_decl_done = 0;
    pv_rule *rule = NULL;
    css_skip_ws(&ps);
    if (ps.pos >= ps.len) {
      break;
    }
    if (css_peek(&ps) == ';') {
      css_advance(&ps);
      continue;
    }
    if (css_peek(&ps) == '}') {
      css_diag(&ps, "CCB-P1002", "unexpected '}' in CSS", ps.line);
      css_advance(&ps);
      continue;
    }
    sel_line = ps.line;
    sel_start = ps.pos;
    while (ps.pos < ps.len) {
      char c = ps.text[ps.pos];
      if (c == '{' || c == ';' || c == '}') {
        term = c;
        break;
      }
      css_advance(&ps);
    }
    sel_end = ps.pos;
    sel = ps.text + sel_start;
    sel_len = sel_end - sel_start;
    css_trim(&sel, &sel_len);
    if (term == ';') {
      if (sel_len > 0) {
        css_diag(&ps, "CCB-P1002", "expected '{' in CSS rule", ps.line);
      }
      css_advance(&ps);
      continue;
    }
    if (term == '}') {
      if (sel_len > 0) {
        css_diag(&ps, "CCB-P1002", "expected '{' in CSS rule", ps.line);
      }
      css_advance(&ps);
      continue;
    }
    if (term == 0) {
      if (sel_len > 0) {
        css_diag(&ps, "CCB-P1002", "unclosed CSS rule", ps.line);
      }
      break;
    }
    // term == '{'
    if (sel_len == 0) {
      css_diag(&ps, "CCB-P1002", "empty CSS selector", ps.line);
      css_advance(&ps);
      css_skip_block(&ps);
      continue;
    }
    css_advance(&ps); // consume '{'
    rule = css_find_or_create(&ps, sel, sel_len, sel_line, &is_new);
    for (;;) {
      long decl_line = 0;
      size_t prop_start = 0;
      size_t prop_end = 0;
      const char *prop = NULL;
      size_t prop_len = 0;
      int pterm = 0;
      size_t val_start = 0;
      size_t val_end = 0;
      const char *val = NULL;
      size_t val_len = 0;
      int vterm = 0;
      css_skip_ws(&ps);
      if (ps.pos >= ps.len) {
        css_diag(&ps, "CCB-P1002", "unclosed CSS block", ps.line);
        break;
      }
      if (css_peek(&ps) == '}') {
        css_advance(&ps);
        break;
      }
      if (css_peek(&ps) == ';') {
        css_advance(&ps);
        continue;
      }
      decl_line = ps.line;
      prop_start = ps.pos;
      while (ps.pos < ps.len) {
        char c = ps.text[ps.pos];
        if (c == ':' || c == ';' || c == '}') {
          pterm = c;
          break;
        }
        css_advance(&ps);
      }
      prop_end = ps.pos;
      prop = ps.text + prop_start;
      prop_len = prop_end - prop_start;
      css_trim(&prop, &prop_len);
      if (pterm == ';') {
        if (prop_len > 0) {
          css_diag(&ps, "CCB-P1002", "malformed CSS declaration", decl_line);
        }
        css_advance(&ps);
        continue;
      }
      if (pterm == '}') {
        if (prop_len > 0) {
          css_diag(&ps, "CCB-P1002", "malformed CSS declaration", decl_line);
        }
        css_advance(&ps);
        break;
      }
      if (pterm == 0) {
        if (prop_len > 0) {
          css_diag(&ps, "CCB-P1002", "unclosed CSS block", decl_line);
        }
        break;
      }
      css_advance(&ps); // consume ':'
      val_start = ps.pos;
      while (ps.pos < ps.len) {
        char c = ps.text[ps.pos];
        if (c == ';' || c == '}') {
          vterm = c;
          break;
        }
        css_advance(&ps);
      }
      val_end = ps.pos;
      val = ps.text + val_start;
      val_len = val_end - val_start;
      if (prop_len == 0) {
        css_diag(&ps, "CCB-P1002", "malformed CSS declaration", decl_line);
      } else {
        css_apply(&ps, rule, is_new, &first_decl_done, prop, prop_len,
          val, val_len, decl_line);
      }
      if (vterm == ';') {
        css_advance(&ps);
        continue;
      }
      if (vterm == '}') {
        css_advance(&ps);
        break;
      }
      css_diag(&ps, "CCB-P1002", "unclosed CSS block", ps.line);
      break;
    }
    if (ps.pos >= ps.len) {
      break;
    }
  }
  if (rules_io != NULL) {
    *rules_io = ps.rules;
  }
  if (nrules_io != NULL) {
    *nrules_io = ps.nrules;
  }
  return CCB_OK;
}

// --------------------------------------------------------------- Layout --

static int box_type_eq(const pv_box *box, const char *type) {
  return box != NULL && box->type != NULL && strcmp(box->type, type) == 0;
}

static int box_is_leaf_type(const pv_box *box) {
  return box_type_eq(box, "text") || box_type_eq(box, "button") ||
    box_type_eq(box, "textfield") || box_type_eq(box, "list") ||
    box_type_eq(box, "image") || box_type_eq(box, "spacer");
}

static int box_is_branch(const pv_box *box) {
  return box_type_eq(box, "branch") || box_type_eq(box, "branch-entry") ||
    (box != NULL && box->is_branch);
}

static int box_is_container(const pv_box *box) {
  if (box == NULL) {
    return 0;
  }
  if (box_is_leaf_type(box)) {
    return 0;
  }
  if (box_type_eq(box, "screen") || box_type_eq(box, "column") ||
      box_type_eq(box, "row") || box_is_branch(box)) {
    return 1;
  }
  if (box_type_eq(box, "box")) {
    // Phase 2: a generic box with children contains them, but a
    // childless one is a leaf-like control (a Switch, a bare view)
    // and takes the 1-row leaf height — otherwise it sums to zero
    // rows and vanishes from the wireframe entirely.
    return box->nchildren > 0;
  }
  return box->nchildren > 0;
}

static const pv_rule *layout_find_rule(const pv_project *p, const pv_box *box) {
  size_t i = 0;
  if (p == NULL || box == NULL || box->type == NULL || p->rules == NULL) {
    return NULL;
  }
  if (p->layout_screen_name == NULL) {
    // Unit tests laying out by hand: first selector match wins, any scope.
    for (i = 0; i < p->nrules; i++) {
      if (p->rules[i].selector != NULL &&
          strcmp(p->rules[i].selector, box->type) == 0) {
        return &p->rules[i];
      }
    }
    return NULL;
  }
  // Scope rule: a rule scoped to another screen does not apply here.
  // An exact-scope rule (the per-screen copy made by the parser's
  // merge split) wins over a NULL-scope common rule, regardless of
  // order, because the copy already carries the common properties.
  for (i = 0; i < p->nrules; i++) {
    if (p->rules[i].scope != NULL &&
        strcmp(p->rules[i].scope, p->layout_screen_name) == 0 &&
        p->rules[i].selector != NULL &&
        strcmp(p->rules[i].selector, box->type) == 0) {
      return &p->rules[i];
    }
  }
  for (i = 0; i < p->nrules; i++) {
    if (p->rules[i].scope == NULL &&
        p->rules[i].selector != NULL &&
        strcmp(p->rules[i].selector, box->type) == 0) {
      return &p->rules[i];
    }
  }
  return NULL;
}

// Round(px * interior / dp), half away from zero, without libm.
static long layout_scale_px(long px, long interior, long dp) {
  long long num = 0;
  long long half = 0;
  if (dp <= 0 || interior <= 0) {
    return 0;
  }
  num = (long long)px * (long long)interior;
  half = dp / 2;
  if (num >= 0) {
    return (long)((num + half) / dp);
  }
  return (long)(-((-num + half) / dp));
}

static long layout_scale_pct(long pct, long parent) {
  long long num = 0;
  if (parent <= 0) {
    return 0;
  }
  num = (long long)pct * (long long)parent;
  if (num >= 0) {
    return (long)((num + 50) / 100);
  }
  return (long)(-((-num + 50) / 100));
}

// F2: columns to dp, rounded half away from zero via the same
// no-libm pattern as layout_scale_px; rows to dp at 10dp per row.
long pv_cols_to_dp(long cols, long interior_cols, long phone_dp_width) {
  long long num = 0;
  if (interior_cols <= 0 || phone_dp_width <= 0 || cols <= 0) {
    return 0;
  }
  num = (long long)cols * (long long)phone_dp_width;
  return (long)((num + interior_cols / 2) / interior_cols);
}

long pv_rows_to_dp(long rows) {
  return rows <= 0 ? 0 : rows * 10;
}

// Scaled sides, top/right/bottom/left. Padding is clamped at zero;
// margins keep their sign (negative margins are a real input).
static void layout_sides(const pv_project *p, const pv_rule *rule, int padding,
    long out[4]) {
  int i = 0;
  const long *src = NULL;
  int has = 0;
  for (i = 0; i < 4; i++) {
    out[i] = 0;
  }
  if (rule == NULL) {
    return;
  }
  if (padding) {
    has = rule->has_padding;
    src = rule->padding;
  } else {
    has = rule->has_margin;
    src = rule->margin;
  }
  if (!has) {
    return;
  }
  for (i = 0; i < 4; i++) {
    long v = layout_scale_px(src[i], p->interior_cols, p->phone_dp_width);
    if (padding && v < 0) {
      v = 0;
    }
    out[i] = v;
  }
}

static int layout_rule_width(const pv_project *p, const pv_rule *rule,
    long parent_content_w, long *out) {
  if (rule == NULL || !rule->has_width) {
    return 0;
  }
  if (rule->width.kind == PV_LEN_PX) {
    *out = layout_scale_px(rule->width.value, p->interior_cols,
      p->phone_dp_width);
  } else if (rule->width.kind == PV_LEN_PCT) {
    *out = layout_scale_pct(rule->width.value, parent_content_w);
  } else {
    *out = 0;
  }
  return 1;
}

static int layout_is_row(const pv_box *box, const pv_rule *rule) {
  if (rule != NULL && rule->has_flex_dir) {
    return rule->flex_row;
  }
  return box_type_eq(box, "row");
}

static int layout_is_absolute(const pv_project *p, const pv_box *box) {
  const pv_rule *rule = layout_find_rule(p, box);
  return rule != NULL && rule->has_position && rule->absolute;
}

// Border cells for a rule: the px border-width scaled like any other
// px value; 1 cell when the scaled value is >= 1, else no border
// (a border-width that scales to 0 draws nothing).
static int layout_border_cells(const pv_project *p, const pv_rule *rule) {
  long scaled = 0;
  if (rule == NULL || !rule->has_border_width) {
    return 0;
  }
  scaled = layout_scale_px(rule->border_width, p->interior_cols,
    p->phone_dp_width);
  return scaled >= 1 ? 1 : 0;
}

// F1 line height in rows for a text leaf's rule: one line of text at
// font-size * 1.2, with one row per ~10dp (the F2 row quantum), so
// rows = round(font-size * 1.2 / 10), minimum 1 row; 1 row when the
// rule sets no font-size (the pre-F1 behavior). Integer-only
// ((fs * 12 + 50) / 100); long long math so no font-size value the
// parser accepts can overflow.
static long layout_line_height_rows(const pv_rule *rule) {
  long long v = 0;
  long rows = 0;
  if (rule == NULL || !rule->has_font_size) {
    return 1;
  }
  v = (long long)rule->font_size * 12;
  rows = (long)((v + 50) / 100);
  return rows < 1 ? 1 : rows;
}

static long layout_box(pv_project *p, pv_box *box, long x, long y, long w,
    long parent_content_w, int depth);

static void layout_absolute(pv_project *p, pv_box *box, long content_w, int depth) {
  const pv_rule *rule = layout_find_rule(p, box);
  long lx = 0;
  long ty = 0;
  long cw = 0;
  long m[4];
  if (rule != NULL && rule->has_left) {
    lx = layout_scale_px(rule->left, p->interior_cols, p->phone_dp_width);
  }
  if (rule != NULL && rule->has_top) {
    ty = layout_scale_px(rule->top, p->interior_cols, p->phone_dp_width);
  }
  if (!layout_rule_width(p, rule, content_w, &cw)) {
    layout_sides(p, rule, 0, m);
    cw = content_w - m[1] - m[3];
    if (cw < 0) {
      cw = 0;
    }
  }
  layout_box(p, box, lx, ty, cw, content_w, depth);
}

static long layout_box(pv_project *p, pv_box *box, long x, long y, long w,
    long parent_content_w, int depth) {
  const pv_rule *rule = NULL;
  long pad[4];
  long content_x = 0;
  long content_w = 0;
  long content_y = 0;
  long h = 0;
  if (box == NULL) {
    return 0;
  }
  rule = layout_find_rule(p, box);
  box->rule = rule;
  box->x = x;
  box->y = y;
  box->w = w < 0 ? 0 : w;
  box->has_border = layout_border_cells(p, rule);
  layout_sides(p, rule, 1, pad);
  // Scaled padding, for the renderer to inset leaf content. Children
  // are positioned inside the parent content box below (content_x /
  // content_y already include the parent's padding), so parent padding
  // insets children without further changes here.
  box->pad_top_rows = pad[0];
  box->pad_right_cols = pad[1];
  box->pad_bottom_rows = pad[2];
  box->pad_left_cols = pad[3];
  // A bordered box insets its content box by 1 cell on all sides
  // (the outline itself occupies the border cells).
  content_x = x + pad[3] + box->has_border;
  content_w = box->w - pad[1] - pad[3] - 2 * box->has_border;
  if (content_w < 0) {
    content_w = 0;
  }
  content_y = y + pad[0] + box->has_border;
  if (depth > 512) {
    box->h = 1 + pad[0] + pad[2];
    return box->h;
  }
  if (!box_is_container(box)) {
    // Leaf heights: base rows + vertical padding. A spacer's base is
    // its height rule (px scaled, % of the parent content width, the
    // only parent size known bottom-up); otherwise 1 row.
    if (box_type_eq(box, "list")) {
      // Real rows when the list carries children (one row per child),
      // else the 4 placeholder rows for a bare list of unknown count.
      h = (box->nchildren > 0 ? (long)box->nchildren : 4) +
        pad[0] + pad[2];
    } else if (box_type_eq(box, "image")) {
      // F3: a declared height sizes the box (px scaled like the
      // spacer convention, % of the parent content width); with no
      // declared height the 3-row placeholder stands. A declared
      // width is already honored by the caller's fixed-width logic.
      // No intrinsic size exists in the convention, so one declared
      // side cannot preserve an aspect ratio — the other side
      // keeps its default.
      long ih = 3;
      if (rule != NULL && rule->has_height) {
        if (rule->height.kind == PV_LEN_PX) {
          ih = layout_scale_px(rule->height.value, p->interior_cols,
            p->phone_dp_width);
        } else if (rule->height.kind == PV_LEN_PCT) {
          ih = layout_scale_pct(rule->height.value, parent_content_w);
        }
        if (ih < 0) {
          ih = 0;
        }
      }
      h = ih + pad[0] + pad[2];
    } else if (box_type_eq(box, "spacer") && rule != NULL && rule->has_height) {
      long hv = 0;
      if (rule->height.kind == PV_LEN_PX) {
        hv = layout_scale_px(rule->height.value, p->interior_cols,
          p->phone_dp_width);
      } else if (rule->height.kind == PV_LEN_PCT) {
        hv = layout_scale_pct(rule->height.value, parent_content_w);
      }
      if (hv < 0) {
        hv = 0;
      }
      h = hv + pad[0] + pad[2];
    } else if (box_type_eq(box, "text") || box_type_eq(box, "button") ||
        box_type_eq(box, "textfield")) {
      // F1: one line of text at the rule's line height, plus
      // vertical padding. A text leaf with no font-size rule keeps
      // the old 1-row base.
      h = layout_line_height_rows(rule) + pad[0] + pad[2];
    } else {
      h = 1 + pad[0] + pad[2];
    }
    // A bordered box grows by 2 rows: top + bottom border.
    h += 2 * box->has_border;
    box->h = h;
    return h;
  }
  if (layout_is_row(box, rule)) {
    // Row: fixed widths honored first, the remainder shared equally
    // among auto children. Height is the sum of the children's
    // occupied heights plus vertical padding (per the task's layout
    // rules, containers sum their children even in a row).
    size_t i = 0;
    size_t n_nonabs = 0;
    size_t n_auto = 0;
    long total_fixed = 0;
    long gap = 0;
    long remainder = 0;
    long auto_base = 0;
    long auto_extra = 0;
    long cursor_x = 0;
    long sum_h = 0;
    size_t auto_idx = 0;
    int first = 1;
    if (rule != NULL && rule->has_gap) {
      gap = layout_scale_px(rule->gap, p->interior_cols, p->phone_dp_width);
      if (gap < 0) {
        gap = 0;
      }
    }
    for (i = 0; i < box->nchildren; i++) {
      pv_box *child = box->children[i];
      const pv_rule *cr = NULL;
      long m[4];
      long fw = 0;
      if (child == NULL || layout_is_absolute(p, child)) {
        continue;
      }
      cr = layout_find_rule(p, child);
      layout_sides(p, cr, 0, m);
      n_nonabs++;
      if (layout_rule_width(p, cr, content_w, &fw)) {
        total_fixed += m[1] + m[3] + fw;
      } else {
        total_fixed += m[1] + m[3];
        n_auto++;
      }
    }
    if (n_nonabs > 0) {
      remainder = content_w - total_fixed - gap * (long)(n_nonabs - 1);
    } else {
      remainder = 0;
    }
    if (n_auto > 0 && remainder > 0) {
      auto_base = remainder / (long)n_auto;
      auto_extra = remainder % (long)n_auto;
    }
    cursor_x = content_x;
    {
      // Wrap placement (flex-wrap: wrap on a row container only):
      // children go left-to-right; a child that would exceed the
      // content width starts a new line whose y advances by the
      // previous line's height (the tallest occupied child height
      // on that line). Total height is unchanged by wrapping: it
      // still sums every child's occupied height (sum_h below).
      int wrap = rule != NULL && rule->has_flex_wrap && rule->flex_wrap;
      long cursor_y = content_y;
      long line_h = 0;
      int first_in_line = 1;
      for (i = 0; i < box->nchildren; i++) {
        pv_box *child = box->children[i];
        const pv_rule *cr = NULL;
        long m[4];
        long cw = 0;
        long ch = 0;
        long occupied = 0;
        if (child == NULL || layout_is_absolute(p, child)) {
          continue;
        }
        cr = layout_find_rule(p, child);
        layout_sides(p, cr, 0, m);
        if (layout_rule_width(p, cr, content_w, &cw)) {
          // Fixed width, honored as written.
        } else {
          cw = auto_base + (auto_idx < (size_t)auto_extra ? 1 : 0);
          auto_idx++;
        }
        if (wrap && !first_in_line &&
            cursor_x + gap + m[3] + cw + m[1] > content_x + content_w) {
          cursor_y += line_h;
          cursor_x = content_x;
          line_h = 0;
          first_in_line = 1;
        }
        if (!first_in_line) {
          cursor_x += gap;
        }
        ch = layout_box(p, child, cursor_x + m[3], cursor_y + m[0], cw,
          content_w, depth + 1);
        occupied = m[0] + ch + m[2];
        if (occupied > line_h) {
          line_h = occupied;
        }
        sum_h += occupied;
        cursor_x += m[3] + cw + m[1];
        first_in_line = 0;
      }
    }
    (void)first;
    for (i = 0; i < box->nchildren; i++) {
      pv_box *child = box->children[i];
      if (child != NULL && layout_is_absolute(p, child)) {
        layout_absolute(p, child, content_w, depth + 1);
      }
    }
    h = pad[0] + sum_h + pad[2] + 2 * box->has_border;
    box->h = h;
    return h;
  }
  // Column (and box/branch/screen): stack children vertically.
  {
    size_t i = 0;
    long gap = 0;
    long cursor_y = content_y;
    int first = 1;
    // flex-wrap on a non-row container is ignored, with a warning
    // naming the selector (a row container took the row branch
    // above, so reaching here means this container is not a row).
    if (rule != NULL && rule->has_flex_wrap) {
      ccb_diag d;
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "flex-wrap on non-row container '");
      ccb_str_append(&msg, rule->selector != NULL ? rule->selector : "?");
      ccb_str_append(&msg, "' is ignored");
      ccb_diag_init(&d);
      ccb_diag_set_full(&d, "CCB-P1014", "preview",
        msg.buf != NULL ? msg.buf : "", NULL, -1, -1, NULL, NULL, NULL);
      ccb_diag_list_push(&p->diags, &d);
      ccb_str_free(&msg);
    }
    if (rule != NULL && rule->has_gap) {
      gap = layout_scale_px(rule->gap, p->interior_cols, p->phone_dp_width);
      if (gap < 0) {
        gap = 0;
      }
    }
    for (i = 0; i < box->nchildren; i++) {
      pv_box *child = box->children[i];
      const pv_rule *cr = NULL;
      long m[4];
      long cw = 0;
      long ch = 0;
      if (child == NULL || layout_is_absolute(p, child)) {
        continue;
      }
      if (!first && gap > 0) {
        cursor_y += gap;
      }
      cr = layout_find_rule(p, child);
      layout_sides(p, cr, 0, m);
      if (!layout_rule_width(p, cr, content_w, &cw)) {
        cw = content_w - m[1] - m[3];
        if (cw < 0) {
          cw = 0;
        }
      }
      ch = layout_box(p, child, content_x + m[3], cursor_y + m[0], cw,
        content_w, depth + 1);
      cursor_y += m[0] + ch + m[2];
      first = 0;
    }
    for (i = 0; i < box->nchildren; i++) {
      pv_box *child = box->children[i];
      if (child != NULL && layout_is_absolute(p, child)) {
        layout_absolute(p, child, content_w, depth + 1);
      }
    }
    h = pad[0] + (cursor_y - content_y) + pad[2] + 2 * box->has_border;
    box->h = h;
    return h;
  }
}

// Length in bytes of the UTF-8 sequence starting at s (bounded),
// mirroring the renderer's sequence lengths so invalid bytes are
// handled the same way there: one width-1 cell per byte sequence.
static size_t css_utf8_seq_len(const unsigned char *s, size_t remaining) {
  unsigned char c = 0;
  if (remaining == 0) {
    return 0;
  }
  c = s[0];
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    return remaining >= 2 ? 2 : 1;
  }
  if ((c & 0xF0) == 0xE0) {
    return remaining >= 3 ? 3 : 1;
  }
  if ((c & 0xF8) == 0xF0) {
    return remaining >= 4 ? 4 : 1;
  }
  return 1;
}

// Decode one sequence to a codepoint; 0 when the bytes are not valid
// UTF-8 (same validation as the renderer).
static int css_utf8_decode(const unsigned char *s, size_t len, long *cp) {
  unsigned char c0 = 0;
  unsigned char c1 = 0;
  if (len == 0 || cp == NULL) {
    return 0;
  }
  c0 = s[0];
  if (len == 1) {
    if (c0 < 0x80) {
      *cp = c0;
      return 1;
    }
    return 0;
  }
  c1 = s[1];
  if (c1 < 0x80 || c1 > 0xBF) {
    return 0;
  }
  if (len == 2) {
    if (c0 < 0xC2 || c0 > 0xDF) {
      return 0;
    }
    *cp = ((long)(c0 & 0x1F) << 6) | (c1 & 0x3F);
    return 1;
  }
  if (len == 3) {
    unsigned char c2 = s[2];
    if (c2 < 0x80 || c2 > 0xBF) {
      return 0;
    }
    if (c0 == 0xE0 && c1 < 0xA0) {
      return 0;
    }
    if (c0 == 0xED && c1 > 0x9F) {
      return 0;
    }
    if (c0 < 0xE0 || c0 > 0xEF) {
      return 0;
    }
    *cp = ((long)(c0 & 0x0F) << 12) | ((long)(c1 & 0x3F) << 6) |
      (c2 & 0x3F);
    return 1;
  }
  if (len == 4) {
    unsigned char c2 = s[2];
    unsigned char c3 = s[3];
    if (c2 < 0x80 || c2 > 0xBF || c3 < 0x80 || c3 > 0xBF) {
      return 0;
    }
    if (c0 == 0xF0 && c1 < 0x90) {
      return 0;
    }
    if (c0 == 0xF4 && c1 > 0x8F) {
      return 0;
    }
    if (c0 < 0xF0 || c0 > 0xF4) {
      return 0;
    }
    *cp = ((long)(c0 & 0x07) << 18) | ((long)(c1 & 0x3F) << 12) |
      ((long)(c2 & 0x3F) << 6) | (c3 & 0x3F);
    return 1;
  }
  return 0;
}

// wcwidth-style cell width, matching the renderer: 2 for wide
// CJK/emoji ranges, 1 otherwise (invalid bytes count as 1).
static int css_char_width(long cp) {
  if ((cp >= 0x1100 && cp <= 0x115F) ||
      (cp >= 0x2329 && cp <= 0x232A) ||
      (cp >= 0x2E80 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) ||
      (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE10 && cp <= 0xFE19) ||
      (cp >= 0xFE30 && cp <= 0xFE6F) ||
      (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||
      (cp >= 0x1F000 && cp <= 0x1FAFF) ||
      (cp >= 0x20000 && cp <= 0x2FFFD) ||
      (cp >= 0x30000 && cp <= 0x3FFFD)) {
    return 2;
  }
  return 1;
}

// Display width of a string in terminal cells.
static long css_display_width(const char *s) {
  long w = 0;
  size_t len = 0;
  size_t i = 0;
  if (s == NULL) {
    return 0;
  }
  len = strlen(s);
  while (i < len) {
    size_t sl = css_utf8_seq_len((const unsigned char *)s + i, len - i);
    long cp = 0;
    if (sl == 0) {
      break;
    }
    if (css_utf8_decode((const unsigned char *)s + i, sl, &cp)) {
      w += css_char_width(cp);
    } else {
      w += 1;
    }
    i += sl;
  }
  return w;
}

// Rendered width of a leaf's painted text in cells, or -1 when it
// paints none. Lists with children paint one "• label" row per child,
// so the widest row decides; bare lists paint "• item" (6 cells).
static long clip_rendered_len(const pv_box *box) {
  if (box == NULL || box_is_container(box) || box_is_branch(box)) {
    return -1;
  }
  if (box_type_eq(box, "text")) {
    return box->label != NULL ? css_display_width(box->label) : -1;
  }
  if (box_type_eq(box, "button")) {
    return box->label != NULL ? css_display_width(box->label) + 4 : 3;
  }
  if (box_type_eq(box, "textfield")) {
    return box->label != NULL ? css_display_width(box->label) + 2 : 2;
  }
  if (box_type_eq(box, "list")) {
    size_t k = 0;
    long widest = 6; // "• item" for a bare list or an unlabeled child
    for (k = 0; k < box->nchildren; k++) {
      pv_box *child = box->children[k];
      long row = 0;
      if (child != NULL && child->label != NULL) {
        row = 2 + css_display_width(child->label);
      } else {
        row = 6;
      }
      if (row > widest) {
        widest = row;
      }
    }
    return widest;
  }
  if (box_type_eq(box, "image")) {
    return 9; // "[ image ]"
  }
  if (box_type_eq(box, "spacer")) {
    return -1;
  }
  // Generic/unknown leaf: "[Type]".
  return box->type != NULL ? css_display_width(box->type) + 2 : -1;
}

typedef struct {
  pv_box **items;
  size_t count;
  size_t cap;
} box_list;

static void box_list_push(box_list *l, pv_box *box) {
  if (l->count == l->cap) {
    size_t ncap = l->cap == 0 ? 16 : l->cap * 2;
    l->items = ccb_xrealloc(l->items, ncap * sizeof(pv_box *));
    l->cap = ncap;
  }
  l->items[l->count] = box;
  l->count++;
}

static void collect_boxes(pv_box *box, box_list *l) {
  size_t i = 0;
  if (box == NULL) {
    return;
  }
  box_list_push(l, box);
  for (i = 0; i < box->nchildren; i++) {
    collect_boxes(box->children[i], l);
  }
}

static int box_is_ancestor(pv_box *anc, pv_box *desc) {
  size_t i = 0;
  if (anc == NULL || desc == NULL) {
    return 0;
  }
  if (anc == desc) {
    return 1;
  }
  for (i = 0; i < anc->nchildren; i++) {
    if (box_is_ancestor(anc->children[i], desc)) {
      return 1;
    }
  }
  return 0;
}

typedef struct {
  long p1003;
  long p1004;
  long p1009;
} layout_caps;

static void layout_diag(pv_project *p, layout_caps *caps, const char *code,
    const char *message) {
  long *counter = NULL;
  ccb_diag d;
  if (strcmp(code, "CCB-P1003") == 0) {
    counter = &caps->p1003;
  } else if (strcmp(code, "CCB-P1004") == 0) {
    counter = &caps->p1004;
  } else {
    counter = &caps->p1009;
  }
  if (*counter >= 10) {
    return;
  }
  (*counter)++;
  ccb_diag_init(&d);
  ccb_diag_set_full(&d, code, "preview", message, NULL, -1, -1,
    NULL, NULL, NULL);
  ccb_diag_list_push(&p->diags, &d);
}

// sRGB -> linear-light table, scaled by 1,000,000: entry c is
// round(linear(c / 255) * 1e6) with the WCAG piecewise curve
// (c/12.92 below 0.04045, else ((c + 0.055) / 1.055) ^ 2.4). A table
// keeps contrast free of libm, matching this codebase's no-libm
// convention; the values were generated from that formula, not
// hand-typed.
static const long kSrgbLinear[] = {
  0, 304, 607, 911, 1214, 1518, 1821, 2125,
  2428, 2732, 3035, 3347, 3677, 4025, 4391, 4777,
  5182, 5605, 6049, 6512, 6995, 7499, 8023, 8568,
  9134, 9721, 10330, 10960, 11612, 12286, 12983, 13702,
  14444, 15209, 15996, 16807, 17642, 18500, 19382, 20289,
  21219, 22174, 23153, 24158, 25187, 26241, 27321, 28426,
  29557, 30713, 31896, 33105, 34340, 35601, 36889, 38204,
  39546, 40915, 42311, 43735, 45186, 46665, 48172, 49707,
  51269, 52861, 54480, 56128, 57805, 59511, 61246, 63010,
  64803, 66626, 68478, 70360, 72272, 74214, 76185, 78187,
  80220, 82283, 84376, 86500, 88656, 90842, 93059, 95307,
  97587, 99899, 102242, 104616, 107023, 109462, 111932, 114435,
  116971, 119538, 122139, 124772, 127438, 130136, 132868, 135633,
  138432, 141263, 144128, 147027, 149960, 152926, 155926, 158961,
  162029, 165132, 168269, 171441, 174647, 177888, 181164, 184475,
  187821, 191202, 194618, 198069, 201556, 205079, 208637, 212231,
  215861, 219526, 223228, 226966, 230740, 234551, 238398, 242281,
  246201, 250158, 254152, 258183, 262251, 266356, 270498, 274677,
  278894, 283149, 287441, 291771, 296138, 300544, 304987, 309469,
  313989, 318547, 323143, 327778, 332452, 337164, 341914, 346704,
  351533, 356400, 361307, 366253, 371238, 376262, 381326, 386429,
  391572, 396755, 401978, 407240, 412543, 417885, 423268, 428690,
  434154, 439657, 445201, 450786, 456411, 462077, 467784, 473531,
  479320, 485150, 491021, 496933, 502886, 508881, 514918, 520996,
  527115, 533276, 539479, 545724, 552011, 558340, 564712, 571125,
  577580, 584078, 590619, 597202, 603827, 610496, 617207, 623960,
  630757, 637597, 644480, 651406, 658375, 665387, 672443, 679542,
  686685, 693872, 701102, 708376, 715694, 723055, 730461, 737910,
  745404, 752942, 760525, 768151, 775822, 783538, 791298, 799103,
  806952, 814847, 822786, 830770, 838799, 846873, 854993, 863157,
  871367, 879622, 887923, 896269, 904661, 913099, 921582, 930111,
  938686, 947307, 955973, 964686, 973445, 982251, 991102, 1000000
};
static long lint_luminance(long r, long g, long b) {
  // 0.2126 / 0.7152 / 0.0722, coefficients scaled by 10000, so the
  // result stays scaled by 1e6 like the table entries.
  return (2126L * kSrgbLinear[r] + 7152L * kSrgbLinear[g] +
    722L * kSrgbLinear[b]) / 10000L;
}

static void lint_diag(pv_project *p, const char *code, const char *message) {
  ccb_diag d;
  ccb_diag_init(&d);
  ccb_diag_set_full(&d, code, "preview", message, NULL, -1, -1,
    NULL, NULL, NULL);
  ccb_diag_list_push(&p->diags, &d);
}

// Lint pass, after layout for one screen (see docs/preview.md):
// CCB-P1017 tap target too small (button/textfield under 8 columns
// or 2 rows, in wireframe cells; the message also reports the size
// in dp, but the thresholds stay in cells). Chunk C precision: a
// dimension counts as too small only when it is known. Width is
// always layout-computed, so w < 8 always counts; height without a
// font-size rule is the 1-row default, not a measurement, so h < 2
// counts only when the effective rule sets font-size. Report what
// is known, stay quiet about what is not. CCB-P1018 text contrast
// below threshold (text/button/textfield; foreground = the leaf
// rule's color, background = the leaf rule's background-color else
// the screen rule's; either side missing or unresolvable to RGB
// skips silently; threshold 4.5:1, or 3.0:1 at font-size >= 24px).
static void layout_lint(pv_project *p, pv_box *root, box_list *all) {
  const char *screen_bg = NULL;
  size_t i = 0;
  if (root != NULL && root->rule != NULL) {
    screen_bg = root->rule->bg;
  }
  for (i = 0; i < all->count; i++) {
    pv_box *b = all->items[i];
    const pv_rule *rule = NULL;
    ccb_str msg;
    if (b == NULL || b->type == NULL) {
      continue;
    }
    rule = b->rule;
    if (box_type_eq(b, "button") || box_type_eq(b, "textfield")) {
      // Chunk C: height without font-size is the 1-row default,
      // not a known measurement, so it never triggers on its own.
      if (b->w < 8 ||
          (b->h < 2 && rule != NULL && rule->has_font_size)) {
        ccb_str_init(&msg);
        ccb_str_append(&msg, "tap target too small: ");
        ccb_str_append(&msg, b->type);
        if (b->label != NULL) {
          ccb_str_append(&msg, " \"");
          ccb_str_append(&msg, b->label);
          ccb_str_append(&msg, "\"");
        }
        // F2: thresholds stay in cells (see docs/preview.md for why
        // a 48dp rule would misfire); the message reports the DP
        // size alongside so the platform unit is visible.
        ccb_str_appendf(&msg, " is %ldx%ld cells (%lddp x %lddp; "
          "minimum 8x2 cells)", b->w, b->h,
          pv_cols_to_dp(b->w, p->interior_cols, p->phone_dp_width),
          pv_rows_to_dp(b->h));
        lint_diag(p, "CCB-P1017", msg.buf != NULL ? msg.buf : "");
        ccb_str_free(&msg);
      }
    }
    if (box_type_eq(b, "text") || box_type_eq(b, "button") ||
        box_type_eq(b, "textfield")) {
      const char *fg = rule != NULL ? rule->fg : NULL;
      const char *bg = NULL;
      long fr = 0;
      long fg_g = 0;
      long fb = 0;
      long br = 0;
      long bg_g = 0;
      long bb = 0;
      long l1 = 0;
      long l2 = 0;
      double ratio = 0.0;
      double threshold = 4.5;
      if (rule != NULL && rule->bg != NULL) {
        bg = rule->bg;
      } else {
        bg = screen_bg;
      }
      if (fg == NULL || bg == NULL) {
        continue;
      }
      if (!pv_color_rgb(fg, &fr, &fg_g, &fb) ||
          !pv_color_rgb(bg, &br, &bg_g, &bb)) {
        continue; // unresolvable side: contrast cannot be judged
      }
      l1 = lint_luminance(fr, fg_g, fb);
      l2 = lint_luminance(br, bg_g, bb);
      if (l1 < l2) {
        long tmp = l1;
        l1 = l2;
        l2 = tmp;
      }
      // Luminances are scaled by 1e6, so 0.05 becomes 50000.
      ratio = (double)(l1 + 50000L) / (double)(l2 + 50000L);
      if (rule != NULL && rule->has_font_size && rule->font_size >= 24) {
        threshold = 3.0;
      }
      if (ratio < threshold) {
        ccb_str_init(&msg);
        ccb_str_appendf(&msg, "text contrast %.1f:1 below %.1f:1 for ",
          ratio, threshold);
        ccb_str_append(&msg, b->type);
        if (b->label != NULL) {
          ccb_str_append(&msg, " \"");
          ccb_str_append(&msg, b->label);
          ccb_str_append(&msg, "\"");
        }
        lint_diag(p, "CCB-P1018", msg.buf != NULL ? msg.buf : "");
        ccb_str_free(&msg);
      }
    }
  }
}

void pv_layout_screen(pv_project *p, pv_screen *screen) {
  pv_box *root = NULL;
  long interior = 0;
  long root_h = 0;
  box_list all;
  layout_caps caps;
  size_t i = 0;
  size_t j = 0;
  if (p == NULL || screen == NULL || screen->root == NULL) {
    return;
  }
  interior = p->interior_cols;
  if (interior < 0) {
    interior = 0;
  }
  root = screen->root;
  p->layout_screen_name = screen->name;
  layout_box(p, root, 0, 0, interior, interior, 0);
  p->layout_screen_name = NULL;
  root_h = root->h;
  memset(&all, 0, sizeof(all));
  memset(&caps, 0, sizeof(caps));
  collect_boxes(root, &all);
  // CCB-P1003: overlap between non-ancestor-related boxes.
  for (i = 0; i < all.count; i++) {
    for (j = i + 1; j < all.count; j++) {
      pv_box *a = all.items[i];
      pv_box *b = all.items[j];
      ccb_str msg;
      if (a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0) {
        continue;
      }
      if (box_is_ancestor(a, b) || box_is_ancestor(b, a)) {
        continue;
      }
      if (a->x < b->x + b->w && b->x < a->x + a->w &&
          a->y < b->y + b->h && b->y < a->y + a->h) {
        ccb_str_init(&msg);
        ccb_str_append(&msg, "boxes overlap: '");
        ccb_str_append(&msg, a->type != NULL ? a->type : "?");
        ccb_str_append(&msg, "' and '");
        ccb_str_append(&msg, b->type != NULL ? b->type : "?");
        ccb_str_append(&msg, "'");
        layout_diag(p, &caps, "CCB-P1003", msg.buf != NULL ? msg.buf : "");
        ccb_str_free(&msg);
      }
    }
  }
  // CCB-P1004: box extending past the interior bounds. The vertical
  // bound is the root's computed height (absolute boxes past it are
  // clipped by the renderer, hence off-screen).
  for (i = 0; i < all.count; i++) {
    pv_box *b = all.items[i];
    ccb_str msg;
    if (b->x < 0 || b->y < 0 || b->x + b->w > interior ||
        b->y + b->h > root_h) {
      ccb_str_init(&msg);
      ccb_str_append(&msg, "box '");
      ccb_str_append(&msg, b->type != NULL ? b->type : "?");
      ccb_str_append(&msg, "' extends past the interior bounds");
      layout_diag(p, &caps, "CCB-P1004", msg.buf != NULL ? msg.buf : "");
      ccb_str_free(&msg);
    }
  }
  // CCB-P1009: rendered label wider than the box's content width.
  for (i = 0; i < all.count; i++) {
    pv_box *b = all.items[i];
    long rendered = clip_rendered_len(b);
    long pad[4];
    long content_w = 0;
    ccb_str msg;
    if (rendered < 0) {
      continue;
    }
    layout_sides(p, b->rule, 1, pad);
    content_w = b->w - pad[1] - pad[3] - 2 * b->has_border;
    if (content_w < 0) {
      content_w = 0;
    }
    if (rendered > content_w) {
      ccb_str_init(&msg);
      ccb_str_append(&msg, "text clipped to its box: '");
      ccb_str_append(&msg, b->label != NULL ? b->label : (b->type != NULL ? b->type : "?"));
      ccb_str_append(&msg, "'");
      layout_diag(p, &caps, "CCB-P1009", msg.buf != NULL ? msg.buf : "");
      ccb_str_free(&msg);
    }
  }
  // Lint diagnostics (P1017/P1018), after layout, over all boxes.
  layout_lint(p, root, &all);
  free(all.items);
}
