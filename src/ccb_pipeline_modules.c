// ccb_pipeline_modules: library-module registry + path libraries.
// Split from ccb_pipeline.c.

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
#include "ccb_pipeline_internal.h"

// ---------- library modules ----------

// A library module is built into an intermediate AAR and then consumed
// through the existing AAR machinery: its manifest merges via the
// merge step, its res compiles at link, its classes dex, its assets
// package. The module step runs after deps (so declared AAR classes
// are on the library classpath) and before merge-manifest (so module
// manifests merge). Modules build in config topological order
// (dependencies first); each module registers its outputs as it
// finishes, so a dependent module's classpath already contains its
// dependencies' classes.

// Find a module by name (config order is topological).
ccb_module *ccb_pipe_module(ccb_pipe *p, const char *name) {
  for (size_t i = 0; i < p->cfg->modules_count; i++) {
    if (strcmp(p->cfg->modules[i].name, name) == 0) {
      return &p->cfg->modules[i];
    }
  }
  return NULL;
}

// Rewrite generated R.java files so fields are non-final: a library's
// R fields must resolve at runtime against the app's R class, because
// the app link assigns fresh resource IDs and kotlinc would otherwise
// inline the library-link-time constants. Only field declarations
// match ("public static final int " / "public static final int[]");
// class declarations ("public static final class") are untouched.
static void ccb_definalize_text(char *text) {
  static const char needle[] = "public static final int";
  size_t nlen = strlen(needle);
  char *r = text;
  char *w = text;
  while (*r != '\0') {
    if (strncmp(r, needle, nlen) == 0 &&
        (r[nlen] == ' ' || (r[nlen] == '[' && r[nlen + 1] == ']'))) {
      memcpy(w, "public static int", 17);
      w += 17;
      r += nlen;
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
}

ccb_status ccb_mod_definalize_r(const char *gen_dir) {
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(gen_dir, ".java", &files, &nfiles) != CCB_OK) {
    return CCB_ERR_BUILD;
  }
  for (size_t i = 0; i < nfiles; i++) {
    char *text = NULL;
    size_t len = 0;
    if (ccb_read_file(files[i], &text, &len) != CCB_OK) {
      ccb_free_str_array(files, nfiles);
      return CCB_ERR_BUILD;
    }
    ccb_definalize_text(text);
    if (ccb_write_file(files[i], text, strlen(text)) != CCB_OK) {
      free(text);
      ccb_free_str_array(files, nfiles);
      return CCB_ERR_BUILD;
    }
    free(text);
  }
  ccb_free_str_array(files, nfiles);
  return CCB_OK;
}

// Hash every collected file under dir into the fingerprint. Returns 0
// on success, -1 on read failure. A missing dir contributes nothing.
int ccb_mod_fp_files(ccb_pipe *p, ccb_fp_builder *b, const char *dir,
    const char *ext) {
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(dir, ext, &files, &nfiles) != CCB_OK) {
    return -1;
  }
  int bad = 0;
  for (size_t i = 0; i < nfiles; i++) {
    if (ccb_fp_add_file(p, b, files[i]) != 0) {
      bad = 1;
      break;
    }
  }
  ccb_free_str_array(files, nfiles);
  return bad ? -1 : 0;
}

// Count collected files under dir (0 when the dir is missing).
size_t ccb_mod_file_count(const char *dir, const char *ext) {
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(dir, ext, &files, &nfiles) != CCB_OK) {
    return 0;
  }
  ccb_free_str_array(files, nfiles);
  return nfiles;
}

// Add every file under dir to the zip writer as <prefix>/<relpath>,
// skipping files for which skip returns true (NULL skips nothing).
// Files come sorted from ccb_collect_files, so the archive is
// deterministic. An empty prefix adds bare relative paths.
ccb_status ccb_mod_zip_tree_filtered(ccb_zip_writer *w, const char *dir,
    const char *prefix, int (*skip)(const char *path)) {
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(dir, NULL, &files, &nfiles) != CCB_OK) {
    return CCB_ERR_BUILD;
  }
  ccb_status rc = CCB_OK;
  size_t plen = strlen(dir);
  for (size_t i = 0; i < nfiles; i++) {
    if (skip != NULL && skip(files[i])) {
      continue;
    }
    const char *rel = files[i] + plen + 1;
    char *arc = prefix[0] == '\0' ? ccb_xstrdup(rel) : ccb_path_join(prefix, rel);
    if (ccb_zip_writer_add_file(w, arc, files[i]) != CCB_OK) {
      free(arc);
      rc = CCB_ERR_BUILD;
      break;
    }
    free(arc);
  }
  ccb_free_str_array(files, nfiles);
  return rc;
}

ccb_status ccb_mod_zip_tree(ccb_zip_writer *w, const char *dir,
    const char *prefix) {
  return ccb_mod_zip_tree_filtered(w, dir, prefix, NULL);
}

// Basename of a .class file: R classes (R.class, R$*.class) must never
// reach the dex input, because the app link generates the real R class
// for the module package and d8 rejects the duplicate.
int ccb_mod_is_r_class(const char *path) {
  const char *base = strrchr(path, '/');
  base = base == NULL ? path : base + 1;
  if (strcmp(base, "R.class") == 0) {
    return 1;
  }
  return strncmp(base, "R$", 2) == 0;
}

// Fill a task-owned artifact from a module build's context. No
// pipe state is touched: the artifact is the task's own output, and
// the driver merges it later (ccb_mod_register_artifact).
// classes_jar is NULL for a resources-only module (no classes to
// contribute); res/assets are NULL when the module has none.
void ccb_mod_artifact_collect(ccb_mod_artifact *a, ccb_module *m,
    const ccb_mod_ctx *ctx, const char *kind) {
  memset(a, 0, sizeof(*a));
  a->aar_path = ccb_xstrdup(ctx->aar_path);
  a->classes_jar = ctx->has_sources ? ccb_xstrdup(ctx->classes_jar) : NULL;
  a->manifest_src = ccb_xstrdup(m->manifest);
  ccb_str disp;
  ccb_str_init(&disp);
  ccb_str_append(&disp, kind);
  ccb_str_append_c(&disp, ' ');
  ccb_str_append(&disp, m->name);
  a->display_name = ccb_str_steal(&disp);
  a->res_dir = m->res != NULL ? ccb_xstrdup(m->res) : NULL;
  a->assets_dir = m->assets != NULL ? ccb_xstrdup(m->assets) : NULL;
  a->package = ccb_xstrdup(m->package);
}

void ccb_mod_artifact_free(ccb_mod_artifact *a) {
  if (a == NULL) {
    return;
  }
  free(a->aar_path);
  free(a->classes_jar);
  free(a->manifest_src);
  free(a->display_name);
  free(a->res_dir);
  free(a->assets_dir);
  free(a->package);
  memset(a, 0, sizeof(*a));
}

// Register a module's outputs in the AAR machinery. Runs in every mode
// (explain, dry-run, real): downstream steps plan from these lists.
// classes.jar is registered only when the module has real sources;
// a resources-only module contributes no classes. kind is "module"
// for [modules.<name>] and "library" for [deps.library.<name>].
// Single-writer: only the driver thread calls this, in topological
// order, after the owning task completed.
void ccb_mod_register_artifact(ccb_pipe *p,
    const ccb_mod_artifact *a) {
  ccb_push_str(&p->merge_aar_paths, &p->merge_aar_paths_count,
    &p->merge_aar_paths_cap, ccb_xstrdup(a->aar_path));
  ccb_push_str(&p->merge_aar_srcs, &p->merge_aar_srcs_count,
    &p->merge_aar_srcs_cap, ccb_xstrdup(a->manifest_src));
  ccb_push_str(&p->merge_aar_names, &p->merge_aar_names_count,
    &p->merge_aar_names_cap, ccb_xstrdup(a->display_name));
  if (a->classes_jar != NULL) {
    ccb_push_str(&p->aar_class_jars, &p->aar_class_jars_count,
      &p->aar_class_jars_cap, ccb_xstrdup(a->classes_jar));
  }
  if (a->res_dir != NULL) {
    ccb_push_str(&p->aar_res_dirs, &p->aar_res_dirs_count,
      &p->aar_res_dirs_cap, ccb_xstrdup(a->res_dir));
    ccb_push_str(&p->aar_res_names, &p->aar_res_names_count,
      &p->aar_res_names_cap, ccb_xstrdup(a->display_name));
  }
  if (a->assets_dir != NULL) {
    ccb_push_str(&p->aar_assets_dirs, &p->aar_assets_count,
      &p->aar_assets_cap, ccb_xstrdup(a->assets_dir));
  }
  ccb_push_str(&p->aar_packages, &p->aar_packages_count,
    &p->aar_packages_cap, ccb_xstrdup(a->package));
}

// ---------- path libraries ----------

// Build each [deps.library.<name>] project into
// intermediates/libraries/<name>/<name>.aar and register it in the
// AAR machinery, in declaration order. A built .aar path needs no
// build: stage 0 already appended it to the declared AARs.
ccb_status ccb_step_libraries(ccb_pipe *p) {
  if (p->cfg->libraries_count == 0) {
    if (p->opts->explain) {
      printf("libraries: skip (no path libraries)\n");
    }
    return CCB_OK;
  }
  for (size_t i = 0; i < p->cfg->libraries_count; i++) {
    ccb_module *m = &p->cfg->libraries[i];
    if (m->is_file) {
      continue;
    }
    ccb_str key;
    ccb_str_init(&key);
    ccb_str_append(&key, "library-");
    ccb_str_append(&key, m->name);
    ccb_status rc = ccb_build_module_at(p, m, p->libraries_dir, NULL,
      key.buf, "library", 1);
    ccb_str_free(&key);
    if (rc != CCB_OK) {
      return rc;
    }
  }
  return CCB_OK;
}

// Output AAR of a library project: build/<name>-<version>.aar.
char *ccb_library_aar_path(const ccb_config *cfg) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, cfg->build_dir);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, cfg->name);
  ccb_str_append_c(&s, '-');
  ccb_str_append(&s, cfg->version);
  ccb_str_append(&s, ".aar");
  return ccb_str_steal(&s);
}

// Build this project as a library: one module synthesized from the
// project config, no merge/link/dex/package. The AAR lands at
// build/<name>-<version>.aar.
ccb_status ccb_step_library(ccb_pipe *p) {
  ccb_config *cfg = p->cfg;
  ccb_module m;
  memset(&m, 0, sizeof(m));
  m.name = cfg->name;
  m.dir = cfg->project_root;
  m.manifest = cfg->manifest;
  m.res = cfg->res;
  m.assets = cfg->assets;
  m.kotlin_dir = cfg->kotlin_dir;
  m.java_dir = cfg->java_dir;
  m.package = cfg->package;
  char *aar_path = ccb_library_aar_path(cfg);
  // aar_path ownership moves into the module context.
  return ccb_build_module_at(p, &m, p->libraries_dir, aar_path,
    "library", "library", 0);
}
