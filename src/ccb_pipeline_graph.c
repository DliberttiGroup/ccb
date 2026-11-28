// ccb_pipeline_graph implementation. See ccb_pipeline_graph.h.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_config.h"
#include "ccb_diag.h"
#include "ccb_graph.h"
#include "ccb_pipeline_graph.h"
#include "ccb_pipeline_internal.h"

// ---------- thin wrappers over the existing step functions ----------

// Run one step, marking the task skipped when the step's cache hit
// bumped the pipe's skip counter.
static ccb_status ccb_run_step(void *ctx, ccb_task *t,
    ccb_status (*fn)(ccb_pipe *)) {
  ccb_pipe *p = ctx;
  int before = p->steps_skipped;
  ccb_status rc = fn(p);
  if (rc == CCB_OK && p->steps_skipped > before) {
    t->skipped = 1;
  }
  return rc;
}

// deps takes an extra out-param the driver never reads; normalize it.
static ccb_status ccb_step_deps_only(ccb_pipe *p) {
  int ran = 0;
  return ccb_step_deps(p, &ran);
}

static ccb_status ccb_task_deps(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_deps_only);
}

static ccb_status ccb_task_libraries(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_libraries);
}

static ccb_status ccb_task_modules(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_modules);
}

static ccb_status ccb_task_merge_manifest(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_merge_manifest);
}

static ccb_status ccb_task_compile_res(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_compile_res);
}

static ccb_status ccb_task_link(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_link);
}

static ccb_status ccb_task_code(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_code);
}

static ccb_status ccb_task_dex(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_dex);
}

static ccb_status ccb_task_dexcheck(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_dexcheck);
}

static ccb_status ccb_task_native(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_native);
}

static ccb_status ccb_task_package(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_package);
}

static ccb_status ccb_task_package_aab(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_package_aab);
}

static ccb_status ccb_task_align(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_align);
}

static ccb_status ccb_task_sign(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_sign);
}

static ccb_status ccb_task_verify(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_verify);
}

static ccb_status ccb_task_library(void *ctx, ccb_task *t) {
  return ccb_run_step(ctx, t, ccb_step_library);
}

// ---------- builder ----------

typedef struct {
  ccb_graph *g;
  ccb_diag_list *diags;
  const char *prev; // previous task ID in the linear chain
  int failed;
} ccb_graph_builder;

// Add a task chained after the previous one, consuming the
// project model. Returns NULL (and marks the builder failed) when
// the task cannot be added.
static ccb_task *ccb_gb_add(ccb_graph_builder *b, const char *id,
    const char *type, ccb_task_cache cache, ccb_task_run_fn run) {
  ccb_task *t = ccb_graph_add_task(b->g, id, type, cache, run,
    b->diags);
  if (t == NULL) {
    b->failed = 1;
    return NULL;
  }
  ccb_task_add_input(t, "project-model",
    CCB_TASK_INPUT_GENERATED);
  if (b->prev != NULL) {
    ccb_task_add_dep(t, b->prev);
  }
  b->prev = t->id;
  return t;
}

static void ccb_gb_source(ccb_task *t, const char *path) {
  if (t == NULL || path == NULL) {
    return;
  }
  // Config lists may repeat a path (the same AAR twice, a java_extra
  // dir equal to the java dir); the graph engine rejects duplicate
  // declarations, so the builder declares each source input once.
  for (size_t i = 0; i < t->inputs_count; i++) {
    if (strcmp(t->inputs[i].name, path) == 0) {
      return;
    }
  }
  ccb_task_add_input(t, path, CCB_TASK_INPUT_SOURCE);
}

static void ccb_gb_gen(ccb_task *t, const char *name) {
  if (t != NULL) {
    ccb_task_add_input(t, name, CCB_TASK_INPUT_GENERATED);
  }
}

static void ccb_gb_out(ccb_task *t, const char *name) {
  if (t != NULL) {
    ccb_task_add_output(t, name);
  }
}

static ccb_task *ccb_gb_deps(ccb_graph_builder *b,
    const ccb_config *cfg) {
  ccb_task *t = ccb_gb_add(b, "deps", "aar",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_deps);
  for (size_t i = 0; i < cfg->aars_count; i++) {
    ccb_gb_source(t, cfg->aars[i]);
  }
  ccb_gb_out(t, "aar-extracted");
  return t;
}

static void ccb_gb_app(ccb_graph_builder *b, const ccb_config *cfg,
    int aab) {
  ccb_gb_deps(b, cfg);
  ccb_task *t = ccb_gb_add(b, "libraries", "library",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_libraries);
  for (size_t i = 0; i < cfg->libraries_count; i++) {
    ccb_gb_source(t, cfg->libraries[i].dir);
  }
  ccb_gb_out(t, "library-aars");
  t = ccb_gb_add(b, "modules", "module",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_modules);
  for (size_t i = 0; i < cfg->modules_count; i++) {
    ccb_gb_source(t, cfg->modules[i].dir);
  }
  ccb_gb_out(t, "module-aars");
  t = ccb_gb_add(b, "merge-manifest", "manifest",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_merge_manifest);
  ccb_gb_source(t, cfg->manifest);
  ccb_gb_gen(t, "aar-extracted");
  ccb_gb_gen(t, "library-aars");
  ccb_gb_gen(t, "module-aars");
  ccb_gb_out(t, "merged-manifest");
  t = ccb_gb_add(b, "compile-res", "aapt2",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_compile_res);
  ccb_gb_source(t, cfg->res);
  ccb_gb_gen(t, "aar-extracted");
  ccb_gb_gen(t, "library-aars");
  ccb_gb_gen(t, "module-aars");
  ccb_gb_out(t, "compiled-res");
  t = ccb_gb_add(b, "link", "aapt2",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_link);
  ccb_gb_gen(t, "compiled-res");
  ccb_gb_gen(t, "merged-manifest");
  ccb_gb_out(t, "linked-apk");
  ccb_gb_out(t, "r-java");
  t = ccb_gb_add(b, "compile-code", "kotlin",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_code);
  ccb_gb_gen(t, "r-java");
  ccb_gb_source(t, cfg->kotlin_dir);
  ccb_gb_source(t, cfg->java_dir);
  for (size_t i = 0; i < cfg->java_extra_dirs_count; i++) {
    ccb_gb_source(t, cfg->java_extra_dirs[i]);
  }
  ccb_gb_gen(t, "aar-extracted");
  ccb_gb_gen(t, "module-aars");
  ccb_gb_out(t, "classes");
  t = ccb_gb_add(b, "dex", "dex",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_dex);
  ccb_gb_gen(t, "classes");
  ccb_gb_gen(t, "aar-extracted");
  ccb_gb_gen(t, "module-aars");
  ccb_gb_out(t, "dex");
  t = ccb_gb_add(b, "dexcheck", "dexcheck",
    CCB_TASK_CACHE_ALWAYS_RUN, ccb_task_dexcheck);
  ccb_gb_gen(t, "dex");
  t = ccb_gb_add(b, "native", "native",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_native);
  for (size_t i = 0; i < cfg->ndk_sources_count; i++) {
    ccb_gb_source(t, cfg->ndk_sources[i]);
  }
  ccb_gb_source(t, cfg->cmake_lists);
  ccb_gb_out(t, "native-libs");
  if (aab) {
    t = ccb_gb_add(b, "package-aab", "package",
      CCB_TASK_CACHE_FINGERPRINT, ccb_task_package_aab);
  } else {
    t = ccb_gb_add(b, "package", "package",
      CCB_TASK_CACHE_FINGERPRINT, ccb_task_package);
  }
  ccb_gb_gen(t, "linked-apk");
  ccb_gb_gen(t, "dex");
  ccb_gb_gen(t, "native-libs");
  ccb_gb_source(t, cfg->assets);
  ccb_gb_out(t, "unsigned-artifact");
  if (!aab) {
    t = ccb_gb_add(b, "align", "zipalign",
      CCB_TASK_CACHE_FINGERPRINT, ccb_task_align);
    ccb_gb_gen(t, "unsigned-artifact");
    ccb_gb_out(t, "aligned-artifact");
  }
  t = ccb_gb_add(b, "sign", "apksigner",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_sign);
  ccb_gb_gen(t, aab ? "unsigned-artifact" : "aligned-artifact");
  ccb_gb_out(t, "signed-artifact");
  t = ccb_gb_add(b, "verify", "verify",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_verify);
  ccb_gb_gen(t, "signed-artifact");
}

static void ccb_gb_library(ccb_graph_builder *b,
    const ccb_config *cfg) {
  ccb_gb_deps(b, cfg);
  ccb_task *t = ccb_gb_add(b, "library", "library",
    CCB_TASK_CACHE_FINGERPRINT, ccb_task_library);
  ccb_gb_gen(t, "aar-extracted");
  ccb_gb_out(t, "library-aar");
}

ccb_graph *ccb_pipeline_graph_build(const ccb_config *cfg, int aab,
    ccb_diag_list *diags) {
  ccb_graph *g = ccb_graph_create();
  ccb_graph_builder b;
  memset(&b, 0, sizeof(b));
  b.g = g;
  b.diags = diags;
  ccb_task *config = ccb_graph_add_task(g, "config", "config",
    CCB_TASK_CACHE_FINGERPRINT, NULL, diags);
  if (config == NULL) {
    ccb_graph_free(g);
    return NULL;
  }
  ccb_task_add_input(config, "ccb.toml",
    CCB_TASK_INPUT_SOURCE);
  ccb_task_add_output(config, "project-model");
  b.prev = "config";
  if (cfg->is_library) {
    ccb_gb_library(&b, cfg);
  } else {
    ccb_gb_app(&b, cfg, aab);
  }
  if (b.failed) {
    ccb_graph_free(g);
    return NULL;
  }
  if (ccb_graph_validate(g, diags) != CCB_OK) {
    ccb_graph_free(g);
    return NULL;
  }
  return g;
}

void ccb_pipeline_graph_config_done(ccb_graph *g, long duration_ms) {
  ccb_task *t = ccb_graph_find(g, "config");
  if (t == NULL) {
    return;
  }
  t->state = CCB_TASK_DONE;
  t->duration_ms = duration_ms;
}
