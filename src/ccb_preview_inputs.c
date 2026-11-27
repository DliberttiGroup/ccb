// ccb_preview_inputs: layout XML + nav-graph + manifest inputs.
// Split from ccb_preview.c (Slices 2/3 inputs).

#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_preview_scenario.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include "ccb_json.h"
#include "ccb_preview_internal.h"

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
void pv_pair_add(ccb_arena *arena, char ***keys_io, char ***vals_io,
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

// Call fn for every opening tag in the text: tag_start/tag_end bound
// the tag content (between '<' and '>'); local is the tag name's
// local part (after the last '.'), not NUL-terminated. Comments,
// closing tags, processing instructions, and declarations are
// skipped; '>' inside a quoted attribute value does not end a tag.
void pv_xml_scan_tags(const char *text, size_t len, pv_tag_fn fn,
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

void pv_layout_tag(void *ctx_v, const char *text, size_t tag_start,
    size_t tag_end, const char *local, size_t local_len) {
  pv_layout_ctx *ctx = ctx_v;
  char *onclick = NULL;
  char *id = NULL;
  char *label_text = NULL;
  size_t n = 0;
  char **next_m = NULL;
  char **next_i = NULL;
  char **next_t = NULL;
  if (local_len < 6 || memcmp(local + local_len - 6, "Button", 6) != 0) {
    return;
  }
  onclick = pv_xml_attr(text, tag_start, tag_end, "android:onClick");
  if (onclick == NULL) {
    return;
  }
  id = pv_xml_attr(text, tag_start, tag_end, "android:id");
  // Phase 2: the layout's own android:text is the label fallback
  // when no [preview.bindings] entry names the button — the layout
  // declares the label, so an onClick button whose text is right
  // there in the XML is bound without a config table.
  label_text = pv_xml_attr(text, tag_start, tag_end, "android:text");
  n = *ctx->n_io;
  // xrealloc preserves the existing entries; only the new slot is
  // filled here.
  next_m = ccb_xrealloc(*ctx->methods_io, (n + 1) * sizeof(*next_m));
  next_i = ccb_xrealloc(*ctx->ids_io, (n + 1) * sizeof(*next_i));
  next_t = ccb_xrealloc(*ctx->texts_io, (n + 1) * sizeof(*next_t));
  next_m[n] = onclick;
  next_i[n] = id != NULL ? ccb_xstrdup(pv_res_name(id)) : NULL;
  next_t[n] = label_text;
  free(id);
  *ctx->methods_io = next_m;
  *ctx->ids_io = next_i;
  *ctx->texts_io = next_t;
  *ctx->n_io = n + 1;
}

void pv_graph_tag(void *ctx_v, const char *text, size_t tag_start,
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
void pv_xml_inputs(const char *path, char ***files_io,
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
void pv_screen_edge_add(ccb_arena *arena, pv_screen *screen,
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

// True when the component tree contains a BUTTON box with the
// given label. Only buttons match: an onClick edge's `from` is a
// button label by construction, and a TextView that happens to
// share the text (a screen title, e.g. "Settings" on the Settings
// screen) is a different component — matching it would attach the
// edge to a screen that has no such button. (Phase 2: layout
// wireframes carry title TextViews, so label-only matching
// mis-attached the Main settings-button edge to the Settings
// screen itself.)
int pv_tree_has_label(pv_box *box, const char *label) {
  size_t i = 0;
  if (box == NULL || label == NULL) {
    return 0;
  }
  if (box->label != NULL && strcmp(box->label, label) == 0 &&
      box->type != NULL && strcmp(box->type, "button") == 0) {
    return 1;
  }
  for (i = 0; i < box->nchildren; i++) {
    if (pv_tree_has_label(box->children[i], label)) {
      return 1;
    }
  }
  return 0;
}

const char *pv_binding_lookup(const ccb_config *cfg,
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
void pv_reconcile_derived(pv_project *p, ccb_diag_list *diags) {
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
