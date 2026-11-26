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
#include "ccb_net.h"
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

// Append kotlinc compiler-plugin arguments: -Xplugin=<jar> per
// resolved plugin jar, then -P plugin:<id>:<key>=<value> per
// configured option. The option string is "plugin-id:key=value";
// the "-P plugin:" prefix is added here, so the config never
// carries kotlinc's flag syntax.
static void ccb_push_plugin_args(ccb_pipe *p, ccb_argv *av) {
  for (size_t i = 0; i < p->cfg->kotlin_plugin_jars_count; i++) {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "-Xplugin=");
    ccb_str_append(&s, p->cfg->kotlin_plugin_jars[i]);
    ccb_argv_push(av, s.buf);
    ccb_str_free(&s);
  }
  for (size_t i = 0; i < p->cfg->kotlin_plugin_options_count; i++) {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "-P plugin:");
    ccb_str_append(&s, p->cfg->kotlin_plugin_options[i]);
    ccb_argv_push(av, s.buf);
    ccb_str_free(&s);
  }
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

// Record resolved plugin jars (by content hash, when present) and
// plugin options (by value) in a compile fingerprint, so a plugin
// change recompiles instead of serving stale classes.
static void ccb_fp_add_plugins(ccb_pipe *p, ccb_fp_builder *b,
    int *failed) {
  for (size_t i = 0; i < p->cfg->kotlin_plugin_jars_count && !*failed; i++) {
    if (ccb_path_exists(p->cfg->kotlin_plugin_jars[i]) &&
        ccb_fp_add_file(p, b, p->cfg->kotlin_plugin_jars[i]) != 0) {
      *failed = 1;
    }
  }
  for (size_t i = 0; i < p->cfg->kotlin_plugin_options_count; i++) {
    ccb_fp_add(b, "kotlin_plugin_option",
      p->cfg->kotlin_plugin_options[i]);
  }
}

static void ccb_pipe_diag(ccb_pipe *p, const char *code, const char *phase, const char *msg) {
  ccb_diag d = ccb_diag_make(code, phase, msg);
  ccb_diag_list_push(p->diags, &d);
}

// Tail of a tool's output: the actionable part. The returned pointer
// borrows err (never NULL).
static const char *ccb_tool_tail(const char *err) {
  size_t len = strlen(err);
  const char *tail = err;
  if (len > 2000) {
    tail = err + len - 2000;
    while (*tail != '\0' && *tail != '\n') {
      tail++;
    }
  }
  return tail;
}

// Record a tool failure diagnostic: `what` is the first sentence
// (e.g. "tool exited with code 1"), followed by the tail of the
// tool's output. cmd is owned by the diagnostic.
static void ccb_tool_diag(ccb_pipe *p, const char *code,
    const char *phase, char *cmd, const char *suggestion,
    const char *what, const char *tail) {
  ccb_str msg;
  ccb_str_init(&msg);
  ccb_str_append(&msg, what);
  if (tail[0] != '\0') {
    ccb_str_append(&msg, ": ");
    ccb_str_append(&msg, tail);
  }
  ccb_diag d = ccb_diag_make(code, phase, msg.buf);
  d.command = cmd;
  d.suggestion = ccb_xstrdup(suggestion);
  ccb_diag_list_push(p->diags, &d);
  ccb_str_free(&msg);
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
    char what[64];
    snprintf(what, sizeof(what), "tool exited with code %d", r.exit_code);
    ccb_tool_diag(p, code, phase, ccb_str_steal(&cmd), suggestion, what,
      ccb_tool_tail(r.err));
    ccb_proc_result_free(&r);
    return CCB_ERR_BUILD;
  }
  ccb_str_free(&cmd);
  ccb_proc_result_free(&r);
  return CCB_OK;
}

// Run kotlinc, via the daemon when enabled, else directly. The daemon
// is opt-in ([build] kotlinc_daemon, or --kotlinc-daemon /
// --no-kotlinc-daemon on the build command line). When it is enabled
// but unusable (jars missing, cannot start, cannot connect), the build
// falls back to plain kotlinc and only fails if that fails too, so a
// stale or crashed daemon never fails a build. A real compile error
// through the daemon fails the build directly, with the daemon's
// error output as the diagnostic: the client exits 0 on compile
// errors, so the "result code: 0" line in its output is the success
// signal, not the exit code.
static ccb_status ccb_run_kotlinc(ccb_pipe *p, char *const argv[],
    char *const env_extra[], const char *suggestion) {
  int daemon = p->cfg->kotlinc_daemon;
  if (p->opts->kotlinc_daemon_on) {
    daemon = 1;
  }
  if (p->opts->kotlinc_daemon_off) {
    daemon = 0;
  }
  if (!daemon) {
    return ccb_run_tool(p, "kotlinc", "CCB-E3004", argv, env_extra,
      suggestion);
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  char *run_dir = ccb_daemon_run_dir(ar, p->cfg->build_dir);
  char **dav = ccb_daemon_compile_argv(ar, p->sdk->java,
    p->sdk->kotlin_home, run_dir, p->cfg->kotlinc_daemon_idle_seconds,
    argv);
  if (dav == NULL) {
    ccb_log_info(&p->log,
      "kotlinc daemon jars missing, falling back to kotlinc");
    return ccb_run_tool(p, "kotlinc", "CCB-E3004", argv, env_extra,
      suggestion);
  }
  ccb_str cmd;
  ccb_str_init(&cmd);
  ccb_proc_render(dav, &cmd);
  if (p->opts->dry_run) {
    printf("%s\n", cmd.buf);
    ccb_str_free(&cmd);
    return CCB_OK;
  }
  if (ccb_mkdir_p(run_dir) != CCB_OK) {
    ccb_log_info(&p->log,
      "cannot create kotlinc daemon run dir, falling back to kotlinc");
    ccb_str_free(&cmd);
    return ccb_run_tool(p, "kotlinc", "CCB-E3004", argv, env_extra,
      suggestion);
  }
  ccb_log_phase_start(&p->log, "kotlinc");
  ccb_proc_result r;
  ccb_status rc = ccb_proc_run(dav, NULL, env_extra, &r);
  if (rc != CCB_OK) {
    ccb_log_info(&p->log,
      "kotlinc daemon did not start, falling back to kotlinc");
    ccb_str_free(&cmd);
    return ccb_run_tool(p, "kotlinc", "CCB-E3004", argv, env_extra,
      suggestion);
  }
  ccb_log_tool_output(&p->log, "kotlinc", r.out, r.err);
  if (r.exit_code == 0 && ccb_daemon_output_ok(r.out, r.err)) {
    ccb_str_free(&cmd);
    ccb_proc_result_free(&r);
    return CCB_OK;
  }
  if (r.exit_code == 0 && ccb_daemon_output_has_errors(r.out, r.err)) {
    const char *tail = ccb_tool_tail(r.err);
    if (strstr(r.err, "ERROR\t") == NULL) {
      tail = ccb_tool_tail(r.out);
    }
    ccb_tool_diag(p, "CCB-E3004", "kotlinc", ccb_str_steal(&cmd),
      suggestion, "kotlinc reported errors", tail);
    ccb_proc_result_free(&r);
    return CCB_ERR_BUILD;
  }
  ccb_log_info(&p->log,
    "kotlinc daemon unavailable, falling back to kotlinc");
  ccb_str_free(&cmd);
  ccb_proc_result_free(&r);
  return ccb_run_tool(p, "kotlinc", "CCB-E3004", argv, env_extra,
    suggestion);
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
    char *mpkg = ccb_manifest_package(mtext);
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
    // are freed here (result_free would free the paths too). The
    // lockfile entries are not consumed by the build, so they are
    // freed in full.
    free(mv.aars);
    free(mv.jars);
    ccb_maven_entries_free(mv.entries, mv.entries_count);
  }
  // Compiler plugins (cache-only: builds never touch the network).
  // Resolved jars are passed to kotlinc as -Xplugin=... in the
  // compile-code step (and the module compile step). A cache miss
  // fails here, before any stage runs, like a Maven miss.
  if (p->cfg->kotlin_plugins_count > 0) {
    char **pjars = NULL;
    size_t pjars_count = 0;
    if (ccb_maven_resolve_plugins(p->cfg, 0, p->opts->verbose,
        p->opts->dry_run || p->opts->explain, &pjars, &pjars_count,
        p->diags) != CCB_OK) {
      return CCB_ERR_TOOLCHAIN;
    }
    p->cfg->kotlin_plugin_jars = pjars;
    p->cfg->kotlin_plugin_jars_count = pjars_count;
  }
  // Directories.
  p->intermediates = ccb_path_join(p->cfg->build_dir, "intermediates");
  p->gen_dir = ccb_path_join(p->intermediates, "gen");
  p->classes_dir = ccb_path_join(p->intermediates, "classes");
  p->dex_dir = ccb_path_join(p->intermediates, "dex");
  p->aar_dir = ccb_path_join(p->intermediates, "aar");
  p->modules_dir = ccb_path_join(p->intermediates, "modules");
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

// ---------- library modules ----------

// ccb_jvm_env is defined with the kotlinc step below; the module step
// needs it first.
static char **ccb_jvm_env(ccb_pipe *p, ccb_arena *ar);

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
static ccb_module *ccb_pipe_module(ccb_pipe *p, const char *name) {
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

static ccb_status ccb_mod_definalize_r(const char *gen_dir) {
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
static int ccb_mod_fp_files(ccb_pipe *p, ccb_fp_builder *b, const char *dir,
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
static size_t ccb_mod_file_count(const char *dir, const char *ext) {
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(dir, ext, &files, &nfiles) != CCB_OK) {
    return 0;
  }
  ccb_free_str_array(files, nfiles);
  return nfiles;
}

// Add every file under dir to the zip writer as <prefix>/<relpath>.
// Files come sorted from ccb_collect_files, so the archive is
// deterministic. An empty prefix adds bare relative paths.
static ccb_status ccb_mod_zip_tree(ccb_zip_writer *w, const char *dir,
    const char *prefix) {
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(dir, NULL, &files, &nfiles) != CCB_OK) {
    return CCB_ERR_BUILD;
  }
  ccb_status rc = CCB_OK;
  size_t plen = strlen(dir);
  for (size_t i = 0; i < nfiles; i++) {
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

// Basename of a .class file: R classes (R.class, R$*.class) must never
// reach the dex input, because the app link generates the real R class
// for the module package and d8 rejects the duplicate.
static int ccb_mod_is_r_class(const char *path) {
  const char *base = strrchr(path, '/');
  base = base == NULL ? path : base + 1;
  if (strcmp(base, "R.class") == 0) {
    return 1;
  }
  return strncmp(base, "R$", 2) == 0;
}

// Register a module's outputs in the AAR machinery. Runs in every mode
// (explain, dry-run, real): downstream steps plan from these lists.
// classes.jar is registered only when the module has real sources;
// a resources-only module contributes no classes.
static void ccb_mod_register(ccb_pipe *p, ccb_module *m, const char *aar_path,
    const char *classes_jar) {
  ccb_push_str(&p->merge_aar_paths, &p->merge_aar_paths_count,
    &p->merge_aar_paths_cap, ccb_xstrdup(aar_path));
  ccb_push_str(&p->merge_aar_srcs, &p->merge_aar_srcs_count,
    &p->merge_aar_srcs_cap, ccb_xstrdup(m->manifest));
  ccb_str disp;
  ccb_str_init(&disp);
  ccb_str_append(&disp, "module ");
  ccb_str_append(&disp, m->name);
  ccb_push_str(&p->merge_aar_names, &p->merge_aar_names_count,
    &p->merge_aar_names_cap, ccb_str_steal(&disp));
  if (classes_jar != NULL) {
    ccb_push_str(&p->aar_class_jars, &p->aar_class_jars_count,
      &p->aar_class_jars_cap, ccb_xstrdup(classes_jar));
  }
  if (m->res != NULL) {
    ccb_push_str(&p->aar_res_dirs, &p->aar_res_dirs_count,
      &p->aar_res_dirs_cap, ccb_xstrdup(m->res));
  }
  if (m->assets != NULL) {
    ccb_push_str(&p->aar_assets_dirs, &p->aar_assets_count,
      &p->aar_assets_cap, ccb_xstrdup(m->assets));
  }
  ccb_push_str(&p->aar_packages, &p->aar_packages_count,
    &p->aar_packages_cap, ccb_xstrdup(m->package));
}

// ---------- one library module ----------

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
  int has_sources;
  int has_res_files;
} ccb_mod_ctx;

static void ccb_mod_ctx_init(ccb_mod_ctx *c, ccb_pipe *p, ccb_module *m) {
  c->p = p;
  c->m = m;
  c->work = ccb_path_join(p->modules_dir, m->name);
  ccb_str an;
  ccb_str_init(&an);
  ccb_str_append(&an, m->name);
  ccb_str_append(&an, ".aar");
  c->aar_path = ccb_path_join(c->work, an.buf);
  ccb_str_free(&an);
  c->gen_dir = ccb_path_join(c->work, "gen");
  c->classes_dir = ccb_path_join(c->work, "classes");
  c->classes_jar = ccb_path_join(c->work, "classes.jar");
  c->compiled = ccb_path_join(c->work, "compiled.zip");
  c->link_out = ccb_path_join(c->work, "lib.ap_");
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
}

// A module name must not collide with a declared AAR's extraction
// directory (aar/<base>), or the two would overwrite each other.
// Returns 1 on collision (diagnostic recorded), 0 otherwise.
static int ccb_mod_name_clash(ccb_pipe *p, ccb_module *m) {
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    char *base = ccb_basename(p->cfg->aars[i]);
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
      ccb_str_append(&msg, p->cfg->aars[i]);
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
  ccb_fp_add(&b, "build_tools", p->sdk->build_tools_version);
  ccb_fp_add(&b, "aapt2", p->sdk->aapt2 == NULL ? "" : p->sdk->aapt2);
  ccb_fp_add(&b, "kotlinc", p->sdk->kotlin_version);
  ccb_fp_add(&b, "jvm_target", p->cfg->jvm_target);
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
  for (size_t i = 0; i < p->cfg->jars_count && !failed; i++) {
    if (ccb_fp_add_file(p, &b, p->cfg->jars[i]) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < p->aar_class_jars_count && !failed; i++) {
    if (ccb_path_exists(p->aar_class_jars[i]) &&
        ccb_fp_add_file(p, &b, p->aar_class_jars[i]) != 0) {
      failed = 1;
    }
  }
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
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->aapt2);
  ccb_argv_push(&av, "compile");
  ccb_argv_push(&av, "--dir");
  ccb_argv_push(&av, m->res);
  ccb_argv_push(&av, "-o");
  ccb_argv_push(&av, c->compiled);
  ccb_status rc = ccb_run_tool(p, "aapt2-compile", "CCB-E3002", av.v,
    NULL, "check the module resource files for errors");
  free(av.v);
  if (rc != CCB_OK) {
    return rc;
  }
  if (!p->opts->dry_run && ccb_mkdir_p(c->gen_dir) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "modules",
      "cannot create module R.java directory");
    return CCB_ERR_BUILD;
  }
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->aapt2);
  ccb_argv_push(&av, "link");
  ccb_argv_push(&av, "-o");
  ccb_argv_push(&av, c->link_out);
  ccb_argv_push(&av, "--manifest");
  ccb_argv_push(&av, m->manifest);
  ccb_argv_push(&av, "-I");
  ccb_argv_push(&av, p->sdk->android_jar);
  ccb_argv_push(&av, "--java");
  ccb_argv_push(&av, c->gen_dir);
  ccb_argv_push(&av, "--auto-add-overlay");
  ccb_argv_push(&av, "--min-sdk-version");
  ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
  ccb_argv_push(&av, "--target-sdk-version");
  ccb_argv_pushf(&av, "%ld", p->cfg->target_sdk);
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

// Classpath: android.jar + declared jars + declared and already-built
// module AAR classes (dependencies register as they finish, in
// topological order).
static char *ccb_mod_classpath(ccb_mod_ctx *c) {
  ccb_pipe *p = c->p;
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
  return ccb_str_steal(&cp);
}

// Compile the module's sources with kotlinc, then its Java sources
// with javac. Takes ownership of cp_str and the collected file lists.
static ccb_status ccb_mod_run_kotlinc(ccb_mod_ctx *c, char *cp_str,
    ccb_mod_srcs *s) {
  ccb_pipe *p = c->p;
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  ccb_argv jav;
  ccb_str jcp;
  char *jcp_str = NULL;
  char **env = NULL;
  ccb_status rc = CCB_OK;
  if (!p->opts->dry_run && ccb_mkdir_p(c->classes_dir) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3016", "modules",
      "cannot create module classes directory");
    rc = CCB_ERR_BUILD;
    goto done;
  }
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->kotlinc);
  ccb_argv_push(&av, "-jvm-target");
  ccb_argv_push(&av, p->cfg->jvm_target);
  ccb_argv_push(&av, "-d");
  ccb_argv_push(&av, c->classes_dir);
  ccb_argv_push(&av, "-cp");
  ccb_argv_push(&av, cp_str);
  ccb_push_plugin_args(p, &av);
  for (size_t i = 0; i < s->nkt; i++) {
    ccb_argv_push(&av, s->kt[i]);
  }
  for (size_t i = 0; i < s->nrjava; i++) {
    ccb_argv_push(&av, s->rjava[i]);
  }
  for (size_t i = 0; i < s->nup; i++) {
    ccb_argv_push(&av, s->up[i]);
  }
  for (size_t i = 0; i < s->njava; i++) {
    ccb_argv_push(&av, s->java[i]);
  }
  env = ccb_jvm_env(p, ar);
  rc = ccb_run_kotlinc(p, av.v, env,
    "check the module Kotlin/Java sources for errors");
  free(av.v);
  if (rc != CCB_OK) {
    goto done;
  }
  if (s->njava > 0) {
    ccb_str_init(&jcp);
    ccb_str_append(&jcp, c->classes_dir);
    ccb_str_append_c(&jcp, ':');
    ccb_str_append(&jcp, cp_str);
    jcp_str = ccb_str_steal(&jcp);
    memset(&jav, 0, sizeof(jav));
    jav.ar = ar;
    ccb_argv_push(&jav, p->sdk->javac);
    ccb_argv_push(&jav, "-source");
    ccb_argv_push(&jav, p->cfg->jvm_target);
    ccb_argv_push(&jav, "-target");
    ccb_argv_push(&jav, p->cfg->jvm_target);
    ccb_argv_push(&jav, "-cp");
    ccb_argv_push(&jav, jcp_str);
    ccb_argv_push(&jav, "-d");
    ccb_argv_push(&jav, c->classes_dir);
    for (size_t i = 0; i < s->njava; i++) {
      ccb_argv_push(&jav, s->java[i]);
    }
    rc = ccb_run_tool(p, "javac", "CCB-E3005", jav.v, env,
      "check the module Java sources for errors");
    free(jav.v);
    free(jcp_str);
  }
done:
  ccb_mod_srcs_free(s);
  free(cp_str);
  return rc;
}

static ccb_status ccb_mod_compile_sources(ccb_mod_ctx *c) {
  char *cp_str = ccb_mod_classpath(c);
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
  size_t plen = strlen(c->classes_dir);
  int bad = 0;
  for (size_t i = 0; i < nall; i++) {
    if (ccb_mod_is_r_class(all[i])) {
      continue;
    }
    const char *rel = all[i] + plen + 1;
    if (ccb_zip_writer_add_file(&w, rel, all[i]) != CCB_OK) {
      bad = 1;
      break;
    }
  }
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
// resources and sources, pack classes.jar, assemble the AAR.
static ccb_status ccb_mod_run(ccb_mod_ctx *c, const char *fp,
    const char *key) {
  ccb_pipe *p = c->p;
  if (!p->opts->dry_run) {
    ccb_remove_recursive(c->work);
    if (ccb_mkdir_p(c->work) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "modules",
        "cannot create module work directory");
      return CCB_ERR_BUILD;
    }
  }
  if (c->has_sources && c->has_res_files &&
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
    ccb_cache_set_step(p->cache, key, fp);
    p->cache_dirty = 1;
  }
  p->steps_run++;
  return CCB_OK;
}

static ccb_status ccb_build_one_module(ccb_pipe *p, ccb_module *m) {
  if (ccb_mod_name_clash(p, m)) {
    return CCB_ERR_BUILD;
  }
  ccb_mod_ctx c;
  ccb_mod_ctx_init(&c, p, m);
  ccb_str key;
  ccb_str_init(&key);
  ccb_str_append(&key, "module-");
  ccb_str_append(&key, m->name);
  char *fp = NULL;
  const char *old = NULL;
  ccb_status rc = CCB_OK;
  if (ccb_mod_fingerprint(p, m, &fp, &old, key.buf) != 0) {
    ccb_pipe_diag(p, "CCB-E3016", "modules", "cannot read module inputs");
    rc = CCB_ERR_BUILD;
    goto done;
  }
  int run = 1;
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 &&
      ccb_path_exists(c.aar_path) &&
      (!c.has_sources || ccb_path_exists(c.classes_jar))) {
    run = 0;
  }
  if (p->opts->explain) {
    printf("module %s: %s\n", m->name,
      run ? "run (inputs changed)" : "skip (inputs unchanged)");
    ccb_mod_register(p, m, c.aar_path, c.has_sources ? c.classes_jar : NULL);
    goto done;
  }
  if (run) {
    if (ccb_mod_run(&c, fp, key.buf) != CCB_OK) {
      rc = CCB_ERR_BUILD;
      goto done;
    }
  } else {
    p->steps_skipped++;
    ccb_log_cache(&p->log, "modules", 1, "module inputs unchanged");
  }
  ccb_mod_register(p, m, c.aar_path, c.has_sources ? c.classes_jar : NULL);
done:
  free(fp);
  ccb_str_free(&key);
  ccb_mod_ctx_free(&c);
  return rc;
}

static ccb_status ccb_step_modules(ccb_pipe *p) {
  if (p->cfg->modules_count == 0) {
    if (p->opts->explain) {
      printf("modules: skip (no library modules)\n");
    }
    return CCB_OK;
  }
  for (size_t i = 0; i < p->cfg->modules_count; i++) {
    ccb_status rc = ccb_build_one_module(p, &p->cfg->modules[i]);
    if (rc != CCB_OK) {
      return rc;
    }
  }
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

// Env for JVM tools: heap fallback + JAVA_HOME derived from the resolved
// java. The JAVA_TOOL_OPTIONS cap only binds tools whose launcher sets
// no heap of its own (R8, javac, keytool): the d8 wrapper defaults to
// -Xmx2G, so d8 gets a sized -J flag instead (see ccb_d8_heap_mb),
// and the kotlinc/apksigner wrappers set their own.
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
    // Classpath contents, not just paths: a changed dependency jar
    // (including a rebuilt library module) must recompile app code,
    // or stale app classes could reference changed library APIs. Jars
    // are hashed only when present: in dry-run on a fresh tree the
    // extracted AAR classes do not exist yet, and their absence must
    // not fail planning (a missing jar still fails the build at dex
    // time with a clear error).
    for (size_t i = 0; i < p->cfg->jars_count && !failed; i++) {
      if (ccb_path_exists(p->cfg->jars[i]) &&
          ccb_fp_add_file(p, &b, p->cfg->jars[i]) != 0) {
        failed = 1;
      }
    }
    for (size_t i = 0; i < p->aar_class_jars_count && !failed; i++) {
      if (ccb_path_exists(p->aar_class_jars[i]) &&
          ccb_fp_add_file(p, &b, p->aar_class_jars[i]) != 0) {
        failed = 1;
      }
    }
    ccb_fp_add_plugins(p, &b, &failed);
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
      ccb_push_plugin_args(p, &av);
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
      ccb_status rc = ccb_run_kotlinc(p, av.v, env,
        "check the Kotlin/Java sources for errors");
      free(av.v);
      if (rc != CCB_OK) {
        free(cp_str);
        free(fp);
        goto code_fail;
      }
    }
    // javac: compile the Java sources AND the generated R.java.
    // kotlinc resolves R symbols from the sources but emits no
    // R.class files for them (bug-aar: library R classes such as
    // androidx.recyclerview.R$attr were missing at runtime), so
    // javac must compile R.java too.
    if (njava > 0 || nrjava > 0) {
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
      for (size_t i = 0; i < nrjava; i++) {
        ccb_argv_push(&av, rjava_files[i]);
      }
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

// True when the project already feeds kotlin-stdlib to dex itself
// (a [deps] jar or a Maven-resolved jar), so auto-adding it would
// make d8/R8 reject the duplicate classes.
static int ccb_dex_has_stdlib(ccb_pipe *p) {
  for (size_t i = 0; i < p->cfg->jars_count; i++) {
    const char *base = strrchr(p->cfg->jars[i], '/');
    base = base != NULL ? base + 1 : p->cfg->jars[i];
    // kotlinc's own jar is "kotlin-stdlib.jar"; Maven's is
    // "kotlin-stdlib-<version>.jar" — both supply the same classes.
    if (strcmp(base, "kotlin-stdlib.jar") == 0 ||
        strncmp(base, "kotlin-stdlib-", 14) == 0) {
      return 1;
    }
  }
  return 0;
}

// True when the project has Kotlin sources.
static int ccb_dex_has_kotlin(ccb_pipe *p) {
  if (p->cfg->kotlin_dir == NULL) {
    return 0;
  }
  char **kt = NULL;
  size_t nkt = 0;
  ccb_collect_files(p->cfg->kotlin_dir, ".kt", &kt, &nkt);
  ccb_free_str_array(kt, nkt);
  return nkt > 0;
}

// The kotlinc stdlib jar ($KOTLIN_HOME/lib/kotlin-stdlib.jar) to add
// as a dex program input, or NULL when the project has no Kotlin
// sources, already supplies the stdlib, or kotlinc is unusable.
// kotlinc output references the runtime, but nothing ever fed it to
// d8/R8 as a program input (bug-stdlib: a no-deps Kotlin project
// shipped an APK that crashed on launch with NoClassDefFoundError:
// kotlin.jvm.internal.Intrinsics). Auto-freed by the caller.
static char *ccb_dex_stdlib(ccb_pipe *p) {
  if (!ccb_dex_has_kotlin(p) || ccb_dex_has_stdlib(p)) {
    return NULL;
  }
  if (p->sdk->kotlin_home == NULL) {
    return NULL;
  }
  char *stdlib = ccb_path_join3(p->sdk->kotlin_home, "lib", "kotlin-stdlib.jar");
  if (stdlib == NULL || access(stdlib, R_OK) != 0) {
    free(stdlib);
    return NULL;
  }
  return stdlib;
}

// ---------- d8 heap sizing (opt-d8-heap) ----------
//
// The d8 wrapper script defaults to -Xmx2G, and that command-line
// flag beats CCB's JAVA_TOOL_OPTIONS=-Xmx1g (the JVM applies
// JAVA_TOOL_OPTIONS first, command-line options last, and the last
// -Xmx wins — verified with -XX:+PrintFlagsFinal). So every d8 run
// used to reserve 2G, and four parallel builds committed 8G on a
// 6G VM. The wrapper takes a -J option instead: a leading
// -JXmx<N>m becomes the java -Xmx<N>m and suppresses the 2G
// default, so CCB sizes the heap from the actual dex inputs.
//
// Formula: heap = 256MB + 16MB per MiB of program inputs (app
// .class files + [deps] jars + AAR classes.jars + the stdlib input,
// cached .dex or jar), capped at 1024MB. Floor evidence (2026-11-25,
// build-tools 36.0.0): d8 succeeds at 128m on the Showcase warm
// inputs (2.6MB in, dex byte-identical to the 2G run) and on the
// stdlib jar alone (1.9MB), and at 256m on 10,000 synthetic classes
// + stdlib dex (8.2MB in) with no time penalty vs 1024m — so the
// 256MB floor is 2x the proven minimum for a normal build, and the
// cap keeps four parallel d8 runs at 4G. The heap never changes the
// dex output, so it is not part of the dex fingerprint. R8 is
// deliberately untouched: whole-program shrinking does not follow
// this input-size rule, and its 1G JAVA_TOOL_OPTIONS cap already
// equals the cap here.

// Heap in MB for a dex run over input_bytes of program inputs:
// floor + 16MB per whole MiB, capped. Pure and deterministic, so
// tests can call it directly.
long ccb_d8_heap_mb(uint64_t input_bytes) {
  uint64_t mib = input_bytes / (1024 * 1024);
  if (mib >= (CCB_D8_HEAP_CAP_MB - CCB_D8_HEAP_FLOOR_MB) / CCB_D8_HEAP_MB_PER_MIB) {
    return CCB_D8_HEAP_CAP_MB;
  }
  return CCB_D8_HEAP_FLOOR_MB + (long)(mib * CCB_D8_HEAP_MB_PER_MIB);
}

// Size of one file, or 0 when it is missing or unreadable. Missing
// inputs are 0, not an error: in dry-run mode the classes and jars
// do not exist yet, and the heap just falls back toward the floor.
static uint64_t ccb_dex_file_bytes(const char *path) {
  struct stat st;
  if (path == NULL || stat(path, &st) != 0 || st.st_size < 0) {
    return 0;
  }
  return (uint64_t)st.st_size;
}

// Total bytes of everything d8 will read as program inputs: the app
// .class files, [deps] jars, AAR classes.jars, and the stdlib input
// (cached .dex when one is in use, else the jar).
static uint64_t ccb_dex_input_bytes(ccb_pipe *p, const char *stdlib_input) {
  uint64_t total = 0;
  char **cfiles = NULL;
  size_t ncfiles = 0;
  if (ccb_path_is_dir(p->classes_dir)) {
    ccb_collect_files(p->classes_dir, ".class", &cfiles, &ncfiles);
  }
  for (size_t i = 0; i < ncfiles; i++) {
    total += ccb_dex_file_bytes(cfiles[i]);
  }
  ccb_free_str_array(cfiles, ncfiles);
  for (size_t i = 0; i < p->cfg->jars_count; i++) {
    total += ccb_dex_file_bytes(p->cfg->jars[i]);
  }
  for (size_t i = 0; i < p->aar_class_jars_count; i++) {
    total += ccb_dex_file_bytes(p->aar_class_jars[i]);
  }
  total += ccb_dex_file_bytes(stdlib_input);
  return total;
}

// ---------- stdlib dex cache (opt-dex / opt-cache) ----------
//
// d8 on the full kotlin-stdlib jar takes ~40s on this VM, on every
// clean build, even though the result never changes. Dex it once into
// the global cache ($XDG_CACHE_HOME/ccb/dex-stdlib, else
// ~/.cache/ccb/dex-stdlib — the same root as the Maven cache, so it
// survives `ccb clean`) and pass the cached .dex to d8 as a program
// input instead of the jar: d8 merges .dex inputs with the app
// classes (verified by hand: a merged dex holds both Intrinsics and
// the app class, in ~8s instead of ~42s).
//
// The entry is one file, <key>.dex, published by atomic rename, so a
// reader never sees a partial file. The key is a fingerprint of the
// stdlib jar hash + min-api + build-tools version, so a kotlinc
// upgrade, a min-api change, or a build-tools upgrade all miss and
// re-dex. A missing or corrupt entry falls back to the jar: the
// build never fails because of the cache, it just goes slow again.
// R8 is excluded: it takes the stdlib as a program-input JAR, not
// dex, so release + minify keeps the jar path exactly as before.

// Cache key for a dexed stdlib. NULL when the jar cannot be hashed.
static char *ccb_stdlib_dex_key(ccb_pipe *p, const char *stdlib) {
  char *hash = ccb_cache_hash_file(stdlib);
  if (hash == NULL) {
    return NULL;
  }
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  ccb_fp_add(&b, "stdlib", hash);
  ccb_fp_addl(&b, "min_sdk", p->cfg->min_sdk);
  ccb_fp_add(&b, "build_tools", p->sdk->build_tools_version);
  char *key = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
  ccb_fp_free(&b);
  free(hash);
  return key;
}

// Global cache dir for dexed stdlibs, or NULL when no cache base
// exists. The caller frees the result.
static char *ccb_stdlib_dex_dir(void) {
  char *root = ccb_net_cache_root();
  if (root == NULL) {
    return NULL;
  }
  char *dir = ccb_path_join(root, "dex-stdlib");
  free(root);
  return dir;
}

// True when path is a whole dex file: the dex magic plus a header
// file_size that matches the file on disk. A truncated or foreign
// file fails, so it is re-dexed instead of fed to d8.
static int ccb_dex_file_valid(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return 0;
  }
  unsigned char head[112];
  size_t n = fread(head, 1, sizeof(head), f);
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long size = ftell(f);
  fclose(f);
  if (n != sizeof(head) || size < 0) {
    return 0;
  }
  // Dex magic is "dex\n" + 3 version digits + NUL, e.g. "dex\n038".
  // ("dex\n0" as a literal is only 5 bytes, so compare the prefix
  // and the first version digit instead.)
  if (memcmp(head, "dex\n", 4) != 0 || head[4] != '0') {
    return 0;
  }
  uint32_t file_size = (uint32_t)head[32] | ((uint32_t)head[33] << 8) |
    ((uint32_t)head[34] << 16) | ((uint32_t)head[35] << 24);
  return file_size == (uint32_t)size;
}

// Dex the stdlib jar alone into a temp dir, then atomically publish
// the resulting classes.dex as dest. Returns 0 on success, -1 on
// any failure (temp dir removed, dest untouched). The caller falls
// back to the jar on -1, so this never fails a build. A stdlib too
// big for one dex (d8 without --multidex refuses, or emits a
// classes2.dex) also returns -1: the jar path handles it.
static int ccb_stdlib_dex_populate(ccb_pipe *p, const char *stdlib, const char *dest) {
  char *dir = ccb_stdlib_dex_dir();
  if (dir == NULL) {
    return -1;
  }
  if (ccb_mkdir_p(dir) != CCB_OK) {
    free(dir);
    return -1;
  }
  char *tmp = NULL;
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, dir);
    ccb_str_append(&s, "/.tmp-");
    ccb_str_appendf(&s, "%ld", (long)getpid());
    tmp = ccb_str_steal(&s);
  }
  if (ccb_mkdir_p(tmp) != CCB_OK) {
    free(tmp);
    free(dir);
    return -1;
  }
  int rc = -1;
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->d8);
  // Sized heap for the stdlib-only dex (see ccb_d8_heap_mb): the
  // input here is just the stdlib jar, so this lands on the floor.
  ccb_argv_pushf(&av, "-JXmx%ldm", ccb_d8_heap_mb(ccb_dex_file_bytes(stdlib)));
  ccb_argv_push(&av, "--min-api");
  ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
  ccb_argv_push(&av, "--lib");
  ccb_argv_push(&av, p->sdk->android_jar);
  ccb_argv_push(&av, "--output");
  ccb_argv_push(&av, tmp);
  ccb_argv_push(&av, stdlib);
  char **env = ccb_jvm_env(p, ar);
  ccb_proc_result r;
  // Deliberately NOT ccb_run_tool: a failed cache population must
  // not record a build diagnostic, it just falls back to the jar.
  if (ccb_proc_run(av.v, NULL, env, &r) == CCB_OK) {
    if (r.exit_code == 0) {
      char *dex = ccb_path_join(tmp, "classes.dex");
      char *dex2 = ccb_path_join(tmp, "classes2.dex");
      if (dex != NULL && ccb_path_exists(dex) && dex2 != NULL && !ccb_path_exists(dex2) &&
          ccb_dex_file_valid(dex) && rename(dex, dest) == 0) {
        rc = 0;
      }
      free(dex);
      free(dex2);
    }
    ccb_proc_result_free(&r);
  }
  free(av.v);
  ccb_remove_recursive(tmp);
  free(tmp);
  free(dir);
  return rc;
}

// The stdlib input for d8: the cached dex when one is valid (or can
// be made), else NULL to mean "use the jar". Never fails the build.
static char *ccb_stdlib_dex_input(ccb_pipe *p, const char *stdlib) {
  char *key = ccb_stdlib_dex_key(p, stdlib);
  if (key == NULL) {
    return NULL;
  }
  char *dir = ccb_stdlib_dex_dir();
  if (dir == NULL) {
    free(key);
    return NULL;
  }
  char *name = NULL;
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, key);
    ccb_str_append(&s, ".dex");
    name = ccb_str_steal(&s);
  }
  char *path = ccb_path_join(dir, name);
  free(name);
  free(dir);
  if (path == NULL) {
    free(key);
    return NULL;
  }
  if (ccb_dex_file_valid(path)) {
    ccb_log_cache(&p->log, "dex", 1, "stdlib dex cache hit");
    free(key);
    return path;
  }
  if (!p->opts->dry_run && ccb_stdlib_dex_populate(p, stdlib, path) == 0) {
    ccb_log_info(&p->log, "stdlib dex cached: %s", path);
    free(key);
    return path;
  }
  free(path);
  free(key);
  return NULL;
}

// ---------- R8 keep rules (opt-apk) ----------
//
// A minify build runs R8, which tree-shakes everything not reachable
// from a kept entry point -- including manifest components and layout
// view classes, which no code references. With no keeps at all R8
// shrinks the whole program away and writes no dex (verified on the
// Showcase: the dex dir held only resource files). CCB therefore
// generates the keep rules for those entry points, so the project's
// own proguard_rules file only needs the keeps R8 cannot infer at
// all: reflection-only references via Class.forName strings. The
// generated file lives at build/intermediates/r8-keep.pro and is
// passed to R8 ahead of the project's rules. Its text is part of the
// dex fingerprint, so a manifest or layout change re-runs dex.
//
// Collected, in order: the app manifest's entry points, every AAR
// manifest's entry points (declared AARs from their zips, library
// modules from their manifest files -- the same union the merged
// manifest holds, so the rules are identical in real, explain, and
// dry-run modes), and the view classes referenced from layout XML
// (app + module res dirs, declared AAR zips). The stdlib dex cache
// stays bypassed on this path, as before: R8 takes the stdlib as a
// program-input JAR, not dex, because only R8 can see which stdlib
// classes the kept code actually reaches.

static void ccb_r8_push(char ***arr, size_t *count, size_t *cap, const char *s) {
  if (*count == *cap) {
    size_t ncap = *cap == 0 ? 8 : *cap * 2;
    *arr = ccb_xrealloc(*arr, ncap * sizeof(**arr));
    *cap = ncap;
  }
  (*arr)[(*count)++] = ccb_xstrdup(s);
}

static int ccb_r8_is_name_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
    (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '$';
}

// True when s (len bytes) looks like a fully-qualified class name:
// a letter or underscore first, then name chars, with at least one dot.
static int ccb_r8_is_class_name(const char *s, size_t len) {
  if (len < 3) {
    return 0;
  }
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) {
    return 0;
  }
  int dot = 0;
  for (size_t i = 1; i < len; i++) {
    if (!ccb_r8_is_name_char(s[i])) {
      return 0;
    }
    if (s[i] == '.') {
      dot = 1;
    }
  }
  return dot;
}

// Index just past the markup starting at i ("<!--...-->", "<?...?>",
// "<!...>", "</...>"), or len when unterminated.
static size_t ccb_r8_skip_markup(const char *text, size_t len, size_t i) {
  if (i + 3 < len && strncmp(text + i, "<!--", 4) == 0) {
    size_t j = i + 4;
    while (j + 2 < len && strncmp(text + j, "-->", 3) != 0) {
      j++;
    }
    return j + 3 <= len ? j + 3 : len;
  }
  if (i + 1 < len && text[i + 1] == '?') {
    size_t j = i + 2;
    while (j + 1 < len && strncmp(text + j, "?>", 2) != 0) {
      j++;
    }
    return j + 2 <= len ? j + 2 : len;
  }
  size_t j = i + 1;
  while (j < len && text[j] != '>') {
    j++;
  }
  return j < len ? j + 1 : len;
}

// Class-valued layout attributes, scanned with their opening quote:
// <view class="...">, <fragment android:name="...">, and
// app:layout_behavior="..." (CoordinatorLayout behaviors).
// tools:context is design-time only and is deliberately not scanned.
static const char *ccb_r8_layout_attrs[] = {
  "class=\"", "class='",
  "android:name=\"", "android:name='",
  "layout_behavior=\"", "layout_behavior='",
};

// Attribute match at i: returns the characters consumed (through the
// end of the value) when text[i] starts a class-valued attribute,
// else 0.
static size_t ccb_r8_scan_attr(const char *text, size_t len, size_t i,
    char ***out, size_t *n, size_t *cap) {
  for (size_t a = 0; a < CCB_ARRAY_LEN(ccb_r8_layout_attrs); a++) {
    const char *attr = ccb_r8_layout_attrs[a];
    size_t alen = strlen(attr);
    if (i + alen > len || strncmp(text + i, attr, alen) != 0) {
      continue;
    }
    size_t start = i + alen;
    size_t j = start;
    while (j < len && text[j] != '"' && text[j] != '\'') {
      j++;
    }
    if (ccb_r8_is_class_name(text + start, j - start)) {
      char *name = ccb_xstrndup(text + start, j - start);
      ccb_r8_push(out, n, cap, name);
      free(name);
    }
    return j - i;
  }
  return 0;
}

// Scan one layout XML text for class references: element names with a
// dot (custom views) and the class-valued attributes above. Comments
// and processing instructions are skipped, so a dotted name inside a
// comment never becomes a keep.
static void ccb_r8_scan_layout_text(const char *text, char ***out, size_t *n, size_t *cap) {
  size_t len = strlen(text);
  size_t i = 0;
  while (i < len) {
    if (text[i] == '<' && i + 1 < len &&
        (text[i + 1] == '!' || text[i + 1] == '?' || text[i + 1] == '/')) {
      i = ccb_r8_skip_markup(text, len, i);
      continue;
    }
    if (text[i] == '<') {
      size_t start = i + 1;
      size_t j = start;
      while (j < len && (ccb_r8_is_name_char(text[j]) || text[j] == '-' || text[j] == ':')) {
        j++;
      }
      if (j > start && ccb_r8_is_class_name(text + start, j - start)) {
        char *name = ccb_xstrndup(text + start, j - start);
        ccb_r8_push(out, n, cap, name);
        free(name);
      }
      i = j;
      continue;
    }
    size_t adv = ccb_r8_scan_attr(text, len, i, out, n, cap);
    i += adv == 0 ? 1 : adv;
  }
}

// True when path sits directly under a layout dir ("layout" or
// "layout-...").
static int ccb_r8_is_layout_file(const char *path) {
  char *dir = ccb_dirname(path);
  char *base = ccb_basename(dir);
  int ok = strncmp(base, "layout", 6) == 0 && (base[6] == '\0' || base[6] == '-');
  free(dir);
  free(base);
  return ok;
}

// Scan the layout XML files under one res dir. A missing res dir
// contributes nothing. Returns 0 on success, -1 on a read failure.
static int ccb_r8_scan_layout_dir(const char *res_dir, char ***out, size_t *n, size_t *cap) {
  if (res_dir == NULL) {
    return 0;
  }
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(res_dir, ".xml", &files, &nfiles) != CCB_OK) {
    return -1;
  }
  int rc = 0;
  for (size_t i = 0; i < nfiles; i++) {
    if (!ccb_r8_is_layout_file(files[i])) {
      continue;
    }
    char *text = NULL;
    size_t tlen = 0;
    if (ccb_read_file(files[i], &text, &tlen) != CCB_OK) {
      rc = -1;
      break;
    }
    ccb_r8_scan_layout_text(text, out, n, cap);
    free(text);
  }
  ccb_free_str_array(files, nfiles);
  return rc;
}

// Scan the layout XML entries of one AAR zip (res/layout*/*.xml).
// An AAR with no layouts contributes nothing. Returns 0 on success,
// -1 when the zip cannot be opened or an entry cannot be read.
static int ccb_r8_scan_aar_layouts(const char *aar_path, char ***out, size_t *n, size_t *cap) {
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(aar_path, &a) != CCB_OK) {
    return -1;
  }
  int rc = 0;
  for (size_t i = 0; i < a->count && rc == 0; i++) {
    const ccb_zip_entry *e = &a->entries[i];
    if (strncmp(e->name, "res/layout", 10) != 0) {
      continue;
    }
    if (e->name[10] != '/' && e->name[10] != '-') {
      continue;
    }
    size_t nl = strlen(e->name);
    if (nl < 4 || strcmp(e->name + nl - 4, ".xml") != 0) {
      continue;
    }
    void *data = NULL;
    size_t dlen = 0;
    if (ccb_zip_read_entry(a, e, &data, &dlen) != CCB_OK) {
      rc = -1;
      break;
    }
    char *text = ccb_xmalloc(dlen + 1);
    memcpy(text, data, dlen);
    text[dlen] = '\0';
    free(data);
    ccb_r8_scan_layout_text(text, out, n, cap);
    free(text);
  }
  ccb_zip_close(a);
  return rc;
}

// Append one manifest document's entry points. manifest_path is a
// file on disk; fallback_pkg resolves relative names when the
// manifest declares no package (the app package, for AARs).
// Returns 0 on success, -1 on a read or parse failure.
static int ccb_r8_add_manifest_file(const char *manifest_path,
    const char *fallback_pkg, char ***out, size_t *n, size_t *cap) {
  char *text = NULL;
  size_t tlen = 0;
  if (ccb_read_file(manifest_path, &text, &tlen) != CCB_OK) {
    return -1;
  }
  char **names = NULL;
  size_t nnames = 0;
  char *err = NULL;
  int rc = ccb_manifest_entry_points(text, fallback_pkg, &names, &nnames, &err);
  free(err);
  free(text);
  if (rc != 0) {
    return -1;
  }
  for (size_t i = 0; i < nnames; i++) {
    ccb_r8_push(out, n, cap, names[i]);
  }
  ccb_free_str_array(names, nnames);
  return 0;
}

// Append one AAR manifest's entry points from the AAR zip. An AAR
// with no manifest contributes nothing. Returns 0 on success, -1
// when the zip cannot be read or the manifest does not parse.
static int ccb_r8_add_aar_manifest(const char *aar_path,
    const char *fallback_pkg, char ***out, size_t *n, size_t *cap) {
  int failed = 0;
  char *text = ccb_aar_manifest_text(aar_path, &failed);
  if (failed) {
    return -1;
  }
  if (text == NULL) {
    return 0;
  }
  char **names = NULL;
  size_t nnames = 0;
  char *err = NULL;
  int rc = ccb_manifest_entry_points(text, fallback_pkg, &names, &nnames, &err);
  free(err);
  free(text);
  if (rc != 0) {
    return -1;
  }
  for (size_t i = 0; i < nnames; i++) {
    ccb_r8_push(out, n, cap, names[i]);
  }
  ccb_free_str_array(names, nnames);
  return 0;
}

// Collect every class name the keep rules must cover: manifest entry
// points (app + AARs + modules) and layout view classes (app + AARs
// + modules). Returns 0 on success, -1 when an input cannot be read
// or a manifest does not parse.
static int ccb_r8_collect_names(ccb_pipe *p, char ***out, size_t *n, size_t *cap) {
  if (ccb_r8_add_manifest_file(p->cfg->manifest, NULL, out, n, cap) != 0) {
    return -1;
  }
  if (ccb_r8_scan_layout_dir(p->cfg->res, out, n, cap) != 0) {
    return -1;
  }
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    if (ccb_r8_add_aar_manifest(p->cfg->aars[i], p->cfg->package, out, n, cap) != 0) {
      return -1;
    }
    if (ccb_r8_scan_aar_layouts(p->cfg->aars[i], out, n, cap) != 0) {
      return -1;
    }
  }
  for (size_t i = 0; i < p->cfg->modules_count; i++) {
    ccb_module *m = &p->cfg->modules[i];
    if (ccb_r8_add_manifest_file(m->manifest, p->cfg->package, out, n, cap) != 0) {
      return -1;
    }
    if (ccb_r8_scan_layout_dir(m->res, out, n, cap) != 0) {
      return -1;
    }
  }
  return 0;
}

static int ccb_r8_cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// The generated keep rules text, or NULL when an input cannot be
// read or a manifest does not parse. The caller frees the result.
static char *ccb_r8_keep_rules(ccb_pipe *p) {
  char **names = NULL;
  size_t n = 0;
  size_t cap = 0;
  if (ccb_r8_collect_names(p, &names, &n, &cap) != 0) {
    ccb_free_str_array(names, n);
    return NULL;
  }
  if (n > 1) {
    qsort(names, n, sizeof(*names), ccb_r8_cmp_str);
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    if (kept > 0 && strcmp(names[i], names[kept - 1]) == 0) {
      free(names[i]);
      continue;
    }
    names[kept++] = names[i];
  }
  n = kept;
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s,
    "# Generated by CCB for release + minify: entry points R8 cannot\n"
    "# see from code (manifest components, layout view classes). The\n"
    "# app's own classes are kept whole by the package rule below;\n"
    "# the project's own proguard_rules file is passed to R8 as well\n"
    "# -- put reflection-only keeps (Class.forName strings) there, CCB\n"
    "# cannot infer them.\n");
  ccb_str_append(&s, "-keep class ");
  ccb_str_append(&s, p->cfg->package);
  ccb_str_append(&s, ".** { *; }\n");
  size_t plen = strlen(p->cfg->package);
  for (size_t i = 0; i < n; i++) {
    if (strncmp(names[i], p->cfg->package, plen) == 0 && names[i][plen] == '.') {
      continue;
    }
    ccb_str_append(&s, "-keep class ");
    ccb_str_append(&s, names[i]);
    ccb_str_append(&s, " { *; }\n");
  }
  ccb_free_str_array(names, n);
  return ccb_str_steal(&s);
}

static ccb_status ccb_step_dex(ccb_pipe *p) {
  CCB_AUTO_FREE char *stdlib = ccb_dex_stdlib(p);
  int use_r8 = strcmp(p->cfg->variant, "release") == 0 && p->cfg->minify;
  if (use_r8 && p->sdk->r8_jar == NULL) {
    ccb_pipe_diag(p, "CCB-E3014", "r8",
      "release minification needs r8: set [sdk] r8_jar to an r8 jar "
      "(com.android.tools:r8 from Google's Maven repository), or run "
      "`ccb setup` in a minify project to install it");
    return CCB_ERR_BUILD;
  }
  // Generated keep rules for R8 (opt-apk): manifest components,
  // layout view classes, and the app's own package. Computed before
  // the fingerprint so the rules text is part of it.
  CCB_AUTO_FREE char *keep_rules = NULL;
  if (use_r8) {
    keep_rules = ccb_r8_keep_rules(p);
    if (keep_rules == NULL) {
      ccb_pipe_diag(p, "CCB-E3017", "r8",
        "cannot generate R8 keep rules from the manifest and layouts");
      return CCB_ERR_BUILD;
    }
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
    if (!failed) {
      ccb_fp_add(&b, "r8-keep", keep_rules);
    }
  }
  for (size_t i = 0; i < p->cfg->jars_count && !failed; i++) {
    if (ccb_fp_add_file(p, &b, p->cfg->jars[i]) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < p->aar_class_jars_count && !failed; i++) {
    // In explain/dry-run mode on a fresh tree, a library module's
    // classes.jar does not exist yet (modules never execute in
    // planning modes); skipping it here keeps planning working, and
    // the missing jar still fails the real build at d8 time.
    if ((p->opts->explain || p->opts->dry_run) &&
        !ccb_path_exists(p->aar_class_jars[i])) {
      continue;
    }
    if (ccb_fp_add_file(p, &b, p->aar_class_jars[i]) != 0) {
      failed = 1;
    }
  }
  if (!failed && stdlib != NULL && ccb_fp_add_file(p, &b, stdlib) != 0) {
    failed = 1;
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
  // The generated keep rules file (R8 only): written in real mode,
  // named in the command in dry-run mode like every other
  // intermediate the command references.
  CCB_AUTO_FREE char *keep_path = NULL;
  if (use_r8) {
    keep_path = ccb_path_join(p->intermediates, "r8-keep.pro");
    if (!p->opts->dry_run &&
        ccb_write_file(keep_path, keep_rules, strlen(keep_rules)) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "r8", "cannot write generated R8 keep rules");
      free(fp);
      return CCB_ERR_BUILD;
    }
  }
  // The stdlib input for d8: the cached dex when one is valid, else
  // the jar. R8 takes the jar (program input), never a dex.
  CCB_AUTO_FREE char *stdlib_dex = NULL;
  const char *stdlib_input = stdlib;
  if (!use_r8 && stdlib != NULL) {
    stdlib_dex = ccb_stdlib_dex_input(p, stdlib);
    if (stdlib_dex != NULL) {
      stdlib_input = stdlib_dex;
    }
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  if (use_r8) {
    // R8 shrinks + obfuscates + dexes in one pass. The generated
    // keep rules (above) keep the manifest components, layout view
    // classes, and the app's own classes; the project's proguard
    // rules only add the keeps CCB cannot infer (reflection).
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
    // tolerates them), so the annotations jar kotlinc output references
    // must be a library input too. The stdlib itself is a program
    // input now (added below, like the d8 path), never a --lib: the
    // same jar as both would be a duplicate. When it is not a
    // program input anywhere (no Kotlin sources and no [deps]/Maven
    // stdlib jar), it stays a --lib so R8 can still resolve Kotlin
    // references from library code.
    if (stdlib == NULL && !ccb_dex_has_stdlib(p)) {
      char *sl = ccb_path_join3(p->sdk->kotlin_home, "lib", "kotlin-stdlib.jar");
      if (sl != NULL && access(sl, R_OK) == 0) {
        ccb_argv_push(&av, "--lib");
        ccb_argv_push(&av, sl);
      }
      free(sl);
    }
    {
      char *annot = ccb_path_join3(p->sdk->kotlin_home, "lib", "annotations-13.0.jar");
      if (annot != NULL && access(annot, R_OK) == 0) {
        ccb_argv_push(&av, "--lib");
        ccb_argv_push(&av, annot);
      }
      free(annot);
    }
    ccb_argv_push(&av, "--pg-conf");
    ccb_argv_push(&av, keep_path);
    ccb_argv_push(&av, "--pg-conf");
    ccb_argv_push(&av, p->cfg->proguard_rules);
    ccb_argv_push(&av, "--output");
    ccb_argv_push(&av, p->dex_dir);
  } else {
    ccb_argv_push(&av, p->sdk->d8);
    // Sized heap (see ccb_d8_heap_mb): the -J flag must lead, so
    // the wrapper takes it as the java heap instead of its 2G
    // default, and it also beats JAVA_TOOL_OPTIONS=-Xmx1g.
    ccb_argv_pushf(&av, "-JXmx%ldm",
      ccb_d8_heap_mb(ccb_dex_input_bytes(p, stdlib_input)));
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
  if (stdlib_input != NULL) {
    ccb_argv_push(&av, stdlib_input);
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

// ---------- dexcheck ----------
//
// Post-build check (change-dexcheck): every class the dex
// references must be defined in the dex, or the build stops here,
// before packaging. This is what the Intrinsics and RecyclerView
// R$attr launch crashes were: the APK built and verified fine,
// then crashed on the phone. The check runs even when the dex
// step was cache-skipped — the dex files on disk are still the
// real output — and it is milliseconds, so it has no cache of
// its own. It also runs for AAB builds: it sits before package,
// which is the shared packaging step for both formats.
static ccb_status ccb_step_dexcheck(ccb_pipe *p) {
  if (p->opts->explain) {
    printf("dexcheck: run (verify dex classes)\n");
    return CCB_OK;
  }
  if (p->opts->dry_run) {
    return CCB_OK;
  }
  // Mirror the dex step's skip case: with no classes or jars at
  // all there is no dex to check.
  char **class_files = NULL;
  size_t nclass = 0;
  if (ccb_path_is_dir(p->classes_dir)) {
    ccb_collect_files(p->classes_dir, ".class", &class_files, &nclass);
  }
  if (nclass == 0 && p->cfg->jars_count == 0 &&
      p->aar_class_jars_count == 0) {
    ccb_log_info(&p->log, "no classes or jars, skipping dexcheck");
    ccb_free_str_array(class_files, nclass);
    return CCB_OK;
  }
  ccb_free_str_array(class_files, nclass);
  char **missing = NULL;
  size_t nmissing = 0;
  char *err = NULL;
  if (ccb_dexcheck_dir(p->dex_dir, &missing, &nmissing, &err) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3025", "dexcheck", err);
    free(err);
    return CCB_ERR_BUILD;
  }
  if (nmissing > 0) {
    ccb_str m;
    ccb_str_init(&m);
    ccb_str_appendf(&m,
      "dex references %zu class(es) missing from the dex: ", nmissing);
    size_t shown = nmissing < 10 ? nmissing : 10;
    for (size_t i = 0; i < shown; i++) {
      if (i > 0) {
        ccb_str_append(&m, ", ");
      }
      ccb_str_append(&m, missing[i]);
    }
    if (nmissing > shown) {
      ccb_str_appendf(&m, " (+%zu more)", nmissing - shown);
    }
    ccb_str_append(&m,
      "; build stops before packaging — a missing class crashes "
      "at launch with NoClassDefFoundError");
    ccb_pipe_diag(p, "CCB-E3025", "dexcheck", m.buf);
    ccb_str_free(&m);
    ccb_free_str_array(missing, nmissing);
    return CCB_ERR_BUILD;
  }
  ccb_free_str_array(missing, nmissing);
  // Deliberately NOT counted in steps_run: dexcheck has no cache
  // and runs on every build, so counting it would make a no-op
  // rebuild never report "up to date". It is a gate, not a
  // producing step; it only speaks on failure.
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

// ---------- package ----------

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

static void ccb_pkg_ctx_init(ccb_pkg_ctx *c, ccb_pipe *p, int is_aab) {
  memset(c, 0, sizeof(*c));
  c->p = p;
  c->is_aab = is_aab;
  // AAB layout: link output goes under base/ (manifest at
  // base/manifest/AndroidManifest.xml, resources.pb at base/resources.pb,
  // res/* at base/res/*), dex at base/dex/, assets at base/assets/,
  // native libs at base/lib/<abi>/, plus BundleConfig.pb at the root.
  // Cache key and intermediates are separate from the APK path.
  c->pkg_key = is_aab ? "package-aab" : "package";
  c->link_src = is_aab ? p->linked_aab : p->linked_apk;
  c->pkg_out = is_aab ? p->unsigned_aab : p->unsigned_apk;
  c->fmt_label = is_aab ? "AAB" : "APK";
  c->asset_prefix = is_aab ? "base/assets/" : "assets/";
  c->so_prefix = is_aab ? "base/lib/" : "lib/";
}

static void ccb_pkg_ctx_free(ccb_pkg_ctx *c) {
  ccb_free_str_array(c->dex_files, c->ndex);
  ccb_free_str_array(c->asset_paths, c->nassets);
  ccb_free_str_array(c->asset_names, c->nassets);
  ccb_free_str_array(c->so_paths, c->nso);
  ccb_free_str_array(c->so_names, c->nso);
}

static void ccb_pkg_push_asset(ccb_pkg_ctx *c, char *path, char *name) {
  if (c->nassets == c->assets_cap) {
    size_t ncap = c->assets_cap == 0 ? 16 : c->assets_cap * 2;
    c->asset_paths = ccb_xrealloc(c->asset_paths, ncap * sizeof(*c->asset_paths));
    c->asset_names = ccb_xrealloc(c->asset_names, ncap * sizeof(*c->asset_names));
    c->assets_cap = ncap;
  }
  c->asset_paths[c->nassets] = path;
  c->asset_names[c->nassets] = name;
  c->nassets++;
}

static void ccb_pkg_push_so(ccb_pkg_ctx *c, char *path, char *name) {
  if (c->nso == c->so_cap) {
    size_t ncap = c->so_cap == 0 ? 16 : c->so_cap * 2;
    c->so_paths = ccb_xrealloc(c->so_paths, ncap * sizeof(*c->so_paths));
    c->so_names = ccb_xrealloc(c->so_names, ncap * sizeof(*c->so_names));
    c->so_cap = ncap;
  }
  c->so_paths[c->nso] = path;
  c->so_names[c->nso] = name;
  c->nso++;
}

// Record one native library, flagging a duplicate archive name. A
// duplicate keeps the first entry; the conflict name is kept for the
// diagnostic (the last duplicate wins, as before).
static void ccb_pkg_add_so(ccb_pkg_ctx *c, char *path, char *name) {
  if (ccb_name_seen(c->so_names, c->nso, name)) {
    c->so_conflict = 1;
    c->so_conflict_name = name;
    free(path);
    return;
  }
  ccb_pkg_push_so(c, path, name);
}

static char *ccb_pkg_asset_name(ccb_pkg_ctx *c, const char *rel) {
  ccb_str an;
  ccb_str_init(&an);
  ccb_str_append(&an, c->asset_prefix);
  ccb_str_append(&an, rel);
  return ccb_str_steal(&an);
}

static char *ccb_pkg_so_name(ccb_pkg_ctx *c, const char *abi,
    const char *base) {
  ccb_str sn;
  ccb_str_init(&sn);
  ccb_str_append(&sn, c->so_prefix);
  ccb_str_append(&sn, abi);
  ccb_str_append_c(&sn, '/');
  ccb_str_append(&sn, base);
  return ccb_str_steal(&sn);
}

// Record one AAR asset, unless the app already provides an asset
// with the same archive name (app assets win). Takes ownership of
// path and name on either outcome.
static void ccb_pkg_add_aar_asset(ccb_pkg_ctx *c, char *path, char *name) {
  if (!ccb_name_seen(c->asset_names, c->nassets, name)) {
    ccb_pkg_push_asset(c, path, name);
    return;
  }
  free(name);
  free(path);
}

// Assets: app assets win over AAR assets with the same archive name.
static void ccb_pkg_collect_assets(ccb_pkg_ctx *c) {
  ccb_pipe *p = c->p;
  if (p->cfg->assets != NULL) {
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(p->cfg->assets, NULL, &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      const char *rel = files[i] + strlen(p->cfg->assets) + 1;
      ccb_pkg_push_asset(c, files[i], ccb_pkg_asset_name(c, rel));
    }
    free(files);
  }
  for (size_t ai = 0; ai < p->aar_assets_count; ai++) {
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(p->aar_assets_dirs[ai], NULL, &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      const char *rel = files[i] + strlen(p->aar_assets_dirs[ai]) + 1;
      ccb_pkg_add_aar_asset(c, files[i], ccb_pkg_asset_name(c, rel));
    }
    free(files);
  }
}

// Native .so files: intermediates/jni/<abi>/*.so + AAR jni.
// Subdirectories of an AAR jni dir are ABIs.
static void ccb_pkg_collect_so(ccb_pkg_ctx *c) {
  ccb_pipe *p = c->p;
  for (size_t ai = 0; ai < p->cfg->ndk_abis_count; ai++) {
    char *abi_dir = ccb_path_join3(p->intermediates, "jni", p->cfg->ndk_abis[ai]);
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(abi_dir, ".so", &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      char *base = ccb_basename(files[i]);
      char *sname = ccb_pkg_so_name(c, p->cfg->ndk_abis[ai], base);
      free(base);
      ccb_pkg_add_so(c, files[i], sname);
    }
    free(files);
    free(abi_dir);
  }
  for (size_t ai = 0; ai < p->aar_jni_count; ai++) {
    char **all = NULL;
    size_t nall = 0;
    ccb_collect_files(p->aar_jni_dirs[ai], ".so", &all, &nall);
    for (size_t i = 0; i < nall; i++) {
      // Path: <jni>/<abi>/<file>.so -> abi is the parent dir name.
      char *parent = ccb_dirname(all[i]);
      char *abi = ccb_basename(parent);
      free(parent);
      char *base = ccb_basename(all[i]);
      char *sname = ccb_pkg_so_name(c, abi, base);
      free(base);
      free(abi);
      ccb_pkg_add_so(c, all[i], sname);
    }
    free(all);
  }
}

// Fingerprint the packaging inputs: link output, dex files, assets,
// and native libraries (archive names + file contents). In
// explain/dry-run mode on a fresh tree the link output does not exist
// yet (link never executes in planning modes); skipping it keeps
// planning working, and a missing link output still fails the real
// build here with a clear error. Sets *fp_out (owned, NULL on
// failure) and *old_out (borrowed cache fingerprint, NULL on
// failure). Returns 0 on success, -1 when an input cannot be read.
static int ccb_pkg_fingerprint(ccb_pkg_ctx *c, char **fp_out,
    const char **old_out) {
  ccb_pipe *p = c->p;
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  int failed = 0;
  if ((p->opts->explain || p->opts->dry_run) &&
      !ccb_path_exists(c->link_src)) {
    // Skip: planning only.
  } else if (ccb_fp_add_file(p, &b, c->link_src) != 0) {
    failed = 1;
  }
  for (size_t i = 0; i < c->ndex && !failed; i++) {
    char *base = ccb_basename(c->dex_files[i]);
    ccb_fp_add(&b, base, "");
    free(base);
    if (ccb_fp_add_file(p, &b, c->dex_files[i]) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < c->nassets && !failed; i++) {
    ccb_fp_add(&b, c->asset_names[i], "");
    if (ccb_fp_add_file(p, &b, c->asset_paths[i]) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < c->nso && !failed; i++) {
    ccb_fp_add(&b, c->so_names[i], "");
    if (ccb_fp_add_file(p, &b, c->so_paths[i]) != 0) {
      failed = 1;
    }
  }
  *fp_out = NULL;
  *old_out = NULL;
  if (!failed) {
    *fp_out = ccb_cache_fingerprint((const char **)b.keys,
      (const char **)b.vals, b.count);
    *old_out = ccb_cache_step_fp(p->cache, c->pkg_key);
  }
  ccb_fp_free(&b);
  return failed ? -1 : 0;
}

// Copy the link output's entries into the package, renaming them for
// the AAB base/ layout. The archive must already be open.
static ccb_status ccb_pkg_write_linked(ccb_pkg_ctx *c, ccb_zip_writer *w,
    ccb_zip_archive *linked) {
  ccb_status rc = CCB_OK;
  for (size_t i = 0; i < linked->count && rc == CCB_OK; i++) {
    const ccb_zip_entry *e = &linked->entries[i];
    size_t nl = strlen(e->name);
    if (nl == 0 || e->name[nl - 1] == '/') {
      continue;
    }
    void *data = NULL;
    size_t len = 0;
    if (ccb_zip_read_entry(linked, e, &data, &len) != CCB_OK) {
      rc = CCB_ERR_BUILD;
      break;
    }
    char *arc_name = c->is_aab ? ccb_aab_entry_name(e->name) : ccb_xstrdup(e->name);
    if (ccb_zip_writer_add_data(w, arc_name, data, len) != CCB_OK) {
      rc = CCB_ERR_BUILD;
    }
    free(arc_name);
    free(data);
  }
  return rc;
}

// Read one file and add it to the package under arc_name.
static ccb_status ccb_pkg_write_file(ccb_zip_writer *w,
    const char *arc_name, const char *path) {
  char *text = NULL;
  size_t tlen = 0;
  if (ccb_read_file(path, &text, &tlen) != CCB_OK) {
    return CCB_ERR_BUILD;
  }
  ccb_status rc = ccb_zip_writer_add_data(w, arc_name, text, tlen);
  free(text);
  return rc;
}

// Write the unsigned package: link output entries, dex files, assets,
// native libraries, and (AAB only) BundleConfig.pb.
static ccb_status ccb_pkg_write(ccb_pkg_ctx *c) {
  ccb_pipe *p = c->p;
  ccb_zip_writer w;
  if (ccb_zip_writer_init(&w, c->pkg_out) != CCB_OK) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "cannot create unsigned ");
    ccb_str_append(&msg, c->fmt_label);
    ccb_pipe_diag(p, "CCB-E3016", "package", msg.buf);
    ccb_str_free(&msg);
    return CCB_ERR_BUILD;
  }
  ccb_zip_archive *linked = NULL;
  if (ccb_zip_open(c->link_src, &linked) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3007", "package", "cannot open linked resources");
    ccb_zip_writer_abort(&w);
    return CCB_ERR_BUILD;
  }
  ccb_status wrc = ccb_pkg_write_linked(c, &w, linked);
  ccb_zip_close(linked);
  for (size_t i = 0; i < c->ndex && wrc == CCB_OK; i++) {
    char *base = ccb_basename(c->dex_files[i]);
    char *arc_name = base;
    ccb_str dex_name;
    if (c->is_aab) {
      ccb_str_init(&dex_name);
      ccb_str_append(&dex_name, "base/dex/");
      ccb_str_append(&dex_name, base);
      arc_name = dex_name.buf;
    }
    if (ccb_pkg_write_file(&w, arc_name, c->dex_files[i]) != CCB_OK) {
      wrc = CCB_ERR_BUILD;
    }
    if (c->is_aab) {
      ccb_str_free(&dex_name);
    }
    free(base);
  }
  for (size_t i = 0; i < c->nassets && wrc == CCB_OK; i++) {
    if (ccb_pkg_write_file(&w, c->asset_names[i], c->asset_paths[i]) != CCB_OK) {
      wrc = CCB_ERR_BUILD;
    }
  }
  for (size_t i = 0; i < c->nso && wrc == CCB_OK; i++) {
    if (ccb_pkg_write_file(&w, c->so_names[i], c->so_paths[i]) != CCB_OK) {
      wrc = CCB_ERR_BUILD;
    }
  }
  if (c->is_aab && wrc == CCB_OK) {
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
    ccb_str_append(&msg, c->fmt_label);
    ccb_str_append(&msg, " entries");
    ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
    ccb_str_free(&msg);
    ccb_zip_writer_abort(&w);
    return CCB_ERR_BUILD;
  }
  if (ccb_zip_writer_finish(&w) != CCB_OK) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "cannot finish ");
    ccb_str_append(&msg, c->fmt_label);
    ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
    ccb_str_free(&msg);
    return CCB_ERR_BUILD;
  }
  return CCB_OK;
}

// Shared APK/AAB packaging: collect inputs, fingerprint, explain or
// cache-skip, then write the unsigned package.
static ccb_status ccb_package_format(ccb_pipe *p, int is_aab) {
  ccb_pkg_ctx c;
  ccb_pkg_ctx_init(&c, p, is_aab);
  if (ccb_path_is_dir(p->dex_dir)) {
    ccb_collect_files(p->dex_dir, ".dex", &c.dex_files, &c.ndex);
  }
  ccb_pkg_collect_assets(&c);
  ccb_pkg_collect_so(&c);
  char *fp = NULL;
  const char *old = NULL;
  int failed = ccb_pkg_fingerprint(&c, &fp, &old) != 0;
  ccb_status rc = CCB_OK;
  if (c.so_conflict) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "duplicate native library in ");
    ccb_str_append(&msg, c.fmt_label);
    ccb_str_append(&msg, ": ");
    ccb_str_append(&msg, c.so_conflict_name);
    ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
    ccb_str_free(&msg);
    free(c.so_conflict_name);
    free(fp);
    rc = CCB_ERR_BUILD;
    goto done;
  }
  if (failed) {
    ccb_pipe_diag(p, "CCB-E3007", "package", "cannot read packaging inputs");
    free(fp);
    rc = CCB_ERR_BUILD;
    goto done;
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
    goto done;
  }
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_path_exists(c.pkg_out)) {
    free(fp);
    p->package_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "package", 1, "packaging inputs unchanged");
    goto done;
  }
  p->package_will_run = 1;
  if (!p->opts->dry_run) {
    if (ccb_pkg_write(&c) != CCB_OK) {
      free(fp);
      rc = CCB_ERR_BUILD;
      goto done;
    }
    ccb_cache_set_step(p->cache, c.pkg_key, fp);
    free(fp);
    p->cache_dirty = 1;
    p->steps_run++;
  } else {
    free(fp);
    p->steps_run++;
  }
done:
  ccb_pkg_ctx_free(&c);
  return rc;
}

static ccb_status ccb_step_package(ccb_pipe *p) {
  return ccb_package_format(p, 0);
}

static ccb_status ccb_step_package_aab(ccb_pipe *p) {
  return ccb_package_format(p, 1);
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
  free(p->modules_dir);
  ccb_free_str_array(p->merge_aar_paths, p->merge_aar_paths_count);
  ccb_free_str_array(p->merge_aar_srcs, p->merge_aar_srcs_count);
  ccb_free_str_array(p->merge_aar_names, p->merge_aar_names_count);
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
    rc = ccb_step_modules(&p);
  }
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
    rc = ccb_step_dexcheck(&p);
  }
  if (rc == CCB_OK) {
    rc = ccb_step_native(&p);
  }
  if (rc == CCB_OK) {
    rc = p.opts->aab ? ccb_step_package_aab(&p) : ccb_step_package(&p);
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
