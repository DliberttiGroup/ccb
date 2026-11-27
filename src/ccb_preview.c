#include "ccb_common.h"
#include "ccb_preview.h"
#include "ccb_preview_scenario.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include "ccb_json.h"
#include "ccb_preview_internal.h"

// ccb_preview: project orchestration for terminal preview.
// Loads a [preview] project (flow first, then CSS with the scope rule),
// lays out every screen, runs the diagnostics pass, and emits JSON.
// Implements pv_project_load / pv_project_free / pv_project_diagnose /
// pv_project_json from ccb_preview.h. Never touches the SDK/toolchain.

void pv_diag(ccb_diag_list *diags, const char *code,
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
int pv_manifest_has(pv_project *p, const char *target) {
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
const char *pv_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

// True when the CSS file's basename is exactly "common.css".
int pv_is_common_css(const char *path) {
  return strcmp(pv_basename(path), "common.css") == 0;
}

// Screen name for a per-screen CSS file: the basename minus ".css",
// but only when a screen of that name exists; NULL otherwise (the
// caller treats NULL + non-common as an ignored file).
const char *pv_css_screen(pv_project *p, const char *path) {
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
