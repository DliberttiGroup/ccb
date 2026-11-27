// ccb_preview_internal.h: shared internals for the ccb_preview.c split:
// file-local types, macros, and the helpers called across the split
// files. Not public API: only the split files include this header.
#ifndef CCB_PREVIEW_INTERNAL_H
#define CCB_PREVIEW_INTERNAL_H

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

typedef struct {
  char *name;
  char *value;
} pv_res_entry;

typedef struct {
  pv_res_entry *items;
  size_t count, cap;
} pv_res_table;

typedef void (*pv_tag_fn)(void *ctx, const char *text, size_t tag_start,
  size_t tag_end, const char *local, size_t local_len);

typedef struct {
  char ***methods_io;
  char ***ids_io;
  char ***texts_io;
  size_t *n_io;
} pv_layout_ctx;

typedef struct {
  ccb_arena *arena;
  const char *package;
  char ***keys_io;
  char ***vals_io;
  size_t *n_io;
} pv_graph_ctx;

typedef struct {
  ccb_arena *arena;
  const char *package;
  char ***acts_io;
  size_t *nacts_io;
} pv_manifest_ctx;

void pv_diag(ccb_diag_list *diags, const char *code,
    const char *message, const char *file, long line);
char *pv_normalize_class(const char *package, const char *name);
int pv_names_match(const char *package, const char *a, const char *b);
pv_screen *pv_screen_find(pv_project *p, const char *target);
int pv_manifest_has(pv_project *p, const char *target);
const char *pv_basename(const char *path);
int pv_is_common_css(const char *path);
const char *pv_css_screen(pv_project *p, const char *path);
void pv_res_parse_file(ccb_arena *arena, const char *path,
    int is_color, ccb_diag_list *diags, pv_res_table *table);
void pv_res_resolve_labels(ccb_arena *arena, pv_box *box,
    pv_res_table *strings, ccb_diag_list *diags);
void pv_res_resolve_colors(ccb_arena *arena, pv_project *p,
    pv_res_table *colors, ccb_diag_list *diags);
void pv_pair_add(ccb_arena *arena, char ***keys_io, char ***vals_io,
    size_t *n_io, char *key, char *val, int first_wins);
void pv_xml_scan_tags(const char *text, size_t len, pv_tag_fn fn,
    void *ctx);
void pv_layout_tag(void *ctx_v, const char *text, size_t tag_start,
    size_t tag_end, const char *local, size_t local_len);
void pv_graph_tag(void *ctx_v, const char *text, size_t tag_start,
    size_t tag_end, const char *local, size_t local_len);
void pv_manifest_activities(ccb_arena *arena, const char *manifest_path,
    const char *package, char ***acts_io, size_t *nacts_io);
void pv_xml_inputs(const char *path, char ***files_io,
    size_t *nfiles_io);
char *pv_preview_layout_path(const ccb_config *cfg);
char *pv_preview_nav_path(const ccb_config *cfg);
void pv_screen_edge_add(ccb_arena *arena, pv_screen *screen,
    char *from, char *target, int unknown, char **args, size_t nargs,
    int *args_named, char *branch);
int pv_tree_has_label(pv_box *box, const char *label);
const char *pv_binding_lookup(const ccb_config *cfg,
    const char *key);
void pv_reconcile_derived(pv_project *p, ccb_diag_list *diags);
void pv_preview_scan_xml(const ccb_config *cfg, ccb_arena *arena,
    char ***graph_keys_io, char ***graph_vals_io, size_t *ngraph_io,
    char ***onclick_methods_io, char ***onclick_labels_io,
    size_t *nonclick_io, ccb_diag_list *diags);
void pv_project_assemble(pv_project *p, pv_screen **screens,
    size_t nscreens, pv_pending_edge **pending, size_t npending,
    pv_css_entry *css_entries, size_t ncss,
    const char *resources_dir, const ccb_scenario *scenario,
    ccb_diag_list *diags, char **graph_vals, size_t ngraph,
    char **manifest_activities, size_t nmanifest_activities,
    const char *package);
int pv_preview_applicable(const ccb_config *cfg);
void pv_merge_real_screens(ccb_arena *arena,
    pv_screen ***screens_io, size_t *nscreens_io,
    pv_pending_edge ***pending_io, size_t *npending_io,
    pv_screen **real_screens, size_t nreal,
    pv_pending_edge **real_pending, size_t nreal_pending,
    const char *launcher, const char *package,
    ccb_diag_list *diags);
ccb_status pv_project_load_scenario(const ccb_config *cfg,
    long frame_width, int ascii, int strict, ccb_diag_list *diags,
    const ccb_scenario *scenario, pv_project **out);
void pv_project_free(pv_project *p);
long pv_project_diagnose(pv_project *p);

#endif // CCB_PREVIEW_INTERNAL_H
