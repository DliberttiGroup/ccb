// ccb_pipeline implementation, part 1: context, tool runner, stage 0,
// AAR deps, aapt2 compile + link.
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
#include "ccb_zip.h"
#include "ccb_bundle.h"
#include "ccb_pipeline.h"
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

// ---------- small helpers ----------

typedef struct {
  char **v;
  size_t n;
  size_t cap;
  ccb_arena *ar;
} ccb_argv;

static void ccb_argv_push(ccb_argv *a, const char *s) {
  if (a->n + 1 >= a->cap) {
    size_t ncap = a->cap == 0 ? 16 : a->cap * 2;
    a->v = ccb_xrealloc(a->v, ncap * sizeof(*a->v));
    a->cap = ncap;
  }
  a->v[a->n++] = ccb_arena_strdup(a->ar, s);
  a->v[a->n] = NULL;
}

static void ccb_argv_pushf(ccb_argv *a, const char *fmt, long num) {
  char buf[64];
  snprintf(buf, sizeof(buf), fmt, num);
  ccb_argv_push(a, buf);
}

// Key for cache maps: path relative to the project root when possible.
static char *ccb_rel_key(ccb_pipe *p, const char *path) {
  size_t rl = strlen(p->cfg->project_root);
  if (strncmp(path, p->cfg->project_root, rl) == 0 && path[rl] == '/') {
    return ccb_xstrdup(path + rl + 1);
  }
  if (rl == 1 && path[0] == '/') {
    return ccb_xstrdup(path + 1);
  }
  return ccb_xstrdup(path);
}

typedef struct {
  char **keys;
  char **vals;
  size_t count;
  size_t cap;
} ccb_fp_builder;

static void ccb_fp_add(ccb_fp_builder *b, const char *key, const char *val) {
  if (b->count == b->cap) {
    size_t ncap = b->cap == 0 ? 16 : b->cap * 2;
    b->keys = ccb_xrealloc(b->keys, ncap * sizeof(*b->keys));
    b->vals = ccb_xrealloc(b->vals, ncap * sizeof(*b->vals));
    b->cap = ncap;
  }
  b->keys[b->count] = ccb_xstrdup(key);
  b->vals[b->count] = ccb_xstrdup(val);
  b->count++;
}

static void ccb_fp_addl(ccb_fp_builder *b, const char *key, long val) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld", val);
  ccb_fp_add(b, key, buf);
}

static void ccb_fp_free(ccb_fp_builder *b) {
  ccb_free_str_array(b->keys, b->count);
  ccb_free_str_array(b->vals, b->count);
  memset(b, 0, sizeof(*b));
}

// Hash a file and record it in the fingerprint + cache file map.
// Returns 0 on success, -1 on read failure.
static int ccb_fp_add_file(ccb_pipe *p, ccb_fp_builder *b, const char *path) {
  char *hash = ccb_cache_hash_file(path);
  if (hash == NULL) {
    return -1;
  }
  char *key = ccb_rel_key(p, path);
  ccb_fp_add(b, key, hash);
  ccb_cache_set_file(p->cache, key, hash);
  free(key);
  free(hash);
  return 0;
}

static void ccb_pipe_diag(ccb_pipe *p, const char *code, const char *phase, const char *msg) {
  ccb_diag d = ccb_diag_make(code, phase, msg);
  ccb_diag_list_push(p->diags, &d);
}

// Run a tool. In dry-run mode only the command is printed. env_extra
// entries are "K=V" strings (borrowed). On failure a diagnostic is
// recorded and CCB_ERR_BUILD returned (callers override the status for
// signing/toolchain failures where needed).
static ccb_status ccb_run_tool(ccb_pipe *p, const char *phase, const char *code, char *const argv[],
    char *const env_extra[], const char *suggestion) {
  ccb_str cmd;
  ccb_str_init(&cmd);
  ccb_proc_render(argv, &cmd);
  if (p->opts->dry_run) {
    printf("%s\n", cmd.buf);
    ccb_str_free(&cmd);
    return CCB_OK;
  }
  ccb_log_phase_start(&p->log, phase);
  ccb_proc_result r;
  ccb_status rc = ccb_proc_run(argv, NULL, env_extra, &r);
  if (rc != CCB_OK) {
    ccb_diag d = ccb_diag_make(code, phase, "could not start tool");
    d.command = ccb_str_steal(&cmd);
    d.suggestion = ccb_xstrdup(suggestion);
    ccb_diag_list_push(p->diags, &d);
    return CCB_ERR_BUILD;
  }
  ccb_log_tool_output(&p->log, phase, r.out, r.err);
  if (r.exit_code != 0) {
    // Keep the tail of stderr: the actionable part of tool output.
    const char *tail = r.err;
    size_t len = strlen(r.err);
    if (len > 2000) {
      tail = r.err + len - 2000;
      while (*tail != '\0' && *tail != '\n') {
        tail++;
      }
    }
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_appendf(&msg, "tool exited with code %d", r.exit_code);
    if (tail[0] != '\0') {
      ccb_str_append(&msg, ": ");
      ccb_str_append(&msg, tail);
    }
    ccb_diag d = ccb_diag_make(code, phase, msg.buf);
    d.command = ccb_str_steal(&cmd);
    d.suggestion = ccb_xstrdup(suggestion);
    ccb_diag_list_push(p->diags, &d);
    ccb_str_free(&msg);
    ccb_proc_result_free(&r);
    return CCB_ERR_BUILD;
  }
  ccb_str_free(&cmd);
  ccb_proc_result_free(&r);
  return CCB_OK;
}

// ---------- stage 0 ----------

static ccb_status ccb_stage0(ccb_pipe *p) {
  char cwd[4096];
  const char *start = p->opts->workdir;
  if (start == NULL) {
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      ccb_pipe_diag(p, "CCB-E1009", "config", "cannot determine working directory");
      return CCB_ERR_CONFIG;
    }
    start = cwd;
  }
  char *config_path = NULL;
  if (ccb_config_discover(start, p->opts->config_path, &config_path) != CCB_OK) {
    if (p->opts->config_path != NULL) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "config file not found: ");
      ccb_str_append(&msg, p->opts->config_path);
      ccb_pipe_diag(p, "CCB-E1009", "config", msg.buf);
      ccb_str_free(&msg);
    } else {
      ccb_pipe_diag(p, "CCB-E1009", "config", "no ccb.toml found in this directory or any parent");
    }
    return CCB_ERR_CONFIG;
  }
  if (ccb_config_load(config_path, p->opts->variant_override, &p->cfg, p->diags) != CCB_OK) {
    free(config_path);
    return CCB_ERR_CONFIG;
  }
  free(config_path);
  // The manifest's package must match [project] package: aapt2 takes the
  // package from the manifest, so a mismatch would silently ship the
  // wrong application ID.
  {
    char *mtext = NULL;
    size_t mlen = 0;
    if (ccb_read_file(p->cfg->manifest, &mtext, &mlen) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E1006", "config", "cannot read manifest");
      return CCB_ERR_CONFIG;
    }
    const char *m = strstr(mtext, "package=\"");
    char *mpkg = NULL;
    if (m != NULL) {
      m += 9;
      const char *end = strchr(m, '"');
      if (end != NULL) {
        mpkg = ccb_xstrndup(m, (size_t)(end - m));
      }
    }
    if (mpkg == NULL) {
      ccb_pipe_diag(p, "CCB-E1005", "config", "manifest must declare a package attribute");
      free(mtext);
      return CCB_ERR_CONFIG;
    }
    if (strcmp(mpkg, p->cfg->package) != 0) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "manifest package '");
      ccb_str_append(&msg, mpkg);
      ccb_str_append(&msg, "' does not match project.package '");
      ccb_str_append(&msg, p->cfg->package);
      ccb_str_append(&msg, "'");
      ccb_pipe_diag(p, "CCB-E1005", "config", msg.buf);
      ccb_str_free(&msg);
      free(mpkg);
      free(mtext);
      return CCB_ERR_CONFIG;
    }
    free(mpkg);
    free(mtext);
  }
  if (ccb_sdk_resolve(p->cfg, &p->sdk, p->diags) != CCB_OK) {
    return CCB_ERR_TOOLCHAIN;
  }
  // Release signing pre-checks: fail before any compilation.
  if (strcmp(p->cfg->variant, "release") == 0) {
    if (!p->cfg->has_release_signing) {
      ccb_pipe_diag(p, "CCB-E3001", "sign", "release build requires a [signing.release] section");
      return CCB_ERR_SIGNING;
    }
    if (!ccb_path_exists(p->cfg->release_store)) {
      ccb_pipe_diag(p, "CCB-E3001", "sign", "release keystore does not exist");
      return CCB_ERR_SIGNING;
    }
    if (getenv(p->cfg->release_store_pw_env) == NULL) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "release store password env var is not set: ");
      ccb_str_append(&msg, p->cfg->release_store_pw_env);
      ccb_pipe_diag(p, "CCB-E3001", "sign", msg.buf);
      ccb_str_free(&msg);
      return CCB_ERR_SIGNING;
    }
    if (getenv(p->cfg->release_key_pw_env) == NULL) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "release key password env var is not set: ");
      ccb_str_append(&msg, p->cfg->release_key_pw_env);
      ccb_pipe_diag(p, "CCB-E3001", "sign", msg.buf);
      ccb_str_free(&msg);
      return CCB_ERR_SIGNING;
    }
  }
  // Maven dependencies (cache-only: builds never touch the network).
  // Resolved AARs/JARs join the local deps, so the deps step, manifest
  // merge, classpath, and dex all treat them as local files. A cache
  // miss fails here, before any stage runs, naming the coordinate and
  // pointing at `ccb setup`.
  if (p->cfg->maven_count > 0) {
    ccb_maven_result mv;
    ccb_status mrc = ccb_maven_resolve(p->cfg, 0, p->opts->verbose,
      p->opts->dry_run || p->opts->explain, &mv, p->diags);
    if (mrc != CCB_OK) {
      return CCB_ERR_TOOLCHAIN;
    }
    for (size_t i = 0; i < mv.aars_count; i++) {
      p->cfg->aars = ccb_xrealloc(p->cfg->aars,
        (p->cfg->aars_count + 1) * sizeof(*p->cfg->aars));
      p->cfg->aars[p->cfg->aars_count++] = mv.aars[i];
    }
    for (size_t i = 0; i < mv.jars_count; i++) {
      p->cfg->jars = ccb_xrealloc(p->cfg->jars,
        (p->cfg->jars_count + 1) * sizeof(*p->cfg->jars));
      p->cfg->jars[p->cfg->jars_count++] = mv.jars[i];
    }
    // Ownership of the paths moved into cfg; only the arrays themselves
    // are freed here (result_free would free the paths too).
    free(mv.aars);
    free(mv.jars);
  }
  // Directories.
  p->intermediates = ccb_path_join(p->cfg->build_dir, "intermediates");
  p->gen_dir = ccb_path_join(p->intermediates, "gen");
  p->classes_dir = ccb_path_join(p->intermediates, "classes");
  p->dex_dir = ccb_path_join(p->intermediates, "dex");
  p->aar_dir = ccb_path_join(p->intermediates, "aar");
  p->out_dir = ccb_path_join3(p->cfg->build_dir, "outputs", p->cfg->variant);
  p->linked_apk = ccb_path_join(p->intermediates, "linked.ap_");
  p->linked_aab = ccb_path_join(p->intermediates, "linked-aab.ap_");
  p->unsigned_apk = ccb_path_join(p->intermediates, "app-unsigned.apk");
  p->unsigned_aab = ccb_path_join(p->intermediates, "app-unsigned.aab");
  p->aligned_apk = ccb_path_join(p->intermediates, "app-aligned.apk");
  if (!p->opts->dry_run && !p->opts->explain) {
    if (ccb_mkdir_p(p->intermediates) != CCB_OK || ccb_mkdir_p(p->out_dir) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "build", "cannot create build directories");
      return CCB_ERR_BUILD;
    }
    // Probe cache: record the versions resolve just probed (or hit)
    // so the next build skips the JVM probes. Resolve runs before
    // this mkdir, so it only reads the cache; the write happens here.
    if (p->sdk->java != NULL && p->sdk->java_major > 0) {
      char ver[32];
      snprintf(ver, sizeof(ver), "%d", p->sdk->java_major);
      ccb_sdk_probe_cache_store(p->cfg->build_dir, "java", p->sdk->java, ver);
    }
    if (p->sdk->kotlinc != NULL && p->sdk->kotlin_version != NULL) {
      ccb_sdk_probe_cache_store(p->cfg->build_dir, "kotlinc", p->sdk->kotlinc, p->sdk->kotlin_version);
    }
  }
  // Tool fingerprint.
  {
    ccb_str fp;
    ccb_str_init(&fp);
    ccb_str_append(&fp, "bt=");
    ccb_str_append(&fp, p->sdk->build_tools_version);
    if (p->sdk->aapt2 != NULL) {
      ccb_str_append(&fp, "|aapt2=");
      ccb_str_append(&fp, p->sdk->aapt2);
    }
    if (p->sdk->zipalign != NULL) {
      ccb_str_append(&fp, "|zipalign=");
      ccb_str_append(&fp, p->sdk->zipalign);
    }
    ccb_str_append(&fp, "|kt=");
    ccb_str_append(&fp, p->sdk->kotlin_version);
    ccb_str_appendf(&fp, "|java=%d", p->sdk->java_major);
    ccb_str_appendf(&fp, "|compile=%ld", p->cfg->compile_sdk);
    p->tool_fingerprint = ccb_str_steal(&fp);
  }
  // Cache: loaded for build and explain (explain is read-only and never
  // saves). Dry-run skips it entirely: it prints every command anyway.
  if (!p->opts->dry_run) {
    char *cache_path = ccb_path_join(p->cfg->build_dir, "ccb-cache.json");
    ccb_cache_load(cache_path, &p->cache);
    free(cache_path);
    if (p->cache->tool_fingerprint == NULL ||
        strcmp(p->cache->tool_fingerprint, p->tool_fingerprint) != 0) {
      // Tool upgrade (or first build): invalidate every step.
      ccb_cache_free(p->cache);
      p->cache = ccb_xcalloc(1, sizeof(*p->cache));
      p->cache->tool_fingerprint = ccb_xstrdup(p->tool_fingerprint);
      p->cache_dirty = 1;
    }
  } else {
    p->cache = ccb_xcalloc(1, sizeof(*p->cache));
  }
  return CCB_OK;
}

static ccb_status ccb_save_cache(ccb_pipe *p) {
  if (p->opts->dry_run || p->opts->explain) {
    return CCB_OK;
  }
  char *cache_path = ccb_path_join(p->cfg->build_dir, "ccb-cache.json");
  ccb_status rc = ccb_cache_save(p->cache, cache_path);
  free(cache_path);
  return rc;
}

// ---------- deps / AAR extraction ----------

// Text-search for package="..." in an AAR manifest (AAR manifests are
// plain XML, unlike compiled app manifests).
static char *ccb_aar_package(const char *manifest_text) {
  const char *m = strstr(manifest_text, "package=\"");
  if (m == NULL) {
    return NULL;
  }
  m += 9;
  const char *end = strchr(m, '"');
  if (end == NULL) {
    return NULL;
  }
  return ccb_xstrndup(m, (size_t)(end - m));
}

static void ccb_push_str(char ***arr, size_t *count, size_t *cap, char *s) {
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
static ccb_status ccb_step_deps(ccb_pipe *p, int *ran) {
  *ran = 0;
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
          char *pkg = ccb_aar_package(text);
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
static char *ccb_aar_manifest_text(const char *aar_path, int *failed) {
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
static ccb_status ccb_step_merge_manifest(ccb_pipe *p) {
  if (p->cfg->aars_count == 0) {
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
  const char **aar_texts = ccb_xcalloc(p->cfg->aars_count, sizeof(*aar_texts));
  const char **aar_names = ccb_xcalloc(p->cfg->aars_count, sizeof(*aar_names));
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    aar_names[i] = p->cfg->aars[i];
  }
  int read_failed = 0;
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    int failed = 0;
    aar_texts[i] = ccb_aar_manifest_text(p->cfg->aars[i], &failed);
    if (failed) {
      read_failed = 1;
      break;
    }
  }
  if (read_failed) {
    ccb_pipe_diag(p, "CCB-E3015", "merge-manifest", "cannot read AAR manifest");
    for (size_t i = 0; i < p->cfg->aars_count; i++) {
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
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    ccb_fp_add(&b, p->cfg->aars[i], aar_texts[i] != NULL ? aar_texts[i] : "<absent>");
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
    for (size_t i = 0; i < p->cfg->aars_count; i++) {
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
      p->cfg->aars_count, &warns, &err);
  for (size_t i = 0; i < warns.count; i++) {
    ccb_log_warning(&p->log, "%s", warns.msgs[i]);
  }
  ccb_manifest_warnings_free(&warns);
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
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

// ---------- aapt2 compile ----------

typedef struct {
  char *res_dir;
  char *zip_path;
} ccb_res_target;

static ccb_status ccb_step_compile_res(ccb_pipe *p) {
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

static ccb_status ccb_step_link(ccb_pipe *p) {
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
      !(p->opts->explain && p->compile_will_run)) {
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
    } else {
      printf("link: run (link inputs changed)\n");
    }
    free(fp);
    p->link_will_run = 1;
    return CCB_OK;
  }
  p->link_will_run = 1;
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
  for (size_t i = 0; i < p->aar_res_zips_count; i++) {
    ccb_argv_push(&av, p->aar_res_zips[i]);
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

// ---------- kotlinc (+ javac) ----------

static char *ccb_java_home(ccb_pipe *p) {
  char *bin = ccb_dirname(p->sdk->java);
  char *home = ccb_dirname(bin);
  free(bin);
  return home;
}

// Env for JVM tools: heap cap + JAVA_HOME derived from the resolved java.
// PATH is also prefixed with the JDK bin dir: the build-tools wrapper
// scripts (d8, apksigner) exec plain `java` from PATH and ignore
// JAVA_HOME, so without this they fail on machines where java is not
// already on PATH.
static char **ccb_jvm_env(ccb_pipe *p, ccb_arena *ar) {
  char **env = ccb_arena_alloc(ar, 4 * sizeof(*env));
  env[0] = ccb_arena_strdup(ar, "JAVA_TOOL_OPTIONS=-Xmx1g");
  ccb_str jh;
  ccb_str_init(&jh);
  ccb_str_append(&jh, "JAVA_HOME=");
  char *home = ccb_java_home(p);
  ccb_str_append(&jh, home);
  free(home);
  env[1] = ccb_arena_strdup(ar, jh.buf);
  ccb_str_free(&jh);
  char *bin = ccb_dirname(p->sdk->java);
  ccb_str path;
  ccb_str_init(&path);
  ccb_str_append(&path, "PATH=");
  ccb_str_append(&path, bin);
  const char *old_path = getenv("PATH");
  if (old_path != NULL) {
    ccb_str_append_c(&path, ':');
    ccb_str_append(&path, old_path);
  }
  free(bin);
  env[2] = ccb_arena_strdup(ar, path.buf);
  ccb_str_free(&path);
  env[3] = NULL;
  return env;
}

static char *ccb_classes_fingerprint(ccb_pipe *p) {
  char **files = NULL;
  size_t nfiles = 0;
  if (!ccb_path_is_dir(p->classes_dir)) {
    return ccb_xstrdup("");
  }
  if (ccb_collect_files(p->classes_dir, ".class", &files, &nfiles) != CCB_OK) {
    return NULL;
  }
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  int failed = 0;
  for (size_t i = 0; i < nfiles; i++) {
    if (ccb_fp_add_file(p, &b, files[i]) != 0) {
      failed = 1;
      break;
    }
  }
  ccb_free_str_array(files, nfiles);
  if (failed) {
    ccb_fp_free(&b);
    return NULL;
  }
  char *fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
  ccb_fp_free(&b);
  return fp;
}

static ccb_status ccb_step_code(ccb_pipe *p) {
  // Declared first: an early goto below must never jump over a
  // cleanup-attributed declaration, or cleanup would run on garbage.
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  char **kt_files = NULL;
  size_t nkt = 0;
  char **java_files = NULL;
  size_t njava = 0;
  char **rjava_files = NULL;
  size_t nrjava = 0;
  if (p->cfg->kotlin_dir != NULL) {
    ccb_collect_files(p->cfg->kotlin_dir, ".kt", &kt_files, &nkt);
  }
  if (p->cfg->java_dir != NULL) {
    ccb_collect_files(p->cfg->java_dir, ".java", &java_files, &njava);
  }
  if (ccb_path_is_dir(p->gen_dir)) {
    ccb_collect_files(p->gen_dir, ".java", &rjava_files, &nrjava);
  }
  if (nkt == 0 && njava == 0 && nrjava == 0) {
    if (p->opts->explain) {
      printf("compile-code: skip (no sources)\n");
    }
    ccb_log_info(&p->log, "no sources, skipping compile-code");
    p->code_will_run = 0;
    p->steps_skipped++;
    goto code_done;
  }
  // Classpath: android.jar + jars + AAR classes.jars.
  ccb_str cp;
  ccb_str_init(&cp);
  ccb_str_append(&cp, p->sdk->android_jar);
  for (size_t i = 0; i < p->cfg->jars_count; i++) {
    ccb_str_append_c(&cp, ':');
    ccb_str_append(&cp, p->cfg->jars[i]);
  }
  for (size_t i = 0; i < p->aar_class_jars_count; i++) {
    ccb_str_append_c(&cp, ':');
    ccb_str_append(&cp, p->aar_class_jars[i]);
  }
  char *cp_str = ccb_str_steal(&cp);
  {
    ccb_fp_builder b;
    memset(&b, 0, sizeof(b));
    ccb_fp_add(&b, "kotlinc", p->sdk->kotlin_version);
    ccb_fp_add(&b, "jvm_target", p->cfg->jvm_target);
    ccb_fp_add(&b, "classpath", cp_str);
    int failed = 0;
    for (size_t i = 0; i < nkt && !failed; i++) {
      if (ccb_fp_add_file(p, &b, kt_files[i]) != 0) {
        failed = 1;
      }
    }
    for (size_t i = 0; i < njava && !failed; i++) {
      if (ccb_fp_add_file(p, &b, java_files[i]) != 0) {
        failed = 1;
      }
    }
    for (size_t i = 0; i < nrjava && !failed; i++) {
      if (ccb_fp_add_file(p, &b, rjava_files[i]) != 0) {
        failed = 1;
      }
    }
    char *fp = NULL;
    const char *old = NULL;
    if (!failed) {
      fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
      old = ccb_cache_step_fp(p->cache, "compile-code");
    }
    ccb_fp_free(&b);
    if (failed) {
      ccb_pipe_diag(p, "CCB-E3004", "kotlinc", "cannot read source files");
      free(cp_str);
      goto code_fail;
    }
    if (p->opts->explain) {
      if (p->link_will_run) {
        printf("compile-code: run (link outputs will change)\n");
        p->code_will_run = 1;
      } else if (old != NULL && strcmp(old, fp) == 0) {
        printf("compile-code: skip (inputs unchanged)\n");
        p->code_will_run = 0;
      } else {
        printf("compile-code: run (source inputs changed)\n");
        p->code_will_run = 1;
      }
      free(cp_str);
      free(fp);
      goto code_done;
    }
    if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0) {
      free(cp_str);
      free(fp);
      p->code_will_run = 0;
      p->steps_skipped++;
      ccb_log_cache(&p->log, "compile-code", 1, "source inputs unchanged");
      goto code_done;
    }
    p->code_will_run = 1;
    if (!p->opts->dry_run) {
      ccb_remove_recursive(p->classes_dir);
      if (ccb_mkdir_p(p->classes_dir) != CCB_OK) {
        ccb_pipe_diag(p, "CCB-E3016", "kotlinc", "cannot create classes directory");
        free(cp_str);
        free(fp);
        goto code_fail;
      }
    }
    // kotlinc: Kotlin + R.java + Java sources (kotlinc resolves Java
    // symbols from sources passed on the command line).
    {
      ccb_argv av;
      memset(&av, 0, sizeof(av));
      av.ar = ar;
      ccb_argv_push(&av, p->sdk->kotlinc);
      ccb_argv_push(&av, "-jvm-target");
      ccb_argv_push(&av, p->cfg->jvm_target);
      ccb_argv_push(&av, "-d");
      ccb_argv_push(&av, p->classes_dir);
      ccb_argv_push(&av, "-cp");
      ccb_argv_push(&av, cp_str);
      for (size_t i = 0; i < nkt; i++) {
        ccb_argv_push(&av, kt_files[i]);
      }
      for (size_t i = 0; i < nrjava; i++) {
        ccb_argv_push(&av, rjava_files[i]);
      }
      for (size_t i = 0; i < njava; i++) {
        ccb_argv_push(&av, java_files[i]);
      }
      char **env = ccb_jvm_env(p, ar);
      ccb_status rc = ccb_run_tool(p, "kotlinc", "CCB-E3004", av.v, env,
        "check the Kotlin/Java sources for errors");
      free(av.v);
      if (rc != CCB_OK) {
        free(cp_str);
        free(fp);
        goto code_fail;
      }
    }
    // javac: canonical compilation of the Java sources (excluding R.java,
    // which kotlinc already compiled).
    if (njava > 0) {
      ccb_str jcp;
      ccb_str_init(&jcp);
      ccb_str_append(&jcp, p->classes_dir);
      ccb_str_append_c(&jcp, ':');
      ccb_str_append(&jcp, cp_str);
      char *jcp_str = ccb_str_steal(&jcp);
      ccb_argv av;
      memset(&av, 0, sizeof(av));
      av.ar = ar;
      ccb_argv_push(&av, p->sdk->javac);
      ccb_argv_push(&av, "-source");
      ccb_argv_push(&av, p->cfg->jvm_target);
      ccb_argv_push(&av, "-target");
      ccb_argv_push(&av, p->cfg->jvm_target);
      ccb_argv_push(&av, "-cp");
      ccb_argv_push(&av, jcp_str);
      ccb_argv_push(&av, "-d");
      ccb_argv_push(&av, p->classes_dir);
      for (size_t i = 0; i < njava; i++) {
        ccb_argv_push(&av, java_files[i]);
      }
      char **env = ccb_jvm_env(p, ar);
      ccb_status rc = ccb_run_tool(p, "javac", "CCB-E3005", av.v, env,
        "check the Java sources for errors");
      free(av.v);
      free(jcp_str);
      if (rc != CCB_OK) {
        free(cp_str);
        free(fp);
        goto code_fail;
      }
    }
    free(cp_str);
    ccb_cache_set_step(p->cache, "compile-code", fp);
    free(fp);
    p->cache_dirty = 1;
    p->steps_run++;
  }
code_done:
  free(p->classes_fp);
  p->classes_fp = ccb_classes_fingerprint(p);
  ccb_free_str_array(kt_files, nkt);
  ccb_free_str_array(java_files, njava);
  ccb_free_str_array(rjava_files, nrjava);
  return CCB_OK;
code_fail:
  ccb_free_str_array(kt_files, nkt);
  ccb_free_str_array(java_files, njava);
  ccb_free_str_array(rjava_files, nrjava);
  return CCB_ERR_BUILD;
}

// ---------- d8 ----------

static ccb_status ccb_step_dex(ccb_pipe *p) {
  int use_r8 = strcmp(p->cfg->variant, "release") == 0 && p->cfg->minify;
  if (use_r8 && p->sdk->r8_jar == NULL) {
    ccb_pipe_diag(p, "CCB-E3014", "r8",
      "release minification needs r8: set [sdk] r8_jar to an r8 jar "
      "(com.android.tools:r8 from Google's Maven repository), or run "
      "`ccb setup` in a minify project to install it");
    return CCB_ERR_BUILD;
  }
  char **class_files = NULL;
  size_t nclass = 0;
  if (ccb_path_is_dir(p->classes_dir)) {
    ccb_collect_files(p->classes_dir, ".class", &class_files, &nclass);
  }
  if (nclass == 0 && p->cfg->jars_count == 0 && p->aar_class_jars_count == 0) {
    if (p->opts->explain) {
      printf("dex: skip (no classes or jars)\n");
    }
    ccb_log_info(&p->log, "no classes or jars, skipping dex");
    p->dex_will_run = 0;
    p->steps_skipped++;
    ccb_free_str_array(class_files, nclass);
    return CCB_OK;
  }
  ccb_free_str_array(class_files, nclass);
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  ccb_fp_add(&b, "build_tools", p->sdk->build_tools_version);
  ccb_fp_addl(&b, "min_sdk", p->cfg->min_sdk);
  ccb_fp_add(&b, "classes", p->classes_fp == NULL ? "" : p->classes_fp);
  int failed = 0;
  if (use_r8) {
    ccb_fp_add(&b, "tool", "r8");
    if (ccb_fp_add_file(p, &b, p->sdk->r8_jar) != 0) {
      failed = 1;
    }
    if (!failed && ccb_fp_add_file(p, &b, p->cfg->proguard_rules) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < p->cfg->jars_count && !failed; i++) {
    if (ccb_fp_add_file(p, &b, p->cfg->jars[i]) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < p->aar_class_jars_count && !failed; i++) {
    if (ccb_fp_add_file(p, &b, p->aar_class_jars[i]) != 0) {
      failed = 1;
    }
  }
  char *fp = NULL;
  const char *old = NULL;
  if (!failed) {
    fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
    old = ccb_cache_step_fp(p->cache, "dex");
  }
  ccb_fp_free(&b);
  if (failed) {
    ccb_pipe_diag(p, "CCB-E3006", "d8", "cannot read dex inputs");
    return CCB_ERR_BUILD;
  }
  if (p->opts->explain) {
    if (p->code_will_run) {
      printf("dex: run (classes will change)\n");
      p->dex_will_run = 1;
    } else if (old != NULL && strcmp(old, fp) == 0) {
      printf("dex: skip (inputs unchanged)\n");
      p->dex_will_run = 0;
    } else {
      printf("dex: run (dex inputs changed)\n");
      p->dex_will_run = 1;
    }
    free(fp);
    return CCB_OK;
  }
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_path_exists(p->dex_dir)) {
    char **dexes = NULL;
    size_t ndex = 0;
    ccb_collect_files(p->dex_dir, ".dex", &dexes, &ndex);
    ccb_free_str_array(dexes, ndex);
    if (ndex > 0) {
      free(fp);
      p->dex_will_run = 0;
      p->steps_skipped++;
      ccb_log_cache(&p->log, "dex", 1, "dex inputs unchanged");
      return CCB_OK;
    }
  }
  p->dex_will_run = 1;
  if (!p->opts->dry_run) {
    ccb_remove_recursive(p->dex_dir);
    if (ccb_mkdir_p(p->dex_dir) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "d8", "cannot create dex directory");
      free(fp);
      return CCB_ERR_BUILD;
    }
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  if (use_r8) {
    // R8 shrinks + obfuscates + dexes in one pass. Unlike AGP, CCB
    // does not auto-keep manifest components: the proguard rules must
    // keep every entry point (activities, services, receivers).
    // The r8 jar's manifest names a Main-Class it does not contain, so
    // java -jar fails; invoke the compiler class directly on the classpath.
    ccb_argv_push(&av, p->sdk->java);
    ccb_argv_push(&av, "-cp");
    ccb_argv_push(&av, p->sdk->r8_jar);
    ccb_argv_push(&av, "com.android.tools.r8.R8");
    ccb_argv_push(&av, "--release");
    ccb_argv_push(&av, "--min-api");
    ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
    ccb_argv_push(&av, "--lib");
    ccb_argv_push(&av, p->sdk->android_jar);
    // R8 fails on classes referenced from program code (unlike d8, which
    // tolerates them), so the Kotlin runtime + annotations jars that
    // kotlinc output references must be library inputs too.
    {
      char *stdlib = ccb_path_join3(p->sdk->kotlin_home, "lib", "kotlin-stdlib.jar");
      if (stdlib != NULL && access(stdlib, R_OK) == 0) {
        ccb_argv_push(&av, "--lib");
        ccb_argv_push(&av, stdlib);
      }
      free(stdlib);
      char *annot = ccb_path_join3(p->sdk->kotlin_home, "lib", "annotations-13.0.jar");
      if (annot != NULL && access(annot, R_OK) == 0) {
        ccb_argv_push(&av, "--lib");
        ccb_argv_push(&av, annot);
      }
      free(annot);
    }
    ccb_argv_push(&av, "--pg-conf");
    ccb_argv_push(&av, p->cfg->proguard_rules);
    ccb_argv_push(&av, "--output");
    ccb_argv_push(&av, p->dex_dir);
  } else {
    ccb_argv_push(&av, p->sdk->d8);
    ccb_argv_push(&av, "--min-api");
    ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
    ccb_argv_push(&av, "--lib");
    ccb_argv_push(&av, p->sdk->android_jar);
    ccb_argv_push(&av, "--output");
    ccb_argv_push(&av, p->dex_dir);
  }
  // Pass .class files individually, not the directory: kotlinc also
  // writes META-INF/*.kotlin_module into the output dir, and d8 rejects
  // non-class files found in a directory input.
  {
    char **cfiles = NULL;
    size_t ncfiles = 0;
    ccb_collect_files(p->classes_dir, ".class", &cfiles, &ncfiles);
    for (size_t i = 0; i < ncfiles; i++) {
      ccb_argv_push(&av, cfiles[i]);
    }
    ccb_free_str_array(cfiles, ncfiles);
  }
  for (size_t i = 0; i < p->cfg->jars_count; i++) {
    ccb_argv_push(&av, p->cfg->jars[i]);
  }
  for (size_t i = 0; i < p->aar_class_jars_count; i++) {
    ccb_argv_push(&av, p->aar_class_jars[i]);
  }
  char **env = ccb_jvm_env(p, ar);
  ccb_status rc;
  if (use_r8) {
    rc = ccb_run_tool(p, "r8", "CCB-E3017", av.v, env,
      "check the proguard rules and that all entry points are kept");
  } else {
    rc = ccb_run_tool(p, "d8", "CCB-E3006", av.v, env,
      "check that all classes are compatible with the min SDK");
  }
  free(av.v);
  if (rc != CCB_OK) {
    free(fp);
    return CCB_ERR_BUILD;
  }
  ccb_cache_set_step(p->cache, "dex", fp);
  free(fp);
  p->cache_dirty = 1;
  p->steps_run++;
  return CCB_OK;
}

// ---------- NDK native libs ----------

static const char *ccb_abi_triple(const char *abi) {
  if (strcmp(abi, "arm64-v8a") == 0) {
    return "aarch64-linux-android";
  }
  if (strcmp(abi, "armeabi-v7a") == 0) {
    return "armv7-linux-androideabi";
  }
  if (strcmp(abi, "x86_64") == 0) {
    return "x86_64-linux-android";
  }
  return "i686-linux-android";
}

// Project name sanitized into a C identifier-ish lib name.
static char *ccb_lib_name(const char *project) {
  ccb_str s;
  ccb_str_init(&s);
  for (const char *c = project; *c != '\0'; c++) {
    if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_') {
      ccb_str_append_c(&s, *c);
    } else if (*c >= 'A' && *c <= 'Z') {
      ccb_str_append_c(&s, (char)(*c + 32));
    } else {
      ccb_str_append_c(&s, '_');
    }
  }
  if (s.len == 0) {
    ccb_str_append(&s, "app");
  }
  return ccb_str_steal(&s);
}

static ccb_status ccb_step_native(ccb_pipe *p) {
  if (!p->cfg->has_ndk) {
    if (p->opts->explain) {
      printf("native: skip (no [ndk] section)\n");
    }
    p->native_will_run = 0;
    return CCB_OK;
  }
  char *libname = ccb_lib_name(p->cfg->name);
  ccb_str sysroot;
  ccb_str_init(&sysroot);
  ccb_str_append(&sysroot, p->sdk->ndk_home);
  ccb_str_append(&sysroot, "/toolchains/llvm/prebuilt/");
  // Host tag: reuse the sdk's clang path to derive it.
  {
    const char *clang = p->sdk->ndk_clang;
    const char *prebuilt = strstr(clang, "/prebuilt/");
    const char *bin = strstr(clang, "/bin/clang");
    char *host = ccb_xstrndup(prebuilt + 10, (size_t)(bin - (prebuilt + 10)));
    ccb_str_append(&sysroot, host);
    ccb_str_append(&sysroot, "/sysroot");
    free(host);
  }
  int any_ran = 0;
  for (size_t ai = 0; ai < p->cfg->ndk_abis_count; ai++) {
    const char *abi = p->cfg->ndk_abis[ai];
    ccb_fp_builder b;
    memset(&b, 0, sizeof(b));
    ccb_fp_add(&b, "ndk", p->cfg->ndk_version);
    ccb_fp_add(&b, "abi", abi);
    ccb_fp_addl(&b, "min_sdk", p->cfg->min_sdk);
    for (size_t i = 0; i < p->cfg->ndk_cflags_count; i++) {
      ccb_fp_add(&b, "cflag", p->cfg->ndk_cflags[i]);
    }
    int failed = 0;
    for (size_t i = 0; i < p->cfg->ndk_sources_count && !failed; i++) {
      if (ccb_fp_add_file(p, &b, p->cfg->ndk_sources[i]) != 0) {
        failed = 1;
      }
    }
    char *fp = NULL;
    const char *old = NULL;
    if (!failed) {
      fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
      char step[64];
      snprintf(step, sizeof(step), "native-%s", abi);
      old = ccb_cache_step_fp(p->cache, step);
    }
    ccb_fp_free(&b);
    char *abi_dir = ccb_path_join3(p->intermediates, "jni", abi);
    char *soname = NULL;
    {
      ccb_str sn;
      ccb_str_init(&sn);
      ccb_str_append(&sn, "lib");
      ccb_str_append(&sn, libname);
      ccb_str_append(&sn, ".so");
      soname = ccb_str_steal(&sn);
    }
    char *so_path = ccb_path_join(abi_dir, soname);
    if (failed) {
      ccb_pipe_diag(p, "CCB-E3012", "ndk", "cannot read native sources");
      free(abi_dir);
      free(soname);
      free(so_path);
      ccb_str_free(&sysroot);
      free(libname);
      return CCB_ERR_BUILD;
    }
    if (p->opts->explain) {
      if (old != NULL && strcmp(old, fp) == 0) {
        printf("native-%s: skip (inputs unchanged)\n", abi);
      } else {
        printf("native-%s: run (native inputs changed)\n", abi);
        any_ran = 1;
      }
      free(fp);
      free(abi_dir);
      free(soname);
      free(so_path);
      continue;
    }
    if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_path_exists(so_path)) {
      free(fp);
      free(abi_dir);
      free(soname);
      free(so_path);
      p->steps_skipped++;
      ccb_log_cache(&p->log, "ndk", 1, "native inputs unchanged");
      continue;
    }
    any_ran = 1;
    if (!p->opts->dry_run) {
      ccb_remove_recursive(abi_dir);
      if (ccb_mkdir_p(abi_dir) != CCB_OK) {
        ccb_pipe_diag(p, "CCB-E3016", "ndk", "cannot create native output directory");
        free(fp);
        free(abi_dir);
        free(soname);
        free(so_path);
          ccb_str_free(&sysroot);
        free(libname);
        return CCB_ERR_BUILD;
      }
    }
    // Compile each source, then link shared.
    char *triple = NULL;
    {
      ccb_str t;
      ccb_str_init(&t);
      ccb_str_append(&t, ccb_abi_triple(abi));
      ccb_str_appendf(&t, "%ld", p->cfg->min_sdk);
      triple = ccb_str_steal(&t);
    }
    char **objs = NULL;
    size_t nobjs = 0;
    ccb_status rc = CCB_OK;
    for (size_t si = 0; si < p->cfg->ndk_sources_count && rc == CCB_OK; si++) {
      char *base = ccb_basename(p->cfg->ndk_sources[si]);
      char *dot = strrchr(base, '.');
      if (dot != NULL) {
        *dot = '\0';
      }
      ccb_str obj;
      ccb_str_init(&obj);
      ccb_str_append(&obj, base);
      ccb_str_append(&obj, ".o");
      char *obj_path = ccb_path_join(abi_dir, obj.buf);
      ccb_str_free(&obj);
      free(base);
      CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
      ccb_argv av;
      memset(&av, 0, sizeof(av));
      av.ar = ar;
      ccb_argv_push(&av, p->sdk->ndk_clang);
      ccb_argv_push(&av, "-target");
      ccb_argv_push(&av, triple);
      ccb_argv_push(&av, "--sysroot");
      ccb_argv_push(&av, sysroot.buf);
      ccb_argv_push(&av, "-fPIC");
      ccb_argv_push(&av, "-O2");
      for (size_t fi = 0; fi < p->cfg->ndk_cflags_count; fi++) {
        ccb_argv_push(&av, p->cfg->ndk_cflags[fi]);
      }
      ccb_argv_push(&av, "-c");
      ccb_argv_push(&av, p->cfg->ndk_sources[si]);
      ccb_argv_push(&av, "-o");
      ccb_argv_push(&av, obj_path);
      rc = ccb_run_tool(p, "ndk", "CCB-E3012", av.v, NULL,
        "check the native sources for errors");
      free(av.v);
      if (rc == CCB_OK) {
        if (nobjs == 0) {
          objs = ccb_xmalloc(p->cfg->ndk_sources_count * sizeof(*objs));
        }
        objs[nobjs++] = obj_path;
      } else {
        free(obj_path);
      }
    }
    free(triple);
    if (rc == CCB_OK) {
      CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
      ccb_argv av;
      memset(&av, 0, sizeof(av));
      av.ar = ar;
      ccb_argv_push(&av, p->sdk->ndk_clang);
      ccb_argv_push(&av, "-target");
      {
        ccb_str t;
        ccb_str_init(&t);
        ccb_str_append(&t, ccb_abi_triple(abi));
        ccb_str_appendf(&t, "%ld", p->cfg->min_sdk);
        ccb_argv_push(&av, t.buf);
        ccb_str_free(&t);
      }
      ccb_argv_push(&av, "--sysroot");
      ccb_argv_push(&av, sysroot.buf);
      ccb_argv_push(&av, "-shared");
      {
        ccb_str sn;
        ccb_str_init(&sn);
        ccb_str_append(&sn, "-Wl,-soname,");
        ccb_str_append(&sn, soname);
        ccb_argv_push(&av, sn.buf);
        ccb_str_free(&sn);
      }
      for (size_t oi = 0; oi < nobjs; oi++) {
        ccb_argv_push(&av, objs[oi]);
      }
      ccb_argv_push(&av, "-o");
      ccb_argv_push(&av, so_path);
      rc = ccb_run_tool(p, "ndk", "CCB-E3013", av.v, NULL,
        "check the native link for errors (missing symbols, bad objects)");
      free(av.v);
    }
    ccb_free_str_array(objs, nobjs);
    if (rc != CCB_OK) {
      free(fp);
      free(abi_dir);
      free(soname);
      free(so_path);
      ccb_str_free(&sysroot);
      free(libname);
      return CCB_ERR_BUILD;
    }
    char step[64];
    snprintf(step, sizeof(step), "native-%s", abi);
    ccb_cache_set_step(p->cache, step, fp);
    free(fp);
    p->cache_dirty = 1;
    p->steps_run++;
    free(abi_dir);
    free(soname);
    free(so_path);
  }
  p->native_will_run = any_ran;
  ccb_str_free(&sysroot);
  free(libname);
  return CCB_OK;
}

// ---------- package ----------

static int ccb_name_seen(char **names, size_t count, const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

// Map a link-output entry name to its AAB archive name. APK builds keep
// names verbatim and never call this.
static char *ccb_aab_entry_name(const char *name) {
  if (strcmp(name, "AndroidManifest.xml") == 0) {
    return ccb_xstrdup("base/manifest/AndroidManifest.xml");
  }
  if (strcmp(name, "resources.pb") == 0) {
    return ccb_xstrdup("base/resources.pb");
  }
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "base/");
  ccb_str_append(&s, name);
  return ccb_str_steal(&s);
}

static ccb_status ccb_step_package(ccb_pipe *p) {
  // AAB layout: link output goes under base/ (manifest at
  // base/manifest/AndroidManifest.xml, resources.pb at base/resources.pb,
  // res/* at base/res/*), dex at base/dex/, assets at base/assets/,
  // native libs at base/lib/<abi>/, plus BundleConfig.pb at the root.
  // Cache key and intermediates are separate from the APK path.
  int is_aab = p->opts->aab;
  const char *pkg_key = is_aab ? "package-aab" : "package";
  const char *link_src = is_aab ? p->linked_aab : p->linked_apk;
  const char *pkg_out = is_aab ? p->unsigned_aab : p->unsigned_apk;
  const char *fmt_label = is_aab ? "AAB" : "APK";
  const char *asset_prefix = is_aab ? "base/assets/" : "assets/";
  const char *so_prefix = is_aab ? "base/lib/" : "lib/";
  char **dex_files = NULL;
  size_t ndex = 0;
  if (ccb_path_is_dir(p->dex_dir)) {
    ccb_collect_files(p->dex_dir, ".dex", &dex_files, &ndex);
  }
  // Archive names for assets and .so files, for the fingerprint.
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  int failed = 0;
  if (ccb_fp_add_file(p, &b, link_src) != 0) {
    failed = 1;
  }
  for (size_t i = 0; i < ndex && !failed; i++) {
    char *base = ccb_basename(dex_files[i]);
    ccb_fp_add(&b, base, "");
    free(base);
    if (ccb_fp_add_file(p, &b, dex_files[i]) != 0) {
      failed = 1;
    }
  }
  // Assets: app assets win over AAR assets with the same archive name.
  char **asset_paths = NULL;
  char **asset_names = NULL;
  size_t nassets = 0;
  size_t assets_cap = 0;
  if (p->cfg->assets != NULL) {
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(p->cfg->assets, NULL, &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      const char *rel = files[i] + strlen(p->cfg->assets) + 1;
      ccb_str an;
      ccb_str_init(&an);
      ccb_str_append(&an, asset_prefix);
      ccb_str_append(&an, rel);
      char *aname = ccb_str_steal(&an);
      if (nassets == assets_cap) {
        size_t ncap = assets_cap == 0 ? 16 : assets_cap * 2;
        asset_paths = ccb_xrealloc(asset_paths, ncap * sizeof(*asset_paths));
        asset_names = ccb_xrealloc(asset_names, ncap * sizeof(*asset_names));
        assets_cap = ncap;
      }
      asset_paths[nassets] = files[i];
      asset_names[nassets] = aname;
      nassets++;
    }
    free(files);
  }
  for (size_t ai = 0; ai < p->aar_assets_count; ai++) {
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(p->aar_assets_dirs[ai], NULL, &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      const char *rel = files[i] + strlen(p->aar_assets_dirs[ai]) + 1;
      ccb_str an;
      ccb_str_init(&an);
      ccb_str_append(&an, asset_prefix);
      ccb_str_append(&an, rel);
      char *aname = ccb_str_steal(&an);
      if (!ccb_name_seen(asset_names, nassets, aname)) {
        if (nassets == assets_cap) {
          size_t ncap = assets_cap == 0 ? 16 : assets_cap * 2;
          asset_paths = ccb_xrealloc(asset_paths, ncap * sizeof(*asset_paths));
          asset_names = ccb_xrealloc(asset_names, ncap * sizeof(*asset_names));
          assets_cap = ncap;
        }
        asset_paths[nassets] = files[i];
        asset_names[nassets] = aname;
        nassets++;
      } else {
        free(aname);
        free(files[i]);
      }
    }
    free(files);
  }
  for (size_t i = 0; i < nassets && !failed; i++) {
    ccb_fp_add(&b, asset_names[i], "");
    if (ccb_fp_add_file(p, &b, asset_paths[i]) != 0) {
      failed = 1;
    }
  }
  // Native .so files: intermediates/jni/<abi>/*.so + AAR jni.
  char **so_paths = NULL;
  char **so_names = NULL;
  size_t nso = 0;
  size_t so_cap = 0;
  int so_conflict = 0;
  char *so_conflict_name = NULL;
  for (size_t ai = 0; ai < p->cfg->ndk_abis_count; ai++) {
    char *abi_dir = ccb_path_join3(p->intermediates, "jni", p->cfg->ndk_abis[ai]);
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(abi_dir, ".so", &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      char *base = ccb_basename(files[i]);
      ccb_str sn;
      ccb_str_init(&sn);
      ccb_str_append(&sn, so_prefix);
      ccb_str_append(&sn, p->cfg->ndk_abis[ai]);
      ccb_str_append_c(&sn, '/');
      ccb_str_append(&sn, base);
      free(base);
      char *sname = ccb_str_steal(&sn);
      if (ccb_name_seen(so_names, nso, sname)) {
        so_conflict = 1;
        so_conflict_name = sname;
        free(files[i]);
      } else {
        if (nso == so_cap) {
          size_t ncap = so_cap == 0 ? 16 : so_cap * 2;
          so_paths = ccb_xrealloc(so_paths, ncap * sizeof(*so_paths));
          so_names = ccb_xrealloc(so_names, ncap * sizeof(*so_names));
          so_cap = ncap;
        }
        so_paths[nso] = files[i];
        so_names[nso] = sname;
        nso++;
      }
    }
    free(files);
    free(abi_dir);
  }
  for (size_t ai = 0; ai < p->aar_jni_count; ai++) {
    char **abi_dirs = NULL;
    size_t nabis = 0;
    // Subdirectories of the AAR jni dir are ABIs.
    {
      char **all = NULL;
      size_t nall = 0;
      ccb_collect_files(p->aar_jni_dirs[ai], ".so", &all, &nall);
      for (size_t i = 0; i < nall; i++) {
        // Path: <jni>/<abi>/<file>.so -> abi is the parent dir name.
        char *parent = ccb_dirname(all[i]);
        char *abi = ccb_basename(parent);
        free(parent);
        ccb_str sn;
        ccb_str_init(&sn);
        ccb_str_append(&sn, so_prefix);
        ccb_str_append(&sn, abi);
        ccb_str_append_c(&sn, '/');
        char *base = ccb_basename(all[i]);
        ccb_str_append(&sn, base);
        free(base);
        free(abi);
        char *sname = ccb_str_steal(&sn);
        if (ccb_name_seen(so_names, nso, sname)) {
          so_conflict = 1;
          so_conflict_name = sname;
          free(all[i]);
        } else {
          if (nso == so_cap) {
            size_t ncap = so_cap == 0 ? 16 : so_cap * 2;
            so_paths = ccb_xrealloc(so_paths, ncap * sizeof(*so_paths));
            so_names = ccb_xrealloc(so_names, ncap * sizeof(*so_names));
            so_cap = ncap;
          }
          so_paths[nso] = all[i];
          so_names[nso] = sname;
          nso++;
        }
      }
      free(all);
    }
    CCB_UNUSED(abi_dirs);
    CCB_UNUSED(nabis);
  }
  for (size_t i = 0; i < nso && !failed; i++) {
    ccb_fp_add(&b, so_names[i], "");
    if (ccb_fp_add_file(p, &b, so_paths[i]) != 0) {
      failed = 1;
    }
  }
  char *fp = NULL;
  const char *old = NULL;
  if (!failed) {
    fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
    old = ccb_cache_step_fp(p->cache, pkg_key);
  }
  ccb_fp_free(&b);
  if (so_conflict) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "duplicate native library in ");
    ccb_str_append(&msg, fmt_label);
    ccb_str_append(&msg, ": ");
    ccb_str_append(&msg, so_conflict_name);
    ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
    ccb_str_free(&msg);
    free(so_conflict_name);
    free(fp);
    goto package_fail;
  }
  if (failed) {
    ccb_pipe_diag(p, "CCB-E3007", "package", "cannot read packaging inputs");
    free(fp);
    goto package_fail;
  }
  if (p->opts->explain) {
    if (p->link_will_run || p->code_will_run || p->dex_will_run || p->native_will_run) {
      printf("package: run (upstream outputs will change)\n");
      p->package_will_run = 1;
    } else if (old != NULL && strcmp(old, fp) == 0) {
      printf("package: skip (inputs unchanged)\n");
      p->package_will_run = 0;
    } else {
      printf("package: run (packaging inputs changed)\n");
      p->package_will_run = 1;
    }
    free(fp);
    goto package_done;
  }
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_path_exists(pkg_out)) {
    free(fp);
    p->package_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "package", 1, "packaging inputs unchanged");
    goto package_done;
  }
  p->package_will_run = 1;
  if (!p->opts->dry_run) {
    ccb_zip_writer w;
    if (ccb_zip_writer_init(&w, pkg_out) != CCB_OK) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "cannot create unsigned ");
      ccb_str_append(&msg, fmt_label);
      ccb_pipe_diag(p, "CCB-E3016", "package", msg.buf);
      ccb_str_free(&msg);
      free(fp);
      goto package_fail;
    }
    ccb_status wrc = CCB_OK;
    ccb_zip_archive *linked = NULL;
    if (ccb_zip_open(link_src, &linked) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3007", "package", "cannot open linked resources");
      ccb_zip_writer_abort(&w);
      free(fp);
      goto package_fail;
    }
    for (size_t i = 0; i < linked->count && wrc == CCB_OK; i++) {
      const ccb_zip_entry *e = &linked->entries[i];
      size_t nl = strlen(e->name);
      if (nl == 0 || e->name[nl - 1] == '/') {
        continue;
      }
      void *data = NULL;
      size_t len = 0;
      if (ccb_zip_read_entry(linked, e, &data, &len) != CCB_OK) {
        wrc = CCB_ERR_BUILD;
        break;
      }
      char *arc_name = is_aab ? ccb_aab_entry_name(e->name) : ccb_xstrdup(e->name);
      if (ccb_zip_writer_add_data(&w, arc_name, data, len) != CCB_OK) {
        wrc = CCB_ERR_BUILD;
      }
      free(arc_name);
      free(data);
    }
    ccb_zip_close(linked);
    for (size_t i = 0; i < ndex && wrc == CCB_OK; i++) {
      char *base = ccb_basename(dex_files[i]);
      char *text = NULL;
      size_t tlen = 0;
      if (ccb_read_file(dex_files[i], &text, &tlen) != CCB_OK) {
        free(base);
        wrc = CCB_ERR_BUILD;
        break;
      }
      char *arc_name = base;
      ccb_str dex_name;
      if (is_aab) {
        ccb_str_init(&dex_name);
        ccb_str_append(&dex_name, "base/dex/");
        ccb_str_append(&dex_name, base);
        arc_name = dex_name.buf;
      }
      if (ccb_zip_writer_add_data(&w, arc_name, text, tlen) != CCB_OK) {
        wrc = CCB_ERR_BUILD;
      }
      if (is_aab) {
        ccb_str_free(&dex_name);
      }
      free(base);
      free(text);
    }
    for (size_t i = 0; i < nassets && wrc == CCB_OK; i++) {
      char *text = NULL;
      size_t tlen = 0;
      if (ccb_read_file(asset_paths[i], &text, &tlen) != CCB_OK) {
        wrc = CCB_ERR_BUILD;
        break;
      }
      if (ccb_zip_writer_add_data(&w, asset_names[i], text, tlen) != CCB_OK) {
        wrc = CCB_ERR_BUILD;
      }
      free(text);
    }
    for (size_t i = 0; i < nso && wrc == CCB_OK; i++) {
      char *text = NULL;
      size_t tlen = 0;
      if (ccb_read_file(so_paths[i], &text, &tlen) != CCB_OK) {
        wrc = CCB_ERR_BUILD;
        break;
      }
      if (ccb_zip_writer_add_data(&w, so_names[i], text, tlen) != CCB_OK) {
        wrc = CCB_ERR_BUILD;
      }
      free(text);
    }
    if (is_aab && wrc == CCB_OK) {
      uint8_t *bc = NULL;
      size_t bc_len = 0;
      if (ccb_bundle_config_encode("1.0.0", &bc, &bc_len) != CCB_OK ||
          ccb_zip_writer_add_data(&w, "BundleConfig.pb", bc, bc_len) != CCB_OK) {
        wrc = CCB_ERR_BUILD;
      }
      free(bc);
    }
    if (wrc != CCB_OK) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "cannot write ");
      ccb_str_append(&msg, fmt_label);
      ccb_str_append(&msg, " entries");
      ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
      ccb_str_free(&msg);
      ccb_zip_writer_abort(&w);
      free(fp);
      goto package_fail;
    }
    if (ccb_zip_writer_finish(&w) != CCB_OK) {
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "cannot finish ");
      ccb_str_append(&msg, fmt_label);
      ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
      ccb_str_free(&msg);
      free(fp);
      goto package_fail;
    }
    ccb_cache_set_step(p->cache, pkg_key, fp);
    free(fp);
    p->cache_dirty = 1;
    p->steps_run++;
  } else {
    free(fp);
    p->steps_run++;
  }
package_done:
  ccb_free_str_array(dex_files, ndex);
  ccb_free_str_array(asset_paths, nassets);
  ccb_free_str_array(asset_names, nassets);
  ccb_free_str_array(so_paths, nso);
  ccb_free_str_array(so_names, nso);
  return CCB_OK;
package_fail:
  ccb_free_str_array(dex_files, ndex);
  ccb_free_str_array(asset_paths, nassets);
  ccb_free_str_array(asset_names, nassets);
  ccb_free_str_array(so_paths, nso);
  ccb_free_str_array(so_names, nso);
  return CCB_ERR_BUILD;
}

// ---------- zipalign ----------

static ccb_status ccb_step_align(ccb_pipe *p) {
  char *hash = ccb_cache_hash_file(p->unsigned_apk);
  if (hash == NULL && !p->opts->dry_run && !p->opts->explain) {
    ccb_pipe_diag(p, "CCB-E3008", "zipalign", "unsigned APK is missing (packaging must run first)");
    return CCB_ERR_BUILD;
  }
  const char *old = ccb_cache_step_fp(p->cache, "align");
  // The align fingerprint pairs the unsigned-APK hash with the
  // effective zipalign path, so switching the zipalign tool
  // invalidates the step even when the APK bytes are identical.
  ccb_str afp;
  ccb_str_init(&afp);
  ccb_str_append(&afp, p->sdk->zipalign == NULL ? "" : p->sdk->zipalign);
  ccb_str_append(&afp, "|");
  ccb_str_append(&afp, hash == NULL ? "" : hash);
  char *align_fp = ccb_str_steal(&afp);
  if (p->opts->explain) {
    if (p->package_will_run) {
      printf("align: run (packaging outputs will change)\n");
      p->align_will_run = 1;
    } else if (old != NULL && hash != NULL && strcmp(old, align_fp) == 0) {
      printf("align: skip (inputs unchanged)\n");
      p->align_will_run = 0;
    } else {
      printf("align: run (packaging outputs changed)\n");
      p->align_will_run = 1;
    }
    free(hash);
    free(align_fp);
    return CCB_OK;
  }
  if (!p->opts->dry_run && old != NULL && hash != NULL && strcmp(old, align_fp) == 0 &&
      ccb_path_exists(p->aligned_apk)) {
    free(hash);
    free(align_fp);
    p->align_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "align", 1, "packaging outputs unchanged");
    return CCB_OK;
  }
  p->align_will_run = 1;
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->zipalign);
  ccb_argv_push(&av, "-f");
  ccb_argv_push(&av, "4");
  ccb_argv_push(&av, p->unsigned_apk);
  ccb_argv_push(&av, p->aligned_apk);
  ccb_status rc = ccb_run_tool(p, "zipalign", "CCB-E3008", av.v, NULL,
    "check that the unsigned APK is a valid ZIP");
  free(av.v);
  if (rc != CCB_OK) {
    free(hash);
    free(align_fp);
    return CCB_ERR_BUILD;
  }
  if (!p->opts->dry_run && hash != NULL) {
    ccb_cache_set_step(p->cache, "align", align_fp);
    p->cache_dirty = 1;
  }
  free(align_fp);
  free(hash);
  p->steps_run++;
  return CCB_OK;
}

// ---------- sign ----------

static char *ccb_keytool_path(ccb_pipe *p) {
  char *home = ccb_java_home(p);
  char *kt = ccb_path_join(home, "bin/keytool");
  free(home);
  if (access(kt, X_OK) == 0) {
    return kt;
  }
  free(kt);
  return ccb_sdk_find_on_path("keytool");
}

static ccb_status ccb_step_sign(ccb_pipe *p) {
  // AAB signing reuses the APK sign step verbatim (same keystore,
  // alias, and password rules, including debug keystore generation)
  // with two bundle differences: the input is the unaligned bundle
  // (bundles are not zipaligned) and apksigner gets --min-sdk-version
  // because it cannot read the manifest from base/manifest/.
  int is_aab = p->opts->aab;
  const char *sign_key = is_aab ? "sign-aab" : "sign";
  const char *sign_in = is_aab ? p->unsigned_aab : p->aligned_apk;
  char *apk_path = ccb_path_join(p->out_dir, is_aab ? "app.aab" : "app.apk");
  int is_debug = strcmp(p->cfg->variant, "debug") == 0;
  char *keystore = NULL;
  if (is_debug) {
    if (p->cfg->debug_store != NULL) {
      keystore = ccb_xstrdup(p->cfg->debug_store);
    } else {
      const char *home = getenv("HOME");
      keystore = home == NULL ? ccb_xstrdup(".android/debug.keystore")
        : ccb_path_join3(home, ".android", "debug.keystore");
    }
  } else {
    keystore = ccb_xstrdup(p->cfg->release_store);
  }
  const char *alias = is_debug ? p->cfg->debug_alias : p->cfg->release_alias;
  char *hash = ccb_cache_hash_file(sign_in);
  if (hash == NULL && !p->opts->dry_run && !p->opts->explain) {
    ccb_pipe_diag(p, "CCB-E3009", "sign", is_aab
      ? "unsigned AAB is missing (packaging must run first)"
      : "aligned APK is missing (alignment must run first)");
    free(apk_path);
    free(keystore);
    return CCB_ERR_SIGNING;
  }
  ccb_str fpbuf;
  ccb_str_init(&fpbuf);
  ccb_str_append(&fpbuf, p->cfg->variant);
  ccb_str_append_c(&fpbuf, '|');
  ccb_str_append(&fpbuf, keystore);
  ccb_str_append_c(&fpbuf, '|');
  ccb_str_append(&fpbuf, hash == NULL ? "" : hash);
  char *keystore_hash = ccb_cache_hash_file(keystore);
  if (keystore_hash != NULL) {
    ccb_str_append_c(&fpbuf, '|');
    ccb_str_append(&fpbuf, keystore_hash);
    free(keystore_hash);
  }
  char *fp = ccb_str_steal(&fpbuf);
  const char *old = ccb_cache_step_fp(p->cache, sign_key);
  if (p->opts->explain) {
    if (p->align_will_run || (is_aab && p->package_will_run)) {
      printf("sign: run (%s will change)\n", is_aab ? "unsigned AAB" : "aligned APK");
      p->sign_will_run = 1;
    } else if (old != NULL && strcmp(old, fp) == 0) {
      printf("sign: skip (inputs unchanged)\n");
      p->sign_will_run = 0;
    } else {
      printf("sign: run (signing inputs changed)\n");
      p->sign_will_run = 1;
    }
    free(fp);
    free(hash);
    free(apk_path);
    free(keystore);
    return CCB_OK;
  }
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_path_exists(apk_path)) {
    free(fp);
    free(hash);
    free(apk_path);
    free(keystore);
    p->sign_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "sign", 1, "signing inputs unchanged");
    return CCB_OK;
  }
  p->sign_will_run = 1;
  // Debug keystore: create the standard one when it does not exist yet.
  if (is_debug && !ccb_path_exists(keystore) && !p->opts->dry_run) {
    char *parent = ccb_dirname(keystore);
    if (ccb_mkdir_p(parent) != CCB_OK) {
      free(parent);
      ccb_pipe_diag(p, "CCB-E3009", "sign", "cannot create debug keystore directory");
      free(fp);
      free(hash);
      free(apk_path);
      free(keystore);
      return CCB_ERR_SIGNING;
    }
    free(parent);
    char *keytool = ccb_keytool_path(p);
    if (keytool == NULL) {
      ccb_pipe_diag(p, "CCB-E3009", "sign", "keytool not found, cannot create debug keystore");
      free(fp);
      free(hash);
      free(apk_path);
      free(keystore);
      return CCB_ERR_SIGNING;
    }
    CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, keytool);
    ccb_argv_push(&av, "-genkeypair");
    ccb_argv_push(&av, "-keystore");
    ccb_argv_push(&av, keystore);
    ccb_argv_push(&av, "-alias");
    ccb_argv_push(&av, alias);
    ccb_argv_push(&av, "-keyalg");
    ccb_argv_push(&av, "RSA");
    ccb_argv_push(&av, "-keysize");
    ccb_argv_push(&av, "2048");
    ccb_argv_push(&av, "-validity");
    ccb_argv_push(&av, "10000");
    ccb_argv_push(&av, "-storepass");
    ccb_argv_push(&av, "android");
    ccb_argv_push(&av, "-keypass");
    ccb_argv_push(&av, "android");
    ccb_argv_push(&av, "-dname");
    ccb_argv_push(&av, "CN=Android Debug,O=Android,C=US");
    char **kenv = ccb_jvm_env(p, ar);
    ccb_status rc = ccb_run_tool(p, "sign", "CCB-E3009", av.v, kenv,
      "check that keytool can write the debug keystore");
    free(av.v);
    free(keytool);
    if (rc != CCB_OK) {
      free(fp);
      free(hash);
      free(apk_path);
      free(keystore);
      return CCB_ERR_SIGNING;
    }
  }
  {
    CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, p->sdk->apksigner);
    ccb_argv_push(&av, "sign");
    if (is_aab) {
      ccb_argv_push(&av, "--min-sdk-version");
      ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
    }
    ccb_argv_push(&av, "--ks");
    ccb_argv_push(&av, keystore);
    ccb_argv_push(&av, "--ks-key-alias");
    ccb_argv_push(&av, alias);
    ccb_argv_push(&av, "--ks-pass");
    if (is_debug) {
      ccb_argv_push(&av, "pass:android");
    } else {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "env:");
      ccb_str_append(&s, p->cfg->release_store_pw_env);
      ccb_argv_push(&av, s.buf);
      ccb_str_free(&s);
    }
    ccb_argv_push(&av, "--key-pass");
    if (is_debug) {
      ccb_argv_push(&av, "pass:android");
    } else {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "env:");
      ccb_str_append(&s, p->cfg->release_key_pw_env);
      ccb_argv_push(&av, s.buf);
      ccb_str_free(&s);
    }
    ccb_argv_push(&av, "--out");
    ccb_argv_push(&av, apk_path);
    ccb_argv_push(&av, sign_in);
    char **senv = ccb_jvm_env(p, ar);
    ccb_status rc = ccb_run_tool(p, "sign", "CCB-E3009", av.v, senv,
      "check the keystore, alias, and passwords");
    free(av.v);
    if (rc != CCB_OK) {
      free(fp);
      free(hash);
      free(apk_path);
      free(keystore);
      return CCB_ERR_SIGNING;
    }
  }
  if (!p->opts->dry_run) {
    ccb_cache_set_step(p->cache, sign_key, fp);
    p->cache_dirty = 1;
  }
  free(fp);
  free(hash);
  free(apk_path);
  free(keystore);
  p->steps_run++;
  return CCB_OK;
}

// ---------- verify ----------

// jarsigner lives next to keytool in the JDK; fall back to PATH like
// the keytool helper does.
static char *ccb_jarsigner_path(ccb_pipe *p) {
  char *home = ccb_java_home(p);
  char *js = ccb_path_join(home, "bin/jarsigner");
  free(home);
  if (access(js, X_OK) == 0) {
    return js;
  }
  free(js);
  return ccb_sdk_find_on_path("jarsigner");
}

static ccb_status ccb_step_verify(ccb_pipe *p) {
  // apksigner verify hard-requires AndroidManifest.xml at the ZIP
  // root, so it cannot verify a bundle (manifest lives at
  // base/manifest/). AAB verification is therefore jarsigner -verify
  // over the v1 JAR signature apksigner wrote, plus a structural
  // check that the required bundle entries are present.
  int is_aab = p->opts->aab;
  const char *verify_key = is_aab ? "verify-aab" : "verify";
  const char *fmt_label = is_aab ? "AAB" : "APK";
  char *apk_path = ccb_path_join(p->out_dir, is_aab ? "app.aab" : "app.apk");
  char *hash = ccb_cache_hash_file(apk_path);
  if (hash == NULL && !p->opts->dry_run && !p->opts->explain) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "signed ");
    ccb_str_append(&msg, fmt_label);
    ccb_str_append(&msg, " is missing (signing must run first)");
    ccb_pipe_diag(p, "CCB-E3010", "verify", msg.buf);
    ccb_str_free(&msg);
    free(apk_path);
    return CCB_ERR_BUILD;
  }
  const char *old = ccb_cache_step_fp(p->cache, verify_key);
  if (p->opts->explain) {
    if (p->sign_will_run) {
      printf("verify: run (signed %s will change)\n", fmt_label);
    } else if (old != NULL && hash != NULL && strcmp(old, hash) == 0) {
      printf("verify: skip (%s unchanged)\n", fmt_label);
    } else {
      printf("verify: run (%s changed)\n", fmt_label);
    }
    free(hash);
    free(apk_path);
    return CCB_OK;
  }
  if (!p->opts->dry_run && old != NULL && hash != NULL && strcmp(old, hash) == 0) {
    free(hash);
    free(apk_path);
    p->steps_skipped++;
    ccb_log_cache(&p->log, "verify", 1, "signed output unchanged");
    return CCB_OK;
  }
  if (is_aab) {
    char *jarsigner = ccb_jarsigner_path(p);
    if (jarsigner == NULL) {
      ccb_pipe_diag(p, "CCB-E3010", "verify", "jarsigner not found, cannot verify AAB");
      free(hash);
      free(apk_path);
      return CCB_ERR_BUILD;
    }
    CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, jarsigner);
    ccb_argv_push(&av, "-verify");
    ccb_argv_push(&av, apk_path);
    char **venv = ccb_jvm_env(p, ar);
    ccb_status rc = ccb_run_tool(p, "verify", "CCB-E3010", av.v, venv,
      "the AAB failed JAR signature verification; rebuild from clean");
    free(av.v);
    free(jarsigner);
    if (rc != CCB_OK) {
      free(hash);
      free(apk_path);
      return CCB_ERR_BUILD;
    }
    // Structural check: the entries bundletool and Play require.
    ccb_zip_archive *aab = NULL;
    int entries_ok = 0;
    if (ccb_zip_open(apk_path, &aab) == CCB_OK) {
      int has_manifest = ccb_zip_find(aab, "base/manifest/AndroidManifest.xml") != NULL;
      int has_config = ccb_zip_find(aab, "BundleConfig.pb") != NULL;
      int has_sig = 0;
      for (size_t i = 0; i < aab->count; i++) {
        if (strncmp(aab->entries[i].name, "META-INF/", 9) == 0) {
          has_sig = 1;
          break;
        }
      }
      entries_ok = has_manifest && has_config && has_sig;
      ccb_zip_close(aab);
    }
    if (!entries_ok) {
      ccb_pipe_diag(p, "CCB-E3010", "verify",
        "AAB is missing required entries (base/manifest/AndroidManifest.xml, "
        "BundleConfig.pb, or META-INF signatures)");
      free(hash);
      free(apk_path);
      return CCB_ERR_BUILD;
    }
    if (!p->opts->dry_run && hash != NULL) {
      ccb_cache_set_step(p->cache, verify_key, hash);
      p->cache_dirty = 1;
    }
    free(hash);
    free(apk_path);
    p->steps_run++;
    return CCB_OK;
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->apksigner);
  ccb_argv_push(&av, "verify");
  ccb_argv_push(&av, apk_path);
  char **venv = ccb_jvm_env(p, ar);
  ccb_status rc = ccb_run_tool(p, "verify", "CCB-E3010", av.v, venv,
    "the APK failed signature verification; rebuild from clean");
  free(av.v);
  if (rc != CCB_OK) {
    free(hash);
    free(apk_path);
    return CCB_ERR_BUILD;
  }
  CCB_AUTO_ARENA ccb_arena *ar2 = ccb_arena_create();
  ccb_argv av2;
  memset(&av2, 0, sizeof(av2));
  av2.ar = ar2;
  ccb_argv_push(&av2, p->sdk->aapt2);
  ccb_argv_push(&av2, "dump");
  ccb_argv_push(&av2, "badging");
  ccb_argv_push(&av2, apk_path);
  rc = ccb_run_tool(p, "verify", "CCB-E3011", av2.v, NULL,
    "the APK failed aapt2 badging; it is not a well-formed app package");
  free(av2.v);
  if (rc != CCB_OK) {
    free(hash);
    free(apk_path);
    return CCB_ERR_BUILD;
  }
  if (!p->opts->dry_run && hash != NULL) {
    ccb_cache_set_step(p->cache, verify_key, hash);
    p->cache_dirty = 1;
  }
  free(hash);
  free(apk_path);
  p->steps_run++;
  return CCB_OK;
}

// ---------- entry point + cleanup ----------

static void ccb_pipe_free(ccb_pipe *p) {
  ccb_config_free(p->cfg);
  ccb_sdk_free(p->sdk);
  ccb_cache_free(p->cache);
  free(p->intermediates);
  free(p->gen_dir);
  free(p->classes_dir);
  free(p->dex_dir);
  free(p->aar_dir);
  free(p->out_dir);
  free(p->unsigned_apk);
  free(p->aligned_apk);
  free(p->linked_apk);
  free(p->linked_aab);
  free(p->unsigned_aab);
  free(p->classes_fp);
  free(p->tool_fingerprint);
  free(p->merged_manifest);
  free(p->merge_fp);
  ccb_free_str_array(p->aar_res_dirs, p->aar_res_dirs_count);
  ccb_free_str_array(p->aar_res_zips, p->aar_res_zips_count);
  ccb_free_str_array(p->aar_class_jars, p->aar_class_jars_count);
  ccb_free_str_array(p->aar_packages, p->aar_packages_count);
  ccb_free_str_array(p->aar_assets_dirs, p->aar_assets_count);
  ccb_free_str_array(p->aar_jni_dirs, p->aar_jni_count);
}

void ccb_build_result_free(ccb_build_result *r) {
  free(r->apk_path);
  r->apk_path = NULL;
}

ccb_status ccb_pipeline_build(const ccb_build_opts *opts, ccb_build_result *result, ccb_diag_list *diags) {
  memset(result, 0, sizeof(*result));
  ccb_pipe p;
  memset(&p, 0, sizeof(p));
  p.opts = opts;
  p.diags = diags;
  ccb_log_init(&p.log, opts->json, opts->verbose);
  p.log.explain = opts->explain;
  ccb_status rc = ccb_stage0(&p);
  if (rc != CCB_OK) {
    ccb_pipe_free(&p);
    return rc;
  }
  int deps_ran = 0;
  rc = ccb_step_deps(&p, &deps_ran);
  if (rc == CCB_OK) {
    rc = ccb_step_merge_manifest(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_compile_res(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_link(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_code(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_dex(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_native(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_package(&p);
  }
  // Bundles are not zipaligned: alignment is an APK-install concern,
  // and Play rejects aligned bundles. Sign consumes the unaligned
  // bundle directly.
  if (rc == CCB_OK && !p.opts->aab) {
    rc = ccb_step_align(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_sign(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_verify(&p);
  }
  if (rc == CCB_OK) {
    ccb_save_cache(&p);
    result->steps_run = p.steps_run;
    result->steps_skipped = p.steps_skipped;
    result->up_to_date = (p.steps_run == 0 && !opts->dry_run && !opts->explain) ? 1 : 0;
    result->aab = p.opts->aab;
    result->apk_path = ccb_path_join(p.out_dir, p.opts->aab ? "app.aab" : "app.apk");
  } else {
    // Save progress: completed steps stay cached so a retry resumes.
    ccb_save_cache(&p);
  }
  ccb_pipe_free(&p);
  return rc;
}
