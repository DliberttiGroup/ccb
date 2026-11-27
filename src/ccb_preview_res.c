// ccb_preview_res: @string/@color resource table for preview.
// Split from ccb_preview.c.

#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_preview_scenario.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include "ccb_json.h"
#include "ccb_preview_internal.h"

// ---------- @string/@color resources ----------

// One resource table (strings or colors), parsed from a values XML
// file. Entries are arena-owned; only the items array is malloc'd
// and is freed after resolution.

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
void pv_res_parse_file(ccb_arena *arena, const char *path,
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
void pv_res_resolve_labels(ccb_arena *arena, pv_box *box,
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
void pv_res_resolve_colors(ccb_arena *arena, pv_project *p,
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
