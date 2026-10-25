// ccb_preview_tree: terminal tree client for ccb preview (Chunk 4).
// See ccb_preview_tree.h for the ownership rules and the split
// between the testable state machine and the terminal layer.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_json.h"
#include "ccb_preview_tree.h"
#include "ccb_str.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// ---------- JSON field helpers (defaults for missing fields) ----------

static const char *tree_json_str(const ccb_json_value *obj,
    const char *key) {
  const ccb_json_value *v = NULL;
  if (obj == NULL) {
    return NULL;
  }
  v = ccb_json_obj_get(obj, key);
  if (v != NULL && v->type == CCB_JSON_STR) {
    return v->str;
  }
  return NULL;
}

static int tree_json_bool(const ccb_json_value *obj, const char *key) {
  const ccb_json_value *v = NULL;
  if (obj == NULL) {
    return 0;
  }
  v = ccb_json_obj_get(obj, key);
  return v != NULL && v->type == CCB_JSON_BOOL && v->boolean;
}

static const ccb_json_value *tree_json_arr(
    const ccb_json_value *obj, const char *key) {
  const ccb_json_value *v = NULL;
  if (obj == NULL) {
    return NULL;
  }
  v = ccb_json_obj_get(obj, key);
  if (v != NULL && v->type == CCB_JSON_ARR) {
    return v;
  }
  return NULL;
}

// ---------- state machine ----------

void pv_tree_init(pv_tree *t) {
  if (t == NULL) {
    return;
  }
  memset(t, 0, sizeof(*t));
  t->trace_steps = -1;
}

void pv_tree_free(pv_tree *t) {
  if (t == NULL) {
    return;
  }
  if (t->arena != NULL) {
    ccb_arena_destroy(t->arena);
    t->arena = NULL;
  }
  free(t->feed_buf);
  t->feed_buf = NULL;
  t->feed_len = 0;
  t->feed_cap = 0;
}

static void tree_build_edge(ccb_arena *arena,
    const ccb_json_value *e, pv_tree_edge *out) {
  const char *from = NULL;
  const char *target = NULL;
  const char *branch = NULL;
  const ccb_json_value *args = NULL;
  const ccb_json_value *prov = NULL;
  size_t i = 0;
  memset(out, 0, sizeof(*out));
  from = tree_json_str(e, "from");
  out->from = ccb_arena_strdup(arena, from != NULL ? from : "?");
  target = tree_json_str(e, "target");
  out->target = target != NULL ? ccb_arena_strdup(arena, target) : NULL;
  out->unknown = tree_json_bool(e, "unknown");
  args = tree_json_arr(e, "args");
  if (args != NULL && args->count > 0) {
    out->args = ccb_arena_alloc(arena,
      args->count * sizeof(*out->args));
    out->nargs = args->count;
    for (i = 0; i < args->count; i++) {
      const ccb_json_value *a = args->items[i];
      out->args[i] = (a != NULL && a->type == CCB_JSON_STR &&
        a->str != NULL) ? ccb_arena_strdup(arena, a->str) : NULL;
    }
    prov = tree_json_arr(e, "args_provenance");
    if (prov != NULL && prov->count == args->count) {
      int has_filled = 0;
      for (i = 0; i < args->count; i++) {
        const ccb_json_value *p = prov->items[i];
        if (p != NULL && p->type == CCB_JSON_STR && p->str != NULL) {
          has_filled = 1;
          break;
        }
      }
      if (has_filled) {
        out->args_prov = ccb_arena_alloc(arena,
          args->count * sizeof(*out->args_prov));
        for (i = 0; i < args->count; i++) {
          const ccb_json_value *p = prov->items[i];
          out->args_prov[i] = (p != NULL && p->type == CCB_JSON_STR &&
            p->str != NULL) ? ccb_arena_strdup(arena, p->str) : NULL;
        }
      }
    }
  }
  branch = tree_json_str(e, "branch");
  out->branch = branch != NULL ? ccb_arena_strdup(arena, branch) : NULL;
  out->taken = tree_json_bool(e, "taken");
}

static void tree_build_branch(ccb_arena *arena,
    const ccb_json_value *b, pv_tree_branch *out) {
  const char *label = NULL;
  const char *picked_entry = NULL;
  const char *picked_prov = NULL;
  const ccb_json_value *entries = NULL;
  const ccb_json_value *picked = NULL;
  size_t i = 0;
  size_t nvalid = 0;
  size_t dst = 0;
  memset(out, 0, sizeof(*out));
  label = tree_json_str(b, "label");
  out->label = ccb_arena_strdup(arena, label != NULL ? label : "?");
  entries = tree_json_arr(b, "entries");
  if (entries != NULL) {
    for (i = 0; i < entries->count; i++) {
      const ccb_json_value *en = entries->items[i];
      if (en != NULL && en->type == CCB_JSON_STR && en->str != NULL) {
        nvalid++;
      }
    }
    if (nvalid > 0) {
      out->entries = ccb_arena_alloc(arena,
        nvalid * sizeof(*out->entries));
      out->nentries = nvalid;
      for (i = 0; i < entries->count; i++) {
        const ccb_json_value *en = entries->items[i];
        if (en != NULL && en->type == CCB_JSON_STR &&
            en->str != NULL) {
          out->entries[dst++] = ccb_arena_strdup(arena, en->str);
        }
      }
    }
  }
  picked = ccb_json_obj_get(b, "picked");
  if (picked != NULL && picked->type == CCB_JSON_OBJ) {
    picked_entry = tree_json_str(picked, "entry");
    picked_prov = tree_json_str(picked, "provenance");
    out->picked = picked_entry != NULL ?
      ccb_arena_strdup(arena, picked_entry) : NULL;
    out->picked_prov = picked_prov != NULL ?
      ccb_arena_strdup(arena, picked_prov) : NULL;
  }
}

static void tree_build_screen(ccb_arena *arena,
    const ccb_json_value *s, pv_tree_screen *out) {
  const char *name = NULL;
  const ccb_json_value *edges = NULL;
  const ccb_json_value *branches = NULL;
  size_t i = 0;
  memset(out, 0, sizeof(*out));
  name = tree_json_str(s, "name");
  out->name = ccb_arena_strdup(arena, name != NULL ? name : "?");
  out->entry = tree_json_bool(s, "entry");
  out->reachable = tree_json_bool(s, "reachable");
  edges = tree_json_arr(s, "edges");
  if (edges != NULL && edges->count > 0) {
    out->out = ccb_arena_alloc(arena,
      edges->count * sizeof(*out->out));
    out->nout = edges->count;
    for (i = 0; i < edges->count; i++) {
      const ccb_json_value *e = edges->items[i];
      if (e != NULL && e->type == CCB_JSON_OBJ) {
        tree_build_edge(arena, e, &out->out[i]);
      } else {
        // A malformed edge keeps its slot with a placeholder label,
        // so edge order never shifts under a bad payload.
        memset(&out->out[i], 0, sizeof(out->out[i]));
        out->out[i].from = ccb_arena_strdup(arena, "?");
        out->out[i].unknown = 1;
      }
    }
  }
  branches = tree_json_arr(s, "branches");
  if (branches != NULL && branches->count > 0) {
    out->branches = ccb_arena_alloc(arena,
      branches->count * sizeof(*out->branches));
    out->nbranches = branches->count;
    for (i = 0; i < branches->count; i++) {
      const ccb_json_value *b = branches->items[i];
      if (b != NULL && b->type == CCB_JSON_OBJ) {
        tree_build_branch(arena, b, &out->branches[i]);
      } else {
        memset(&out->branches[i], 0, sizeof(out->branches[i]));
        out->branches[i].label = ccb_arena_strdup(arena, "?");
      }
    }
  }
}

int pv_tree_apply_json(pv_tree *t, const ccb_json_value *tree) {
  const ccb_json_value *screens_v = NULL;
  const ccb_json_value *diags_v = NULL;
  const ccb_json_value *trace_v = NULL;
  const ccb_json_value *steps_v = NULL;
  const char *scenario = NULL;
  ccb_arena *arena = NULL;
  pv_tree_screen *screens = NULL;
  size_t nscreens = 0;
  pv_tree_diag *diags = NULL;
  size_t ndiags = 0;
  size_t i = 0;
  size_t dst = 0;
  char *old_sel = NULL;
  size_t old_index = 0;
  long trace_steps = -1;
  if (t == NULL || tree == NULL || tree->type != CCB_JSON_OBJ) {
    return -1;
  }
  screens_v = tree_json_arr(tree, "screens");
  if (screens_v == NULL) {
    return -1;
  }
  // Remember the old selection by name before the arena swap.
  old_index = t->sel;
  if (t->have_tree && t->nscreens > 0 && t->sel < t->nscreens &&
      t->screens[t->sel].name != NULL) {
    old_sel = ccb_xstrdup(t->screens[t->sel].name);
  }
  arena = ccb_arena_create();
  // Screens: two passes so the array is exact — entries without a
  // string name are skipped, never rendered as "?".
  for (i = 0; i < screens_v->count; i++) {
    const ccb_json_value *s = screens_v->items[i];
    if (s != NULL && s->type == CCB_JSON_OBJ &&
        tree_json_str(s, "name") != NULL) {
      nscreens++;
    }
  }
  if (nscreens > 0) {
    screens = ccb_arena_alloc(arena, nscreens * sizeof(*screens));
    for (i = 0; i < screens_v->count; i++) {
      const ccb_json_value *s = screens_v->items[i];
      if (s != NULL && s->type == CCB_JSON_OBJ &&
          tree_json_str(s, "name") != NULL) {
        tree_build_screen(arena, s, &screens[dst++]);
      }
    }
  }
  diags_v = tree_json_arr(tree, "diagnostics");
  if (diags_v != NULL && diags_v->count > 0) {
    const char *code = NULL;
    const char *message = NULL;
    const char *file = NULL;
    const ccb_json_value *line_v = NULL;
    diags = ccb_arena_alloc(arena, diags_v->count * sizeof(*diags));
    ndiags = diags_v->count;
    for (i = 0; i < diags_v->count; i++) {
      const ccb_json_value *d = diags_v->items[i];
      memset(&diags[i], 0, sizeof(diags[i]));
      if (d == NULL || d->type != CCB_JSON_OBJ) {
        diags[i].code = ccb_arena_strdup(arena, "");
        diags[i].message = ccb_arena_strdup(arena, "");
        continue;
      }
      code = tree_json_str(d, "code");
      message = tree_json_str(d, "message");
      file = tree_json_str(d, "file");
      line_v = ccb_json_obj_get(d, "line");
      diags[i].code = ccb_arena_strdup(arena,
        code != NULL ? code : "");
      diags[i].message = ccb_arena_strdup(arena,
        message != NULL ? message : "");
      diags[i].file = file != NULL ? ccb_arena_strdup(arena, file) :
        NULL;
      diags[i].line = (line_v != NULL && line_v->type == CCB_JSON_NUM) ?
        (long)line_v->num : 0;
    }
  }
  scenario = tree_json_str(tree, "scenario");
  trace_v = ccb_json_obj_get(tree, "trace");
  if (trace_v != NULL && trace_v->type == CCB_JSON_OBJ) {
    steps_v = ccb_json_obj_get(trace_v, "steps");
    if (steps_v != NULL && steps_v->type == CCB_JSON_NUM) {
      trace_steps = (long)steps_v->num;
    }
  }
  // Swap: the old tree dies with its arena only after the new one
  // is fully built, so a failed shape check above keeps it.
  if (t->arena != NULL) {
    ccb_arena_destroy(t->arena);
  }
  t->arena = arena;
  t->screens = screens;
  t->nscreens = nscreens;
  t->diags = diags;
  t->ndiags = ndiags;
  t->scenario = scenario != NULL ? ccb_arena_strdup(arena, scenario) :
    NULL;
  t->trace_steps = trace_steps;
  t->have_tree = 1;
  if (nscreens == 0) {
    t->sel = 0;
  } else if (old_sel != NULL) {
    int found = 0;
    for (i = 0; i < nscreens; i++) {
      if (screens[i].name != NULL &&
          strcmp(screens[i].name, old_sel) == 0) {
        t->sel = i;
        found = 1;
        break;
      }
    }
    if (!found) {
      t->sel = old_index < nscreens ? old_index : nscreens - 1;
    }
  } else if (t->sel >= nscreens) {
    t->sel = nscreens - 1;
  }
  free(old_sel);
  return 0;
}

int pv_tree_apply_publish(pv_tree *t, const char *json_text) {
  ccb_json_value *v = NULL;
  char *err = NULL;
  int rc = 0;
  if (t == NULL || json_text == NULL) {
    return -1;
  }
  v = ccb_json_parse((ccb_slice){ json_text, strlen(json_text) },
    &err);
  free(err);
  if (v == NULL) {
    return -1;
  }
  rc = pv_tree_apply_json(t, v);
  ccb_json_free(v);
  return rc;
}

// ---------- server-byte feed (Content-Length framing) ----------

// Find the end of the header block ("\r\n\r\n") in buf[0..len).
// Returns the offset just past it, or -1 when not fully present.
static long tree_feed_header_end(const char *buf, size_t len) {
  size_t i = 0;
  if (len < 4) {
    return -1;
  }
  for (i = 0; i + 4 <= len; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n') {
      return (long)(i + 4);
    }
  }
  return -1;
}

// Parse the Content-Length value from a header block (no trailing
// "\r\n\r\n"). Header names match case-insensitively; the value
// must be digits with optional surrounding spaces. Returns the
// length, or -1 when absent or malformed.
static long tree_feed_content_length(const char *buf, size_t len) {
  size_t line_start = 0;
  while (line_start < len) {
    size_t line_end = line_start;
    while (line_end < len && buf[line_end] != '\r' &&
        buf[line_end] != '\n') {
      line_end++;
    }
    // "Content-Length:" is 15 chars; compare case-insensitively.
    if (line_end - line_start >= 15) {
      static const char *kName = "Content-Length:";
      int match = 1;
      size_t k = 0;
      for (k = 0; k < 15; k++) {
        char a = buf[line_start + k];
        char b = kName[k];
        if (a >= 'A' && a <= 'Z') {
          a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
          b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
          match = 0;
          break;
        }
      }
      if (match) {
        size_t p = line_start + 15;
        long value = 0;
        int any = 0;
        while (p < line_end && (buf[p] == ' ' || buf[p] == '\t')) {
          p++;
        }
        while (p < line_end && buf[p] >= '0' && buf[p] <= '9') {
          value = value * 10 + (buf[p] - '0');
          if (value > 64L * 1024L * 1024L) {
            return -1;
          }
          any = 1;
          p++;
        }
        while (p < line_end && (buf[p] == ' ' || buf[p] == '\t')) {
          p++;
        }
        if (any && p == line_end) {
          return value;
        }
        return -1;
      }
    }
    line_start = line_end;
    while (line_start < len && (buf[line_start] == '\r' ||
        buf[line_start] == '\n')) {
      line_start++;
    }
  }
  return -1;
}

int pv_tree_feed(pv_tree *t, const char *bytes, size_t len) {
  int applied = 0;
  if (t == NULL || bytes == NULL || len == 0) {
    return 0;
  }
  if (t->feed_len + len > t->feed_cap) {
    size_t ncap = t->feed_cap == 0 ? 4096 : t->feed_cap;
    while (ncap < t->feed_len + len) {
      ncap *= 2;
    }
    t->feed_buf = ccb_xrealloc(t->feed_buf, ncap);
    t->feed_cap = ncap;
  }
  memcpy(t->feed_buf + t->feed_len, bytes, len);
  t->feed_len += len;
  for (;;) {
    long hend = tree_feed_header_end(t->feed_buf, t->feed_len);
    long clen = 0;
    size_t total = 0;
    ccb_json_value *msg = NULL;
    char *err = NULL;
    if (hend < 0) {
      // No complete header yet; a runaway buffer without one is
      // garbage, not a slow server — drop it and resume.
      if (t->feed_len > 1024 * 1024) {
        t->feed_len = 0;
      }
      break;
    }
    clen = tree_feed_content_length(t->feed_buf, (size_t)hend);
    if (clen < 0) {
      // Garbage header line: discard through it and reframe.
      memmove(t->feed_buf, t->feed_buf + hend, t->feed_len - hend);
      t->feed_len -= (size_t)hend;
      continue;
    }
    total = (size_t)hend + (size_t)clen;
    if (t->feed_len < total) {
      break; // body still arriving
    }
    msg = ccb_json_parse(
      (ccb_slice){ t->feed_buf + hend, (size_t)clen }, &err);
    free(err);
    if (msg != NULL) {
      const char *method = tree_json_str(msg, "method");
      if (method != NULL && strcmp(method, "ccb/publishTree") == 0) {
        const ccb_json_value *params = ccb_json_obj_get(msg,
          "params");
        if (params != NULL && pv_tree_apply_json(t, params) == 0) {
          applied++;
        }
      }
      ccb_json_free(msg);
    }
    // Responses and other methods are ignored, never fatal.
    memmove(t->feed_buf, t->feed_buf + total, t->feed_len - total);
    t->feed_len -= total;
  }
  return applied;
}

// ---------- selection ----------

void pv_tree_apply_key(pv_tree *t, pv_tree_key key) {
  if (t == NULL) {
    return;
  }
  if (key == PV_TREE_KEY_QUIT) {
    t->quit = 1;
    return;
  }
  if (t->nscreens == 0) {
    return;
  }
  if (key == PV_TREE_KEY_UP) {
    if (t->sel > 0) {
      t->sel--;
    }
  } else if (key == PV_TREE_KEY_DOWN) {
    if (t->sel + 1 < t->nscreens) {
      t->sel++;
    }
  }
}

int pv_tree_select(pv_tree *t, const char *name) {
  size_t i = 0;
  if (t == NULL || name == NULL) {
    return 0;
  }
  for (i = 0; i < t->nscreens; i++) {
    if (t->screens[i].name != NULL &&
        strcmp(t->screens[i].name, name) == 0) {
      t->sel = i;
      return 1;
    }
  }
  return 0;
}

const pv_tree_screen *pv_tree_selected(const pv_tree *t) {
  if (t == NULL || t->nscreens == 0 || t->sel >= t->nscreens) {
    return NULL;
  }
  return &t->screens[t->sel];
}

// ---------- render ----------

// Render one edge's arguments: ("lit", unknown) with per-slot
// provenance marks. The published JSON carries no named-arg flag,
// so every literal renders quoted, matching the tree format.
static void tree_render_args(ccb_str *out, const pv_tree_edge *e) {
  size_t k = 0;
  if (e->nargs == 0) {
    return;
  }
  ccb_str_append(out, "(");
  for (k = 0; k < e->nargs; k++) {
    if (k > 0) {
      ccb_str_append(out, ", ");
    }
    if (e->args[k] != NULL) {
      ccb_str_append(out, "\"");
      ccb_str_append(out, e->args[k]);
      ccb_str_append(out, "\"");
      if (e->args_prov != NULL && e->args_prov[k] != NULL) {
        ccb_str_append(out, " [");
        ccb_str_append(out, e->args_prov[k]);
        ccb_str_append(out, "]");
      }
    } else {
      ccb_str_append(out, "unknown");
    }
  }
  ccb_str_append(out, ")");
}

static void tree_render_edge_line(ccb_str *out, const pv_tree_edge *e,
    const char *arrow) {
  ccb_str_append(out, arrow);
  ccb_str_append(out, " ");
  if (e->unknown || e->target == NULL) {
    ccb_str_append(out, "unknown");
  } else {
    ccb_str_append(out, e->target);
  }
  tree_render_args(out, e);
  ccb_str_append(out, " via \"");
  ccb_str_append(out, e->from != NULL ? e->from : "?");
  ccb_str_append(out, "\"");
  if (e->branch != NULL) {
    ccb_str_append(out, " [");
    ccb_str_append(out, e->branch);
    ccb_str_append(out, "]");
  }
  if (e->taken) {
    ccb_str_append(out, " [trace]");
  }
  ccb_str_append(out, "\n");
}

void pv_tree_render(ccb_str *out, const pv_tree *t) {
  const pv_tree_screen *sel = NULL;
  size_t i = 0;
  size_t j = 0;
  if (out == NULL || t == NULL) {
    return;
  }
  ccb_str_append(out, "ccb preview \xe2\x80\x94 live logic tree\n");
  if (t->scenario != NULL || t->trace_steps >= 0) {
    int need_sep = 0;
    if (t->scenario != NULL) {
      ccb_str_append(out, "scenario: ");
      ccb_str_append(out, t->scenario);
      need_sep = 1;
    }
    if (t->trace_steps >= 0) {
      if (need_sep) {
        ccb_str_append(out, " | ");
      }
      ccb_str_appendf(out, "trace: %ld steps", t->trace_steps);
    }
    ccb_str_append(out, "\n");
  }
  if (!t->have_tree) {
    ccb_str_append(out, "\nwaiting for logic tree...\n\n");
    ccb_str_append(out,
      "j/k or up/down: move | q: quit\n");
    return;
  }
  ccb_str_appendf(out, "%zu screens\n\n", t->nscreens);
  if (t->nscreens == 0) {
    ccb_str_append(out, "no screens in this project\n\n");
  } else {
    ccb_str_append(out, "Screens:\n");
    for (i = 0; i < t->nscreens; i++) {
      const pv_tree_screen *s = &t->screens[i];
      ccb_str_append(out, i == t->sel ? "> " : "  ");
      ccb_str_append(out, s->name != NULL ? s->name : "?");
      if (s->entry) {
        ccb_str_append(out, " [entry]");
      }
      if (!s->reachable) {
        ccb_str_append(out, " (unreachable)");
      }
      ccb_str_append(out, "\n");
    }
    ccb_str_append(out, "\n");
  }
  sel = pv_tree_selected(t);
  if (sel != NULL) {
    ccb_str_append(out, sel->name != NULL ? sel->name : "?");
    if (sel->entry) {
      ccb_str_append(out, " [entry]");
    }
    if (!sel->reachable) {
      ccb_str_append(out, " (unreachable)");
    }
    ccb_str_append(out, ":\n");
    ccb_str_append(out, "  Outgoing:\n");
    if (sel->nout == 0) {
      ccb_str_append(out, "    (no outgoing edges)\n");
    }
    for (i = 0; i < sel->nout; i++) {
      ccb_str_append(out, "    ");
      tree_render_edge_line(out, &sel->out[i], "->");
    }
    ccb_str_append(out, "  Incoming:\n");
    {
      int any_in = 0;
      for (i = 0; i < t->nscreens; i++) {
        const pv_tree_screen *s = &t->screens[i];
        for (j = 0; j < s->nout; j++) {
          const pv_tree_edge *e = &s->out[j];
          if (!e->unknown && e->target != NULL &&
              sel->name != NULL &&
              strcmp(e->target, sel->name) == 0) {
            any_in = 1;
            ccb_str_append(out, "    <- ");
            ccb_str_append(out, s->name != NULL ? s->name : "?");
            ccb_str_append(out, " via \"");
            ccb_str_append(out, e->from != NULL ? e->from : "?");
            ccb_str_append(out, "\"");
            tree_render_args(out, e);
            if (e->branch != NULL) {
              ccb_str_append(out, " [");
              ccb_str_append(out, e->branch);
              ccb_str_append(out, "]");
            }
            if (e->taken) {
              ccb_str_append(out, " [trace]");
            }
            ccb_str_append(out, "\n");
          }
        }
      }
      if (!any_in) {
        ccb_str_append(out, "    (no incoming edges)\n");
      }
    }
    ccb_str_append(out, "  Branches:\n");
    if (sel->nbranches == 0) {
      ccb_str_append(out, "    (no branches on this screen)\n");
    }
    for (i = 0; i < sel->nbranches; i++) {
      const pv_tree_branch *b = &sel->branches[i];
      ccb_str_append(out, "    ");
      ccb_str_append(out, b->label != NULL ? b->label : "?");
      ccb_str_append(out, ": ");
      for (j = 0; j < b->nentries; j++) {
        if (j > 0) {
          ccb_str_append(out, ", ");
        }
        ccb_str_append(out, b->entries[j] != NULL ?
          b->entries[j] : "?");
      }
      if (b->picked != NULL) {
        ccb_str_append(out, " (picked: ");
        ccb_str_append(out, b->picked);
        if (b->picked_prov != NULL) {
          ccb_str_append(out, " [");
          ccb_str_append(out, b->picked_prov);
          ccb_str_append(out, "]");
        }
        ccb_str_append(out, ")");
      }
      ccb_str_append(out, "\n");
    }
    ccb_str_append(out, "\n");
  }
  if (t->ndiags > 0) {
    ccb_str_append(out, "Diagnostics:\n");
    for (i = 0; i < t->ndiags; i++) {
      const pv_tree_diag *d = &t->diags[i];
      ccb_str_append(out, "  ");
      ccb_str_append(out, d->code != NULL ? d->code : "");
      ccb_str_append(out, " ");
      ccb_str_append(out, d->message != NULL ? d->message : "");
      if (d->file != NULL) {
        ccb_str_append(out, " (");
        ccb_str_append(out, d->file);
        if (d->line > 0) {
          ccb_str_appendf(out, ":%ld", d->line);
        }
        ccb_str_append(out, ")");
      }
      ccb_str_append(out, "\n");
    }
    ccb_str_append(out, "\n");
  }
  ccb_str_append(out, "j/k or up/down: move | q: quit\n");
}

// ---------- terminal layer ----------

// Saved terminal state, restored on every exit path. The signal
// handler only records the signal; the loop restores the terminal
// itself, so restoration never runs inside a handler.
static struct termios tree_saved_termios;
static int tree_termios_saved;
static volatile sig_atomic_t tree_got_signal;

static void tree_on_signal(int sig) {
  tree_got_signal = sig;
}

static void tree_restore_termios(void) {
  if (tree_termios_saved) {
    tcsetattr(STDIN_FILENO, TCSANOW, &tree_saved_termios);
    tree_termios_saved = 0;
  }
}

// Read one tree key from stdin (raw mode). Returns 1 and sets *key,
// 0 on EOF (clean end), or -1 on read error / interrupt. Bytes with
// no action (Enter, Backspace, bare Esc, unknown sequences) are
// skipped, like walk mode skips unknown bytes.
static int tree_read_key(pv_tree_key *key) {
  unsigned char c = 0;
  for (;;) {
    ssize_t n = 0;
    for (;;) {
      n = read(STDIN_FILENO, &c, 1);
      if (n == 1) {
        break;
      }
      if (n == 0) {
        return 0;
      }
      if (tree_got_signal) {
        return -1;
      }
      // EINTR without a recorded signal: retry.
    }
    switch (c) {
      case 'q':
        *key = PV_TREE_KEY_QUIT;
        return 1;
      case 'j':
        *key = PV_TREE_KEY_DOWN;
        return 1;
      case 'k':
        *key = PV_TREE_KEY_UP;
        return 1;
      case 0x03: // Ctrl-C with ISIG off arrives as a byte
        *key = PV_TREE_KEY_QUIT;
        return 1;
      case 0x1b: {
        // Arrow keys arrive as ESC [ A/B (or ESC O A/B). A bare Esc
        // (nothing follows within 50 ms) or any other sequence is
        // ignored: the tree view has no Esc action.
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0) {
          break; // bare Esc: ignore, read the next byte
        }
        if (read(STDIN_FILENO, &c, 1) != 1) {
          break;
        }
        if (c != '[' && c != 'O') {
          break;
        }
        if (read(STDIN_FILENO, &c, 1) != 1) {
          break;
        }
        if (c == 'A') {
          *key = PV_TREE_KEY_UP;
          return 1;
        }
        if (c == 'B') {
          *key = PV_TREE_KEY_DOWN;
          return 1;
        }
        break; // other sequence: ignore
      }
      default:
        break; // unknown byte: ignore, read the next one
    }
  }
}

// Write all of buf to fd, retrying EINTR and partial writes.
// Returns 0 on success, -1 on failure.
static int tree_write_all(int fd, const char *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = write(fd, buf + done, len - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}

// Send one framed JSON-RPC message to the server. Returns 0 on
// success, -1 on write failure.
static int tree_send_framed(int fd, const char *body) {
  ccb_str frame;
  int rc = 0;
  ccb_str_init(&frame);
  ccb_str_appendf(&frame, "Content-Length: %zu\r\n\r\n",
    strlen(body));
  ccb_str_append(&frame, body);
  rc = tree_write_all(fd, frame.buf != NULL ? frame.buf : "",
    frame.buf != NULL ? strlen(frame.buf) : 0);
  ccb_str_free(&frame);
  return rc;
}

// Percent-encode a root path for a file:// URI: alphanumerics and
// "/._-~" pass through, everything else becomes %XX (the server
// decodes %XX). Returns a malloc'd string.
static char *tree_uri_escape(const char *path) {
  static const char *kHex = "0123456789ABCDEF";
  ccb_str out;
  const unsigned char *p = (const unsigned char *)path;
  ccb_str_init(&out);
  for (; *p != '\0'; p++) {
    unsigned char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '/' || c == '.' ||
        c == '_' || c == '-' || c == '~') {
      ccb_str_append_c(&out, (char)c);
    } else {
      ccb_str_append_c(&out, '%');
      ccb_str_append_c(&out, kHex[(c >> 4) & 0xf]);
      ccb_str_append_c(&out, kHex[c & 0xf]);
    }
  }
  if (out.buf == NULL) {
    return ccb_xstrdup("");
  }
  return out.buf; // ownership transfers to the caller
}

// Resolve this executable's path: the caller's exe_path, or
// /proc/self/exe when NULL. Returns a malloc'd path or NULL.
static char *tree_resolve_exe(const char *exe_path) {
  char buf[PATH_MAX];
  ssize_t n = 0;
  if (exe_path != NULL) {
    return ccb_xstrdup(exe_path);
  }
  n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) {
    return NULL;
  }
  buf[n] = '\0';
  return ccb_xstrdup(buf);
}

int pv_tree_run(const char *exe_path, const char *root_dir,
    const char *scenario_name, const char *trace_path,
    const char *start_screen) {
  pv_tree t;
  struct termios raw;
  struct sigaction sa, old_int, old_term;
  int have_old = 0;
  char *exe = NULL;
  char *uri = NULL;
  ccb_str init_body;
  int to_child[2];
  int from_child[2];
  pid_t pid = -1;
  int rc = 0;
  int server_eof = 0;
  int start_error = 0;
  char *start_error_msg = NULL;
  if (root_dir == NULL) {
    return 1;
  }
  // Refuse non-TTY stdio before touching the terminal or spawning:
  // never hang.
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "error: tree-live mode requires a terminal: "
      "stdin and stdout must both be TTYs\n");
    return 2;
  }
  exe = tree_resolve_exe(exe_path);
  if (exe == NULL) {
    fprintf(stderr, "error: cannot locate the ccb executable\n");
    return 2;
  }
  if (pipe(to_child) != 0 || pipe(from_child) != 0) {
    fprintf(stderr, "error: cannot create pipes for the preview "
      "server\n");
    free(exe);
    return 1;
  }
  pid = fork();
  if (pid < 0) {
    fprintf(stderr, "error: cannot start the preview server\n");
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    free(exe);
    return 1;
  }
  if (pid == 0) {
    // Child: stdin from the parent's pipe, stdout to the parent's
    // pipe, stderr inherited (server diagnostics stay visible).
    char *argv[8];
    int ai = 0;
    dup2(to_child[0], 0);
    dup2(from_child[1], 1);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    argv[ai++] = exe;
    argv[ai++] = (char *)"lsp";
    if (scenario_name != NULL) {
      argv[ai++] = (char *)"--scenario";
      argv[ai++] = (char *)scenario_name;
    }
    if (trace_path != NULL) {
      argv[ai++] = (char *)"--trace";
      argv[ai++] = (char *)trace_path;
    }
    argv[ai] = NULL;
    execv(exe, argv);
    _exit(127);
  }
  // Parent: close the child's ends.
  close(to_child[0]);
  close(from_child[1]);
  // Initialize the server at this project root. A failed write
  // means the child died at once (bad scenario/trace the CLI
  // missed, or a broken build): report, don't hang.
  uri = tree_uri_escape(root_dir);
  ccb_str_init(&init_body);
  ccb_str_append(&init_body, "{\"jsonrpc\":\"2.0\",\"id\":1,"
    "\"method\":\"initialize\",\"params\":{\"rootUri\":\"file://");
  ccb_str_append(&init_body, uri);
  ccb_str_append(&init_body, "\",\"capabilities\":{}}}");
  if (tree_send_framed(to_child[1], init_body.buf != NULL ?
      init_body.buf : "") != 0 ||
      tree_send_framed(to_child[1],
        "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}") != 0) {
    fprintf(stderr, "error: preview server ended before it could "
      "initialize\n");
    ccb_str_free(&init_body);
    free(uri);
    close(to_child[1]);
    close(from_child[0]);
    // Reap without hanging: the child is already gone or dying.
    waitpid(pid, NULL, 0);
    free(exe);
    return 1;
  }
  ccb_str_free(&init_body);
  free(uri);
  pv_tree_init(&t);
  // Install signal handlers that only record the signal; the loop
  // below restores the terminal on the next iteration.
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = tree_on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, &old_int) == 0 &&
      sigaction(SIGTERM, &sa, &old_term) == 0) {
    have_old = 1;
  }
  if (tcgetattr(STDIN_FILENO, &tree_saved_termios) != 0) {
    fprintf(stderr, "error: cannot read terminal settings\n");
    if (have_old) {
      sigaction(SIGINT, &old_int, NULL);
      sigaction(SIGTERM, &old_term, NULL);
    }
    close(to_child[1]);
    close(from_child[0]);
    waitpid(pid, NULL, 0);
    pv_tree_free(&t);
    free(exe);
    return 1;
  }
  tree_termios_saved = 1;
  raw = tree_saved_termios;
  cfmakeraw(&raw);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    fprintf(stderr, "error: cannot set terminal raw mode\n");
    tree_restore_termios();
    if (have_old) {
      sigaction(SIGINT, &old_int, NULL);
      sigaction(SIGTERM, &old_term, NULL);
    }
    close(to_child[1]);
    close(from_child[0]);
    waitpid(pid, NULL, 0);
    pv_tree_free(&t);
    free(exe);
    return 1;
  }
  // Event loop: render, then wait for a key or a server message.
  // Every exit below restores the terminal before returning.
  while (!t.quit && !tree_got_signal) {
    ccb_str out;
    fd_set rfds;
    int maxfd = 0;
    int s = 0;
    ccb_str_init(&out);
    pv_tree_render(&out, &t);
    fputs("\x1b[2J\x1b[H", stdout);
    fputs(out.buf != NULL ? out.buf : "", stdout);
    fflush(stdout);
    ccb_str_free(&out);
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    FD_SET(from_child[0], &rfds);
    maxfd = STDIN_FILENO > from_child[0] ? STDIN_FILENO :
      from_child[0];
    s = select(maxfd + 1, &rfds, NULL, NULL, NULL);
    if (s < 0) {
      if (errno == EINTR && !tree_got_signal) {
        continue;
      }
      break;
    }
    if (FD_ISSET(from_child[0], &rfds)) {
      char chunk[4096];
      ssize_t n = read(from_child[0], chunk, sizeof(chunk));
      if (n == 0) {
        server_eof = 1;
        break;
      }
      if (n > 0) {
        int had_tree_before = t.have_tree;
        int applied = pv_tree_feed(&t, chunk, (size_t)n);
        if (applied > 0 && !had_tree_before &&
            start_screen != NULL) {
          // First publish: resolve the requested start screen
          // now, while the terminal can still be restored for a
          // clean usage error.
          if (!pv_tree_select(&t, start_screen)) {
            start_error = 1;
            break;
          }
        }
      } else if (errno != EINTR) {
        server_eof = 1;
        break;
      }
    }
    if (FD_ISSET(STDIN_FILENO, &rfds)) {
      pv_tree_key key = PV_TREE_KEY_QUIT;
      int kr = tree_read_key(&key);
      if (kr == 0) {
        break; // EOF on stdin: clean end
      }
      if (kr < 0) {
        break; // read error or signal
      }
      pv_tree_apply_key(&t, key);
    }
  }
  rc = 0;
  tree_restore_termios();
  if (have_old) {
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
  }
  if (start_error) {
    // The start screen names no screen in the first published
    // tree: list what exists, like the plain render does.
    ccb_str msg;
    size_t i = 0;
    ccb_str_init(&msg);
    ccb_str_appendf(&msg, "error: no screen named '%s'\n",
      start_screen);
    ccb_str_append(&msg, "available screens:");
    for (i = 0; i < t.nscreens; i++) {
      ccb_str_append(&msg, " ");
      ccb_str_append(&msg, t.screens[i].name != NULL ?
        t.screens[i].name : "?");
    }
    ccb_str_append(&msg, "\n");
    start_error_msg = msg.buf; // freed below via free()
    rc = 1;
  } else if (server_eof && !t.have_tree) {
    start_error_msg = ccb_xstrdup("error: preview server ended "
      "without sending a logic tree\n");
    rc = 1;
  } else if (server_eof && t.have_tree) {
    start_error_msg = ccb_xstrdup("error: preview server ended "
      "unexpectedly\n");
    rc = 1;
  }
  pv_tree_free(&t);
  // Shut the server down cleanly: shutdown + exit, then a bounded
  // wait, then SIGKILL so a stuck server can never hang the exit.
  // Best effort: the server may already be gone.
  (void)tree_send_framed(to_child[1],
    "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"shutdown\","
    "\"params\":null}");
  (void)tree_send_framed(to_child[1],
    "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
  close(to_child[1]);
  close(from_child[0]);
  {
    int waited = 0;
    int status = 0;
    pid_t w = 0;
    while (waited < 200) {
      w = waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        break;
      }
      if (w < 0) {
        break;
      }
      (void)poll(NULL, 0, 10);
      waited++;
    }
    if (w != pid) {
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
    }
  }
  free(exe);
  if (start_error_msg != NULL) {
    fputs(start_error_msg, stderr);
    free(start_error_msg);
  }
  return rc;
}
