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

// Normalize an Android class name for comparison (see
// ccb_preview.h): strip a leading '.', strip the project package
// prefix + '.', then take the simple name (after the last '.').
char *pv_normalize_class(const char *package, const char *name) {
  const char *s = NULL;
  const char *dot = NULL;
  size_t plen = 0;
  if (name == NULL) {
    return NULL;
  }
  s = name;
  if (s[0] == '.') {
    s++;
  }
  if (package != NULL && package[0] != '\0') {
    plen = strlen(package);
    if (strncmp(s, package, plen) == 0 && s[plen] == '.') {
      s += plen + 1;
    }
  }
  dot = strrchr(s, '.');
  if (dot != NULL) {
    s = dot + 1;
  }
  return ccb_xstrdup(s);
}

// Length of s without one trailing "Activity" or "Fragment", or
// the full length when it has neither. A bare "Activity" or
// "Fragment" is a name, not a suffix, so it is kept whole.
static size_t pv_stripped_len(const char *s) {
  size_t n = strlen(s);
  if (n > 8 && strcmp(s + n - 8, "Activity") == 0) {
    return n - 8;
  }
  if (n > 8 && strcmp(s + n - 8, "Fragment") == 0) {
    return n - 8;
  }
  return n;
}

// True when two names denote the same screen (see ccb_preview.h).
int pv_names_match(const char *package, const char *a, const char *b) {
  char *na = NULL;
  char *nb = NULL;
  size_t la = 0;
  size_t lb = 0;
  int match = 0;
  if (a == NULL || b == NULL) {
    return 0;
  }
  na = pv_normalize_class(package, a);
  nb = pv_normalize_class(package, b);
  if (na == NULL || nb == NULL) {
    free(na);
    free(nb);
    return 0;
  }
  la = strlen(na);
  lb = strlen(nb);
  match = strcmp(na, nb) == 0 ||
    (pv_stripped_len(na) != la && pv_stripped_len(na) == lb &&
      memcmp(na, nb, lb) == 0) ||
    (pv_stripped_len(nb) != lb && pv_stripped_len(nb) == la &&
      memcmp(na, nb, la) == 0);
  free(na);
  free(nb);
  return match;
}

// Find a screen by target name (see ccb_preview.h): an exact
// normalized match wins over a suffix match, so a "Detail" screen
// always beats a "DetailActivity" screen for target "Detail".
pv_screen *pv_screen_find(pv_project *p, const char *target) {
  char *tnorm = NULL;
  size_t i = 0;
  if (p == NULL || target == NULL) {
    return NULL;
  }
  tnorm = pv_normalize_class(p->package, target);
  if (tnorm == NULL) {
    return NULL;
  }
  for (i = 0; i < p->nscreens; i++) {
    char *norm = NULL;
    int eq = 0;
    if (p->screens[i]->name == NULL) {
      continue;
    }
    norm = pv_normalize_class(p->package, p->screens[i]->name);
    eq = norm != NULL && strcmp(norm, tnorm) == 0;
    free(norm);
    if (eq) {
      free(tnorm);
      return p->screens[i];
    }
  }
  free(tnorm);
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        pv_names_match(p->package, p->screens[i]->name, target)) {
      return p->screens[i];
    }
  }
  return NULL;
}

// True when a nav target names a manifest activity (normalized +
// suffix match, same as screens). Manifest activities are known
// targets, not screens: they exempt an edge from CCB-P1005 but
// never create a node.
static int pv_manifest_has(pv_project *p, const char *target) {
  size_t i = 0;
  if (p == NULL || target == NULL) {
    return 0;
  }
  for (i = 0; i < p->nmanifest_activities; i++) {
    if (pv_names_match(p->package, p->manifest_activities[i],
        target)) {
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
  const char *package;
  char ***keys_io;
  char ***vals_io;
  size_t *n_io;
} pv_graph_ctx;

static void pv_graph_tag(void *ctx_v, const char *text, size_t tag_start,
    size_t tag_end, const char *local, size_t local_len) {
  pv_graph_ctx *ctx = ctx_v;
  char *id = NULL;
  char *name = NULL;
  char *norm = NULL;
  (void)local;
  (void)local_len;
  id = pv_xml_attr(text, tag_start, tag_end, "android:id");
  name = pv_xml_attr(text, tag_start, tag_end, "android:name");
  if (id != NULL && name != NULL) {
    // Chunk A: graph values are stored normalized, so a
    // fully-qualified destination compares equal to the flow screen
    // it names (see pv_normalize_class).
    norm = pv_normalize_class(ctx->package, name);
    pv_pair_add(ctx->arena, ctx->keys_io, ctx->vals_io, ctx->n_io,
      ccb_arena_strdup(ctx->arena, pv_res_name(id)),
      ccb_arena_strdup(ctx->arena, norm != NULL ? norm : name), 1);
    free(norm);
  }
  free(id);
  free(name);
}

typedef struct {
  ccb_arena *arena;
  const char *package;
  char ***acts_io;
  size_t *nacts_io;
} pv_manifest_ctx;

static void pv_manifest_tag(void *ctx_v, const char *text, size_t tag_start,
    size_t tag_end, const char *local, size_t local_len) {
  pv_manifest_ctx *ctx = ctx_v;
  char *name = NULL;
  char *norm = NULL;
  size_t i = 0;
  size_t n = 0;
  char **next = NULL;
  // Only <activity> elements count; activity-alias, service,
  // receiver, and provider are not screens and never exempt a nav
  // target. Nested tags (intent-filter children) never match the
  // local name, so no depth tracking is needed.
  if (local_len != 8 || memcmp(local, "activity", 8) != 0) {
    return;
  }
  name = pv_xml_attr(text, tag_start, tag_end, "android:name");
  if (name == NULL) {
    return;
  }
  norm = pv_normalize_class(ctx->package, name);
  free(name);
  if (norm == NULL) {
    return;
  }
  for (i = 0; i < *ctx->nacts_io; i++) {
    if (strcmp((*ctx->acts_io)[i], norm) == 0) {
      free(norm);
      return;
    }
  }
  n = *ctx->nacts_io;
  next = ccb_arena_alloc(ctx->arena, (n + 1) * sizeof(*next));
  for (i = 0; i < n; i++) {
    next[i] = (*ctx->acts_io)[i];
  }
  next[n] = ccb_arena_strdup(ctx->arena, norm);
  free(norm);
  *ctx->acts_io = next;
  *ctx->nacts_io = n + 1;
}

// Collect normalized activity names from a manifest (see header).
void pv_manifest_activities(ccb_arena *arena, const char *manifest_path,
    const char *package, char ***acts_io, size_t *nacts_io) {
  char *text = NULL;
  size_t len = 0;
  pv_manifest_ctx ctx;
  if (arena == NULL || acts_io == NULL || nacts_io == NULL) {
    return;
  }
  *acts_io = NULL;
  *nacts_io = 0;
  if (manifest_path == NULL ||
      ccb_read_file(manifest_path, &text, &len) != CCB_OK) {
    return;
  }
  ctx.arena = arena;
  ctx.package = package;
  ctx.acts_io = acts_io;
  ctx.nacts_io = nacts_io;
  pv_xml_scan_tags(text, len, pv_manifest_tag, &ctx);
  free(text);
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

// Location of the layout inputs: [preview] layout when set
// (strict: the config layer requires the path), else the first
// EXISTING directory among <paths res>/layout and
// <resources>/layout, else none. [paths] res is preferred: the
// build already knows the res root, and [preview] resources is
// documented as the values directory, whose /layout never exists
// — so a values-dir resources must fall through to res instead of
// shadowing it with an empty input set.
char *pv_preview_layout_path(const ccb_config *cfg) {
  if (cfg->preview_layout != NULL) {
    return ccb_xstrdup(cfg->preview_layout);
  }
  if (cfg->res != NULL) {
    char *cand = ccb_path_join(cfg->res, "layout");
    if (ccb_path_exists(cand)) {
      return cand;
    }
    free(cand);
  }
  if (cfg->preview_resources != NULL) {
    char *cand = ccb_path_join(cfg->preview_resources, "layout");
    if (ccb_path_exists(cand)) {
      return cand;
    }
    free(cand);
  }
  return NULL;
}

// Location of the nav-graph inputs: [preview] nav_graph when set
// (strict, as above), else the first EXISTING directory among
// <paths res>/navigation and <resources>/navigation, else none.
char *pv_preview_nav_path(const ccb_config *cfg) {
  if (cfg->preview_nav_graph != NULL) {
    return ccb_xstrdup(cfg->preview_nav_graph);
  }
  if (cfg->res != NULL) {
    char *cand = ccb_path_join(cfg->res, "navigation");
    if (ccb_path_exists(cand)) {
      return cand;
    }
    free(cand);
  }
  if (cfg->preview_resources != NULL) {
    char *cand = ccb_path_join(cfg->preview_resources, "navigation");
    if (ccb_path_exists(cand)) {
      return cand;
    }
    free(cand);
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

// Copy every diagnostic from src into dl (push moves ownership,
// so each item is copied field by field; src stays usable).
// Lets assembly replay cached per-file parse diagnostics in load
// order without consuming the staging cache's lists.
static void pv_diag_replay(ccb_diag_list *dl,
    const ccb_diag_list *src) {
  size_t i = 0;
  if (dl == NULL || src == NULL) {
    return;
  }
  for (i = 0; i < src->count; i++) {
    ccb_diag d;
    const ccb_diag *s = &src->items[i];
    ccb_diag_init(&d);
    ccb_diag_set_full(&d, s->code, s->phase, s->message, s->file,
      s->line, s->col, s->command, s->workdir, s->suggestion);
    ccb_diag_list_push(dl, &d);
  }
}

// Scan the nav-graph and layout XML inputs (see ccb_preview.h).
// Code motion from pv_project_load_scenario: the scan block
// moved verbatim, with dl renamed to diags and the pending-edge
// declarations left in the caller (pending belongs to flow
// parsing, not to the scan).
void pv_preview_scan_xml(const ccb_config *cfg, ccb_arena *arena,
    char ***graph_keys_io, char ***graph_vals_io, size_t *ngraph_io,
    char ***onclick_methods_io, char ***onclick_labels_io,
    size_t *nonclick_io, ccb_diag_list *diags) {
  char **graph_keys = NULL;
  char **graph_vals = NULL;
  size_t ngraph = 0;
  char **onclick_methods = NULL;
  char **onclick_labels = NULL;
  size_t nonclick = 0;
    char *nav_path = pv_preview_nav_path(cfg);
    char **nav_files = NULL;
    size_t nnav = 0;
    char *layout_path = pv_preview_layout_path(cfg);
    char **layout_files = NULL;
    size_t nlayout = 0;
    char **lay_methods = NULL;
    char **lay_ids = NULL;
    size_t nlay = 0;
    char **warned_ids = NULL;
    size_t nwarned_ids = 0;
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
      gctx.package = cfg != NULL ? cfg->package : NULL;
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
          pv_diag(diags, "CCB-P1019", msg.buf != NULL ? msg.buf : "",
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
  if (graph_keys_io != NULL) {
    *graph_keys_io = graph_keys;
  }
  if (graph_vals_io != NULL) {
    *graph_vals_io = graph_vals;
  }
  if (ngraph_io != NULL) {
    *ngraph_io = ngraph;
  }
  if (onclick_methods_io != NULL) {
    *onclick_methods_io = onclick_methods;
  }
  if (onclick_labels_io != NULL) {
    *onclick_labels_io = onclick_labels;
  }
  if (nonclick_io != NULL) {
    *nonclick_io = nonclick;
  }
}

// Assemble a project from pre-parsed inputs (see ccb_preview.h).
// Distribution, reconciliation, resources, scenario, diag adopt,
// and layout moved verbatim from pv_project_load_scenario; only
// the CSS loop is new, merging pre-parsed per-file entries
// instead of parsing from disk.
void pv_project_assemble(pv_project *p, pv_screen **screens,
    size_t nscreens, pv_pending_edge **pending, size_t npending,
    pv_css_entry *css_entries, size_t ncss,
    const char *resources_dir, const ccb_scenario *scenario,
    ccb_diag_list *diags, char **graph_vals, size_t ngraph,
    char **manifest_activities, size_t nmanifest_activities,
    const char *package) {
  ccb_arena *arena = NULL;
  size_t i = 0;
  size_t pi = 0;
  pv_screen **all = NULL;
  size_t nall = 0;
  size_t g = 0;
  if (p == NULL) {
    return;
  }
  arena = p->arena;
  // Chunk A: record the package + manifest activities for
  // pv_project_diagnose (P1005 scope) before anything needs them.
  p->package = package != NULL ? ccb_arena_strdup(arena, package) : NULL;
  if (nmanifest_activities > 0) {
    p->manifest_activities = ccb_arena_alloc(arena,
      nmanifest_activities * sizeof(*p->manifest_activities));
    for (i = 0; i < nmanifest_activities; i++) {
      p->manifest_activities[i] =
        ccb_arena_strdup(arena, manifest_activities[i]);
    }
  } else {
    p->manifest_activities = NULL;
  }
  p->nmanifest_activities = nmanifest_activities;
  // Chunk A: one node per nav-graph destination with no matching
  // flow screen, appended after the flow screens so the entry rule
  // (Home, else first screen) never changes for a project that has
  // flow screens. Matching is pv_names_match, so a DetailActivity
  // destination never duplicates a Detail screen, and two graph
  // ids naming the same destination yield one node. Nodes carry no
  // boxes, so distribution (label match), CSS scoping by label,
  // scenario row/field application (root-NULL guards), and layout
  // (root-NULL early return) all skip them naturally.
  all = ccb_arena_alloc(arena,
    (nscreens + ngraph == 0 ? 1 : nscreens + ngraph) * sizeof(*all));
  for (i = 0; i < nscreens; i++) {
    all[nall++] = screens[i];
  }
  for (g = 0; g < ngraph; g++) {
    pv_screen *node = NULL;
    int found = 0;
    if (graph_vals == NULL || graph_vals[g] == NULL) {
      continue;
    }
    for (i = 0; i < nall; i++) {
      if (all[i]->name != NULL &&
          pv_names_match(package, all[i]->name, graph_vals[g])) {
        found = 1;
        break;
      }
    }
    if (found) {
      continue;
    }
    node = ccb_arena_alloc(arena, sizeof(*node));
    memset(node, 0, sizeof(*node));
    node->name = ccb_arena_strdup(arena, graph_vals[g]);
    node->graph_only = 1;
    all[nall++] = node;
  }
  screens = all;
  nscreens = nall;
  // The screens array is a fresh arena array of borrowed screen
  // pointers; the screens themselves stay caller-owned (the
  // load path parses them into this arena, the LSP server
  // deep-copies its staging fragments into it), except the
  // graph-only nodes above, which are arena-owned here.
  p->screens = ccb_arena_alloc(arena,
    (nscreens == 0 ? 1 : nscreens) * sizeof(*p->screens));
  for (i = 0; i < nscreens; i++) {
    p->screens[i] = screens[i];
  }
  p->nscreens = nscreens;
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
        pv_diag(diags, "CCB-P1021", msg.buf != NULL ? msg.buf : "",
          NULL, -1);
        ccb_str_free(&msg);
      }
    }
  // Declared-beats-derived reconciliation, after all flow files.
  pv_reconcile_derived(p, diags);
  // CSS: common.css first, then per-screen files in sorted order.
  // Entries carry each file's own parsed rules; merging here (not
  // at parse time) is what lets the LSP server reuse per-file
  // cached parses. The merge function reproduces pv_css_parse's
  // cross-file semantics, including the per-screen split, so the
  // merged rules are identical to a direct sequential parse.
  {
    size_t pass = 0;
    size_t ci = 0;
    for (pass = 0; pass < 2; pass++) {
      for (ci = 0; ci < ncss; ci++) {
        const pv_css_entry *entry = &css_entries[ci];
        const char *scope = NULL;
        size_t ri = 0;
        if (pass == 0 && !pv_is_common_css(entry->path)) {
          continue;
        }
        if (pass == 1 && pv_is_common_css(entry->path)) {
          continue;
        }
        if (pass == 1) {
          scope = pv_css_screen(p, entry->path);
          if (scope == NULL) {
            ccb_str msg;
            ccb_str_init(&msg);
            ccb_str_append(&msg, "CSS file ignored: no screen named '");
            ccb_str_append_n(&msg, pv_basename(entry->path),
              strlen(pv_basename(entry->path)) > 4 ?
              strlen(pv_basename(entry->path)) - 4 : 0);
            ccb_str_append(&msg, "'");
            pv_diag(diags, "CCB-P1015",
              msg.buf != NULL ? msg.buf : "", entry->path, -1);
            ccb_str_free(&msg);
            continue;
          }
        }
        // Replay this file's parse diagnostics in load order,
        // then merge its rules.
        if (entry->diags != NULL) {
          pv_diag_replay(diags, entry->diags);
        }
        for (ri = 0; ri < entry->nrules; ri++) {
          pv_css_rule_merge(arena, &p->rules, &p->nrules,
            &entry->rules[ri], entry->path, scope);
        }
      }
    }
  }
  // @string/@color resolution, after flow parsing and CSS parsing
  // but before layout: labels resolve before clip widths are
  // computed, and CSS colors resolve before rules are used.
  if (resources_dir != NULL) {
    pv_res_table strings;
    pv_res_table colors;
    char *spath = NULL;
    char *cpath = NULL;
    size_t si = 0;
    memset(&strings, 0, sizeof(strings));
    memset(&colors, 0, sizeof(colors));
    spath = ccb_path_join(resources_dir, "strings.xml");
    cpath = ccb_path_join(resources_dir, "colors.xml");
    pv_res_parse_file(arena, spath, 0, diags, &strings);
    pv_res_parse_file(arena, cpath, 1, diags, &colors);
    free(spath);
    free(cpath);
    for (si = 0; si < p->nscreens; si++) {
      if (p->screens[si]->root != NULL) {
        pv_res_resolve_labels(arena, p->screens[si]->root, &strings, diags);
      }
    }
    pv_res_resolve_colors(arena, p, &colors, diags);
    free(strings.items);
    free(colors.items);
  }
  // Chunk 1: scenario merge, after resolution (scenario keys match
  // the labels as rendered) and before layout (scenario list rows
  // size the list like static rows do). Warnings go to dl, which is
  // adopted below along with the rest.
  if (scenario != NULL) {
    pv_scenario_apply(p, scenario, diags);
  }
  // Adopt the diag list BEFORE layout: pv_layout_screen pushes its
  // diagnostics directly into p->diags, so the project must own
  // the list by then (adopting it after layout would discard them).
  if (diags != NULL) {
    p->diags = *diags;
    memset(diags, 0, sizeof(*diags));
  } else {
    ccb_diag_list_init(&p->diags);
  }
  // Adopt the diag list BEFORE layout: pv_layout_screen pushes its
  // diagnostics directly into p->diags, so the project must own the
  // list by then (adopting it after layout would discard them).
  for (i = 0; i < p->nscreens; i++) {
    pv_layout_screen(p, p->screens[i]);
  }
  // The project's screen/rule arrays live in the arena; the diag list
  // owns malloc'd strings and is freed by pv_project_free.
  p->capscreens = p->nscreens;
  p->caprules = p->nrules;
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
  pv_screen **screens = NULL;
  size_t nscreens = 0;
  pv_pending_edge **pending = NULL;
  size_t npending = 0;
  // Chunk A: graph values are function-scoped (not scan-block
  // scoped) so pv_project_assemble can create nodes for graph
  // destinations with no flow screen. The arrays are arena-owned
  // by the scan, so they outlive the block either way.
  char **graph_vals = NULL;
  size_t ngraph = 0;
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
  {
    char **graph_keys = NULL;
    char **onclick_methods = NULL;
    char **onclick_labels = NULL;
    size_t nonclick = 0;
    pv_preview_scan_xml(cfg, arena, &graph_keys, &graph_vals,
      &ngraph, &onclick_methods, &onclick_labels, &nonclick, dl);
    // Flow first: screens must exist before CSS files can be scoped.
    // Chunk B: all flow texts are read first so the whole-project
    // function index can be built before any file is parsed; the
    // texts outlive every parse that uses the index. The kept
    // arrays borrow their paths from flow_files (still owned by
    // it) and own their texts.
    if (cfg->preview_flow != NULL) {
      if (ccb_collect_files(cfg->preview_flow, ".kt", &flow_files,
          &nflow) == CCB_OK) {
        char **kept_files = ccb_xcalloc(nflow == 0 ? 1 : nflow,
          sizeof(*kept_files));
        char **kept_texts = ccb_xcalloc(nflow == 0 ? 1 : nflow,
          sizeof(*kept_texts));
        size_t nkept = 0;
        pv_fun_index *index = NULL;
        for (i = 0; i < nflow; i++) {
          char *text = NULL;
          size_t len = 0;
          if (ccb_read_file(flow_files[i], &text, &len) != CCB_OK) {
            continue;
          }
          kept_files[nkept] = flow_files[i];
          kept_texts[nkept] = text;
          nkept++;
        }
        index = pv_fun_index_build(arena, kept_files, kept_texts, nkept);
        for (i = 0; i < nkept; i++) {
          pv_flow_parse_indexed(arena, kept_texts[i], kept_files[i], dl,
            &screens, &nscreens, cfg->preview_binding_keys,
            cfg->preview_binding_values, cfg->preview_binding_count,
            onclick_methods, onclick_labels, nonclick,
            graph_keys, graph_vals, ngraph, &pending, &npending, index);
        }
        // Chunk C: warn for every nav call no parse attributed to
        // an edge, before the index texts are freed.
        pv_sweep_unattributed_navs(index, dl);
        for (i = 0; i < nkept; i++) {
          free(kept_texts[i]);
        }
        free(kept_files);
        free(kept_texts);
        pv_fun_index_free(index);
        ccb_free_str_array(flow_files, nflow);
      }
    }
  }
  // CSS entries: every .css file parsed once, in sorted order,
  // into its own rules + diagnostics. Assembly merges them in
  // load order (common first, then per-screen) and replays the
  // diagnostics of merged files only, so a file assembly skips
  // (CCB-P1015) contributes nothing, exactly as before.
  {
    pv_css_entry *css_entries = NULL;
    size_t ncss_entries = 0;
    ccb_diag_list *css_lists = NULL;
    size_t ci = 0;
    if (cfg->preview_styles != NULL) {
      if (ccb_collect_files(cfg->preview_styles, ".css", &css_files,
          &ncss) == CCB_OK) {
        css_entries = ccb_xcalloc(ncss == 0 ? 1 : ncss,
          sizeof(*css_entries));
        css_lists = ccb_xcalloc(ncss == 0 ? 1 : ncss,
          sizeof(*css_lists));
        ncss_entries = ncss;
        for (ci = 0; ci < ncss; ci++) {
          char *text = NULL;
          size_t len = 0;
          ccb_diag_list_init(&css_lists[ci]);
          if (ccb_read_file(css_files[ci], &text, &len) != CCB_OK) {
            continue;
          }
          css_entries[ci].path = css_files[ci];
          pv_css_parse(arena, text, css_files[ci], p->strict,
            &css_lists[ci], &css_entries[ci].rules,
            &css_entries[ci].nrules);
          css_entries[ci].diags = &css_lists[ci];
          free(text);
        }
      }
    }
    {
      // Chunk A: manifest activities for the P1005 scope, parsed
      // from the configured manifest (none when unconfigured or
      // unreadable — preview never fails on the manifest).
      char **manifest_acts = NULL;
      size_t nmanifest_acts = 0;
      pv_manifest_activities(arena, cfg->manifest, cfg->package,
        &manifest_acts, &nmanifest_acts);
      pv_project_assemble(p, screens, nscreens, pending, npending,
        css_entries, ncss_entries, cfg->preview_resources, scenario,
        dl, graph_vals, ngraph, manifest_acts, nmanifest_acts,
        cfg->package);
    }
    for (ci = 0; ci < ncss_entries; ci++) {
      ccb_diag_list_free(&css_lists[ci]);
    }
    free(css_lists);
    free(css_entries);
    ccb_free_str_array(css_files, ncss);
  }
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
  // BFS from the entry over known (non-unknown) edges, by screen
  // name, to a fixed point. Unknown edges never traverse. Chunk A:
  // the target resolves through pv_screen_find, so an edge to
  // DetailActivity marks the Detail screen reachable.
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
        pv_screen *target = NULL;
        if (edge->unknown || edge->target == NULL) {
          continue;
        }
        target = pv_screen_find(p, edge->target);
        if (target != NULL && !target->reachable) {
          target->reachable = 1;
          changed = 1;
        }
      }
    }
  } while (changed);
  // Missing-target edges: one error per edge. Chunk A: a target is
  // known when it matches a screen (normalized, suffix-aware), a
  // nav-graph destination (now a node, so matched above), or a
  // manifest activity. Anything else is CCB-P1005.
  for (i = 0; i < p->nscreens; i++) {
    pv_screen *screen = p->screens[i];
    size_t e = 0;
    for (e = 0; e < screen->nedges; e++) {
      pv_edge *edge = &screen->edges[e];
      ccb_str msg;
      if (edge->unknown || edge->target == NULL) {
        continue;
      }
      if (pv_screen_find(p, edge->target) != NULL ||
          pv_manifest_has(p, edge->target)) {
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
    if (!screen->graph_only &&
        (screen->root == NULL || screen->root->nchildren == 0)) {
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

// One screen object, exactly as it appears in the "screens"
// array of pv_project_json_screens. first tracks comma
// separation across calls (*first = 1 for the first screen).
// Extracted from pv_project_json_screens so ccb/screenInfo
// can serve one screen without duplicating the writer.
static void pv_json_screen(ccb_str *out, pv_project *p,
    pv_screen *screen, int *first) {
  size_t j = 0;
  int first_branch = 1;
  pv_screen *entry = NULL;
  size_t i = 0;
  if (out == NULL || p == NULL || screen == NULL ||
      first == NULL) {
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
  if (!*first) {
    ccb_str_append(out, ",");
  }
  *first = 0;
    ccb_str_append(out, "{\"name\":");
    pv_json_str(out, screen->name);
    ccb_str_appendf(out, ",\"entry\":%s,\"reachable\":%s",
      screen == entry ? "true" : "false",
      screen->reachable ? "true" : "false");
    // Chunk A: only when set, so output without graph nodes is
    // byte-identical to before.
    if (screen->graph_only) {
      ccb_str_append(out, ",\"graph_only\":true");
    }
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

void pv_project_json_screens(ccb_str *out, pv_project *p,
    pv_screen **screens, size_t nscreens) {
  size_t i = 0;
  int first = 1;
  if (out == NULL || p == NULL) {
    return;
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
    pv_json_screen(out, p, screens[i], &first);
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

void pv_project_json_screen(ccb_str *out, pv_project *p,
    pv_screen *screen) {
  int first = 1;
  pv_json_screen(out, p, screen, &first);
}

void pv_project_json(ccb_str *out, pv_project *p) {
  if (out == NULL || p == NULL) {
    return;
  }
  pv_project_json_screens(out, p, p->screens, p->nscreens);
}
