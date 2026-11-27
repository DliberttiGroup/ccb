// ccb_pipeline_internal.h: shared internals for the ccb_pipeline.c split:
// file-local types, macros, and the helpers called across the split
// files. Not public API: only the split files include this header.
#ifndef CCB_PIPELINE_INTERNAL_H
#define CCB_PIPELINE_INTERNAL_H

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_proc.h"
#include "ccb_config.h"
#include "ccb_sdk.h"
#include "ccb_cache.h"
#include "ccb_manifest.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_resns.h"
#include "ccb_zip.h"
#include "ccb_bundle.h"
#include "ccb_dexcheck.h"
#include "ccb_daemon.h"
#include "ccb_pipeline.h"
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  const ccb_build_opts *opts;
  ccb_log log;
  ccb_diag_list *diags;
  ccb_config *cfg;
  ccb_sdk *sdk;
  ccb_cache *cache;
  int cache_dirty;
  // Directories.
  char *intermediates;
  char *gen_dir;
  char *classes_dir;
  char *dex_dir;
  char *aar_dir;
  char *out_dir;
  char *unsigned_apk;
  char *aligned_apk;
  char *linked_apk;
  char *linked_aab;
  char *unsigned_aab;
  // AAR-derived inputs.
  char **aar_res_dirs;
  size_t aar_res_dirs_count;
  size_t aar_res_dirs_cap;
  // Parallel to aar_res_dirs: display names for resource-collision
  // warnings ("app" never appears here; declared AARs use their path,
  // modules and path libraries "module <name>" / "library <name>").
  char **aar_res_names;
  size_t aar_res_names_count;
  size_t aar_res_names_cap;
  char **aar_res_zips;
  size_t aar_res_zips_count;
  size_t aar_res_zips_cap;
  char **aar_class_jars;
  size_t aar_class_jars_count;
  size_t aar_class_jars_cap;
  char **aar_packages;
  size_t aar_packages_count;
  size_t aar_packages_cap;
  char **aar_assets_dirs;
  size_t aar_assets_count;
  size_t aar_assets_cap;
  char **aar_jni_dirs;
  size_t aar_jni_count;
  size_t aar_jni_cap;
  // AARs the manifest merge sees, in merge order: declared AARs first
  // (existing order), then built library-module AARs in topological
  // order. Without modules this is exactly cfg->aars.
  char **merge_aar_paths;
  size_t merge_aar_paths_count;
  size_t merge_aar_paths_cap;
  // Parallel to merge_aar_paths: for a built module, the module
  // manifest source (the AAR manifest is a copy of it, so merge can
  // read it even in explain/dry-run mode before the AAR is built);
  // NULL for declared AARs (manifest read from the zip).
  char **merge_aar_srcs;
  size_t merge_aar_srcs_count;
  size_t merge_aar_srcs_cap;
  // Parallel to merge_aar_paths: display names for merge warnings
  // (declared AAR path, or "module <name>").
  char **merge_aar_names;
  size_t merge_aar_names_count;
  size_t merge_aar_names_cap;
  // Library-module work area: intermediates/modules/<name>/.
  char *modules_dir;
  // Path-library work area: intermediates/libraries/<name>/.
  char *libraries_dir;
  // Merged manifest (NULL when no AARs): what aapt2 link sees.
  char *merged_manifest;
  // Fingerprint of the merge inputs (app manifest + AAR manifests).
  char *merge_fp;
  // Step outcomes.
  int compile_will_run;
  int link_will_run;
  int code_will_run;
  int dex_will_run;
  int native_will_run;
  int package_will_run;
  int align_will_run;
  int sign_will_run;
  char *classes_fp;
  int steps_run;
  int steps_skipped;
  char *tool_fingerprint;
} ccb_pipe;

typedef struct {
  char **v;
  size_t n;
  size_t cap;
  ccb_arena *ar;
} ccb_argv;

typedef struct {
  char **keys;
  char **vals;
  size_t count;
  size_t cap;
} ccb_fp_builder;

typedef struct {
  ccb_pipe *p;
  ccb_module *m;
  char *work;
  char *aar_path;
  char *gen_dir;
  char *classes_dir;
  char *classes_jar;
  char *compiled;
  char *link_out;
  char *rtxt; // aapt2 --output-text-symbols file (R.txt source)
  int has_sources;
  int has_res_files;
} ccb_mod_ctx;

typedef struct {
  char **kt;
  size_t nkt;
  char **java;
  size_t njava;
  char **rjava;
  size_t nrjava;
  char **up;
  size_t nup;
} ccb_mod_srcs;

typedef struct {
  char *res_dir;
  char *zip_path;
} ccb_res_target;

typedef struct {
  ccb_pipe *p;
  int is_aab;
  const char *pkg_key;
  const char *link_src;
  const char *pkg_out;
  const char *fmt_label;
  const char *asset_prefix;
  const char *so_prefix;
  char **dex_files;
  size_t ndex;
  char **asset_paths;
  char **asset_names;
  size_t nassets;
  size_t assets_cap;
  char **so_paths;
  char **so_names;
  size_t nso;
  size_t so_cap;
  int so_conflict;
  char *so_conflict_name;
} ccb_pkg_ctx;

void ccb_argv_push(ccb_argv *a, const char *s);
void ccb_argv_pushf(ccb_argv *a, const char *fmt, long num);
void ccb_push_plugin_args(ccb_pipe *p, ccb_argv *av);
void ccb_fp_add(ccb_fp_builder *b, const char *key, const char *val);
void ccb_fp_addl(ccb_fp_builder *b, const char *key, long val);
void ccb_fp_free(ccb_fp_builder *b);
int ccb_fp_add_file(ccb_pipe *p, ccb_fp_builder *b, const char *path);
void ccb_fp_add_plugins(ccb_pipe *p, ccb_fp_builder *b,
    int *failed);
void ccb_pipe_diag(ccb_pipe *p, const char *code, const char *phase, const char *msg);
ccb_status ccb_run_tool(ccb_pipe *p, const char *phase, const char *code, char *const argv[],
    char *const env_extra[], const char *suggestion);
ccb_status ccb_run_kotlinc(ccb_pipe *p, char *const argv[],
    char *const env_extra[], const char *suggestion);
void ccb_push_str(char ***arr, size_t *count, size_t *cap, char *s);
ccb_status ccb_step_deps(ccb_pipe *p, int *ran);
char *ccb_aar_manifest_text(const char *aar_path, int *failed);
ccb_status ccb_step_merge_manifest(ccb_pipe *p);
ccb_module *ccb_pipe_module(ccb_pipe *p, const char *name);
ccb_status ccb_mod_definalize_r(const char *gen_dir);
int ccb_mod_fp_files(ccb_pipe *p, ccb_fp_builder *b, const char *dir,
    const char *ext);
size_t ccb_mod_file_count(const char *dir, const char *ext);
ccb_status ccb_mod_zip_tree(ccb_zip_writer *w, const char *dir,
    const char *prefix);
int ccb_mod_is_r_class(const char *path);
void ccb_mod_register(ccb_pipe *p, ccb_module *m, const char *aar_path,
    const char *classes_jar, const char *kind);
ccb_status ccb_build_module_at(ccb_pipe *p, ccb_module *m,
    const char *work_root, char *aar_path, const char *key,
    const char *kind, int do_register);
ccb_status ccb_step_modules(ccb_pipe *p);
ccb_status ccb_step_libraries(ccb_pipe *p);
char *ccb_library_aar_path(const ccb_config *cfg);
ccb_status ccb_step_library(ccb_pipe *p);
ccb_status ccb_step_compile_res(ccb_pipe *p);
ccb_status ccb_step_link(ccb_pipe *p);
char *ccb_java_home(ccb_pipe *p);
char **ccb_jvm_env(ccb_pipe *p, ccb_arena *ar);
ccb_status ccb_step_code(ccb_pipe *p);
int ccb_dex_has_stdlib(ccb_pipe *p);
char *ccb_dex_stdlib(ccb_pipe *p);
long ccb_d8_heap_mb(uint64_t input_bytes);
uint64_t ccb_dex_input_bytes(ccb_pipe *p, const char *stdlib_input);
char *ccb_stdlib_dex_input(ccb_pipe *p, const char *stdlib);
ccb_status ccb_step_dex(ccb_pipe *p);
ccb_status ccb_step_dexcheck(ccb_pipe *p);
ccb_status ccb_step_native(ccb_pipe *p);
ccb_status ccb_step_cmake(ccb_pipe *p, const char *abi);
ccb_status ccb_step_package(ccb_pipe *p);
ccb_status ccb_step_package_aab(ccb_pipe *p);
ccb_status ccb_step_align(ccb_pipe *p);
ccb_status ccb_step_sign(ccb_pipe *p);
ccb_status ccb_step_verify(ccb_pipe *p);

#endif // CCB_PIPELINE_INTERNAL_H
