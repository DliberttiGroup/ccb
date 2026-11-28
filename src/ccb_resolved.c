// ccb_resolved implementation. See ccb_resolved.h.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_config.h"
#include "ccb_resolved.h"

void ccb_resolved_init(ccb_resolved *r) {
  memset(r, 0, sizeof(*r));
}

static void ccb_resolved_push(char ***arr, size_t *count, size_t *cap,
    char *path) {
  if (*count == *cap) {
    size_t ncap = *cap == 0 ? 8 : *cap * 2;
    *arr = ccb_xrealloc(*arr, ncap * sizeof(**arr));
    *cap = ncap;
  }
  (*arr)[(*count)++] = path;
}

void ccb_resolved_from_config(ccb_resolved *r, const ccb_config *cfg) {
  ccb_resolved_init(r);
  for (size_t i = 0; i < cfg->aars_count; i++) {
    ccb_resolved_push_aar(r, ccb_xstrdup(cfg->aars[i]));
  }
  for (size_t i = 0; i < cfg->jars_count; i++) {
    ccb_resolved_push_jar(r, ccb_xstrdup(cfg->jars[i]));
  }
}

void ccb_resolved_push_aar(ccb_resolved *r, char *path) {
  ccb_resolved_push(&r->aars, &r->aars_count, &r->aars_cap, path);
}

void ccb_resolved_push_jar(ccb_resolved *r, char *path) {
  ccb_resolved_push(&r->jars, &r->jars_count, &r->jars_cap, path);
}

void ccb_resolved_push_plugin_jar(ccb_resolved *r, char *path) {
  ccb_resolved_push(&r->plugin_jars, &r->plugin_jars_count,
    &r->plugin_jars_cap, path);
}

void ccb_resolved_free(ccb_resolved *r) {
  ccb_free_str_array(r->aars, r->aars_count);
  ccb_free_str_array(r->jars, r->jars_count);
  ccb_free_str_array(r->plugin_jars, r->plugin_jars_count);
  memset(r, 0, sizeof(*r));
}
