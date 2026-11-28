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
#include "ccb_graph.h"
#include "ccb_pipeline_graph.h"
#include <sys/stat.h>
#include <unistd.h>
#include "ccb_pipeline_internal.h"

// ccb_pipeline implementation, part 1: context, tool runner, stage 0,
// AAR deps, aapt2 compile + link.

// ---------- small helpers ----------

void ccb_argv_push(ccb_argv *a, const char *s) {
  if (a->n + 1 >= a->cap) {
    size_t ncap = a->cap == 0 ? 16 : a->cap * 2;
    a->v = ccb_xrealloc(a->v, ncap * sizeof(*a->v));
    a->cap = ncap;
  }
  a->v[a->n++] = ccb_arena_strdup(a->ar, s);
  a->v[a->n] = NULL;
}

void ccb_argv_pushf(ccb_argv *a, const char *fmt, long num) {
  char buf[64];
  snprintf(buf, sizeof(buf), fmt, num);
  ccb_argv_push(a, buf);
}

// Append kotlinc compiler-plugin arguments: -Xplugin=<jar> per
// resolved plugin jar, then -P plugin:<id>:<key>=<value> per
// configured option. The option string is "plugin-id:key=value";
// the "-P plugin:" prefix is added here, so the config never
// carries kotlinc's flag syntax.
void ccb_push_plugin_args(ccb_pipe *p, ccb_argv *av) {
  for (size_t i = 0; i < p->resolved.plugin_jars_count; i++) {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "-Xplugin=");
    ccb_str_append(&s, p->resolved.plugin_jars[i]);
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

void ccb_fp_add(ccb_fp_builder *b, const char *key, const char *val) {
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

void ccb_fp_addl(ccb_fp_builder *b, const char *key, long val) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld", val);
  ccb_fp_add(b, key, buf);
}

void ccb_fp_free(ccb_fp_builder *b) {
  ccb_free_str_array(b->keys, b->count);
  ccb_free_str_array(b->vals, b->count);
  memset(b, 0, sizeof(*b));
}

// Hash a file and record it in the fingerprint + cache file map.
// Returns 0 on success, -1 on read failure.
int ccb_fp_add_file(ccb_pipe *p, ccb_fp_builder *b, const char *path) {
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
void ccb_fp_add_plugins(ccb_pipe *p, ccb_fp_builder *b,
    int *failed) {
  for (size_t i = 0; i < p->resolved.plugin_jars_count && !*failed; i++) {
    if (ccb_path_exists(p->resolved.plugin_jars[i]) &&
        ccb_fp_add_file(p, b, p->resolved.plugin_jars[i]) != 0) {
      *failed = 1;
    }
  }
  for (size_t i = 0; i < p->cfg->kotlin_plugin_options_count; i++) {
    ccb_fp_add(b, "kotlin_plugin_option",
      p->cfg->kotlin_plugin_options[i]);
  }
}

void ccb_fp_add_aapt2_tools(ccb_pipe *p, ccb_fp_builder *b) {
  ccb_fp_add(b, "build_tools", p->sdk->build_tools_version);
  ccb_fp_add(b, "aapt2", p->sdk->aapt2 == NULL ? "" : p->sdk->aapt2);
}

void ccb_fp_add_kotlin_tools(ccb_pipe *p, ccb_fp_builder *b) {
  ccb_fp_add(b, "kotlinc", p->sdk->kotlin_version);
  ccb_fp_add(b, "jvm_target", p->cfg->jvm_target);
}

void ccb_fp_add_declared_jars(ccb_pipe *p, ccb_fp_builder *b,
    int *failed, int skip_missing) {
  for (size_t i = 0; i < p->resolved.jars_count && !*failed; i++) {
    if (skip_missing && !ccb_path_exists(p->resolved.jars[i])) {
      continue;
    }
    if (ccb_fp_add_file(p, b, p->resolved.jars[i]) != 0) {
      *failed = 1;
    }
  }
}

void ccb_fp_add_aar_class_jars(ccb_pipe *p, ccb_fp_builder *b,
    int *failed, int skip_missing) {
  for (size_t i = 0; i < p->aar_class_jars_count && !*failed; i++) {
    if (skip_missing && !ccb_path_exists(p->aar_class_jars[i])) {
      continue;
    }
    if (ccb_fp_add_file(p, b, p->aar_class_jars[i]) != 0) {
      *failed = 1;
    }
  }
}

void ccb_pipe_diag(ccb_pipe *p, const char *code, const char *phase, const char *msg) {
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
ccb_status ccb_run_tool(ccb_pipe *p, const char *phase, const char *code, char *const argv[],
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
ccb_status ccb_run_kotlinc(ccb_pipe *p, char *const argv[],
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
  // Seed the resolved dependency lists from the declared config;
  // stage 0 appends resolved artifacts to these lists below. The
  // config itself is never mutated after this point.
  ccb_resolved_from_config(&p->resolved, p->cfg);
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
  // Libraries build an unsigned .aar, so they skip this.
  if (!p->cfg->is_library && strcmp(p->cfg->variant, "release") == 0) {
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
  // Resolved AARs/JARs join the resolved lists, so the deps step,
  // manifest merge, classpath, and dex all treat them as local
  // files. The config is immutable after load: resolved artifacts
  // live in p->resolved, never in cfg->aars/cfg->jars. A cache miss
  // fails here, before any stage runs, naming the coordinate and
  // pointing at `ccb setup`.
  if (p->cfg->maven_count > 0) {
    ccb_maven_result mv;
    ccb_status mrc = ccb_maven_resolve(p->cfg, 0, p->opts->verbose,
      p->opts->dry_run || p->opts->explain, &mv, p->diags);
    if (mrc != CCB_OK) {
      return CCB_ERR_TOOLCHAIN;
    }
    for (size_t i = 0; i < mv.aars_count; i++) {
      ccb_resolved_push_aar(&p->resolved, mv.aars[i]);
    }
    for (size_t i = 0; i < mv.jars_count; i++) {
      ccb_resolved_push_jar(&p->resolved, mv.jars[i]);
    }
    // Ownership of the paths moved into resolved; only the arrays
    // themselves are freed here (result_free would free the paths
    // too). The lockfile entries are not consumed by the build, so
    // they are freed in full.
    free(mv.aars);
    free(mv.jars);
    ccb_maven_entries_free(mv.entries, mv.entries_count);
  }
  // A library project builds an .aar, never an AAB.
  if (p->cfg->is_library && p->opts->aab) {
    ccb_pipe_diag(p, "CCB-E1005", "config",
      "library projects cannot build an AAB; they build an .aar");
    return CCB_ERR_CONFIG;
  }
  // Path libraries that point at a built .aar need no build: they
  // join the resolved AARs, so deps/merge/classpath/dex treat them as
  // local files. Project dirs are built by the libraries step.
  for (size_t i = 0; i < p->cfg->libraries_count; i++) {
    if (!p->cfg->libraries[i].is_file) {
      continue;
    }
    ccb_resolved_push_aar(&p->resolved,
      ccb_xstrdup(p->cfg->libraries[i].dir));
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
    for (size_t i = 0; i < pjars_count; i++) {
      ccb_resolved_push_plugin_jar(&p->resolved, pjars[i]);
    }
    free(pjars);
  }
  // Directories.
  p->intermediates = ccb_path_join(p->cfg->build_dir, "intermediates");
  p->gen_dir = ccb_path_join(p->intermediates, "gen");
  p->classes_dir = ccb_path_join(p->intermediates, "classes");
  p->dex_dir = ccb_path_join(p->intermediates, "dex");
  p->aar_dir = ccb_path_join(p->intermediates, "aar");
  p->modules_dir = ccb_path_join(p->intermediates, "modules");
  p->libraries_dir = ccb_path_join(p->intermediates, "libraries");
  p->out_dir = ccb_path_join3(p->cfg->build_dir, "outputs", p->cfg->variant);
  p->linked_apk = ccb_path_join(p->intermediates, "linked.ap_");
  p->linked_aab = ccb_path_join(p->intermediates, "linked-aab.ap_");
  p->unsigned_apk = ccb_path_join(p->intermediates, "app-unsigned.apk");
  p->unsigned_aab = ccb_path_join(p->intermediates, "app-unsigned.aab");
  p->aligned_apk = ccb_path_join(p->intermediates, "app-aligned.apk");
  if (!p->opts->dry_run && !p->opts->explain) {
    // Libraries build an .aar under build/, not an APK under
    // outputs/, so they create no out dir.
    if (ccb_mkdir_p(p->intermediates) != CCB_OK ||
        (!p->cfg->is_library && ccb_mkdir_p(p->out_dir) != CCB_OK)) {
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
  // Tool fingerprint: the single toolchain identity
  // (ccb_sdk_toolchain_identity). A mismatch empties the cache.
  p->tool_fingerprint = ccb_sdk_toolchain_identity(p->sdk, p->cfg);
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

// ---------- entry point + cleanup ----------

static void ccb_pipe_free(ccb_pipe *p) {
  ccb_config_free(p->cfg);
  ccb_sdk_free(p->sdk);
  ccb_resolved_free(&p->resolved);
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
  free(p->libraries_dir);
  ccb_free_str_array(p->merge_aar_paths, p->merge_aar_paths_count);
  ccb_free_str_array(p->merge_aar_srcs, p->merge_aar_srcs_count);
  ccb_free_str_array(p->merge_aar_names, p->merge_aar_names_count);
  ccb_free_str_array(p->aar_res_dirs, p->aar_res_dirs_count);
  ccb_free_str_array(p->aar_res_names, p->aar_res_names_count);
  ccb_free_str_array(p->aar_res_zips, p->aar_res_zips_count);
  ccb_free_str_array(p->aar_class_jars, p->aar_class_jars_count);
  ccb_free_str_array(p->aar_packages, p->aar_packages_count);
  ccb_free_str_array(p->aar_assets_dirs, p->aar_assets_count);
  ccb_free_str_array(p->aar_jni_dirs, p->aar_jni_count);
}

void ccb_build_result_free(ccb_build_result *r) {
  free(r->apk_path);
  r->apk_path = NULL;
  free(r->task_summary);
  r->task_summary = NULL;
}

// The driver: stage 0 loads the project (bootstrap), then the task
// graph (one task per stage, docs/build.md stages 0–14, in the same
// order the old hardcoded driver ran them) executes the rest.
// Stage bodies are untouched; the graph only decides the order and
// carries the per-task state, diagnostics, and timings.
ccb_status ccb_pipeline_build(const ccb_build_opts *opts, ccb_build_result *result, ccb_diag_list *diags) {
  memset(result, 0, sizeof(*result));
  ccb_pipe p;
  memset(&p, 0, sizeof(p));
  p.opts = opts;
  p.diags = diags;
  ccb_log_init(&p.log, opts->json, opts->verbose);
  p.log.explain = opts->explain;
  long t0 = ccb_graph_now_ms();
  ccb_status rc = ccb_stage0(&p);
  if (rc != CCB_OK) {
    ccb_pipe_free(&p);
    return rc;
  }
  ccb_graph *g = ccb_pipeline_graph_build_resolved(p.cfg, &p.resolved,
    opts->aab, diags);
  if (g == NULL) {
    ccb_pipe_free(&p);
    return CCB_ERR_CONFIG;
  }
  ccb_pipeline_graph_config_done(g, ccb_graph_now_ms() - t0);
  if (opts->explain) {
    if (opts->json) {
      ccb_graph_emit_plan_json(g, stdout);
    } else {
      ccb_graph_emit_plan_human(g, stdout);
    }
  }
  rc = ccb_graph_execute(g, &p, &p.log, diags);
  {
    ccb_str sum;
    ccb_str_init(&sum);
    ccb_graph_summary_json(g, &sum);
    result->task_summary = ccb_str_steal(&sum);
  }
  if (rc != CCB_OK) {
    // Save progress: completed steps stay cached so a retry resumes.
    ccb_save_cache(&p);
    ccb_graph_free(g);
    ccb_pipe_free(&p);
    return rc;
  }
  if (p.cfg->is_library) {
    // A library project stops after deps: its single module builds
    // the AAR, and there is no merge/link/dex/package.
    ccb_save_cache(&p);
    result->steps_run = p.steps_run;
    result->steps_skipped = p.steps_skipped;
    result->up_to_date = (p.steps_run == 0 && !opts->dry_run && !opts->explain) ? 1 : 0;
    result->is_library = 1;
    result->apk_path = ccb_library_aar_path(p.cfg);
    ccb_graph_free(g);
    ccb_pipe_free(&p);
    return CCB_OK;
  }
  ccb_save_cache(&p);
  result->steps_run = p.steps_run;
  result->steps_skipped = p.steps_skipped;
  result->up_to_date = (p.steps_run == 0 && !opts->dry_run && !opts->explain) ? 1 : 0;
  result->aab = p.opts->aab;
  result->apk_path = ccb_path_join(p.out_dir, p.opts->aab ? "app.aab" : "app.apk");
  ccb_graph_free(g);
  ccb_pipe_free(&p);
  return CCB_OK;
}
