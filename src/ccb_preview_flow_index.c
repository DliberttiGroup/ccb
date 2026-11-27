// ccb_preview_flow_index: Chunk B whole-project function index.
// Split from ccb_preview_flow.c.

#include "ccb_common.h"
#include "ccb_preview.h"
#include <ctype.h>
#include "ccb_preview_flow_internal.h"

// ---------- Chunk B: whole-project function index ----------
//
// The index maps every top-level fun name to the flow file(s)
// holding it, over borrowed (label, text) pairs. It is built once
// per project load, before any file is parsed; the texts must
// outlive every parse that uses the index. Lookup counts DISTINCT
// files, not definitions: two definitions in one file behave like
// the same-file first match, while one definition in each of two
// files is ambiguous.

pv_fun_index *pv_fun_index_build(ccb_arena *arena, char **labels,
    char **texts, size_t nfiles) {
  pv_fun_index *index = NULL;
  size_t f = 0;
  if (arena == NULL || nfiles == 0) {
    return NULL;
  }
  index = ccb_xmalloc(sizeof(*index));
  memset(index, 0, sizeof(*index));
  index->arena = arena;
  index->labels = labels;
  index->texts = texts;
  index->nfiles = nfiles;
  for (f = 0; f < nfiles; f++) {
    pv_parser q;
    size_t g = 0;
    if (texts[f] == NULL) {
      continue;
    }
    memset(&q, 0, sizeof(q));
    q.arena = arena;
    q.text = texts[f];
    q.text_len = strlen(texts[f]);
    q.file = labels != NULL ? labels[f] : NULL;
    pv_tokenize(&q);
    pv_collect_funs(&q);
    for (g = 0; g < q.nfuns; g++) {
      if (index->nfuns == index->capfuns) {
        size_t ncap = index->capfuns == 0 ? 16 : index->capfuns * 2;
        index->fun_names = ccb_xrealloc(index->fun_names,
          ncap * sizeof(*index->fun_names));
        index->fun_files = ccb_xrealloc(index->fun_files,
          ncap * sizeof(*index->fun_files));
        index->capfuns = ncap;
      }
      index->fun_names[index->nfuns] = q.fun_names[g];
      index->fun_files[index->nfuns] = f;
      index->nfuns++;
    }
    free(q.toks);
    free(q.fun_names);
    free(q.fun_braces);
    free(q.fun_closes);
  }
  return index;
}

void pv_fun_index_free(pv_fun_index *index) {
  if (index == NULL) {
    return;
  }
  free(index->fun_files);
  free(index->fun_names);
  free(index->nav_files);
  free(index->nav_starts);
  free(index);
}

// Chunk C: record one attributed nav call site. The file label
// resolves to an index file by string equality (labels are borrowed
// paths, shared with the parses); an unknown label or a NULL index
// records nothing, never crashes. Duplicate sites are stored once:
// a handler scanned from two buttons attributes the same call twice.
void pv_index_mark_nav(pv_fun_index *index, const char *file,
    size_t start) {
  size_t f = 0;
  size_t i = 0;
  if (index == NULL || file == NULL) {
    return;
  }
  for (f = 0; f < index->nfiles; f++) {
    if (index->labels[f] != NULL && strcmp(index->labels[f], file) == 0) {
      break;
    }
  }
  if (f >= index->nfiles) {
    return;
  }
  for (i = 0; i < index->nnav; i++) {
    if (index->nav_files[i] == f && index->nav_starts[i] == start) {
      return;
    }
  }
  if (index->nnav == index->capnav) {
    size_t ncap = index->capnav == 0 ? 16 : index->capnav * 2;
    index->nav_files = ccb_xrealloc(index->nav_files,
      ncap * sizeof(*index->nav_files));
    index->nav_starts = ccb_xrealloc(index->nav_starts,
      ncap * sizeof(*index->nav_starts));
    index->capnav = ncap;
  }
  index->nav_files[index->nnav] = f;
  index->nav_starts[index->nnav] = start;
  index->nnav++;
}

// True when the token at i starts a nav-ish call: nav/navigateTo,
// startActivity, or NavController navigate(), each followed by '('.
// The same shape the handler scanner consumes; the sweep uses it to
// find every nav call, attributed or not.
static int pv_is_nav_call(pv_token *toks, size_t ntoks, size_t i) {
  pv_token *t = NULL;
  if (i + 1 >= ntoks || toks[i].kind != PV_TK_IDENT) {
    return 0;
  }
  t = &toks[i];
  if (t->text == NULL) {
    return 0;
  }
  if (strcmp(t->text, "nav") != 0 && strcmp(t->text, "navigateTo") != 0 &&
      strcmp(t->text, "startActivity") != 0 &&
      strcmp(t->text, "navigate") != 0) {
    return 0;
  }
  return toks[i + 1].kind == PV_TK_SYM && toks[i + 1].sym == '(';
}

// Chunk C: mark every nav call in a token range of the current
// parser as accounted for, so the sweep stays quiet for it. Used
// when a handler body is deliberately not scanned: an unbound or
// ambiguous handler's navs are already reported by CCB-P1019 or
// CCB-P1024, so repeating them as CCB-P1025 would be noise, not
// news. The navs are reported, just not as edges.
void pv_mark_range_navs(pv_parser *p, size_t start, size_t end) {
  size_t k = 0;
  for (k = start; k < end; k++) {
    if (pv_is_nav_call(p->toks, p->ntoks, k)) {
      pv_index_mark_nav(p->index, p->file, p->toks[k].start);
    }
  }
}

// Same, for a foreign fun body: tokenize the index file's text,
// find the fun, mark its body's nav calls with the foreign label.
void pv_mark_foreign_navs(pv_parser *p, size_t file_idx,
    const char *name) {
  pv_parser q;
  size_t i = 0;
  if (p->index == NULL || file_idx >= p->index->nfiles ||
      p->index->texts[file_idx] == NULL) {
    return;
  }
  memset(&q, 0, sizeof(q));
  q.arena = p->arena;
  q.text = p->index->texts[file_idx];
  q.text_len = strlen(q.text);
  q.file = p->index->labels[file_idx];
  pv_tokenize(&q);
  pv_collect_funs(&q);
  for (i = 0; i < q.nfuns; i++) {
    size_t k = 0;
    if (strcmp(q.fun_names[i], name) != 0) {
      continue;
    }
    for (k = q.fun_braces[i] + 1; k < q.fun_closes[i]; k++) {
      if (pv_is_nav_call(q.toks, q.ntoks, k)) {
        pv_index_mark_nav(p->index, q.file, q.toks[k].start);
      }
    }
    break;
  }
  free(q.toks);
  free(q.fun_names);
  free(q.fun_braces);
  free(q.fun_closes);
}

// Chunk C: warn CCB-P1025 for every nav call in the index files
// that no parse attributed to an edge. The tokenizer skips comments
// and strings, so only real calls are considered; each unattributed
// site warns exactly once, whatever the scan count, because the
// sweep is a single pass over the files and attribution is a set.
// Runs after all files are parsed and before the index is freed;
// True when the nav call at start in file f was recorded as an edge.
static int pv_nav_attributed(pv_fun_index *index, size_t f, size_t start) {
  for (size_t k = 0; k < index->nnav; k++) {
    if (index->nav_files[k] == f && index->nav_starts[k] == start) {
      return 1;
    }
  }
  return 0;
}

// a NULL index or diag list is a no-op, so index-less parses keep
// the exact old behavior.
void pv_sweep_unattributed_navs(pv_fun_index *index,
    ccb_diag_list *diags) {
  size_t f = 0;
  if (index == NULL || diags == NULL) {
    return;
  }
  for (f = 0; f < index->nfiles; f++) {
    pv_parser q;
    size_t i = 0;
    if (index->texts[f] == NULL) {
      continue;
    }
    memset(&q, 0, sizeof(q));
    q.arena = index->arena;
    q.text = index->texts[f];
    q.text_len = strlen(q.text);
    q.file = index->labels[f];
    pv_tokenize(&q);
    for (i = 0; i < q.ntoks; i++) {
      if (!pv_is_nav_call(q.toks, q.ntoks, i)) {
        continue;
      }
      if (!pv_nav_attributed(index, f, q.toks[i].start)) {
        ccb_diag d;
        ccb_diag_init(&d);
        ccb_diag_set_full(&d, "CCB-P1025", "preview",
          "unattributed navigation: this nav call is not recorded "
          "as an edge from any button (nested block, skipped "
          "statement, or helper never scanned from a button); it is "
          "reported here so it is not silently dropped",
          q.file, q.toks[i].line, 1, NULL, NULL, NULL);
        ccb_diag_list_push(diags, &d);
      }
    }
    free(q.toks);
  }
}
