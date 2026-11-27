// ccb_pipeline_res: aapt2 compile + aapt2 link stages.
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

// ---------- aapt2 compile ----------

ccb_status ccb_step_compile_res(ccb_pipe *p) {
  // Deterministic output zip paths.
  char *compiled_dir = ccb_path_join(p->intermediates, "compiled_res");
  ccb_res_target *targets = NULL;
  size_t tcount = 0;
  size_t tcap = 0;
  if (p->cfg->res != NULL) {
    char *zip = ccb_path_join(compiled_dir, "app.zip");
    if (tcount == tcap) {
      tcap = tcap == 0 ? 8 : tcap * 2;
      targets = ccb_xrealloc(targets, tcap * sizeof(*targets));
    }
    targets[tcount].res_dir = p->cfg->res;
    targets[tcount].zip_path = zip;
    tcount++;
  }
  for (size_t i = 0; i < p->aar_res_dirs_count; i++) {
    char name[32];
    snprintf(name, sizeof(name), "aar-%zu.zip", i);
    char *zip = ccb_path_join(compiled_dir, name);
    if (tcount == tcap) {
      tcap = tcap == 0 ? 8 : tcap * 2;
      targets = ccb_xrealloc(targets, tcap * sizeof(*targets));
    }
    targets[tcount].res_dir = p->aar_res_dirs[i];
    targets[tcount].zip_path = zip;
    tcount++;
  }
  for (size_t i = 0; i < tcount; i++) {
    ccb_push_str(&p->aar_res_zips, &p->aar_res_zips_count, &p->aar_res_zips_cap,
      ccb_xstrdup(targets[i].zip_path));
  }
  // Fingerprint over every resource file.
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  ccb_fp_add(&b, "build_tools", p->sdk->build_tools_version);
  ccb_fp_add(&b, "aapt2", p->sdk->aapt2 == NULL ? "" : p->sdk->aapt2);
  int failed = 0;
  for (size_t i = 0; i < tcount && !failed; i++) {
    char **files = NULL;
    size_t nfiles = 0;
    if (ccb_collect_files(targets[i].res_dir, NULL, &files, &nfiles) != CCB_OK) {
      failed = 1;
      break;
    }
    for (size_t j = 0; j < nfiles; j++) {
      if (ccb_fp_add_file(p, &b, files[j]) != 0) {
        failed = 1;
        break;
      }
    }
    ccb_free_str_array(files, nfiles);
  }
  char *fp = NULL;
  const char *old = NULL;
  if (!failed) {
    fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
    old = ccb_cache_step_fp(p->cache, "compile-res");
  }
  ccb_fp_free(&b);
  if (failed) {
    ccb_pipe_diag(p, "CCB-E3002", "aapt2-compile", "cannot read resource files");
    goto compile_fail;
  }
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0) {
    int all_exist = 1;
    for (size_t i = 0; i < tcount; i++) {
      if (!ccb_path_exists(targets[i].zip_path)) {
        all_exist = 0;
        break;
      }
    }
    if (all_exist) {
      free(fp);
      p->steps_skipped++;
      p->compile_will_run = 0;
      if (p->opts->explain) {
        printf("compile-res: skip (resource inputs unchanged)\n");
      }
      ccb_log_cache(&p->log, "compile-res", 1, "resource inputs unchanged");
      goto compile_done;
    }
  }
  if (p->opts->explain) {
    printf("compile-res: run (resource inputs changed)\n");
    p->compile_will_run = 1;
    free(fp);
    goto compile_done;
  }
  if (!p->opts->dry_run) {
    ccb_remove_recursive(compiled_dir);
    if (ccb_mkdir_p(compiled_dir) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "aapt2-compile", "cannot create compiled resource directory");
      free(fp);
      goto compile_fail;
    }
  }
  for (size_t i = 0; i < tcount; i++) {
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(targets[i].res_dir, NULL, &files, &nfiles);
    if (nfiles == 0) {
      ccb_free_str_array(files, nfiles);
      continue;
    }
    ccb_free_str_array(files, nfiles);
    CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, p->sdk->aapt2);
    ccb_argv_push(&av, "compile");
    ccb_argv_push(&av, "--dir");
    ccb_argv_push(&av, targets[i].res_dir);
    ccb_argv_push(&av, "-o");
    ccb_argv_push(&av, targets[i].zip_path);
    ccb_status rc = ccb_run_tool(p, "aapt2-compile", "CCB-E3002", av.v, NULL,
      "check the resource files for errors (bad XML, duplicate names)");
    free(av.v);
    if (rc != CCB_OK) {
      free(fp);
      goto compile_fail;
    }
  }
  ccb_cache_set_step(p->cache, "compile-res", fp);
  free(fp);
  p->cache_dirty = 1;
  p->compile_will_run = 1;
  p->steps_run++;
compile_done:
  for (size_t i = 0; i < tcount; i++) {
    free(targets[i].zip_path);
  }
  free(targets);
  free(compiled_dir);
  return CCB_OK;
compile_fail:
  for (size_t i = 0; i < tcount; i++) {
    free(targets[i].zip_path);
  }
  free(targets);
  free(compiled_dir);
  return CCB_ERR_BUILD;
}

// ---------- aapt2 link ----------

// Cross-module resource namespacing: a resource defined by more than
// one source is won by the earliest source (the app, then libraries in
// merge order). Warn once per collision, naming the winner and the
// shadowed sources. Runs only when the link runs, so a cached link
// stays quiet.
static void ccb_link_warn_collisions(ccb_pipe *p) {
  size_t nsrc = 1 + p->aar_res_dirs_count;
  char **names = ccb_xcalloc(nsrc, sizeof(*names));
  ccb_res_entry **entries = ccb_xcalloc(nsrc, sizeof(*entries));
  size_t *counts = ccb_xcalloc(nsrc, sizeof(*counts));
  names[0] = "app";
  ccb_resns_scan_dir(p->cfg->res, &entries[0], &counts[0]);
  for (size_t i = 0; i < p->aar_res_dirs_count; i++) {
    names[1 + i] = p->aar_res_names[i];
    ccb_resns_scan_dir(p->aar_res_dirs[i], &entries[1 + i],
      &counts[1 + i]);
  }
  ccb_res_collision *colls = NULL;
  size_t ncolls = 0;
  ccb_resns_collisions(names, entries, counts, nsrc, &colls, &ncolls);
  for (size_t i = 0; i < ncolls; i++) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "resource '");
    ccb_str_append(&msg, colls[i].type);
    ccb_str_append_c(&msg, '/');
    ccb_str_append(&msg, colls[i].name);
    ccb_str_append(&msg, "' defined by ");
    ccb_str_append(&msg, colls[i].winner);
    ccb_str_append(&msg, " and ");
    for (size_t k = 0; k < colls[i].nlosers; k++) {
      if (k > 0) {
        ccb_str_append(&msg, ", ");
      }
      ccb_str_append(&msg, colls[i].losers[k]);
    }
    ccb_str_append(&msg, "; ");
    ccb_str_append(&msg, colls[i].winner);
    ccb_str_append(&msg, " wins");
    ccb_log_warning(&p->log, "%s", msg.buf);
    ccb_str_free(&msg);
  }
  ccb_resns_collisions_free(colls, ncolls);
  for (size_t i = 0; i < nsrc; i++) {
    ccb_resns_entries_free(entries[i], counts[i]);
  }
  free(entries);
  free(counts);
  free(names);
}

// True when aapt2 link's second output is intact: gen_dir holds
// at least one non-empty generated .java file (R.java). link_out
// alone is not enough: a failed link can leave gen empty while
// link_out still exists, and the skip would then starve kotlinc
// of R symbols.
static int ccb_link_gen_valid(ccb_pipe *p) {
  char **files = NULL;
  size_t n = 0;
  if (ccb_collect_files(p->gen_dir, ".java", &files, &n) != CCB_OK) {
    return 0;
  }
  int ok = n > 0;
  for (size_t i = 0; i < n && ok; i++) {
    struct stat st;
    if (stat(files[i], &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
      ok = 0;
    }
  }
  ccb_free_str_array(files, n);
  return ok;
}

ccb_status ccb_step_link(ccb_pipe *p) {
  // AAB builds link with --proto-format into a separate intermediate
  // (linked-aab.ap_) under its own cache key, so APK and AAB builds
  // never poison each other's cached link output. R.java output is
  // identical in both formats and stays shared.
  int is_aab = p->opts->aab;
  const char *link_key = is_aab ? "link-aab" : "link";
  const char *link_out = is_aab ? p->linked_aab : p->linked_apk;
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  ccb_fp_add(&b, "build_tools", p->sdk->build_tools_version);
  ccb_fp_add(&b, "aapt2", p->sdk->aapt2 == NULL ? "" : p->sdk->aapt2);
  ccb_fp_add(&b, "package", p->cfg->package);
  // Overlay order: libraries link as -R overlays with the app last,
  // so the app wins conflicts (see the argv below).
  ccb_fp_add(&b, "overlay", "1");
  ccb_fp_add(&b, "version", p->cfg->version);
  ccb_fp_addl(&b, "version_code", p->cfg->version_code);
  ccb_fp_addl(&b, "min_sdk", p->cfg->min_sdk);
  ccb_fp_addl(&b, "target_sdk", p->cfg->target_sdk);
  ccb_fp_addl(&b, "compile_sdk", p->cfg->compile_sdk);
  ccb_fp_add(&b, "merge-manifest", p->merge_fp == NULL ? "" : p->merge_fp);
  const char *compile_fp = ccb_cache_step_fp(p->cache, "compile-res");
  ccb_fp_add(&b, "compile-res", compile_fp == NULL ? "" : compile_fp);
  for (size_t i = 0; i < p->aar_res_zips_count; i++) {
    ccb_fp_add_file(p, &b, p->aar_res_zips[i]);
  }
  int failed = 0;
  if (ccb_fp_add_file(p, &b, p->cfg->manifest) != 0) {
    failed = 1;
  }
  if (ccb_fp_add_file(p, &b, p->sdk->android_jar) != 0) {
    failed = 1;
  }
  for (size_t i = 0; i < p->aar_packages_count; i++) {
    ccb_fp_add(&b, "extra-package", p->aar_packages[i]);
  }
  char *fp = NULL;
  const char *old = NULL;
  if (!failed) {
    fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
    old = ccb_cache_step_fp(p->cache, link_key);
  }
  ccb_fp_free(&b);
  if (failed) {
    ccb_pipe_diag(p, "CCB-E3003", "aapt2-link", "cannot read link inputs");
    return CCB_ERR_BUILD;
  }
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_path_exists(link_out) &&
      ccb_link_gen_valid(p) && !(p->opts->explain && p->compile_will_run)) {
    free(fp);
    p->steps_skipped++;
    if (p->opts->explain) {
      printf("link: skip (link inputs unchanged)\n");
    }
    ccb_log_cache(&p->log, "link", 1, "link inputs unchanged");
    p->link_will_run = 0;
    return CCB_OK;
  }
  if (p->opts->explain) {
    if (p->compile_will_run) {
      printf("link: run (compiled resources will change)\n");
    } else if (old != NULL && strcmp(old, fp) == 0) {
      printf("link: run (generated R.java missing or invalid)\n");
    } else {
      printf("link: run (link inputs changed)\n");
    }
    free(fp);
    p->link_will_run = 1;
    return CCB_OK;
  }
  p->link_will_run = 1;
  ccb_link_warn_collisions(p);
  if (!p->opts->dry_run) {
    ccb_remove_recursive(p->gen_dir);
    if (ccb_mkdir_p(p->gen_dir) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "aapt2-link", "cannot create R.java output directory");
      free(fp);
      return CCB_ERR_BUILD;
    }
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->aapt2);
  ccb_argv_push(&av, "link");
  if (is_aab) {
    ccb_argv_push(&av, "--proto-format");
  }
  ccb_argv_push(&av, "-o");
  ccb_argv_push(&av, link_out);
  ccb_argv_push(&av, "--manifest");
  ccb_argv_push(&av, p->merged_manifest != NULL ? p->merged_manifest : p->cfg->manifest);
  ccb_argv_push(&av, "-I");
  ccb_argv_push(&av, p->sdk->android_jar);
  ccb_argv_push(&av, "--java");
  ccb_argv_push(&av, p->gen_dir);
  ccb_argv_push(&av, "--auto-add-overlay");
  ccb_argv_push(&av, "--min-sdk-version");
  ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
  ccb_argv_push(&av, "--target-sdk-version");
  ccb_argv_pushf(&av, "%ld", p->cfg->target_sdk);
  ccb_argv_push(&av, "--version-code");
  ccb_argv_pushf(&av, "%ld", p->cfg->version_code);
  ccb_argv_push(&av, "--version-name");
  ccb_argv_push(&av, p->cfg->version);
  for (size_t i = 0; i < p->aar_packages_count; i++) {
    ccb_argv_push(&av, "--extra-packages");
    ccb_argv_push(&av, p->aar_packages[i]);
  }
  // Overlay semantics (-R): the last conflicting resource wins. The
  // app zip goes last so the app wins; library zips go in reverse
  // merge order so the first library wins among libraries. A plain
  // positional arg would make a same-named app/library resource a
  // hard link error instead.
  size_t app_idx = p->cfg->res != NULL ? 0 : (size_t)-1;
  for (size_t i = p->aar_res_zips_count; i-- > 0;) {
    if (i == app_idx) {
      continue;
    }
    ccb_argv_push(&av, "-R");
    ccb_argv_push(&av, p->aar_res_zips[i]);
  }
  if (app_idx != (size_t)-1) {
    ccb_argv_push(&av, "-R");
    ccb_argv_push(&av, p->aar_res_zips[app_idx]);
  }
  ccb_status rc = ccb_run_tool(p, "aapt2-link", "CCB-E3003", av.v, NULL,
    "check the manifest and resource configuration");
  free(av.v);
  if (rc != CCB_OK) {
    free(fp);
    return CCB_ERR_BUILD;
  }
  ccb_cache_set_step(p->cache, link_key, fp);
  free(fp);
  p->cache_dirty = 1;
  p->steps_run++;
  return CCB_OK;
}
