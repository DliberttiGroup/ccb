// ccb_pipeline_module: building one library module (compile + AAR).
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

// ---------- one library module ----------

// work is the module work dir; aar_path NULL means <work>/<name>.aar.
static void ccb_mod_ctx_init_at(ccb_mod_ctx *c, ccb_pipe *p, ccb_module *m,
    char *work, char *aar_path) {
  c->p = p;
  c->m = m;
  c->work = work;
  if (aar_path != NULL) {
    c->aar_path = aar_path;
  } else {
    ccb_str an;
    ccb_str_init(&an);
    ccb_str_append(&an, m->name);
    ccb_str_append(&an, ".aar");
    c->aar_path = ccb_path_join(c->work, an.buf);
    ccb_str_free(&an);
  }
  c->gen_dir = ccb_path_join(c->work, "gen");
  c->classes_dir = ccb_path_join(c->work, "classes");
  c->classes_jar = ccb_path_join(c->work, "classes.jar");
  c->compiled = ccb_path_join(c->work, "compiled.zip");
  c->link_out = ccb_path_join(c->work, "lib.ap_");
  c->rtxt = ccb_path_join(c->work, "R.txt");
  // A module produces a classes.jar only when it has real Kotlin/Java
  // sources. Resources alone generate R.java at link time, but the R
  // classes are stripped (see below), so a resources-only module
  // contributes no classes.
  c->has_sources = ccb_mod_file_count(m->kotlin_dir, ".kt") > 0 ||
    ccb_mod_file_count(m->java_dir, ".java") > 0;
  c->has_res_files = ccb_mod_file_count(m->res, NULL) > 0;
}

static void ccb_mod_ctx_free(ccb_mod_ctx *c) {
  free(c->work);
  free(c->aar_path);
  free(c->gen_dir);
  free(c->classes_dir);
  free(c->classes_jar);
  free(c->compiled);
  free(c->link_out);
  free(c->rtxt);
}

// A module name must not collide with a declared AAR's extraction
// directory (aar/<base>), or the two would overwrite each other.
// Returns 1 on collision (diagnostic recorded), 0 otherwise.
static int ccb_mod_name_clash(ccb_pipe *p, ccb_module *m) {
  for (size_t i = 0; i < p->resolved.aars_count; i++) {
    char *base = ccb_basename(p->resolved.aars[i]);
    char *dot = strrchr(base, '.');
    if (dot != NULL) {
      *dot = '\0';
    }
    int clash = strcmp(base, m->name) == 0;
    free(base);
    if (clash) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "module '");
      ccb_str_append(&msg, m->name);
      ccb_str_append(&msg, "' collides with declared AAR '");
      ccb_str_append(&msg, p->resolved.aars[i]);
      ccb_str_append(&msg, "'");
      ccb_pipe_diag(p, "CCB-E3022", "modules", msg.buf);
      ccb_str_free(&msg);
      return 1;
    }
  }
  return 0;
}

// Hash one upstream module's outputs (AAR + generated R.java) into the
// fingerprint. A missing AAR contributes nothing: in dry-run on a fresh
// tree it does not exist yet, and its absence must not fail planning.
// Returns 0 on success, -1 on read failure.
static int ccb_mod_fp_dep(ccb_pipe *p, ccb_fp_builder *b, ccb_module *d) {
  char *dep_work = ccb_path_join(p->modules_dir, d->name);
  ccb_str dan;
  ccb_str_init(&dan);
  ccb_str_append(&dan, d->name);
  ccb_str_append(&dan, ".aar");
  char *dep_aar = ccb_path_join(dep_work, dan.buf);
  ccb_str_free(&dan);
  int rc = 0;
  if (ccb_path_exists(dep_aar) &&
      ccb_fp_add_file(p, b, dep_aar) != 0) {
    rc = -1;
  }
  if (rc == 0) {
    char *dep_gen = ccb_path_join(dep_work, "gen");
    if (ccb_mod_fp_files(p, b, dep_gen, ".java") != 0) {
      rc = -1;
    }
    free(dep_gen);
  }
  free(dep_aar);
  free(dep_work);
  return rc;
}

// Fingerprint: tool versions, module inputs, declared deps, and
// upstream module outputs (AAR + generated R.java). Upstream and
// declared-AAR classes are hashed only when present: in dry-run on
// a fresh tree they do not exist yet, and their absence must not
// fail planning (a missing jar still fails the build at dex time
// with a clear error). Sets *fp_out (owned, NULL on failure) and
// *old_out (borrowed cache fingerprint, NULL on failure). Returns 0
// on success, -1 when an input cannot be read.
static int ccb_mod_fingerprint(ccb_pipe *p, ccb_module *m, char **fp_out,
    const char **old_out, const char *key) {
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  ccb_fp_add_aapt2_tools(p, &b);
  ccb_fp_add_kotlin_tools(p, &b);
  int failed = 0;
  if (ccb_fp_add_file(p, &b, p->sdk->android_jar) != 0) {
    failed = 1;
  }
  if (!failed && ccb_fp_add_file(p, &b, m->manifest) != 0) {
    failed = 1;
  }
  if (!failed && m->res != NULL && ccb_mod_fp_files(p, &b, m->res, NULL) != 0) {
    failed = 1;
  }
  if (!failed && m->assets != NULL &&
      ccb_mod_fp_files(p, &b, m->assets, NULL) != 0) {
    failed = 1;
  }
  if (!failed && m->kotlin_dir != NULL &&
      ccb_mod_fp_files(p, &b, m->kotlin_dir, ".kt") != 0) {
    failed = 1;
  }
  if (!failed && m->java_dir != NULL &&
      ccb_mod_fp_files(p, &b, m->java_dir, ".java") != 0) {
    failed = 1;
  }
  ccb_fp_add_declared_jars(p, &b, &failed, 0);
  ccb_fp_add_aar_class_jars(p, &b, &failed, 1);
  ccb_fp_add_plugins(p, &b, &failed);
  for (size_t j = 0; j < m->depends_count && !failed; j++) {
    ccb_module *d = ccb_pipe_module(p, m->depends[j]);
    if (ccb_mod_fp_dep(p, &b, d) != 0) {
      failed = 1;
    }
  }
  *fp_out = NULL;
  *old_out = NULL;
  if (!failed) {
    *fp_out = ccb_cache_fingerprint((const char **)b.keys,
      (const char **)b.vals, b.count);
    *old_out = ccb_cache_step_fp(p->cache, key);
  }
  ccb_fp_free(&b);
  return failed ? -1 : 0;
}

// Resources: compile + link to generate R.java for the module's own
// package. Only needed when the module has code that can reference R;
// a resources-only module skips the link entirely.
static ccb_status ccb_mod_compile_resources(ccb_mod_ctx *c) {
  ccb_pipe *p = c->p;
  ccb_module *m = c->m;
  ccb_status rc = ccb_aapt2_compile(p, m->res, c->compiled,
    "check the module resource files for errors");
  if (rc != CCB_OK) {
    return rc;
  }
  if (!p->opts->dry_run && ccb_mkdir_p(c->gen_dir) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "modules",
      "cannot create module R.java directory");
    return CCB_ERR_BUILD;
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_aapt2_link_argv(p, &av, c->link_out, m->manifest, c->gen_dir,
    c->rtxt, 0);
  ccb_argv_push(&av, c->compiled);
  rc = ccb_run_tool(p, "aapt2-link", "CCB-E3003", av.v, NULL,
    "check the module manifest and resources");
  free(av.v);
  if (rc != CCB_OK) {
    return rc;
  }
  if (!p->opts->dry_run && ccb_mod_definalize_r(c->gen_dir) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "modules",
      "cannot rewrite module R.java");
    return CCB_ERR_BUILD;
  }
  return CCB_OK;
}

static void ccb_mod_srcs_free(ccb_mod_srcs *s) {
  ccb_free_str_array(s->kt, s->nkt);
  ccb_free_str_array(s->java, s->njava);
  ccb_free_str_array(s->rjava, s->nrjava);
  ccb_free_str_array(s->up, s->nup);
  memset(s, 0, sizeof(*s));
}

// Append one dependency R.java file to the upstream list, growing it.
static void ccb_mod_push_up_rjava(char ***up, size_t *nup, size_t *cap,
    char *file) {
  if (*nup == *cap) {
    *cap = *cap == 0 ? 8 : *cap * 2;
    *up = ccb_xrealloc(*up, *cap * sizeof(**up));
  }
  (*up)[(*nup)++] = file;
}

// Collect one dependency's generated R.java files: a module may
// reference a dependency's R class; those files were de-finalized
// when the dependency built.
static void ccb_mod_collect_dep_rjava(ccb_pipe *p, ccb_module *d,
    char ***up, size_t *nup, size_t *cap) {
  char *dep_work = ccb_path_join(p->modules_dir, d->name);
  char *dep_gen = ccb_path_join(dep_work, "gen");
  char **dfiles = NULL;
  size_t ndfiles = 0;
  if (ccb_collect_files(dep_gen, ".java", &dfiles, &ndfiles) == CCB_OK) {
    for (size_t k = 0; k < ndfiles; k++) {
      ccb_mod_push_up_rjava(up, nup, cap, dfiles[k]);
    }
    free(dfiles);
  }
  free(dep_gen);
  free(dep_work);
}

static void ccb_mod_collect_srcs(ccb_mod_ctx *c, ccb_mod_srcs *s) {
  ccb_module *m = c->m;
  memset(s, 0, sizeof(*s));
  if (m->kotlin_dir != NULL) {
    ccb_collect_files(m->kotlin_dir, ".kt", &s->kt, &s->nkt);
  }
  if (m->java_dir != NULL) {
    ccb_collect_files(m->java_dir, ".java", &s->java, &s->njava);
  }
  if (ccb_path_is_dir(c->gen_dir)) {
    ccb_collect_files(c->gen_dir, ".java", &s->rjava, &s->nrjava);
  }
  size_t up_cap = 0;
  for (size_t j = 0; j < m->depends_count; j++) {
    ccb_module *d = ccb_pipe_module(c->p, m->depends[j]);
    ccb_mod_collect_dep_rjava(c->p, d, &s->up, &s->nup, &up_cap);
  }
}

// Compile the module's sources with kotlinc, then its Java sources
// with javac. Takes ownership of cp_str and the collected file lists.
static ccb_status ccb_mod_run_kotlinc(ccb_mod_ctx *c, char *cp_str,
    ccb_mod_srcs *s) {
  ccb_pipe *p = c->p;
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  char **env = NULL;
  ccb_status rc = CCB_OK;
  if (!p->opts->dry_run && ccb_mkdir_p(c->classes_dir) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "modules",
      "cannot create module classes directory");
    rc = CCB_ERR_BUILD;
    goto done;
  }
  env = ccb_jvm_env(p, ar);
  // kotlinc: skipped for Java-only modules (same rule as the app
  // compile step): javac below compiles the Java sources alone.
  if (s->nkt > 0 && p->sdk->kotlinc != NULL) {
    rc = ccb_run_kotlinc_sources(p, ar, env, c->classes_dir, cp_str,
      s->kt, s->nkt, s->rjava, s->nrjava, s->up, s->nup, s->java,
      s->njava, "check the module Kotlin/Java sources for errors");
    if (rc != CCB_OK) {
      goto done;
    }
  }
  if (s->njava > 0) {
    rc = ccb_run_javac_sources(p, ar, env, c->classes_dir, cp_str,
      s->java, s->njava, NULL, 0, "check the Java sources for errors");
  }
done:
  ccb_mod_srcs_free(s);
  free(cp_str);
  return rc;
}

static ccb_status ccb_mod_compile_sources(ccb_mod_ctx *c) {
  // Classpath: android.jar + declared jars + declared and
  // already-built module AAR classes (dependencies register as they
  // finish, in topological order). Shared with the app code step.
  char *cp_str = ccb_build_classpath(c->p);
  ccb_mod_srcs s;
  ccb_mod_collect_srcs(c, &s);
  return ccb_mod_run_kotlinc(c, cp_str, &s);
}

// classes.jar: every compiled file except the generated R classes
// (the app link generates the real ones; shipping both would make d8
// reject the duplicate).
static ccb_status ccb_mod_write_classes_jar(ccb_mod_ctx *c) {
  ccb_pipe *p = c->p;
  char **all = NULL;
  size_t nall = 0;
  if (ccb_collect_files(c->classes_dir, NULL, &all, &nall) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "modules", "cannot read module classes");
    return CCB_ERR_BUILD;
  }
  size_t nkept = 0;
  for (size_t i = 0; i < nall; i++) {
    if (!ccb_mod_is_r_class(all[i])) {
      nkept++;
    }
  }
  if (nkept == 0) {
    ccb_free_str_array(all, nall);
    return CCB_OK;
  }
  ccb_zip_writer w;
  if (ccb_zip_writer_init(&w, c->classes_jar) != CCB_OK) {
    ccb_free_str_array(all, nall);
    ccb_pipe_diag(p, "CCB-E3016", "modules",
      "cannot create module classes.jar");
    return CCB_ERR_BUILD;
  }
  int bad = ccb_mod_zip_tree_filtered(&w, c->classes_dir, "",
    ccb_mod_is_r_class) != CCB_OK;
  if (bad || ccb_zip_writer_finish(&w) != CCB_OK) {
    ccb_zip_writer_abort(&w);
    ccb_free_str_array(all, nall);
    ccb_pipe_diag(p, "CCB-E3016", "modules",
      "cannot write module classes.jar");
    return CCB_ERR_BUILD;
  }
  ccb_free_str_array(all, nall);
  return CCB_OK;
}

// Assemble the intermediate AAR: classes.jar, manifest, raw res and
// assets (aapt2 compiles them at app link time, as for declared AARs).
static ccb_status ccb_mod_assemble_aar(ccb_mod_ctx *c) {
  ccb_pipe *p = c->p;
  ccb_module *m = c->m;
  ccb_zip_writer w;
  if (ccb_zip_writer_init(&w, c->aar_path) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "modules", "cannot create module AAR");
    return CCB_ERR_BUILD;
  }
  int bad = 0;
  if (c->has_sources && ccb_path_exists(c->classes_jar)) {
    if (ccb_zip_writer_add_file(&w, "classes.jar", c->classes_jar) != CCB_OK) {
      bad = 1;
    }
  }
  if (!bad) {
    char *mtext = NULL;
    size_t mlen = 0;
    if (ccb_read_file(m->manifest, &mtext, &mlen) != CCB_OK ||
        ccb_zip_writer_add_data(&w, "AndroidManifest.xml", mtext,
          mlen) != CCB_OK) {
      bad = 1;
    }
    free(mtext);
  }
  // R.txt: the module link writes it (see ccb_mod_compile_resources);
  // a module with no resources, or a resources-only module whose link
  // never ran, ships an empty one so every AAR carries the file.
  if (!bad) {
    if (ccb_path_exists(c->rtxt)) {
      if (ccb_zip_writer_add_file(&w, "R.txt", c->rtxt) != CCB_OK) {
        bad = 1;
      }
    } else if (ccb_zip_writer_add_data(&w, "R.txt", "", 0) != CCB_OK) {
      bad = 1;
    }
  }
  if (!bad && m->res != NULL &&
      ccb_mod_zip_tree(&w, m->res, "res") != CCB_OK) {
    bad = 1;
  }
  if (!bad && m->assets != NULL &&
      ccb_mod_zip_tree(&w, m->assets, "assets") != CCB_OK) {
    bad = 1;
  }
  if (bad || ccb_zip_writer_finish(&w) != CCB_OK) {
    ccb_zip_writer_abort(&w);
    ccb_pipe_diag(p, "CCB-E3016", "modules",
      "cannot write module AAR");
    return CCB_ERR_BUILD;
  }
  return CCB_OK;
}

// Run a module whose inputs changed: reset the work dir, compile
// resources and sources, pack classes.jar, assemble the AAR. The
// completed step fingerprint goes into the plan-owned delta, never
// into the shared cache: the caller merges it after the run
// succeeds (a failed run's delta is discarded with the plan).
static ccb_status ccb_mod_run(ccb_mod_ctx *c, const char *fp,
    const char *key, ccb_cache_delta *delta) {
  ccb_pipe *p = c->p;
  if (!p->opts->dry_run) {
    ccb_remove_recursive(c->work);
    if (ccb_mkdir_p(c->work) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "modules",
        "cannot create module work directory");
      return CCB_ERR_BUILD;
    }
  }
  // Resources link whenever the module has them, even without code:
  // the link also writes R.txt for the AAR. A resources-only module
  // never compiles the generated R.java (no sources to reference it).
  if (c->has_res_files &&
      ccb_mod_compile_resources(c) != CCB_OK) {
    return CCB_ERR_BUILD;
  }
  if (c->has_sources) {
    if (ccb_mod_compile_sources(c) != CCB_OK) {
      return CCB_ERR_BUILD;
    }
    if (!p->opts->dry_run && ccb_mod_write_classes_jar(c) != CCB_OK) {
      return CCB_ERR_BUILD;
    }
  }
  if (!p->opts->dry_run && ccb_mod_assemble_aar(c) != CCB_OK) {
    return CCB_ERR_BUILD;
  }
  if (!p->opts->dry_run) {
    ccb_cache_delta_set_step(delta, key, fp);
  }
  p->steps_run++;
  return CCB_OK;
}

// Plan one module build: fingerprint + run/skip decision. The plan
// owns the context paths and the fingerprint; the caller frees it
// either way (also on fingerprint failure, where fp is NULL).
int ccb_mod_plan_init(ccb_pipe *p, ccb_module *m,
    const char *work_root, char *aar_path, const char *key,
    ccb_mod_plan *plan) {
  memset(plan, 0, sizeof(*plan));
  ccb_mod_ctx_init_at(&plan->ctx, p, m,
    ccb_path_join(work_root, m->name), aar_path);
  const char *old = NULL;
  if (ccb_mod_fingerprint(p, m, &plan->fp, &old, key) != 0) {
    return -1;
  }
  plan->run = 1;
  if (!p->opts->dry_run && old != NULL && strcmp(old, plan->fp) == 0 &&
      ccb_path_exists(plan->ctx.aar_path) &&
      (!plan->ctx.has_sources ||
        ccb_path_exists(plan->ctx.classes_jar))) {
    plan->run = 0;
  }
  return 0;
}

void ccb_mod_plan_free(ccb_mod_plan *plan) {
  free(plan->fp);
  ccb_cache_delta_free(&plan->delta);
  ccb_mod_ctx_free(&plan->ctx);
  memset(plan, 0, sizeof(*plan));
}

// Build one module (or path library, or the standalone library
// project): plan, run or skip, register outputs. work_root is the
// parent of the <name> work dir; aar_path NULL means
// <work>/<name>.aar. kind is "module" or "library" (display names,
// explain lines, cache stage). do_register 0 builds the AAR without
// registering it in the app's merge lists (standalone library).
// The run owns its outputs through the plan (work dir + delta);
// registration collects them into a task-owned artifact first and
// merges that artifact into the pipe's shared lists in this one
// place, so no two tasks ever append to the shared lists
// concurrently. The per-module graph task ("module-<name>") calls
// this for exactly its module; the sequential driver still runs
// the tasks in plan order.
ccb_status ccb_build_module_at(ccb_pipe *p, ccb_module *m,
    const char *work_root, char *aar_path, const char *key,
    const char *kind, int do_register) {
  if (do_register && ccb_mod_name_clash(p, m)) {
    return CCB_ERR_BUILD;
  }
  ccb_mod_plan plan;
  if (ccb_mod_plan_init(p, m, work_root, aar_path, key, &plan) != 0) {
    ccb_pipe_diag(p, "CCB-E3016", "modules", "cannot read module inputs");
    ccb_mod_plan_free(&plan);
    return CCB_ERR_BUILD;
  }
  ccb_status rc = CCB_OK;
  if (p->opts->explain) {
    printf("%s %s: %s\n", kind, m->name,
      plan.run ? "run (inputs changed)" : "skip (inputs unchanged)");
  } else if (plan.run) {
    if (ccb_mod_run(&plan.ctx, plan.fp, key, &plan.delta) != CCB_OK) {
      rc = CCB_ERR_BUILD;
    } else if (plan.delta.step_count > 0) {
      // Single-writer merge: the run completed, so its step
      // fingerprint may publish; a failed run never reaches here
      // and its delta is discarded with the plan.
      ccb_cache_delta_merge(p->cache, &plan.delta);
      p->cache_dirty = 1;
    }
  } else {
    p->steps_skipped++;
    ccb_log_cache(&p->log, strcmp(kind, "library") == 0 ? "libraries" : "modules",
      1, strcmp(kind, "library") == 0 ? "library inputs unchanged" : "module inputs unchanged");
  }
  if (rc == CCB_OK && do_register) {
    ccb_mod_artifact artifact;
    ccb_mod_artifact_collect(&artifact, m, &plan.ctx, kind);
    ccb_mod_register_artifact(p, &artifact);
    ccb_mod_artifact_free(&artifact);
  }
  ccb_mod_plan_free(&plan);
  return rc;
}
