// ccb_pipeline_deps: deps / AAR extraction + AAR manifest merge.
// Split from ccb_pipeline.c (stages: deps, merge).

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

// ---------- deps / AAR extraction ----------

void ccb_push_str(char ***arr, size_t *count, size_t *cap, char *s) {
  if (*count == *cap) {
    size_t ncap = *cap == 0 ? 8 : *cap * 2;
    *arr = ccb_xrealloc(*arr, ncap * sizeof(**arr));
    *cap = ncap;
  }
  (*arr)[(*count)++] = s;
}

// ---------- deps / AAR extraction ----------
// Scan AAR entries to populate the derived lists WITHOUT extracting, so
// later steps know the AAR contents even when extraction is cached.
ccb_status ccb_step_deps(ccb_pipe *p, int *ran) {
  *ran = 0;
  // The manifest merge sees declared AARs first; the module step
  // appends built library-module AARs after this.
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    ccb_push_str(&p->merge_aar_paths, &p->merge_aar_paths_count,
      &p->merge_aar_paths_cap, ccb_xstrdup(p->cfg->aars[i]));
    ccb_push_str(&p->merge_aar_srcs, &p->merge_aar_srcs_count,
      &p->merge_aar_srcs_cap, NULL);
    ccb_push_str(&p->merge_aar_names, &p->merge_aar_names_count,
      &p->merge_aar_names_cap, ccb_xstrdup(p->cfg->aars[i]));
  }
  if (p->cfg->aars_count == 0) {
    if (p->opts->explain) {
      printf("deps: skip (no AAR dependencies)\n");
    }
    return CCB_OK;
  }
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    const char *aar_path = p->cfg->aars[i];
    char *base = ccb_basename(aar_path);
    char *dot = strrchr(base, '.');
    if (dot != NULL) {
      *dot = '\0';
    }
    char *dest = ccb_path_join(p->aar_dir, base);
    ccb_zip_archive *a = NULL;
    if (ccb_zip_open(aar_path, &a) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3015", "deps", "cannot open AAR file");
      free(base);
      free(dest);
      return CCB_ERR_BUILD;
    }
    int has_res = 0;
    int has_assets = 0;
    int has_jni = 0;
    int has_classes = 0;
    for (size_t e = 0; e < a->count; e++) {
      const ccb_zip_entry *entry = &a->entries[e];
      if (strcmp(entry->name, "classes.jar") == 0) {
        has_classes = 1;
      } else if (strncmp(entry->name, "res/", 4) == 0) {
        has_res = 1;
      } else if (strncmp(entry->name, "assets/", 7) == 0) {
        has_assets = 1;
      } else if (strncmp(entry->name, "jni/", 4) == 0) {
        has_jni = 1;
      } else if (strcmp(entry->name, "AndroidManifest.xml") == 0) {
        void *data = NULL;
        size_t len = 0;
        if (ccb_zip_read_entry(a, entry, &data, &len) == CCB_OK) {
          char *text = ccb_xmalloc(len + 1);
          memcpy(text, data, len);
          text[len] = '\0';
          char *pkg = ccb_manifest_package(text);
          if (pkg != NULL) {
            ccb_push_str(&p->aar_packages, &p->aar_packages_count, &p->aar_packages_cap, pkg);
          }
          free(text);
          free(data);
        }
      }
    }
    ccb_zip_close(a);
    if (has_classes) {
      ccb_push_str(&p->aar_class_jars, &p->aar_class_jars_count, &p->aar_class_jars_cap,
        ccb_path_join(dest, "classes.jar"));
    }
    if (has_res) {
      ccb_push_str(&p->aar_res_dirs, &p->aar_res_dirs_count, &p->aar_res_dirs_cap,
        ccb_path_join(dest, "res"));
      ccb_push_str(&p->aar_res_names, &p->aar_res_names_count, &p->aar_res_names_cap,
        ccb_xstrdup(aar_path));
    }
    if (has_assets) {
      ccb_push_str(&p->aar_assets_dirs, &p->aar_assets_count, &p->aar_assets_cap,
        ccb_path_join(dest, "assets"));
    }
    if (has_jni) {
      ccb_push_str(&p->aar_jni_dirs, &p->aar_jni_count, &p->aar_jni_cap,
        ccb_path_join(dest, "jni"));
    }
    free(base);
    free(dest);
  }
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    if (ccb_fp_add_file(p, &b, p->cfg->aars[i]) != 0) {
      ccb_fp_free(&b);
      ccb_pipe_diag(p, "CCB-E3015", "deps", "cannot read AAR file");
      return CCB_ERR_BUILD;
    }
  }
  char *fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
  ccb_fp_free(&b);
  const char *old = ccb_cache_step_fp(p->cache, "deps");
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0) {
    free(fp);
    p->steps_skipped++;
    if (p->opts->explain) {
      printf("deps: skip (AAR inputs unchanged)\n");
    }
    ccb_log_cache(&p->log, "deps", 1, "AAR inputs unchanged");
    return CCB_OK;
  }
  if (p->opts->explain) {
    printf("deps: run (AAR inputs changed)\n");
    free(fp);
    return CCB_OK;
  }
  ccb_log_phase_start(&p->log, "deps");
  if (!p->opts->dry_run && ccb_mkdir_p(p->aar_dir) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "deps", "cannot create AAR output directory");
    free(fp);
    return CCB_ERR_BUILD;
  }
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    const char *aar_path = p->cfg->aars[i];
    char *base = ccb_basename(aar_path);
    char *dot = strrchr(base, '.');
    if (dot != NULL) {
      *dot = '\0';
    }
    char *dest = ccb_path_join(p->aar_dir, base);
    ccb_zip_archive *a = NULL;
    if (ccb_zip_open(aar_path, &a) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3015", "deps", "cannot open AAR file");
      free(base);
      free(dest);
      free(fp);
      return CCB_ERR_BUILD;
    }
    for (size_t e = 0; e < a->count; e++) {
      const ccb_zip_entry *entry = &a->entries[e];
      size_t nl = strlen(entry->name);
      if (nl == 0 || entry->name[nl - 1] == '/') {
        continue;
      }
      void *data = NULL;
      size_t len = 0;
      if (ccb_zip_read_entry(a, entry, &data, &len) != CCB_OK) {
        ccb_pipe_diag(p, "CCB-E3015", "deps", "cannot extract AAR entry");
        ccb_zip_close(a);
        free(base);
        free(dest);
        free(fp);
        return CCB_ERR_BUILD;
      }
      char *out_path = ccb_path_join(dest, entry->name);
      char *parent = ccb_dirname(out_path);
      int bad = 0;
      if (!p->opts->dry_run) {
        if (ccb_mkdir_p(parent) != CCB_OK || ccb_write_file(out_path, data, len) != CCB_OK) {
          bad = 1;
        }
      }
      free(parent);
      free(out_path);
      free(data);
      if (bad) {
        ccb_pipe_diag(p, "CCB-E3016", "deps", "cannot write extracted AAR entry");
        ccb_zip_close(a);
        free(base);
        free(dest);
        free(fp);
        return CCB_ERR_BUILD;
      }
    }
    ccb_zip_close(a);
    free(base);
    free(dest);
  }
  ccb_cache_set_step(p->cache, "deps", fp);
  free(fp);
  p->cache_dirty = 1;
  *ran = 1;
  p->steps_run++;
  return CCB_OK;
}

// ---------- AAR manifest merge ----------

// Read AndroidManifest.xml from an AAR zip. Returns malloc'd
// NUL-terminated text, or NULL when the AAR has no manifest. Sets
// *failed when the zip cannot be opened or the entry cannot be read.
// Manifests are read from the zips directly, not from the extracted
// copies, so the merge does not depend on the deps extraction cache.
char *ccb_aar_manifest_text(const char *aar_path, int *failed) {
  *failed = 0;
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(aar_path, &a) != CCB_OK) {
    *failed = 1;
    return NULL;
  }
  char *text = NULL;
  for (size_t e = 0; e < a->count; e++) {
    const ccb_zip_entry *entry = &a->entries[e];
    if (strcmp(entry->name, "AndroidManifest.xml") == 0) {
      void *data = NULL;
      size_t len = 0;
      if (ccb_zip_read_entry(a, entry, &data, &len) != CCB_OK) {
        *failed = 1;
        break;
      }
      text = ccb_xmalloc(len + 1);
      memcpy(text, data, len);
      text[len] = '\0';
      free(data);
      break;
    }
  }
  ccb_zip_close(a);
  return text;
}

// Merge AAR manifests into the app manifest (requirement.md §5.2).
// The merged manifest is what aapt2 link sees; the pre-merge app
// manifest stays in the project for --explain/--dry-run inspection,
// and the merged file is left under intermediates/ for inspection.
ccb_status ccb_step_merge_manifest(ccb_pipe *p) {
  if (p->merge_aar_paths_count == 0) {
    if (p->opts->explain) {
      printf("merge-manifest: skip (no AAR dependencies)\n");
    }
    return CCB_OK;
  }
  char *merged_dir = ccb_path_join(p->intermediates, "merged-manifest");
  char *merged_path = ccb_path_join(merged_dir, "AndroidManifest.xml");
  p->merged_manifest = merged_path;
  char *app_text = NULL;
  size_t app_len = 0;
  if (ccb_read_file(p->cfg->manifest, &app_text, &app_len) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "merge-manifest", "cannot read app manifest");
    free(merged_dir);
    return CCB_ERR_BUILD;
  }
  const char **aar_texts = ccb_xcalloc(p->merge_aar_paths_count, sizeof(*aar_texts));
  const char **aar_names = ccb_xcalloc(p->merge_aar_paths_count, sizeof(*aar_names));
  for (size_t i = 0; i < p->merge_aar_paths_count; i++) {
    aar_names[i] = p->merge_aar_names[i];
  }
  int read_failed = 0;
  for (size_t i = 0; i < p->merge_aar_paths_count; i++) {
    int failed = 0;
    if (p->merge_aar_srcs[i] != NULL) {
      // Built library module: the AAR manifest is a copy of the
      // module manifest, which always exists — so merge works in
      // explain/dry-run mode too, before the AAR is built.
      char *text = NULL;
      size_t len = 0;
      if (ccb_read_file(p->merge_aar_srcs[i], &text, &len) != CCB_OK) {
        failed = 1;
      } else {
        aar_texts[i] = text;
      }
    } else {
      aar_texts[i] = ccb_aar_manifest_text(p->merge_aar_paths[i], &failed);
    }
    if (failed) {
      read_failed = 1;
      break;
    }
  }
  if (read_failed) {
    ccb_pipe_diag(p, "CCB-E3015", "merge-manifest", "cannot read AAR manifest");
    for (size_t i = 0; i < p->merge_aar_paths_count; i++) {
      free((void *)aar_texts[i]);
    }
    free(aar_texts);
    free(aar_names);
    free(app_text);
    free(merged_dir);
    return CCB_ERR_BUILD;
  }
  // Fingerprint over the merge inputs, so link can key on it without
  // reading the merged file (which does not exist yet in explain mode
  // on a fresh tree).
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  ccb_fp_add(&b, "app-manifest", app_text);
  for (size_t i = 0; i < p->merge_aar_paths_count; i++) {
    ccb_fp_add(&b, p->merge_aar_paths[i], aar_texts[i] != NULL ? aar_texts[i] : "<absent>");
  }
  p->merge_fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
  ccb_fp_free(&b);
  const char *old = ccb_cache_step_fp(p->cache, "merge-manifest");
  if (!p->opts->dry_run && old != NULL && strcmp(old, p->merge_fp) == 0 &&
      ccb_path_exists(merged_path)) {
    p->steps_skipped++;
    if (p->opts->explain) {
      printf("merge-manifest: skip (merge inputs unchanged)\n");
    }
    ccb_log_cache(&p->log, "merge-manifest", 1, "merge inputs unchanged");
    for (size_t i = 0; i < p->merge_aar_paths_count; i++) {
      free((void *)aar_texts[i]);
    }
    free(aar_texts);
    free(aar_names);
    free(app_text);
    free(merged_dir);
    return CCB_OK;
  }
  // Merge in every mode: this validates the manifests even for
  // --explain and --dry-run, and surfaces conflict warnings.
  ccb_manifest_warnings warns;
  memset(&warns, 0, sizeof(warns));
  char *err = NULL;
  char *merged = ccb_manifest_merge(app_text, aar_texts, aar_names,
      p->merge_aar_paths_count, &warns, &err);
  for (size_t i = 0; i < warns.count; i++) {
    ccb_log_warning(&p->log, "%s", warns.msgs[i]);
  }
  ccb_manifest_warnings_free(&warns);
  for (size_t i = 0; i < p->merge_aar_paths_count; i++) {
    free((void *)aar_texts[i]);
  }
  free(aar_texts);
  free(aar_names);
  free(app_text);
  if (merged == NULL) {
    ccb_pipe_diag(p, "CCB-E3018", "merge-manifest", err != NULL ? err : "manifest merge failed");
    free(err);
    free(merged_dir);
    return CCB_ERR_BUILD;
  }
  if (p->opts->explain) {
    printf("merge-manifest: run (merge inputs changed)\n");
    free(merged);
    free(merged_dir);
    return CCB_OK;
  }
  if (!p->opts->dry_run) {
    if (ccb_mkdir_p(merged_dir) != CCB_OK ||
        ccb_write_file(merged_path, merged, strlen(merged)) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "merge-manifest", "cannot write merged manifest");
      free(merged);
      free(merged_dir);
      return CCB_ERR_BUILD;
    }
  }
  free(merged);
  free(merged_dir);
  ccb_cache_set_step(p->cache, "merge-manifest", p->merge_fp);
  p->cache_dirty = 1;
  p->steps_run++;
  return CCB_OK;
}
