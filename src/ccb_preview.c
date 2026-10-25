// ccb_preview: project orchestration for terminal preview.
// Loads a [preview] project (flow first, then CSS with the scope rule),
// lays out every screen, runs the diagnostics pass, and emits JSON.
// Implements pv_project_load / pv_project_free / pv_project_diagnose /
// pv_project_json from ccb_preview.h. Never touches the SDK/toolchain.
#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_preview_scenario.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include "ccb_json.h"

#define PV_DEFAULT_FRAME_WIDTH 40
#define PV_MIN_FRAME_WIDTH 24
#define PV_DEFAULT_PHONE_DP_WIDTH 390

static void pv_diag(ccb_diag_list *diags, const char *code,
    const char *message, const char *file, long line) {
  ccb_diag d;
  ccb_diag_init(&d);
  ccb_diag_set_full(&d, code, "preview", message, file, line, -1,
    NULL, NULL, NULL);
  ccb_diag_list_push(diags, &d);
}

static int pv_screen_named(pv_project *p, const char *name) {
  size_t i = 0;
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

// Basename of a path (the part after the last '/'), or the whole path.
static const char *pv_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

// True when the CSS file's basename is exactly "common.css".
static int pv_is_common_css(const char *path) {
  return strcmp(pv_basename(path), "common.css") == 0;
}

// Screen name for a per-screen CSS file: the basename minus ".css",
// but only when a screen of that name exists; NULL otherwise (the
// caller treats NULL + non-common as an ignored file).
static const char *pv_css_screen(pv_project *p, const char *path) {
  const char *base = pv_basename(path);
  size_t n = strlen(base);
  size_t i = 0;
  if (n <= 4 || strcmp(base + n - 4, ".css") != 0) {
    return NULL;
  }
  for (i = 0; i < p->nscreens; i++) {
    const char *name = p->screens[i]->name;
    if (name != NULL && strlen(name) == n - 4 &&
        memcmp(name, base, n - 4) == 0) {
      return name;
    }
  }
  return NULL;
}

// ---------- @string/@color resources ----------

// One resource table (strings or colors), parsed from a values XML
// file. Entries are arena-owned; only the items array is malloc'd
// and is freed after resolution.
typedef struct {
  char *name;
  char *value;
} pv_res_entry;

typedef struct {
  pv_res_entry *items;
  size_t count, cap;
} pv_res_table;

static const char *pv_res_find(pv_res_table *t, const char *name) {
  size_t i = 0;
  if (t == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0; i < t->count; i++) {
    if (strcmp(t->items[i].name, name) == 0) {
      return t->items[i].value;
    }
  }
  return NULL;
}

static void pv_res_put(pv_res_table *t, char *name,
    char *value) {
  size_t i = 0;
  for (i = 0; i < t->count; i++) {
    if (strcmp(t->items[i].name, name) == 0) {
      t->items[i].value = value;
      return;
    }
  }
  if (t->count == t->cap) {
    size_t ncap = t->cap == 0 ? 8 : t->cap * 2;
    t->items = ccb_xrealloc(t->items, ncap * sizeof(*t->items));
    t->cap = ncap;
  }
  t->items[t->count].name = name;
  t->items[t->count].value = value;
  t->count++;
}

static int pv_res_is_hex(const char *s) {
  size_t n = 0;
  size_t i = 0;
  if (s == NULL || s[0] != '#') {
    return 0;
  }
  n = strlen(s);
  if (n != 4 && n != 7) {
    return 0;
  }
  for (i = 1; i < n; i++) {
    char c = s[i];
    int hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
      (c >= 'A' && c <= 'F');
    if (!hex) {
      return 0;
    }
  }
  return 1;
}

// Decode the five XML entities in a string value; anything else
// (including an unknown entity) is copied literally.
static char *pv_res_decode(ccb_arena *arena, const char *s, size_t n) {
  ccb_str out;
  size_t i = 0;
  char *result = NULL;
  ccb_str_init(&out);
  while (i < n) {
    if (s[i] == '&') {
      if (i + 5 <= n && memcmp(s + i, "&amp;", 5) == 0) {
        ccb_str_append_c(&out, '&');
        i += 5;
        continue;
      }
      if (i + 4 <= n && memcmp(s + i, "&lt;", 4) == 0) {
        ccb_str_append_c(&out, '<');
        i += 4;
        continue;
      }
      if (i + 4 <= n && memcmp(s + i, "&gt;", 4) == 0) {
        ccb_str_append_c(&out, '>');
        i += 4;
        continue;
      }
      if (i + 6 <= n && memcmp(s + i, "&quot;", 6) == 0) {
        ccb_str_append_c(&out, '"');
        i += 6;
        continue;
      }
      if (i + 6 <= n && memcmp(s + i, "&apos;", 6) == 0) {
        ccb_str_append_c(&out, '\'');
        i += 6;
        continue;
      }
    }
    ccb_str_append_c(&out, s[i]);
    i++;
  }
  result = ccb_arena_strdup(arena, out.buf != NULL ? out.buf : "");
  ccb_str_free(&out);
  return result;
}

static void pv_res_diag(ccb_diag_list *diags, const char *message,
    const char *file) {
  ccb_diag d;
  ccb_diag_init(&d);
  ccb_diag_set_full(&d, "CCB-P1016", "preview", message, file, -1, -1,
    NULL, NULL, NULL);
  ccb_diag_list_push(diags, &d);
}

// Tiny subset parser for Android values XML: <string name="n">v</string>
// and <color name="n">#rrggbb</color>. XML comments are skipped. A
// malformed entry is skipped with a CCB-P1016 warning naming the
// file; a non-hex color value is skipped with CCB-P1016 the same
// way. A missing file is simply an empty table (no diagnostic: a
// project may define only strings, only colors, or neither).
static void pv_res_parse_file(ccb_arena *arena, const char *path,
    int is_color, ccb_diag_list *diags, pv_res_table *table) {
  char *text = NULL;
  size_t len = 0;
  size_t i = 0;
  const char *kind = is_color ? "color" : "string";
  if (ccb_read_file(path, &text, &len) != CCB_OK) {
    return;
  }
  while (i < len) {
    size_t tag = 0;
    size_t name_end = 0;
    size_t tag_end = 0;
    size_t k = 0;
    int self_closing = 0;
    char *name = NULL;
    const char *close_tag = NULL;
    char *close_buf = NULL;
    size_t close_len = 0;
    const char *content = NULL;
    size_t content_len = 0;
    if (i + 4 <= len && memcmp(text + i, "<!--", 4) == 0) {
      const char *end = NULL;
      size_t j = 0;
      for (j = i + 4; j + 3 <= len; j++) {
        if (memcmp(text + j, "-->", 3) == 0) {
          end = text + j;
          break;
        }
      }
      if (end == NULL) {
        break;
      }
      i = (size_t)(end - text) + 3;
      continue;
    }
    if (text[i] != '<') {
      i++;
      continue;
    }
    if (i + 1 >= len || text[i + 1] == '/' || text[i + 1] == '?' ||
        text[i + 1] == '!') {
      // Closing tag, processing instruction, or declaration: skip
      // to the next '>'.
      while (i < len && text[i] != '>') {
        i++;
      }
      if (i < len) {
        i++;
      }
      continue;
    }
    // Tag name: letters only.
    tag = i + 1;
    name_end = tag;
    while (name_end < len && ((text[name_end] >= 'a' &&
        text[name_end] <= 'z') || (text[name_end] >= 'A' &&
        text[name_end] <= 'Z'))) {
      name_end++;
    }
    if (name_end == tag || strlen(kind) != name_end - tag ||
        memcmp(text + tag, kind, name_end - tag) != 0) {
      // Not an entry of this file's kind: skip to the next '>'.
      while (i < len && text[i] != '>') {
        i++;
      }
      if (i < len) {
        i++;
      }
      continue;
    }
    // End of the opening tag, respecting quoted attribute values.
    tag_end = name_end;
    {
      char quote = '\0';
      int found = 0;
      for (k = name_end; k < len; k++) {
        if (quote != '\0') {
          if (text[k] == quote) {
            quote = '\0';
          }
          continue;
        }
        if (text[k] == '"' || text[k] == '\'') {
          quote = text[k];
        } else if (text[k] == '>') {
          tag_end = k;
          found = 1;
          break;
        }
      }
      if (!found) {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg, "malformed resource entry in '");
        ccb_str_append(&msg, path);
        ccb_str_append(&msg, "'");
        pv_res_diag(diags, msg.buf != NULL ? msg.buf : "", path);
        ccb_str_free(&msg);
        break;
      }
    }
    self_closing = tag_end > name_end && text[tag_end - 1] == '/';
    // The name="..." attribute inside the opening tag.
    for (k = name_end; k + 4 < tag_end; k++) {
      if (memcmp(text + k, "name", 4) == 0 &&
          (k == name_end || text[k - 1] == ' ' || text[k - 1] == '\t' ||
           text[k - 1] == '\n' || text[k - 1] == '\r')) {
        size_t v = k + 4;
        char quote = '\0';
        size_t vs = 0;
        size_t ve = 0;
        while (v < tag_end && (text[v] == ' ' || text[v] == '\t' ||
            text[v] == '\n' || text[v] == '\r')) {
          v++;
        }
        if (v < tag_end && text[v] == '=') {
          v++;
          while (v < tag_end && (text[v] == ' ' || text[v] == '\t' ||
              text[v] == '\n' || text[v] == '\r')) {
            v++;
          }
          if (v < tag_end && (text[v] == '"' || text[v] == '\'')) {
            quote = text[v];
            vs = v + 1;
            ve = vs;
            while (ve < tag_end && text[ve] != quote) {
              ve++;
            }
            if (ve < tag_end) {
              name = ccb_arena_strndup(arena, text + vs, ve - vs);
            }
          }
        }
        break;
      }
    }
    if (name == NULL || name[0] == '\0') {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "malformed resource entry in '");
      ccb_str_append(&msg, path);
      ccb_str_append(&msg, "'");
      pv_res_diag(diags, msg.buf != NULL ? msg.buf : "", path);
      ccb_str_free(&msg);
      i = tag_end + 1;
      continue;
    }
    if (self_closing) {
      if (!is_color) {
        pv_res_put(table, name, ccb_arena_strdup(arena, ""));
      } else {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg, "non-hex color value for '");
        ccb_str_append(&msg, name);
        ccb_str_append(&msg, "' in '");
        ccb_str_append(&msg, path);
        ccb_str_append(&msg, "', skipped");
        pv_res_diag(diags, msg.buf != NULL ? msg.buf : "", path);
        ccb_str_free(&msg);
      }
      i = tag_end + 1;
      continue;
    }
    // Closing tag: </string> or </color>.
    close_buf = ccb_arena_alloc(arena, strlen(kind) + 4);
    close_buf[0] = '<';
    close_buf[1] = '/';
    memcpy(close_buf + 2, kind, strlen(kind));
    close_buf[2 + strlen(kind)] = '>';
    close_buf[3 + strlen(kind)] = '\0';
    close_len = strlen(close_buf);
    close_tag = NULL;
    for (k = tag_end + 1; k + close_len <= len; k++) {
      if (memcmp(text + k, close_buf, close_len) == 0) {
        close_tag = text + k;
        break;
      }
    }
    if (close_tag == NULL) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "malformed resource entry in '");
      ccb_str_append(&msg, path);
      ccb_str_append(&msg, "'");
      pv_res_diag(diags, msg.buf != NULL ? msg.buf : "", path);
      ccb_str_free(&msg);
      break;
    }
    content = text + tag_end + 1;
    content_len = (size_t)(close_tag - content);
    if (is_color) {
      // Trim surrounding whitespace, then require a hex value.
      while (content_len > 0 && (content[0] == ' ' ||
          content[0] == '\t' || content[0] == '\n' ||
          content[0] == '\r')) {
        content++;
        content_len--;
      }
      while (content_len > 0 && (content[content_len - 1] == ' ' ||
          content[content_len - 1] == '\t' ||
          content[content_len - 1] == '\n' ||
          content[content_len - 1] == '\r')) {
        content_len--;
      }
      if (!pv_res_is_hex(ccb_arena_strndup(arena, content, content_len))) {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg, "non-hex color value for '");
        ccb_str_append(&msg, name);
        ccb_str_append(&msg, "' in '");
        ccb_str_append(&msg, path);
        ccb_str_append(&msg, "', skipped");
        pv_res_diag(diags, msg.buf != NULL ? msg.buf : "", path);
        ccb_str_free(&msg);
      } else {
        pv_res_put(table, name,
          ccb_arena_strndup(arena, content, content_len));
      }
    } else {
      pv_res_put(table, name,
        pv_res_decode(arena, content, content_len));
    }
    i = (size_t)(close_tag - text) + close_len;
  }
  free(text);
}

// Replace component labels that are exactly "@string/name" with the
// resolved string (branch labels are conditions, not component
// labels, and are left alone). Unresolved references warn (P1016)
// and keep the literal text. Runs before layout, so clip widths
// are computed on the resolved text.
static void pv_res_resolve_labels(ccb_arena *arena, pv_box *box,
    pv_res_table *strings, ccb_diag_list *diags) {
  size_t i = 0;
  if (box == NULL) {
    return;
  }
  if (box->label != NULL && strncmp(box->label, "@string/", 8) == 0 &&
      !box->is_branch && box->type != NULL &&
      strcmp(box->type, "branch-entry") != 0) {
    const char *name = box->label + 8;
    const char *value = pv_res_find(strings, name);
    if (value != NULL) {
      box->label = ccb_arena_strdup(arena, value);
    } else {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "unresolved @string reference '");
      ccb_str_append(&msg, box->label);
      ccb_str_append(&msg, "'");
      pv_res_diag(diags, msg.buf != NULL ? msg.buf : "", NULL);
      ccb_str_free(&msg);
    }
  }
  for (i = 0; i < box->nchildren; i++) {
    pv_res_resolve_labels(arena, box->children[i], strings, diags);
  }
}

// Replace CSS color/background-color values that are exactly
// "@color/name" with the resolved hex. Unresolved references warn
// (P1016) and keep the literal text (which the renderer and the
// contrast check then treat as unresolvable, never as a guess).
static void pv_res_resolve_colors(ccb_arena *arena, pv_project *p,
    pv_res_table *colors, ccb_diag_list *diags) {
  size_t i = 0;
  for (i = 0; i < p->nrules; i++) {
    pv_rule *rule = &p->rules[i];
    char **slots[2];
    size_t s = 0;
    slots[0] = &rule->bg;
    slots[1] = &rule->fg;
    for (s = 0; s < 2; s++) {
      char *v = *slots[s];
      const char *value = NULL;
      if (v == NULL || strncmp(v, "@color/", 7) != 0) {
        continue;
      }
      value = pv_res_find(colors, v + 7);
      if (value != NULL) {
        *slots[s] = ccb_arena_strdup(arena, value);
      } else {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg, "unresolved @color reference '");
        ccb_str_append(&msg, v);
        ccb_str_append(&msg, "'");
        pv_res_diag(diags, msg.buf != NULL ? msg.buf : "", NULL);
        ccb_str_free(&msg);
      }
    }
  }
}

// ---------- Slices 2/3: layout XML + nav-graph inputs ----------
//
// Both are tiny subset parsers (no XML library), in the same spirit
// as the values parser above: they recognize exactly one shape each
// and skip everything else, never guessing. Layout: an element
// whose tag's local name ends in "Button" with an android:onClick
// attribute. Nav graph: any element carrying both android:id and
// android:name (a destination); android:name's simple name (after
// the last '.') is the screen name, exactly as written.

// Append a key/value pair to arena arrays; with first_wins, an
// existing key keeps its first value (deterministic: files are
// collected in sorted order).
static void pv_pair_add(ccb_arena *arena, char ***keys_io, char ***vals_io,
    size_t *n_io, char *key, char *val, int first_wins) {
  size_t n = *n_io;
  size_t k = 0;
  char **next_keys = NULL;
  char **next_vals = NULL;
  if (first_wins) {
    for (k = 0; k < n; k++) {
      if (strcmp((*keys_io)[k], key) == 0) {
        return;
      }
    }
  }
  next_keys = ccb_arena_alloc(arena, (n + 1) * sizeof(*next_keys));
  next_vals = ccb_arena_alloc(arena, (n + 1) * sizeof(*next_vals));
  for (k = 0; k < n; k++) {
    next_keys[k] = (*keys_io)[k];
    next_vals[k] = (*vals_io)[k];
  }
  next_keys[n] = key;
  next_vals[n] = val;
  *keys_io = next_keys;
  *vals_io = next_vals;
  *n_io = n + 1;
}

// Attribute value of `attr` inside the tag text [start, end)
// (between '<' and '>'): walks real attributes, so a name occurring
// inside another attribute's quoted value never matches. Returns a
// malloc'd value, or NULL when the attribute is absent/malformed.
static char *pv_xml_attr(const char *text, size_t start, size_t end,
    const char *attr) {
  size_t i = start;
  size_t alen = strlen(attr);
  while (i < end) {
    size_t ns = 0;
    size_t ne = 0;
    while (i < end && (text[i] == ' ' || text[i] == '\t' ||
        text[i] == '\n' || text[i] == '\r')) {
      i++;
    }
    if (i >= end || text[i] == '/') {
      return NULL;
    }
    ns = i;
    while (i < end && text[i] != ' ' && text[i] != '\t' &&
        text[i] != '\n' && text[i] != '\r' && text[i] != '=' &&
        text[i] != '/' && text[i] != '>') {
      i++;
    }
    ne = i;
    while (i < end && (text[i] == ' ' || text[i] == '\t' ||
        text[i] == '\n' || text[i] == '\r')) {
      i++;
    }
    if (i < end && text[i] == '=') {
      char quote = '\0';
      size_t vs = 0;
      size_t ve = 0;
      i++;
      while (i < end && (text[i] == ' ' || text[i] == '\t' ||
          text[i] == '\n' || text[i] == '\r')) {
        i++;
      }
      if (i < end && (text[i] == '"' || text[i] == '\'')) {
        quote = text[i];
        vs = i + 1;
        ve = vs;
        while (ve < end && text[ve] != quote) {
          ve++;
        }
        if (ve < end) {
          if (ne - ns == alen && memcmp(text + ns, attr, alen) == 0) {
            char *out = ccb_xmalloc(ve - vs + 1);
            memcpy(out, text + vs, ve - vs);
            out[ve - vs] = '\0';
            return out;
          }
          i = ve + 1;
          continue;
        }
      }
      return NULL;
    }
    // A valueless attribute: keep walking.
  }
  return NULL;
}

typedef void (*pv_tag_fn)(void *ctx, const char *text, size_t tag_start,
  size_t tag_end, const char *local, size_t local_len);

// Call fn for every opening tag in the text: tag_start/tag_end bound
// the tag content (between '<' and '>'); local is the tag name's
// local part (after the last '.'), not NUL-terminated. Comments,
// closing tags, processing instructions, and declarations are
// skipped; '>' inside a quoted attribute value does not end a tag.
static void pv_xml_scan_tags(const char *text, size_t len, pv_tag_fn fn,
    void *ctx) {
  size_t i = 0;
  while (i < len) {
    size_t gt = 0;
    size_t ns = 0;
    size_t ne = 0;
    const char *local = NULL;
    size_t local_len = 0;
    char quote = '\0';
    int found = 0;
    size_t k = 0;
    if (i + 4 <= len && memcmp(text + i, "<!--", 4) == 0) {
      size_t j = 0;
      for (j = i + 4; j + 3 <= len; j++) {
        if (memcmp(text + j, "-->", 3) == 0) {
          i = j + 3;
          break;
        }
      }
      if (j + 3 > len) {
        break;
      }
      continue;
    }
    if (text[i] != '<') {
      i++;
      continue;
    }
    if (i + 1 >= len || text[i + 1] == '/' || text[i + 1] == '?' ||
        text[i + 1] == '!') {
      while (i < len && text[i] != '>') {
        i++;
      }
      if (i < len) {
        i++;
      }
      continue;
    }
    for (k = i + 1; k < len; k++) {
      if (quote != '\0') {
        if (text[k] == quote) {
          quote = '\0';
        }
        continue;
      }
      if (text[k] == '"' || text[k] == '\'') {
        quote = text[k];
      } else if (text[k] == '>') {
        gt = k;
        found = 1;
        break;
      }
    }
    if (!found) {
      break;
    }
    ns = i + 1;
    ne = ns;
    while (ne < gt && text[ne] != ' ' && text[ne] != '\t' &&
        text[ne] != '\n' && text[ne] != '\r' && text[ne] != '/' &&
        text[ne] != '>') {
      ne++;
    }
    local = text + ns;
    local_len = ne - ns;
    {
      size_t d = 0;
      for (d = 0; d < local_len; d++) {
        if (local[d] == '.') {
          local = text + ns + d + 1;
          local_len = ne - (ns + d + 1);
        }
      }
    }
    fn(ctx, text, ns, gt, local, local_len);
    i = gt + 1;
  }
}

// Strip an Android resource reference to its entry name:
// "@+id/detail" and "@id/detail" both become "detail"; anything
// without a '/' is kept as-is.
static const char *pv_res_name(const char *v) {
  const char *slash = strrchr(v, '/');
  return slash != NULL ? slash + 1 : v;
}

// Simple name of a qualified class name: after the last '.'.
static const char *pv_simple_name(const char *v) {
  const char *dot = strrchr(v, '.');
  return dot != NULL ? dot + 1 : v;
}

typedef struct {
  char ***methods_io;
  char ***ids_io;
  size_t *n_io;
} pv_layout_ctx;

static void pv_layout_tag(void *ctx_v, const char *text, size_t tag_start,
    size_t tag_end, const char *local, size_t local_len) {
  pv_layout_ctx *ctx = ctx_v;
  char *onclick = NULL;
  char *id = NULL;
  size_t n = 0;
  char **next_m = NULL;
  char **next_i = NULL;
  if (local_len < 6 || memcmp(local + local_len - 6, "Button", 6) != 0) {
    return;
  }
  onclick = pv_xml_attr(text, tag_start, tag_end, "android:onClick");
  if (onclick == NULL) {
    return;
  }
  id = pv_xml_attr(text, tag_start, tag_end, "android:id");
  n = *ctx->n_io;
  // xrealloc preserves the existing entries; only the new slot is
  // filled here.
  next_m = ccb_xrealloc(*ctx->methods_io, (n + 1) * sizeof(*next_m));
  next_i = ccb_xrealloc(*ctx->ids_io, (n + 1) * sizeof(*next_i));
  next_m[n] = onclick;
  next_i[n] = id != NULL ? ccb_xstrdup(pv_res_name(id)) : NULL;
  free(id);
  *ctx->methods_io = next_m;
  *ctx->ids_io = next_i;
  *ctx->n_io = n + 1;
}

typedef struct {
  ccb_arena *arena;
  char ***keys_io;
  char ***vals_io;
  size_t *n_io;
} pv_graph_ctx;

static void pv_graph_tag(void *ctx_v, const char *text, size_t tag_start,
    size_t tag_end, const char *local, size_t local_len) {
  pv_graph_ctx *ctx = ctx_v;
  char *id = NULL;
  char *name = NULL;
  (void)local;
  (void)local_len;
  id = pv_xml_attr(text, tag_start, tag_end, "android:id");
  name = pv_xml_attr(text, tag_start, tag_end, "android:name");
  if (id != NULL && name != NULL) {
    pv_pair_add(ctx->arena, ctx->keys_io, ctx->vals_io, ctx->n_io,
      ccb_arena_strdup(ctx->arena, pv_res_name(id)),
      ccb_arena_strdup(ctx->arena, pv_simple_name(name)), 1);
  }
  free(id);
  free(name);
}

// XML input files for a configured path: the file itself when the
// path is a file, every *.xml under it when it is a directory, and
// nothing when it does not exist (a fallback dir that is absent is
// simply an empty input set). The list is malloc'd; free with
// ccb_free_str_array.
static void pv_xml_inputs(const char *path, char ***files_io,
    size_t *nfiles_io) {
  *files_io = NULL;
  *nfiles_io = 0;
  if (path == NULL || !ccb_path_exists(path)) {
    return;
  }
  if (ccb_path_is_dir(path)) {
    (void)ccb_collect_files(path, ".xml", files_io, nfiles_io);
    return;
  }
  *files_io = ccb_xmalloc(sizeof(**files_io));
  (*files_io)[0] = ccb_xstrdup(path);
  *nfiles_io = 1;
}

// Location of the layout inputs: [preview] layout, else
// <resources>/layout, else <paths res>/layout, else none.
static char *pv_layout_path(const ccb_config *cfg) {
  if (cfg->preview_layout != NULL) {
    return ccb_xstrdup(cfg->preview_layout);
  }
  if (cfg->preview_resources != NULL) {
    return ccb_path_join(cfg->preview_resources, "layout");
  }
  if (cfg->res != NULL) {
    return ccb_path_join(cfg->res, "layout");
  }
  return NULL;
}

// Location of the nav-graph inputs: [preview] nav_graph, else
// <resources>/navigation, else <paths res>/navigation, else none.
static char *pv_nav_path(const ccb_config *cfg) {
  if (cfg->preview_nav_graph != NULL) {
    return ccb_xstrdup(cfg->preview_nav_graph);
  }
  if (cfg->preview_resources != NULL) {
    return ccb_path_join(cfg->preview_resources, "navigation");
  }
  if (cfg->res != NULL) {
    return ccb_path_join(cfg->res, "navigation");
  }
  return NULL;
}

// Append an edge to a screen, with the same dedup key as the flow
// parser's pv_edge_add (from, target, unknown, args, branch).
static void pv_screen_edge_add(ccb_arena *arena, pv_screen *screen,
    char *from, char *target, int unknown, char **args, size_t nargs,
    int *args_named, char *branch) {
  size_t i = 0;
  size_t k = 0;
  for (i = 0; i < screen->nedges; i++) {
    pv_edge *e = &screen->edges[i];
    int same_target = (e->target == NULL && target == NULL) ||
      (e->target != NULL && target != NULL && strcmp(e->target, target) == 0);
    int same_branch = (e->branch == NULL && branch == NULL) ||
      (e->branch != NULL && branch != NULL && strcmp(e->branch, branch) == 0);
    int same_args = e->nargs == nargs;
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
    pv_edge *next = ccb_arena_alloc(arena, ncap * sizeof(*next));
    for (i = 0; i < screen->nedges; i++) {
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
  screen->edges[screen->nedges].derived = 1;
  screen->edges[screen->nedges].branch = branch;
  // Chunk 1: see pv_edge_add in ccb_preview_flow.c.
  screen->edges[screen->nedges].args_scenario = NULL;
  // Chunk 2: see pv_edge_add in ccb_preview_flow.c.
  screen->edges[screen->nedges].args_trace = NULL;
  screen->edges[screen->nedges].trace_taken = 0;
  screen->nedges++;
}

// True when the component tree contains a component box (not the
// screen root, not a branch node) with the given label.
static int pv_tree_has_label(pv_box *box, const char *label) {
  size_t i = 0;
  if (box == NULL || label == NULL) {
    return 0;
  }
  if (box->label != NULL && strcmp(box->label, label) == 0 &&
      strcmp(box->type, "screen") != 0 &&
      strcmp(box->type, "branch") != 0 &&
      strcmp(box->type, "branch-entry") != 0) {
    return 1;
  }
  for (i = 0; i < box->nchildren; i++) {
    if (pv_tree_has_label(box->children[i], label)) {
      return 1;
    }
  }
  return 0;
}

static const char *pv_binding_lookup(const ccb_config *cfg,
    const char *key) {
  size_t i = 0;
  if (cfg == NULL || key == NULL) {
    return NULL;
  }
  for (i = 0; i < cfg->preview_binding_count; i++) {
    if (strcmp(cfg->preview_binding_keys[i], key) == 0) {
      return cfg->preview_binding_values[i];
    }
  }
  return NULL;
}

// Slice 1 reconciliation (the declared-beats-derived post-pass).
// Runs once after every flow file is parsed, so it applies to every
// consumer (render, walk, storyboard, diff, gate): for each derived
// edge, a declared edge from the same button (same `from`) stands.
// Same destination (or both unknown) -> the derived edge is dropped
// silently; different known destinations -> a CCB-P1020 warning
// naming the button and both destinations, and the derived edge is
// dropped; a declared edge of any kind with a derived unknown edge
// -> the derived edge is dropped silently. Derived edges with no
// declared edge for their button are kept unchanged.
static void pv_reconcile_derived(pv_project *p, ccb_diag_list *diags) {
  size_t si = 0;
  for (si = 0; si < p->nscreens; si++) {
    pv_screen *screen = p->screens[si];
    int *keep = NULL;
    size_t w = 0;
    size_t r = 0;
    if (screen->nedges == 0) {
      continue;
    }
    keep = ccb_xmalloc(screen->nedges * sizeof(*keep));
    for (r = 0; r < screen->nedges; r++) {
      pv_edge *e = &screen->edges[r];
      const pv_edge *decl = NULL;
      size_t d = 0;
      keep[r] = 1;
      if (!e->derived) {
        continue;
      }
      for (d = 0; d < screen->nedges; d++) {
        if (d != r && !screen->edges[d].derived &&
            strcmp(screen->edges[d].from, e->from) == 0) {
          decl = &screen->edges[d];
          break;
        }
      }
      if (decl == NULL) {
        continue;
      }
      if (decl->target != NULL && e->target != NULL) {
        if (strcmp(decl->target, e->target) == 0) {
          keep[r] = 0;
        } else {
          ccb_str msg;
          ccb_str_init(&msg);
          ccb_str_append(&msg,
            "declared navigation for button '");
          ccb_str_append(&msg, e->from);
          ccb_str_append(&msg, "' to '");
          ccb_str_append(&msg, decl->target);
          ccb_str_append(&msg,
            "' disagrees with derived navigation to '");
          ccb_str_append(&msg, e->target);
          ccb_str_append(&msg, "'; declared edge kept");
          pv_diag(diags, "CCB-P1020", msg.buf != NULL ? msg.buf : "",
            NULL, -1);
          ccb_str_free(&msg);
          keep[r] = 0;
        }
      } else {
        // Any declared edge (known or unknown) stands over a
        // derived edge that does not match it exactly.
        keep[r] = 0;
      }
    }
    for (r = 0; r < screen->nedges; r++) {
      if (keep[r]) {
        screen->edges[w] = screen->edges[r];
        w++;
      }
    }
    screen->nedges = w;
    free(keep);
  }
}

ccb_status pv_project_load(const ccb_config *cfg, long frame_width, int ascii,
    int strict, ccb_diag_list *diags, pv_project **out) {
  return pv_project_load_scenario(cfg, frame_width, ascii, strict,
    diags, NULL, out);
}

ccb_status pv_project_load_scenario(const ccb_config *cfg,
    long frame_width, int ascii, int strict, ccb_diag_list *diags,
    const ccb_scenario *scenario, pv_project **out) {
  ccb_diag_list own_diags;
  ccb_diag_list *dl = NULL;
  pv_project *p = NULL;
  ccb_arena *arena = NULL;
  char **css_files = NULL;
  size_t ncss = 0;
  char **flow_files = NULL;
  size_t nflow = 0;
  size_t i = 0;
  if (out == NULL) {
    return CCB_ERR_CONFIG;
  }
  *out = NULL;
  if (cfg == NULL || !cfg->has_preview || (cfg->preview_styles == NULL &&
      cfg->preview_flow == NULL)) {
    return CCB_ERR_CONFIG;
  }
  if (diags != NULL) {
    dl = diags;
  } else {
    ccb_diag_list_init(&own_diags);
    dl = &own_diags;
  }
  arena = ccb_arena_create();
  p = ccb_arena_alloc(arena, sizeof(*p));
  memset(p, 0, sizeof(*p));
  p->arena = arena;
  p->ascii = ascii;
  p->strict = strict || cfg->preview_strict;
  p->phone_dp_width = cfg->preview_phone_width > 0 ?
    cfg->preview_phone_width : PV_DEFAULT_PHONE_DP_WIDTH;
  if (frame_width == 0) {
    frame_width = PV_DEFAULT_FRAME_WIDTH;
  }
  if (frame_width < PV_MIN_FRAME_WIDTH) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_appendf(&msg, "frame width %ld clamped to minimum %d",
      frame_width, PV_MIN_FRAME_WIDTH);
    pv_diag(dl, "CCB-P1013", msg.buf != NULL ? msg.buf : "", NULL, -1);
    ccb_str_free(&msg);
    frame_width = PV_MIN_FRAME_WIDTH;
  }
  p->frame_width = frame_width;
  p->interior_cols = frame_width - 2;
  // Slice 3 inputs, before flow parsing: the nav graph (destination
  // id -> screen name) and the layout onClick mapping (method ->
  // bound component label, resolved through [preview.bindings]).
  // A layout button with no binding is the CCB-P1019 case (warning,
  // no edge), exactly like an unbound listener receiver.
  {
    char *nav_path = pv_nav_path(cfg);
    char **nav_files = NULL;
    size_t nnav = 0;
    char **graph_keys = NULL;
    char **graph_vals = NULL;
    size_t ngraph = 0;
    char *layout_path = pv_layout_path(cfg);
    char **layout_files = NULL;
    size_t nlayout = 0;
    char **lay_methods = NULL;
    char **lay_ids = NULL;
    size_t nlay = 0;
    char **onclick_methods = NULL;
    char **onclick_labels = NULL;
    size_t nonclick = 0;
    char **warned_ids = NULL;
    size_t nwarned_ids = 0;
    pv_pending_edge **pending = NULL;
    size_t npending = 0;
    size_t li = 0;
    size_t pi = 0;
    pv_xml_inputs(nav_path, &nav_files, &nnav);
    free(nav_path);
    for (li = 0; li < nnav; li++) {
      char *text = NULL;
      size_t len = 0;
      pv_graph_ctx gctx;
      if (ccb_read_file(nav_files[li], &text, &len) != CCB_OK) {
        continue;
      }
      gctx.arena = arena;
      gctx.keys_io = &graph_keys;
      gctx.vals_io = &graph_vals;
      gctx.n_io = &ngraph;
      pv_xml_scan_tags(text, len, pv_graph_tag, &gctx);
      free(text);
    }
    ccb_free_str_array(nav_files, nnav);
    pv_xml_inputs(layout_path, &layout_files, &nlayout);
    free(layout_path);
    for (li = 0; li < nlayout; li++) {
      char *text = NULL;
      size_t len = 0;
      pv_layout_ctx lctx;
      if (ccb_read_file(layout_files[li], &text, &len) != CCB_OK) {
        continue;
      }
      lctx.methods_io = &lay_methods;
      lctx.ids_io = &lay_ids;
      lctx.n_io = &nlay;
      pv_xml_scan_tags(text, len, pv_layout_tag, &lctx);
      free(text);
    }
    ccb_free_str_array(layout_files, nlayout);
    for (pi = 0; pi < nlay; pi++) {
      const char *label = NULL;
      size_t w = 0;
      int warned = 0;
      const char *id_key = lay_ids[pi] != NULL ? lay_ids[pi] : "";
      for (w = 0; w < nwarned_ids; w++) {
        if (strcmp(warned_ids[w], id_key) == 0 &&
            strcmp(warned_ids[w] + strlen(id_key) + 1,
              lay_methods[pi]) == 0) {
          warned = 1;
          break;
        }
      }
      label = lay_ids[pi] != NULL ?
        pv_binding_lookup(cfg, lay_ids[pi]) : NULL;
      if (label == NULL) {
        ccb_str msg;
        if (!warned) {
          // Dedup key: "id\0method" in one malloc'd string.
          size_t kl = strlen(id_key) + 1 + strlen(lay_methods[pi]) + 1;
          char *key = ccb_xmalloc(kl);
          memcpy(key, id_key, strlen(id_key) + 1);
          memcpy(key + strlen(id_key) + 1, lay_methods[pi],
            strlen(lay_methods[pi]) + 1);
          warned_ids = ccb_xrealloc(warned_ids,
            (nwarned_ids + 1) * sizeof(*warned_ids));
          warned_ids[nwarned_ids] = key;
          nwarned_ids++;
        }
        if (!warned) {
          ccb_str_init(&msg);
          ccb_str_append(&msg,
            "handler found, button not bound: layout button '");
          ccb_str_append(&msg, id_key);
          ccb_str_append(&msg, "' (android:onClick '");
          ccb_str_append(&msg, lay_methods[pi]);
          ccb_str_append(&msg,
            "') has no [preview.bindings] entry; no edge recorded");
          pv_diag(dl, "CCB-P1019", msg.buf != NULL ? msg.buf : "",
            NULL, -1);
          ccb_str_free(&msg);
        }
        continue;
      }
      {
        // Identical (method, label) pairs (e.g. a button declared
        // in two layout variants) collapse to one; distinct labels
        // for one method are all kept.
        size_t q = 0;
        int dup = 0;
        for (q = 0; q < nonclick; q++) {
          if (strcmp(onclick_methods[q], lay_methods[pi]) == 0 &&
              strcmp(onclick_labels[q], label) == 0) {
            dup = 1;
            break;
          }
        }
        if (!dup) {
          pv_pair_add(arena, &onclick_methods, &onclick_labels,
            &nonclick, ccb_arena_strdup(arena, lay_methods[pi]),
            ccb_arena_strdup(arena, label), 0);
        }
      }
    }
    for (pi = 0; pi < nlay; pi++) {
      free(lay_methods[pi]);
      free(lay_ids[pi]);
    }
    free(lay_methods);
    free(lay_ids);
    for (pi = 0; pi < nwarned_ids; pi++) {
      free(warned_ids[pi]);
    }
    free(warned_ids);
    // Flow first: screens must exist before CSS files can be scoped.
    if (cfg->preview_flow != NULL) {
      if (ccb_collect_files(cfg->preview_flow, ".kt", &flow_files,
          &nflow) == CCB_OK) {
        for (i = 0; i < nflow; i++) {
          char *text = NULL;
          size_t len = 0;
          if (ccb_read_file(flow_files[i], &text, &len) != CCB_OK) {
            continue;
          }
          pv_flow_parse_full(arena, text, flow_files[i], dl,
            &p->screens, &p->nscreens, cfg->preview_binding_keys,
            cfg->preview_binding_values, cfg->preview_binding_count,
            onclick_methods, onclick_labels, nonclick,
            graph_keys, graph_vals, ngraph, &pending, &npending);
          free(text);
        }
        ccb_free_str_array(flow_files, nflow);
      }
    }
    // Slice 3 distribution: an onClick edge attaches to every screen
    // whose component tree contains the bound label (the button the
    // layout declares). A label in no screen is CCB-P1021 (warning,
    // no edge): the edge has nowhere to attach, and attaching it to
    // an unrelated screen would be a guess. Distribution runs before
    // reconciliation, so declared edges still beat these derived
    // edges exactly as they beat Slice 1 derived edges. Note the
    // labels compared here are pre-@string-resolution, matching the
    // Slice 1 reconciliation comparison.
    for (pi = 0; pi < npending; pi++) {
      pv_pending_edge *pe = pending[pi];
      int attached = 0;
      size_t si = 0;
      for (si = 0; si < p->nscreens; si++) {
        if (pv_tree_has_label(p->screens[si]->root, pe->from)) {
          pv_screen_edge_add(arena, p->screens[si], pe->from,
            pe->target, pe->unknown, pe->args, pe->nargs,
            pe->args_named, pe->branch);
          attached = 1;
        }
      }
      if (!attached) {
        ccb_str msg;
        ccb_str_init(&msg);
        ccb_str_append(&msg,
          "onClick handler '");
        ccb_str_append(&msg, pe->method);
        ccb_str_append(&msg, "' for button '");
        ccb_str_append(&msg, pe->from);
        ccb_str_append(&msg,
          "' matches no component in any screen; no edge recorded");
        pv_diag(dl, "CCB-P1021", msg.buf != NULL ? msg.buf : "",
          NULL, -1);
        ccb_str_free(&msg);
      }
    }
    // Declared-beats-derived reconciliation, after all flow files.
    pv_reconcile_derived(p, dl);
  }
  // CSS: common.css first, then per-screen files in sorted order.
  if (cfg->preview_styles != NULL) {
    if (ccb_collect_files(cfg->preview_styles, ".css", &css_files,
        &ncss) == CCB_OK) {
      size_t pass = 0;
      for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < ncss; i++) {
          const char *path = css_files[i];
          const char *scope = NULL;
          size_t before = 0;
          size_t j = 0;
          char *text = NULL;
          size_t len = 0;
          if (pass == 0 && !pv_is_common_css(path)) {
            continue;
          }
          if (pass == 1 && pv_is_common_css(path)) {
            continue;
          }
          if (pass == 1) {
            scope = pv_css_screen(p, path);
            if (scope == NULL) {
              ccb_str msg;
              ccb_str_init(&msg);
              ccb_str_append(&msg, "CSS file ignored: no screen named '");
              ccb_str_append_n(&msg, pv_basename(path),
                strlen(pv_basename(path)) > 4 ?
                strlen(pv_basename(path)) - 4 : 0);
              ccb_str_append(&msg, "'");
              pv_diag(dl, "CCB-P1015",
                msg.buf != NULL ? msg.buf : "", path, -1);
              ccb_str_free(&msg);
              continue;
            }
          }
          if (ccb_read_file(path, &text, &len) != CCB_OK) {
            continue;
          }
          // Tag only the rules this file newly appends: rules merged
          // into an earlier selector keep their earlier scope.
          before = p->nrules;
          pv_css_parse(arena, text, path, p->strict, dl, &p->rules,
            &p->nrules);
          free(text);
          if (scope != NULL) {
            for (j = before; j < p->nrules; j++) {
              p->rules[j].scope = ccb_arena_strdup(arena, scope);
            }
          }
        }
      }
      ccb_free_str_array(css_files, ncss);
    }
  }
  // @string/@color resolution, after flow parsing and CSS parsing
  // but before layout: labels resolve before clip widths are
  // computed, and CSS colors resolve before rules are used.
  if (cfg->preview_resources != NULL) {
    pv_res_table strings;
    pv_res_table colors;
    char *spath = NULL;
    char *cpath = NULL;
    size_t si = 0;
    memset(&strings, 0, sizeof(strings));
    memset(&colors, 0, sizeof(colors));
    spath = ccb_path_join(cfg->preview_resources, "strings.xml");
    cpath = ccb_path_join(cfg->preview_resources, "colors.xml");
    pv_res_parse_file(arena, spath, 0, dl, &strings);
    pv_res_parse_file(arena, cpath, 1, dl, &colors);
    free(spath);
    free(cpath);
    for (si = 0; si < p->nscreens; si++) {
      if (p->screens[si]->root != NULL) {
        pv_res_resolve_labels(arena, p->screens[si]->root, &strings, dl);
      }
    }
    pv_res_resolve_colors(arena, p, &colors, dl);
    free(strings.items);
    free(colors.items);
  }
  // Chunk 1: scenario merge, after resolution (scenario keys match
  // the labels as rendered) and before layout (scenario list rows
  // size the list like static rows do). Warnings go to dl, which is
  // adopted below along with the rest.
  if (scenario != NULL) {
    pv_scenario_apply(p, scenario, dl);
  }
  // Adopt the diag list BEFORE layout: pv_layout_screen pushes its
  // diagnostics directly into p->diags, so the project must own the
  // list by then (adopting it after layout would discard them).
  if (diags != NULL) {
    p->diags = *diags;
    memset(diags, 0, sizeof(*diags));
  } else {
    p->diags = own_diags;
  }
  for (i = 0; i < p->nscreens; i++) {
    pv_layout_screen(p, p->screens[i]);
  }
  // The project's screen/rule arrays live in the arena; the diag list
  // owns malloc'd strings and is freed by pv_project_free.
  p->capscreens = p->nscreens;
  p->caprules = p->nrules;
  *out = p;
  return CCB_OK;
}

void pv_project_free(pv_project *p) {
  if (p == NULL) {
    return;
  }
  ccb_diag_list_free(&p->diags);
  if (p->arena != NULL) {
    ccb_arena_destroy(p->arena);
  }
}

int pv_diag_is_error(const char *code, int strict) {
  if (code == NULL) {
    return 0;
  }
  if (strcmp(code, "CCB-P1005") == 0 ||
      strcmp(code, "CCB-P1008") == 0) {
    return 1;
  }
  if (strict && (strcmp(code, "CCB-P1001") == 0 ||
      strcmp(code, "CCB-P1014") == 0)) {
    return 1;
  }
  return 0;
}

long pv_project_diagnose(pv_project *p) {
  pv_screen *entry = NULL;
  size_t entry_idx = 0;
  size_t i = 0;
  size_t j = 0;
  long errors = 0;
  int changed = 0;
  if (p == NULL) {
    return 0;
  }
  if (p->nscreens == 0) {
    pv_diag(&p->diags, "CCB-P1011", "no screens found", NULL, -1);
    return 0;
  }
  entry = p->screens[0];
  entry_idx = 0;
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, "Home") == 0) {
      entry = p->screens[i];
      entry_idx = i;
      break;
    }
  }
  for (i = 0; i < p->nscreens; i++) {
    p->screens[i]->reachable = 0;
  }
  entry->reachable = 1;
  // BFS from the entry over known (non-unknown) edges, by screen name,
  // to a fixed point. Unknown edges never traverse.
  do {
    changed = 0;
    for (i = 0; i < p->nscreens; i++) {
      pv_screen *screen = p->screens[i];
      size_t e = 0;
      if (!screen->reachable) {
        continue;
      }
      for (e = 0; e < screen->nedges; e++) {
        pv_edge *edge = &screen->edges[e];
        if (edge->unknown || edge->target == NULL) {
          continue;
        }
        for (j = 0; j < p->nscreens; j++) {
          if (p->screens[j]->name != NULL &&
              strcmp(p->screens[j]->name, edge->target) == 0 &&
              !p->screens[j]->reachable) {
            p->screens[j]->reachable = 1;
            changed = 1;
          }
        }
      }
    }
  } while (changed);
  // Missing-target edges: one error per edge.
  for (i = 0; i < p->nscreens; i++) {
    pv_screen *screen = p->screens[i];
    size_t e = 0;
    for (e = 0; e < screen->nedges; e++) {
      pv_edge *edge = &screen->edges[e];
      ccb_str msg;
      if (edge->unknown || edge->target == NULL) {
        continue;
      }
      if (pv_screen_named(p, edge->target)) {
        continue;
      }
      ccb_str_init(&msg);
      ccb_str_append(&msg, "navigation to unknown screen '");
      ccb_str_append(&msg, edge->target);
      ccb_str_append(&msg, "'");
      pv_diag(&p->diags, "CCB-P1005", msg.buf != NULL ? msg.buf : "",
        NULL, -1);
      ccb_str_free(&msg);
    }
  }
  // Duplicate screen names: the 2nd and later occurrences are errors.
  for (i = 0; i < p->nscreens; i++) {
    int dup = 0;
    ccb_str msg;
    if (p->screens[i]->name == NULL) {
      continue;
    }
    for (j = 0; j < i; j++) {
      if (p->screens[j]->name != NULL &&
          strcmp(p->screens[j]->name, p->screens[i]->name) == 0) {
        dup = 1;
        break;
      }
    }
    if (!dup) {
      continue;
    }
    ccb_str_init(&msg);
    ccb_str_append(&msg, "duplicate screen '");
    ccb_str_append(&msg, p->screens[i]->name);
    ccb_str_append(&msg, "'");
    pv_diag(&p->diags, "CCB-P1008", msg.buf != NULL ? msg.buf : "",
      NULL, -1);
    ccb_str_free(&msg);
  }
  // Unreachable non-entry screens, and empty screens.
  for (i = 0; i < p->nscreens; i++) {
    pv_screen *screen = p->screens[i];
    ccb_str msg;
    if (!screen->reachable && i != entry_idx) {
      ccb_str_init(&msg);
      ccb_str_append(&msg, "unreachable screen '");
      ccb_str_append(&msg, screen->name != NULL ? screen->name : "?");
      ccb_str_append(&msg, "'");
      pv_diag(&p->diags, "CCB-P1006", msg.buf != NULL ? msg.buf : "",
        NULL, -1);
      ccb_str_free(&msg);
    }
    if (screen->root == NULL || screen->root->nchildren == 0) {
      ccb_str_init(&msg);
      ccb_str_append(&msg, "empty screen '");
      ccb_str_append(&msg, screen->name != NULL ? screen->name : "?");
      ccb_str_append(&msg, "'");
      pv_diag(&p->diags, "CCB-P1007", msg.buf != NULL ? msg.buf : "",
        NULL, -1);
      ccb_str_free(&msg);
    }
  }
  // Error count: missing-target and duplicate-screen are errors; under
  // strict, the subset warnings CCB-P1001 and CCB-P1014 count as errors
  // too. Everything else in the list is a warning.
  for (i = 0; i < p->diags.count; i++) {
    const char *code = p->diags.items[i].code;
    if (code == NULL) {
      continue;
    }
    if (strcmp(code, "CCB-P1005") == 0 ||
        strcmp(code, "CCB-P1008") == 0) {
      errors++;
    } else if (p->strict && (strcmp(code, "CCB-P1001") == 0 ||
        strcmp(code, "CCB-P1014") == 0)) {
      errors++;
    }
  }
  return errors;
}

// ---------- JSON ----------

static void pv_json_str(ccb_str *out, const char *s) {
  if (s == NULL) {
    ccb_str_append(out, "null");
  } else {
    ccb_json_escape(out, s);
  }
}

static void pv_json_box(ccb_str *out, pv_box *box, long interior_cols,
    long phone_dp_width) {
  size_t i = 0;
  ccb_str_append(out, "{\"type\":");
  if (box->type != NULL) {
    ccb_json_escape(out, box->type);
  } else {
    ccb_str_append(out, "\"\"");
  }
  ccb_str_append(out, ",\"label\":");
  pv_json_str(out, box != NULL ? box->label : NULL);
  ccb_str_appendf(out, ",\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld",
    box != NULL ? box->x : 0, box != NULL ? box->y : 0,
    box != NULL ? box->w : 0, box != NULL ? box->h : 0);
  // F2: DP size alongside cells, from the project's scale.
  ccb_str_appendf(out, ",\"w_dp\":%ld,\"h_dp\":%ld",
    pv_cols_to_dp(box != NULL ? box->w : 0, interior_cols,
      phone_dp_width),
    pv_rows_to_dp(box != NULL ? box->h : 0));
  ccb_str_append(out, ",\"children\":[");
  if (box != NULL) {
    for (i = 0; i < box->nchildren; i++) {
      if (i > 0) {
        ccb_str_append(out, ",");
      }
      pv_json_box(out, box->children[i], interior_cols, phone_dp_width);
    }
  }
  ccb_str_append(out, "]");
  // Chunk 1: scenario provenance, only when a scenario supplied
  // something for this box — static boxes stay unmarked.
  if (box != NULL && box->from_scenario) {
    ccb_str_append(out, ",\"provenance\":\"scenario\"");
  }
  if (box != NULL && box->scenario_text != NULL) {
    ccb_str_append(out, ",\"value\":");
    pv_json_str(out, box->scenario_text);
    ccb_str_append(out, ",\"value_provenance\":\"scenario\"");
  }
  ccb_str_append(out, "}");
}

static int pv_box_is_branch(pv_box *box) {
  return box != NULL && (box->is_branch ||
    (box->type != NULL && strcmp(box->type, "branch") == 0));
}

static void pv_json_branches(ccb_str *out, pv_box *box, int *first) {
  size_t i = 0;
  if (box == NULL) {
    return;
  }
  if (pv_box_is_branch(box)) {
    if (!*first) {
      ccb_str_append(out, ",");
    }
    *first = 0;
    ccb_str_append(out, "{\"label\":");
    if (box->label != NULL) {
      ccb_json_escape(out, box->label);
    } else {
      ccb_str_append(out, "\"\"");
    }
    ccb_str_append(out, ",\"entries\":[");
    for (i = 0; i < box->nchildren; i++) {
      pv_box *child = box->children[i];
      if (i > 0) {
        ccb_str_append(out, ",");
      }
      if (child->label != NULL) {
        ccb_json_escape(out, child->label);
      } else {
        ccb_str_append(out, "\"\"");
      }
    }
    // Chunk 1: the scenario-picked entry, if any. Entries stay plain
    // strings; only a pick adds a field, so unpicked branches render
    // exactly as before. Chunk 2: a trace pick is the same field
    // with "trace" provenance, and it wins over a scenario pick of
    // a different entry: the trace is the runtime record.
    {
      const char *picked = NULL;
      const char *prov = NULL;
      for (i = 0; i < box->nchildren; i++) {
        if (box->children[i]->trace_picked) {
          picked = box->children[i]->label;
          prov = "trace";
          break;
        }
      }
      if (picked == NULL) {
        for (i = 0; i < box->nchildren; i++) {
          if (box->children[i]->scenario_picked) {
            picked = box->children[i]->label;
            prov = "scenario";
            break;
          }
        }
      }
      if (picked != NULL) {
        ccb_str_append(out, "],\"picked\":{\"entry\":");
        ccb_json_escape(out, picked);
        ccb_str_append(out, ",\"provenance\":\"");
        ccb_str_append(out, prov);
        ccb_str_append(out, "\"}");
      } else {
        ccb_str_append(out, "]");
      }
    }
    ccb_str_append(out, "}");
  }
  for (i = 0; i < box->nchildren; i++) {
    pv_json_branches(out, box->children[i], first);
  }
}

void pv_project_json_screens(ccb_str *out, pv_project *p,
    pv_screen **screens, size_t nscreens) {
  size_t i = 0;
  size_t j = 0;
  pv_screen *entry = NULL;
  if (out == NULL || p == NULL) {
    return;
  }
  if (p->nscreens > 0) {
    entry = p->screens[0];
    for (i = 0; i < p->nscreens; i++) {
      if (p->screens[i]->name != NULL &&
          strcmp(p->screens[i]->name, "Home") == 0) {
        entry = p->screens[i];
        break;
      }
    }
  }
  ccb_str_append(out, "{\"event\":\"preview\",\"version\":2");
  // Chunk 1: the applied scenario's name, only when one was applied.
  if (p->scenario_name != NULL) {
    ccb_str_append(out, ",\"scenario\":");
    pv_json_str(out, p->scenario_name);
  }
  // Chunk 2: the applied trace's step count, only when one was
  // applied. Steps in the file, whether or not every step matched.
  if (p->trace_applied) {
    ccb_str_appendf(out, ",\"trace\":{\"steps\":%ld}",
      p->trace_step_count);
  }
  ccb_str_append(out, ",\"screens\":[");
  for (i = 0; i < nscreens; i++) {
    pv_screen *screen = screens[i];
    int first_branch = 1;
    if (i > 0) {
      ccb_str_append(out, ",");
    }
    ccb_str_append(out, "{\"name\":");
    pv_json_str(out, screen->name);
    ccb_str_appendf(out, ",\"entry\":%s,\"reachable\":%s",
      screen == entry ? "true" : "false",
      screen->reachable ? "true" : "false");
    ccb_str_append(out, ",\"boxes\":");
    if (screen->root != NULL) {
      pv_json_box(out, screen->root, p->interior_cols,
        p->phone_dp_width);
    } else {
      ccb_str_append(out, "null");
    }
    ccb_str_append(out, ",\"edges\":[");
    for (j = 0; j < screen->nedges; j++) {
      pv_edge *e = &screen->edges[j];
      size_t k = 0;
      if (j > 0) {
        ccb_str_append(out, ",");
      }
      ccb_str_append(out, "{\"from\":");
      pv_json_str(out, e->from);
      ccb_str_append(out, ",\"target\":");
      pv_json_str(out, e->target);
      ccb_str_appendf(out, ",\"unknown\":%s", e->unknown ? "true" : "false");
      ccb_str_append(out, ",\"args\":[");
      for (k = 0; k < e->nargs; k++) {
        if (k > 0) {
          ccb_str_append(out, ",");
        }
        pv_json_str(out, e->args[k]);
      }
      ccb_str_append(out, "],\"branch\":");
      pv_json_str(out, e->branch);
      // Chunk 2: the taken mark, only when an applied trace took
      // this edge.
      if (e->trace_taken) {
        ccb_str_append(out, ",\"taken\":true");
      }
      // Chunk 1: per-argument provenance, only when a scenario
      // filled at least one argument of this edge. Static entries
      // stay null (unmarked); filled ones read "scenario". Chunk 2:
      // trace fills read "trace"; a slot carries at most one.
      if (e->args_scenario != NULL || e->args_trace != NULL) {
        int has_filled = 0;
        for (k = 0; k < e->nargs; k++) {
          if ((e->args_scenario != NULL && e->args_scenario[k]) ||
              (e->args_trace != NULL && e->args_trace[k])) {
            has_filled = 1;
            break;
          }
        }
        if (has_filled) {
          ccb_str_append(out, ",\"args_provenance\":[");
          for (k = 0; k < e->nargs; k++) {
            if (k > 0) {
              ccb_str_append(out, ",");
            }
            if (e->args_trace != NULL && e->args_trace[k]) {
              ccb_str_append(out, "\"trace\"");
            } else if (e->args_scenario != NULL &&
                e->args_scenario[k]) {
              ccb_str_append(out, "\"scenario\"");
            } else {
              ccb_str_append(out, "null");
            }
          }
          ccb_str_append(out, "]");
        }
      }
      ccb_str_append(out, "}");
    }
    ccb_str_append(out, "],\"branches\":[");
    if (screen->root != NULL) {
      pv_json_branches(out, screen->root, &first_branch);
    }
    ccb_str_append(out, "]}");
  }
  ccb_str_append(out, "],\"diagnostics\":[");
  for (i = 0; i < p->diags.count; i++) {
    ccb_diag *d = &p->diags.items[i];
    if (i > 0) {
      ccb_str_append(out, ",");
    }
    ccb_str_append(out, "{\"code\":");
    if (d->code != NULL) {
      ccb_json_escape(out, d->code);
    } else {
      ccb_str_append(out, "\"\"");
    }
    ccb_str_append(out, ",\"phase\":");
    if (d->phase != NULL) {
      ccb_json_escape(out, d->phase);
    } else {
      ccb_str_append(out, "\"\"");
    }
    ccb_str_append(out, ",\"message\":");
    if (d->message != NULL) {
      ccb_json_escape(out, d->message);
    } else {
      ccb_str_append(out, "\"\"");
    }
    ccb_str_append(out, ",\"file\":");
    pv_json_str(out, d->file);
    ccb_str_appendf(out, ",\"line\":%ld}", d->line);
  }
  ccb_str_append(out, "]}");
}

void pv_project_json(ccb_str *out, pv_project *p) {
  if (out == NULL || p == NULL) {
    return;
  }
  pv_project_json_screens(out, p, p->screens, p->nscreens);
}
