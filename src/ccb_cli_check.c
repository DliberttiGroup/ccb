// ccb_cli_check: `ccb check` / `ccb build` command handlers.
// Split from ccb_cli.c.

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_json.h"
#include "ccb_net.h"
#include "ccb_maven.h"
#include "ccb_diag.h"
#include "ccb_log.h"
#include "ccb_proc.h"
#include "ccb_config.h"
#include "ccb_daemon.h"
#include "ccb_sdk.h"
#include "ccb_pipeline.h"
#include "ccb_zip.h"
#include "ccb_preview.h"
#include "ccb_preview_lsp.h"
#include "ccb_preview_tree.h"
#include "ccb_preview_walk.h"
#include "ccb_preview_watch.h"
#include "ccb_preview_gate.h"
#include "ccb_preview_diff.h"
#include "ccb_cli.h"
#include <unistd.h>
#include <sys/stat.h>
#include "ccb_cli_internal.h"

// ---------- check / build / clean / doctor ----------

// Preview build gate, shared by `check` and `build`. Runs only when
// enabled (explicit flag beats config); on failure the concise lines
// go to stderr and the command exits 1. A gate-run failure (2) is a
// config error and exits 2. Returns -1 when the gate is off (the
// caller proceeds normally); 0 means the gate passed. In --json mode
// the failure still surfaces as the stderr lines plus the nonzero
// exit code, not as a JSON event.
static int ccb_run_preview_gate(ccb_args *a, ccb_config *cfg) {
  ccb_str failed;
  int grc = 0;
  if (!pv_gate_enabled(cfg, a->preview_gate_on, a->preview_gate_off)) {
    return -1;
  }
  ccb_str_init(&failed);
  grc = pv_gate_run(cfg, 0, 0, a->preview_strict, &failed);
  if (grc == 1) {
    fputs(failed.buf != NULL ? failed.buf : "", stderr);
    ccb_str_free(&failed);
    return 1;
  }
  if (grc == 2) {
    fprintf(stderr, "error: preview gate could not run\n");
    ccb_str_free(&failed);
    return 2;
  }
  ccb_str_free(&failed);
  return 0;
}

// ---------- `ccb check` library report ----------

// AAR contents summary for `ccb check` library lines.

// Non-empty lines in text (R.txt has one symbol per line).
static size_t ccb_count_lines(const char *text, size_t len) {
  size_t count = 0;
  size_t at = 0;
  while (at < len) {
    size_t end = at;
    while (end < len && text[end] != '\n') {
      end++;
    }
    int nonempty = 0;
    for (size_t k = at; k < end; k++) {
      if (text[k] != '\r' && text[k] != ' ' && text[k] != '\t') {
        nonempty = 1;
        break;
      }
    }
    if (nonempty) {
      count++;
    }
    at = end + 1;
  }
  return count;
}

static void ccb_aar_summarize(const char *path, ccb_aar_summary *s) {
  memset(s, 0, sizeof(*s));
  if (path == NULL || !ccb_path_exists(path)) {
    return;
  }
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(path, &a) != CCB_OK) {
    return;
  }
  s->exists = 1;
  for (size_t i = 0; i < a->count; i++) {
    const char *name = a->entries[i].name;
    if (strcmp(name, "classes.jar") == 0) {
      s->has_classes = 1;
    } else if (strncmp(name, "res/", 4) == 0) {
      s->res_files++;
    } else if (strncmp(name, "assets/", 7) == 0) {
      s->assets++;
    }
  }
  const ccb_zip_entry *rtxt = ccb_zip_find(a, "R.txt");
  if (rtxt != NULL) {
    void *data = NULL;
    size_t len = 0;
    if (ccb_zip_read_entry(a, rtxt, &data, &len) == CCB_OK) {
      s->r_symbols = ccb_count_lines(data, len);
      free(data);
    }
  }
  ccb_zip_close(a);
}

// "(classes.jar, 3 res files, 2 R symbols)" or "(not built yet)".
static void ccb_aar_summary_text(const ccb_aar_summary *s, ccb_str *out) {
  if (!s->exists) {
    ccb_str_append(out, "(not built yet)");
    return;
  }
  ccb_str_append(out, "(");
  ccb_str_append(out, s->has_classes ? "classes.jar" : "no classes.jar");
  ccb_str_appendf(out, ", %zu res files, %zu R symbols", s->res_files,
    s->r_symbols);
  if (s->assets > 0) {
    ccb_str_appendf(out, ", %zu assets", s->assets);
  }
  ccb_str_append(out, ")");
}

// Human check lines for one library: header line + AAR summary line.
static void ccb_check_print_library(const char *header,
    const char *aar_path) {
  ccb_aar_summary sum;
  ccb_aar_summarize(aar_path, &sum);
  ccb_str t;
  ccb_str_init(&t);
  ccb_aar_summary_text(&sum, &t);
  printf("  library %s\n    aar: %s %s\n", header, aar_path, t.buf);
  ccb_str_free(&t);
}

// One JSON library object (no surrounding comma handling: the caller
// tracks it).
static void ccb_check_json_library(ccb_str *s, const char *name,
    const char *version, const char *package, const char *source,
    const char *ref, const char *aar_path) {
  ccb_aar_summary sum;
  ccb_aar_summarize(aar_path, &sum);
  ccb_str_append(s, "{\"name\":");
  ccb_json_escape(s, name);
  ccb_str_append(s, ",\"version\":");
  if (version != NULL) {
    ccb_json_escape(s, version);
  } else {
    ccb_str_append(s, "null");
  }
  ccb_str_append(s, ",\"package\":");
  if (package != NULL) {
    ccb_json_escape(s, package);
  } else {
    ccb_str_append(s, "null");
  }
  ccb_str_append(s, ",\"source\":");
  ccb_json_escape(s, source);
  ccb_str_append(s, ",\"ref\":");
  ccb_json_escape(s, ref);
  ccb_str_append(s, ",\"aar\":");
  ccb_json_escape(s, aar_path);
  ccb_str_appendf(s, ",\"built\":%s,\"classes\":%s,\"res_files\":%zu,"
    "\"r_symbols\":%zu}",
    sum.exists ? "true" : "false",
    sum.has_classes ? "true" : "false", sum.res_files, sum.r_symbols);
}

// Output AAR of a library project: build/<name>-<version>.aar.
static char *ccb_check_library_aar_path(const ccb_config *cfg) {
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

// Built AAR of a consumed path library:
// build/intermediates/libraries/<name>/<name>.aar.
static char *ccb_check_pathlib_aar_path(const ccb_config *cfg,
    const char *name) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, cfg->build_dir);
  ccb_str_append(&s, "/intermediates/libraries/");
  ccb_str_append(&s, name);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, name);
  ccb_str_append(&s, ".aar");
  return ccb_str_steal(&s);
}

int ccb_cmd_check(ccb_args *a) {
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  if (a->preview_gate_on && a->preview_gate_off) {
    fprintf(stderr, "error: --preview-gate cannot be combined with "
      "--no-preview-gate\n");
    return 2;
  }
  ccb_diag_list_init(&diags);
  cfg = ccb_load_config_or_die(a, &diags);
  if (cfg == NULL) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 2;
  }
  ccb_sdk *sdk = NULL;
  if (ccb_sdk_resolve(cfg, &sdk, &diags) != CCB_OK) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    ccb_config_free(cfg);
    return 3;
  }
  // Maven dependencies (cache-only): a missing prefetch is reported
  // here, not as a surprise at build time. The counts print with the
  // rest of the report below, after the ok line. The result stays
  // alive until the report is printed (library lines need the AAR
  // entries), so it is freed at the exits below.
  size_t mv_aars = 0;
  size_t mv_jars = 0;
  int have_maven = 0;
  ccb_maven_result mv;
  memset(&mv, 0, sizeof(mv));
  if (cfg->maven_count > 0) {
    if (ccb_maven_resolve(cfg, 0, a->verbose, 0, &mv, &diags) != CCB_OK) {
      ccb_print_diags(a, &diags);
      ccb_sdk_free(sdk);
      ccb_config_free(cfg);
      ccb_diag_list_free(&diags);
      return 3;
    }
    mv_aars = mv.aars_count;
    mv_jars = mv.jars_count;
    have_maven = 1;
  }
  // Compiler plugins (cache-only): same treatment as Maven deps.
  size_t nplugins = 0;
  int have_plugins = 0;
  if (cfg->kotlin_plugins_count > 0) {
    char **pjars = NULL;
    if (ccb_maven_resolve_plugins(cfg, 0, a->verbose, 0, &pjars,
        &nplugins, &diags) != CCB_OK) {
      ccb_print_diags(a, &diags);
      ccb_sdk_free(sdk);
      ccb_config_free(cfg);
      ccb_diag_list_free(&diags);
      ccb_maven_result_free(&mv);
      return 3;
    }
    have_plugins = 1;
    ccb_free_str_array(pjars, nplugins);
  }
  // Preview build gate (opt-in): runs after toolchain validation so a
  // failing gate does not mask a broken toolchain, and a broken
  // toolchain does not mask gate failures either way — the gate only
  // runs when the toolchain resolved.
  {
    int gate = ccb_run_preview_gate(a, cfg);
    if (gate == 1 || gate == 2) {
      ccb_sdk_free(sdk);
      ccb_config_free(cfg);
      ccb_diag_list_free(&diags);
      ccb_maven_result_free(&mv);
      return gate;
    }
  }
  if (a->json) {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "{\"event\":\"check\",\"ok\":true,\"project\":");
    ccb_json_escape(&s, cfg->name);
    ccb_str_append(&s, ",\"package\":");
    ccb_json_escape(&s, cfg->package);
    ccb_str_append(&s, ",\"variant\":");
    ccb_json_escape(&s, cfg->variant);
    ccb_str_append(&s, ",\"sdk\":");
    ccb_json_escape(&s, sdk->sdk_home);
    ccb_str_append(&s, ",\"build_tools\":");
    ccb_json_escape(&s, sdk->build_tools_version);
    ccb_str_append(&s, ",\"aapt2\":");
    ccb_json_escape(&s, sdk->aapt2);
    ccb_str_append(&s, ",\"aapt2_source\":");
    ccb_json_escape(&s, sdk->aapt2_source == NULL ? "default" : sdk->aapt2_source);
    ccb_str_append(&s, ",\"zipalign\":");
    ccb_json_escape(&s, sdk->zipalign);
    ccb_str_append(&s, ",\"zipalign_source\":");
    ccb_json_escape(&s, sdk->zipalign_source == NULL ? "default" : sdk->zipalign_source);
    if (sdk->kotlin_version[0] == '\0') {
      ccb_str_append(&s, ",\"kotlin\":null");
    } else {
      ccb_str_append(&s, ",\"kotlin\":");
      ccb_json_escape(&s, sdk->kotlin_version);
    }
    ccb_str_appendf(&s, ",\"java\":%d", sdk->java_major);
    if (cfg->is_library) {
      char *aar_path = ccb_check_library_aar_path(cfg);
      ccb_str_append(&s, ",\"library\":");
      ccb_check_json_library(&s, cfg->name, cfg->version, cfg->package,
        "self", cfg->project_root, aar_path);
      free(aar_path);
    }
    {
      int lib_first = 1;
      ccb_str_append(&s, ",\"libraries\":[");
      for (size_t i = 0; i < cfg->libraries_count; i++) {
        ccb_module *m = &cfg->libraries[i];
        char *aar_path = m->is_file ? ccb_xstrdup(m->dir)
          : ccb_check_pathlib_aar_path(cfg, m->name);
        if (!lib_first) {
          ccb_str_append_c(&s, ',');
        }
        lib_first = 0;
        ccb_check_json_library(&s, m->name, m->version, m->package,
          m->is_file ? "file" : "path", m->dir, aar_path);
        free(aar_path);
      }
      if (have_maven) {
        for (size_t i = 0; i < mv.entries_count; i++) {
          ccb_maven_entry *e = &mv.entries[i];
          if (strcmp(e->packaging, "aar") != 0) {
            continue;
          }
          ccb_str coord;
          ccb_str_init(&coord);
          ccb_str_append(&coord, e->group);
          ccb_str_append_c(&coord, ':');
          ccb_str_append(&coord, e->artifact);
          ccb_str_append_c(&coord, ':');
          ccb_str_append(&coord, e->version);
          if (!lib_first) {
            ccb_str_append_c(&s, ',');
          }
          lib_first = 0;
          ccb_check_json_library(&s, e->artifact, e->version, NULL,
            "maven", coord.buf, e->path);
          ccb_str_free(&coord);
        }
      }
      for (size_t i = 0; i < cfg->aars_count; i++) {
        char *base = ccb_basename(cfg->aars[i]);
        char *dot = strrchr(base, '.');
        if (dot != NULL) {
          *dot = '\0';
        }
        if (!lib_first) {
          ccb_str_append_c(&s, ',');
        }
        lib_first = 0;
        ccb_check_json_library(&s, base, NULL, NULL, "file",
          cfg->aars[i], cfg->aars[i]);
        free(base);
      }
      ccb_str_append_c(&s, ']');
    }
    if (have_maven && cfg->maven_exclude_count > 0) {
      ccb_str_append(&s, ",\"maven_excluded\":[");
      for (size_t i = 0; i < cfg->maven_exclude_count; i++) {
        if (i > 0) {
          ccb_str_append_c(&s, ',');
        }
        ccb_json_escape(&s, cfg->maven_exclude[i]);
      }
      ccb_str_append_c(&s, ']');
    }
    ccb_str_append_c(&s, '}');
    printf("%s\n", s.buf);
    ccb_str_free(&s);
  } else {
    printf("ok: %s (%s, variant %s)\n", cfg->name, cfg->package, cfg->variant);
    printf("  sdk: %s\n  build-tools: %s\n  aapt2: %s (%s)\n  zipalign: %s (%s)\n  kotlinc: %s\n  java: %d\n",
      sdk->sdk_home, sdk->build_tools_version,
      sdk->aapt2, sdk->aapt2_source == NULL ? "default" : sdk->aapt2_source,
      sdk->zipalign, sdk->zipalign_source == NULL ? "default" : sdk->zipalign_source,
      sdk->kotlin_version[0] == '\0' ? "none" : sdk->kotlin_version,
      sdk->java_major);
    if (cfg->has_ndk) {
      printf("  ndk: %s\n", sdk->ndk_home);
    }
    if (have_maven) {
      printf("  maven: %zu artifacts (%zu aar, %zu jar)\n",
        mv_aars + mv_jars, mv_aars, mv_jars);
    }
    if (have_plugins) {
      printf("  kotlin plugins: %zu jars\n", nplugins);
    }
    if (have_maven && cfg->maven_exclude_count > 0) {
      printf("  maven: excluded ");
      for (size_t i = 0; i < cfg->maven_exclude_count; i++) {
        if (i > 0) {
          printf(", ");
        }
        printf("%s", cfg->maven_exclude[i]);
      }
      printf("\n");
    }
    if (cfg->is_library) {
      char *aar_path = ccb_check_library_aar_path(cfg);
      ccb_str h;
      ccb_str_init(&h);
      ccb_str_append(&h, cfg->name);
      ccb_str_append_c(&h, ' ');
      ccb_str_append(&h, cfg->version);
      ccb_str_append(&h, " (");
      ccb_str_append(&h, cfg->package);
      ccb_str_append_c(&h, ')');
      ccb_check_print_library(h.buf, aar_path);
      ccb_str_free(&h);
      free(aar_path);
    }
    for (size_t i = 0; i < cfg->libraries_count; i++) {
      ccb_module *m = &cfg->libraries[i];
      ccb_str h;
      ccb_str_init(&h);
      ccb_str_append(&h, m->name);
      if (m->version != NULL) {
        ccb_str_append_c(&h, ' ');
        ccb_str_append(&h, m->version);
      }
      if (m->package != NULL) {
        ccb_str_append(&h, " (");
        ccb_str_append(&h, m->package);
        ccb_str_append_c(&h, ')');
      }
      if (m->is_file) {
        ccb_str_append(&h, " file ");
        ccb_str_append(&h, m->dir);
        ccb_check_print_library(h.buf, m->dir);
      } else {
        ccb_str_append(&h, " path ");
        ccb_str_append(&h, m->dir);
        char *aar_path = ccb_check_pathlib_aar_path(cfg, m->name);
        ccb_check_print_library(h.buf, aar_path);
        free(aar_path);
      }
      ccb_str_free(&h);
    }
    if (have_maven) {
      for (size_t i = 0; i < mv.entries_count; i++) {
        ccb_maven_entry *e = &mv.entries[i];
        if (strcmp(e->packaging, "aar") != 0) {
          continue;
        }
        ccb_str h;
        ccb_str_init(&h);
        ccb_str_append(&h, e->group);
        ccb_str_append_c(&h, ':');
        ccb_str_append(&h, e->artifact);
        ccb_str_append_c(&h, ':');
        ccb_str_append(&h, e->version);
        ccb_str_append(&h, " maven");
        ccb_check_print_library(h.buf, e->path);
        ccb_str_free(&h);
      }
    }
    for (size_t i = 0; i < cfg->aars_count; i++) {
      char *base = ccb_basename(cfg->aars[i]);
      char *dot = strrchr(base, '.');
      if (dot != NULL) {
        *dot = '\0';
      }
      ccb_str h;
      ccb_str_init(&h);
      ccb_str_append(&h, base);
      ccb_str_append(&h, " file ");
      ccb_str_append(&h, cfg->aars[i]);
      ccb_check_print_library(h.buf, cfg->aars[i]);
      ccb_str_free(&h);
      free(base);
    }
  }
  ccb_sdk_free(sdk);
  ccb_config_free(cfg);
  ccb_diag_list_free(&diags);
  ccb_maven_result_free(&mv);
  return 0;
}

int ccb_cmd_build(ccb_args *a) {
  ccb_build_opts opts;
  if (a->preview_gate_on && a->preview_gate_off) {
    fprintf(stderr, "error: --preview-gate cannot be combined with "
      "--no-preview-gate\n");
    return 2;
  }
  if (a->kotlinc_daemon_on && a->kotlinc_daemon_off) {
    fprintf(stderr, "error: --kotlinc-daemon cannot be combined with "
      "--no-kotlinc-daemon\n");
    return 2;
  }
  memset(&opts, 0, sizeof(opts));
  opts.json = a->json;
  opts.verbose = a->verbose;
  opts.dry_run = a->dry_run;
  opts.explain = a->explain;
  opts.aab = a->aab;
  opts.kotlinc_daemon_on = a->kotlinc_daemon_on;
  opts.kotlinc_daemon_off = a->kotlinc_daemon_off;
  opts.variant_override = a->variant;
  opts.config_path = a->config_path;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  // Preview build gate (opt-in), fail-fast before any pipeline stage:
  // a failing gate never starts a build. Config load here is only for
  // the gate; when it fails, the gate is skipped and the pipeline
  // below reports the config error normally.
  {
    ccb_diag_list gdiags;
    ccb_config *gcfg = NULL;
    ccb_diag_list_init(&gdiags);
    gcfg = ccb_load_config_or_die(a, &gdiags);
    if (gcfg != NULL) {
      int gate = ccb_run_preview_gate(a, gcfg);
      ccb_config_free(gcfg);
      ccb_diag_list_free(&gdiags);
      if (gate == 1 || gate == 2) {
        ccb_diag_list_free(&diags);
        return gate;
      }
    } else {
      ccb_diag_list_free(&gdiags);
    }
  }
  ccb_build_result result;
  ccb_status rc = ccb_pipeline_build(&opts, &result, &diags);
  if (rc != CCB_OK) {
    // In JSON mode the machine-readable build summary goes out even
    // on failure: it names the failed task and the cancelled ones.
    if (a->json && result.task_summary != NULL) {
      printf("{\"event\":\"build_summary\",\"tasks\":%s}\n",
        result.task_summary);
    }
    ccb_print_diags(a, &diags);
    ccb_build_result_free(&result);
    ccb_diag_list_free(&diags);
    return ccb_exit_code(rc);
  }
  if (!a->dry_run && !a->explain) {
    if (a->json) {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "{\"event\":\"build_done\",\"");
      ccb_str_append(&s, result.is_library ? "aar" : result.aab ? "aab" : "apk");
      ccb_str_append(&s, "\":");
      ccb_json_escape(&s, result.apk_path);
      ccb_str_appendf(&s, ",\"steps_run\":%d,\"steps_skipped\":%d,\"up_to_date\":%s",
        result.steps_run, result.steps_skipped, result.up_to_date ? "true" : "false");
      // Machine-readable build summary (Phase 1): per-task state,
      // duration, and skip flag, in plan order.
      if (result.task_summary != NULL) {
        ccb_str_append(&s, ",\"tasks\":");
        ccb_str_append(&s, result.task_summary);
      }
      ccb_str_append(&s, "}");
      printf("%s\n", s.buf);
      ccb_str_free(&s);
    } else if (result.up_to_date) {
      printf("up to date: %s\n", result.apk_path);
    } else {
      printf("built %s\n", result.apk_path);
    }
  }
  ccb_build_result_free(&result);
  ccb_diag_list_free(&diags);
  return 0;
}

// `ccb graph`: print the build plan — the task graph in execution
// order with each task's dependencies, inputs, and outputs — without
// building anything. Human table by default, one build_plan JSON
// object with --json. --aab shows the AAB plan (package-aab, no
// align). With --affected INPUT, list the tasks that input
// invalidates instead (human list, or one affected JSON object
// with --json). The config is loaded read-only; no SDK resolution
// happens, so this works without a toolchain installed.
int ccb_cmd_graph(ccb_args *a) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_config *cfg = ccb_load_config_or_die(a, &diags);
  if (cfg == NULL) {
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
    return 2;
  }
  ccb_graph *g = ccb_pipeline_graph_build(cfg, a->aab, &diags);
  if (g == NULL) {
    ccb_print_diags(a, &diags);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
    return 2;
  }
  ccb_status rc;
  if (a->graph_affected != NULL) {
    // Declared file inputs are project-root-resolved absolute
    // paths; a relative --affected path is resolved against the
    // project root when it names a declared input that way.
    // Logical names ("dex", "project-model") and env var names
    // pass through unchanged.
    const char *changed = a->graph_affected;
    char *resolved = NULL;
    if (!ccb_graph_has_input(g, changed) && changed[0] != '/' &&
        cfg->project_root != NULL) {
      resolved = ccb_path_join(cfg->project_root, changed);
      if (ccb_graph_has_input(g, resolved)) {
        changed = resolved;
      }
    }
    if (a->json) {
      rc = ccb_graph_emit_affected_json(g, changed, stdout);
    } else {
      rc = ccb_graph_emit_affected_human(g, changed, stdout);
    }
    free(resolved);
  } else if (a->json) {
    rc = ccb_graph_emit_plan_json(g, stdout);
  } else {
    rc = ccb_graph_emit_plan_human(g, stdout);
  }
  ccb_graph_free(g);
  ccb_config_free(cfg);
  ccb_diag_list_free(&diags);
  return rc == CCB_OK ? 0 : 2;
}
