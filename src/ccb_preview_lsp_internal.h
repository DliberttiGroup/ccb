// ccb_preview_lsp_internal.h: shared internals for the ccb_preview_lsp.c split:
// file-local types, macros, and the helpers called across the split
// files. Not public API: only the split files include this header.
#ifndef CCB_PREVIEW_LSP_INTERNAL_H
#define CCB_PREVIEW_LSP_INTERNAL_H

#include "ccb_preview_lsp.h"
#include "ccb_preview_trace.h"
#include "ccb_preview_watch.h"
#include "ccb_json.h"
#include "ccb_fs.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PV_LSP_FRAME_WIDTH 40

#define PV_LSP_PHONE_DP_WIDTH 390

typedef struct {
  char *path;
  char *text;
} pv_lsp_doc;

typedef struct {
  char *path;
  long long mtime_sec;
  long mtime_nsec;
  long long size;
} pv_lsp_stat;

struct pv_lsp_server {
  char *toml_path; // malloc'd absolute
  char *config_dir; // malloc'd (dirname of toml_path)
  ccb_config *cfg; // owned
  char *scenario_name; // malloc'd or NULL
  pv_trace *trace; // owned or NULL
  // XML inputs, rescanned fresh on every build (small files; the
  // scan's P1019 warnings regenerate in load order each build).
  ccb_arena *xml_arena;
  char **graph_keys;
  char **graph_vals;
  size_t ngraph;
  char **onclick_methods;
  char **onclick_labels;
  size_t nonclick;
  // Staging caches, sorted by path.
  pv_lsp_flow_entry *flow;
  size_t nflow, capflow;
  pv_lsp_css_entry *css;
  size_t ncss, capcss;
  // Open editor documents.
  pv_lsp_doc *docs;
  size_t ndocs, capdocs;
  // Disk watching: the watch context (inotify wakeup) plus this
  // server's own snapshot for path-level diffs.
  pv_watch_ctx *watch_ctx;
  pv_watch_snapshot *watch_snap;
  pv_watch_debounce deb;
  // Extra-file stats (nav/layout/resources), sorted by path.
  pv_lsp_stat *extra;
  size_t nextra, capextra;
  // Served state.
  pv_project *project; // owned, NULL before the first good build
  char *tree_json; // malloc'd pv_project_json output, or NULL
  char *last_published; // malloc'd tree JSON of the last publish
  int shutdown; // shutdown request received
  int editor_dirty; // an editor update needs an immediate rebuild
  int config_dirty; // ccb.toml changed: reload config at rebuild
  int xml_dirty; // nav/layout changed: invalidate the flow cache
  int resources_dirty; // resources changed: rebuild (re-read in build)
  int first_build; // no build has run yet
};

typedef enum {
  PV_LSP_IGNORE,
  PV_LSP_FLOW,
  PV_LSP_CSS,
  PV_LSP_TOML,
  PV_LSP_XML,
  PV_LSP_RESOURCES
} pv_lsp_kind;

typedef struct {
  int fd;
  char *buf;
  size_t len;
  size_t cap;
  size_t pos; // consumed offset
  int eof;
} lsp_reader;

pv_lsp_kind lsp_classify(pv_lsp_server *s, const char *path);
pv_lsp_doc *lsp_doc_find(pv_lsp_server *s, const char *path);
void lsp_doc_set(pv_lsp_server *s, const char *path,
    const char *text);
void lsp_doc_drop(pv_lsp_server *s, const char *path);
void lsp_flow_entry_free(pv_lsp_flow_entry *e);
void lsp_css_entry_free(pv_lsp_css_entry *e);
int lsp_flow_lookup(pv_lsp_server *s, const char *path,
    size_t *idx_io);
int lsp_css_lookup(pv_lsp_server *s, const char *path,
    size_t *idx_io);
pv_lsp_flow_entry *lsp_flow_ensure(pv_lsp_server *s,
    const char *path);
pv_lsp_css_entry *lsp_css_ensure(pv_lsp_server *s,
    const char *path);
void lsp_flow_drop(pv_lsp_server *s, size_t idx);
void lsp_css_drop(pv_lsp_server *s, size_t idx);
pv_screen *lsp_copy_screen(ccb_arena *arena,
    const pv_screen *src);
void lsp_diag_replay(ccb_diag_list *dl,
    const ccb_diag_list *src);
void lsp_str_array_add(char ***arr_io, size_t *n_io,
    size_t *cap_io, const char *path);
int lsp_str_cmp(const void *a, const void *b);
int pv_lsp_rebuild(pv_lsp_server *s);
const char *pv_lsp_tree_json(pv_lsp_server *s);
pv_project *pv_lsp_project(pv_lsp_server *s);
void pv_lsp_did_open(pv_lsp_server *s, const char *path,
    const char *text);
void pv_lsp_did_change(pv_lsp_server *s, const char *path,
    const char *text);
void pv_lsp_did_close(pv_lsp_server *s, const char *path);
int pv_lsp_scan_disk(pv_lsp_server *s);
int lsp_reader_more(lsp_reader *r);
int lsp_reader_next(lsp_reader *r, char **body_io);
void lsp_send_result(int out_fd, const ccb_json_value *id,
    const char *result_json);
void lsp_send_error(int out_fd, const ccb_json_value *id,
    long code, const char *message);
void lsp_maybe_publish(pv_lsp_server *s, int out_fd);
char *lsp_uri_to_path(const char *uri);
void lsp_reroot(pv_lsp_server *s, const char *dir);
char *lsp_init_root(const ccb_json_value *params);
void lsp_json_diagnostics(ccb_str *out, pv_project *p);
void lsp_json_incoming(ccb_str *out, pv_project *p,
    const char *name);

#endif // CCB_PREVIEW_LSP_INTERNAL_H
