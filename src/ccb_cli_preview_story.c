// ccb_cli_preview_story: `ccb preview` storyboard driver (dense logic,
// kept whole). Split from ccb_cli.c: one command's flag/conflict/output
// matrix; splitting it would scatter a single dispatch across files.

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

// ---------- preview storyboard ----------

// Parse a --storyboard list ("A,B,C", names trimmed of surrounding
// whitespace, repeats allowed) into the journey's screens, in order.
// Returns NULL on any error (message already printed to stderr):
// an empty list, an empty or whitespace-only element, or a name
// that is not a screen in the project. All are usage errors (exit 2).
static pv_screen **ccb_preview_storyboard_parse(pv_project *proj,
    const char *list, size_t *out_n) {
  pv_screen **screens = NULL;
  size_t n = 0;
  size_t cap = 0;
  const char *p = NULL;
  *out_n = 0;
  if (list == NULL || list[0] == '\0') {
    fprintf(stderr, "error: --storyboard requires a non-empty list of "
      "screen names\n");
    return NULL;
  }
  p = list;
  for (;;) {
    const char *seg_start = p;
    const char *seg_end = NULL;
    const char *s = NULL;
    const char *e = NULL;
    size_t i = 0;
    pv_screen *found = NULL;
    while (*p != '\0' && *p != ',') {
      p++;
    }
    seg_end = p;
    s = seg_start;
    e = seg_end;
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' ||
        *s == '\r')) {
      s++;
    }
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
        e[-1] == '\r')) {
      e--;
    }
    if (s == e) {
      fprintf(stderr, "error: --storyboard has an empty element\n");
      free(screens);
      return NULL;
    }
    for (i = 0; i < proj->nscreens; i++) {
      const char *name = proj->screens[i]->name;
      if (name != NULL && strlen(name) == (size_t)(e - s) &&
          memcmp(name, s, (size_t)(e - s)) == 0) {
        found = proj->screens[i];
        break;
      }
    }
    if (found == NULL) {
      fprintf(stderr, "error: --storyboard: no screen named '%.*s'\n",
        (int)(e - s), s);
      free(screens);
      return NULL;
    }
    if (n == cap) {
      size_t ncap = cap == 0 ? 4 : cap * 2;
      screens = ccb_xrealloc(screens, ncap * sizeof(*screens));
      cap = ncap;
    }
    screens[n++] = found;
    if (*p == '\0') {
      break;
    }
    p++; // consume ','
  }
  *out_n = n;
  return screens;
}

// Render one storyboard: human mode prints each journey screen's
// frame preceded by "Journey N/M: <Name>", then the normal logic
// tree, then diagnostics to stderr; --json prints the same JSON
// object as a plain preview but with "screens" filtered to the
// journey screens, in journey order (repeats as separate entries).
// --output writes the render to the file and leaves stdout empty.
// Returns 0 (ok), 1 (preview errors), or 2 (output write failure).
static int ccb_preview_render_storyboard(ccb_args *a, pv_project *proj,
    pv_screen **journey, size_t njourney, long errors) {
  size_t i = 0;
  if (a->json) {
    ccb_str out;
    ccb_str_init(&out);
    pv_project_json_screens(&out, proj, journey, njourney);
    ccb_str_append(&out, "\n");
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : ""));
      ccb_str_free(&out);
      return rc == 0 ? (errors > 0 ? 1 : 0) : 2;
    }
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
  } else {
    int color = 0;
    const char *term = NULL;
    ccb_str out;
    if (!a->preview_ascii && a->preview_output == NULL &&
        getenv("NO_COLOR") == NULL) {
      term = getenv("TERM");
      color = term == NULL || strcmp(term, "dumb") != 0;
    }
    ccb_str_init(&out);
    for (i = 0; i < njourney; i++) {
      ccb_str_appendf(&out, "Journey %zu/%zu: %s\n", i + 1, njourney,
        journey[i]->name != NULL ? journey[i]->name : "?");
      pv_render_screen(&out, journey[i], proj->frame_width,
        a->preview_ascii, color);
    }
    pv_render_logic_tree(&out, proj);
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : ""));
      ccb_str_free(&out);
      ccb_print_diags_human(&proj->diags);
      return rc == 0 ? (errors > 0 ? 1 : 0) : 2;
    }
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
    ccb_print_diags_human(&proj->diags);
  }
  return errors > 0 ? 1 : 0;
}

// Shared state for the `preview` stages. A stage that finishes
// the command frees the context (config, diags while owned, watch
// paths) before returning, so no stage repeats the cleanup. After a
// successful project load the diag list belongs to the project, so
// diags_owned is cleared and the context no longer frees it.

static void ccb_preview_ctx_free(ccb_preview_ctx *ctx) {
  free(ctx->toml_path);
  ctx->toml_path = NULL;
  if (ctx->diags_owned) {
    ccb_diag_list_free(&ctx->diags);
    ctx->diags_owned = 0;
  }
  ccb_config_free(ctx->cfg);
  ctx->cfg = NULL;
  ccb_preview_watch_paths_free(ctx->watch_toml, ctx->watch_config_dir,
    ctx->watch_styles, ctx->watch_flow);
  ctx->watch_toml = NULL;
  ctx->watch_config_dir = NULL;
  ctx->watch_styles = NULL;
  ctx->watch_flow = NULL;
}

// Preview stages return 1 when the command is finished (exit code
// in *rc) and 0 to continue to the next stage. The flag-conflict
// checks below return a plain exit code instead: 0 to continue,
// 2 on a conflict.

// Conflicting destinations fail loudly instead of guessing.
static int ccb_preview_check_output_conflicts(ccb_args *a) {
  if (a->preview_output != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --output cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_update_goldens && a->preview_screen != NULL) {
    fprintf(stderr, "error: --screen cannot be combined with "
      "--update-goldens (goldens cover every screen)\n");
    return 2;
  }
  if (a->preview_walk && a->preview_output != NULL) {
    fprintf(stderr, "error: --walk cannot be combined with "
      "--output (walk mode is interactive)\n");
    return 2;
  }
  if (a->preview_walk && a->preview_update_goldens) {
    fprintf(stderr, "error: --walk cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_watch && a->preview_update_goldens) {
    fprintf(stderr, "error: --watch cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_watch && a->preview_walk) {
    fprintf(stderr, "error: --watch cannot be combined with "
      "--walk\n");
    return 2;
  }
  return 0;
}

// Tree-live is interactive like walk, so it rejects the other
// output modes the same way. --scenario/--trace compose (they are
// passed to the spawned server); --screen and a positional screen
// select the initial screen.
static int ccb_preview_check_tree_live_conflicts(ccb_args *a) {
  if (a->preview_tree_live && a->preview_walk) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--walk\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_watch) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--watch\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_output != NULL) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--output (tree-live mode is interactive)\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_update_goldens) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_diff != NULL) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--diff\n");
    return 2;
  }
  if (a->preview_tree_live && a->preview_storyboard != NULL) {
    fprintf(stderr, "error: --tree-live cannot be combined with "
      "--storyboard\n");
    return 2;
  }
  return 0;
}

static int ccb_preview_check_diff_conflicts(ccb_args *a) {
  if (a->preview_diff != NULL && a->preview_watch) {
    fprintf(stderr, "error: --diff cannot be combined with --watch\n");
    return 2;
  }
  if (a->preview_diff != NULL && a->preview_walk) {
    fprintf(stderr, "error: --diff cannot be combined with --walk\n");
    return 2;
  }
  if (a->preview_diff != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --diff cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  return 0;
}

static int ccb_preview_check_storyboard_conflicts(ccb_args *a) {
  if (a->preview_storyboard != NULL && a->preview_screen != NULL) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--screen\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->positional != NULL) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--screen (a positional screen counts as --screen)\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_walk) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--walk\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_watch) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--watch\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_diff != NULL) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--diff\n");
    return 2;
  }
  if (a->preview_storyboard != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --storyboard cannot be combined with "
      "--update-goldens\n");
    return 2;
  }
  return 0;
}

// A scenario applies to the plain render (human or --json) only.
// Walk takes it later, and watch/diff/storyboard/goldens each own
// their own project load; failing loudly beats silently rendering
// without the scenario.
static int ccb_preview_check_scenario_conflicts(ccb_args *a) {
  if (a->preview_scenario != NULL && a->preview_walk) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--walk (walk takes --scenario in a later chunk)\n");
    return 2;
  }
  if (a->preview_scenario != NULL && a->preview_watch) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--watch\n");
    return 2;
  }
  if (a->preview_scenario != NULL && a->preview_diff != NULL) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--diff\n");
    return 2;
  }
  if (a->preview_scenario != NULL && a->preview_storyboard != NULL) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--storyboard\n");
    return 2;
  }
  if (a->preview_scenario != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --scenario cannot be combined with "
      "--update-goldens (goldens cover the no-scenario baseline)\n");
    return 2;
  }
  return 0;
}

// --record only makes sense with --walk (it records the walk), and
// --trace is a render overlay, so it rejects the other render modes
// the same way --scenario does. --trace composes with --scenario
// (scenario applies at load, trace after), and with
// --json/--screen/--output like any plain render.
static int ccb_preview_check_trace_conflicts(ccb_args *a) {
  if (a->preview_record != NULL && !a->preview_walk) {
    fprintf(stderr, "error: --record requires --walk\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_walk) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--walk (record a walk with --record instead)\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_watch) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--watch\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_diff != NULL) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--diff\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_storyboard != NULL) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--storyboard\n");
    return 2;
  }
  if (a->preview_trace != NULL && a->preview_update_goldens) {
    fprintf(stderr, "error: --trace cannot be combined with "
      "--update-goldens (goldens cover the no-trace baseline)\n");
    return 2;
  }
  return 0;
}

// Runs the flag-conflict checks in their original order, so the
// first conflict reported is the same as before.
static int ccb_preview_check_flags(ccb_args *a, int *rc) {
  *rc = ccb_preview_check_output_conflicts(a);
  if (*rc == 0) {
    *rc = ccb_preview_check_tree_live_conflicts(a);
  }
  if (*rc == 0) {
    *rc = ccb_preview_check_diff_conflicts(a);
  }
  if (*rc == 0) {
    *rc = ccb_preview_check_storyboard_conflicts(a);
  }
  if (*rc == 0) {
    *rc = ccb_preview_check_scenario_conflicts(a);
  }
  if (*rc == 0) {
    *rc = ccb_preview_check_trace_conflicts(a);
  }
  return *rc != 0;
}

// No project here at all: preview simply does not apply. A diff,
// though, needs something to compare against, so it is a usage
// error here rather than "not applicable".
static int ccb_preview_no_project(ccb_args *a, int *rc) {
  if (a->preview_diff != NULL) {
    fprintf(stderr, "error: --diff requires a project: no ccb.toml "
      "found in this directory or any parent\n");
    *rc = 2;
    return 1;
  }
  // With --output the message goes to the file, not stdout.
  if (a->preview_output != NULL) {
    if (ccb_preview_write_file(a->preview_output,
        "preview not applicable to this project\n",
        strlen("preview not applicable to this project\n")) != 0) {
      *rc = 2;
    } else {
      *rc = 0;
    }
    return 1;
  }
  printf("preview not applicable to this project\n");
  *rc = 0;
  return 1;
}

// Finds the project config by upward search. On success the path
// is kept in ctx->toml_path.
static int ccb_preview_discover_config(ccb_args *a, ccb_preview_ctx *ctx, int *rc) {
  if (getcwd(ctx->cwd, sizeof(ctx->cwd)) == NULL) {
    fprintf(stderr, "error: cannot determine working directory\n");
    *rc = 2;
    return 1;
  }
  if (ccb_config_discover(ctx->cwd, a->config_path, &ctx->toml_path) == CCB_OK) {
    return 0;
  }
  if (a->config_path == NULL) {
    return ccb_preview_no_project(a, rc);
  }
  // An explicit --config pointing at a missing file is a config error.
  {
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    {
      ccb_diag d = ccb_diag_make("CCB-E1009", "config",
        "config file not found");
      ccb_diag_list_push(&diags, &d);
    }
    ccb_print_diags(a, &diags);
    ccb_diag_list_free(&diags);
  }
  *rc = 2;
  return 1;
}

// Loads the discovered config. For --watch, keeps the toml path,
// its directory, and the resolved preview dirs alive for the loop's
// lifetime: the loop re-loads config from the toml path on every
// change, and the [preview] paths may themselves move.
static int ccb_preview_load_config(ccb_args *a, ccb_preview_ctx *ctx, int *rc) {
  ccb_diag_list_init(&ctx->diags);
  ctx->diags_owned = 1;
  if (ccb_config_load(ctx->toml_path, a->variant, &ctx->cfg, &ctx->diags) != CCB_OK) {
    ccb_print_diags(a, &ctx->diags);
    ccb_preview_ctx_free(ctx);
    *rc = 2;
    return 1;
  }
  if (a->preview_watch) {
    ctx->watch_toml = ccb_xstrdup(ctx->toml_path);
    ctx->watch_config_dir = ccb_dirname(ctx->toml_path);
    ctx->watch_styles = ctx->cfg->preview_styles == NULL ? NULL :
      ccb_xstrdup(ctx->cfg->preview_styles);
    ctx->watch_flow = ctx->cfg->preview_flow == NULL ? NULL :
      ccb_xstrdup(ctx->cfg->preview_flow);
  }
  return 0;
}

// Diff runs its own project load: it must report a project without
// a usable [preview] section as exit 2, not the plain preview's
// exit-0 "not applicable", so it branches before the
// not-applicable early return below. --screen overrides the
// positional screen, as in the normal render path.
static int ccb_preview_run_diff(ccb_args *a, ccb_preview_ctx *ctx, int *rc) {
  const char *diff_screen = a->preview_screen != NULL ?
    a->preview_screen : a->positional;
  long diff_width = a->preview_width;
  ccb_str out;
  ccb_str err;
  int drc = 0;
  if (a->preview_diff == NULL) {
    return 0;
  }
  if (!a->preview_width_set && a->preview_device != NULL) {
    diff_width = strcmp(a->preview_device, "tablet") == 0 ?
      CCB_PREVIEW_TABLET_WIDTH : CCB_PREVIEW_PHONE_WIDTH;
  }
  ccb_str_init(&out);
  ccb_str_init(&err);
  drc = pv_diff_run(ctx->cfg, a->preview_diff, diff_screen, diff_width,
    a->preview_ascii, a->preview_strict, a->json, &out, &err);
  if (err.buf != NULL && err.buf[0] != '\0') {
    fputs(err.buf, stderr);
  }
  if (drc == 0 || drc == 1) {
    if (a->json) {
      ccb_str_append(&out, "\n");
    }
    if (a->preview_output != NULL) {
      int wrc = ccb_preview_write_file(a->preview_output,
        out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : ""));
      ccb_str_free(&out);
      ccb_str_free(&err);
      ccb_preview_ctx_free(ctx);
      *rc = wrc == 0 ? drc : 2;
      return 1;
    }
    fputs(out.buf != NULL ? out.buf : "", stdout);
  }
  ccb_str_free(&out);
  ccb_str_free(&err);
  ccb_preview_ctx_free(ctx);
  *rc = drc;
  return 1;
}

// The plain preview's "not applicable" early return.
static int ccb_preview_check_applicable(ccb_args *a, ccb_preview_ctx *ctx, int *rc) {
  if (pv_preview_applicable(ctx->cfg)) {
    return 0;
  }
  if (a->preview_output != NULL) {
    int wrc = ccb_preview_write_file(a->preview_output,
      "preview not applicable to this project\n",
      strlen("preview not applicable to this project\n"));
    ccb_preview_ctx_free(ctx);
    *rc = wrc == 0 ? 0 : 2;
    return 1;
  }
  printf("preview not applicable to this project\n");
  ccb_preview_ctx_free(ctx);
  *rc = 0;
  return 1;
}

// Resolves the scenario by name before the config is freed. An
// unknown name is a usage error (exit 2), like an unknown --screen
// — distinct from a scenario entry that matches nothing, which
// warns (CCB-P1022) during the load.
static int ccb_preview_resolve_scenario(ccb_args *a, ccb_preview_ctx *ctx, const ccb_scenario **scenario_out, int *rc) {
  size_t i = 0;
  *scenario_out = NULL;
  if (a->preview_scenario == NULL) {
    return 0;
  }
  *scenario_out = ccb_config_scenario(ctx->cfg, a->preview_scenario);
  if (*scenario_out != NULL) {
    return 0;
  }
  fprintf(stderr, "error: unknown scenario '%s'\n", a->preview_scenario);
  fprintf(stderr, "available scenarios:");
  for (i = 0; i < ctx->cfg->preview_scenario_count; i++) {
    fprintf(stderr, " %s",
      ctx->cfg->preview_scenarios[i].name != NULL ?
      ctx->cfg->preview_scenarios[i].name : "?");
  }
  fprintf(stderr, "\n");
  ccb_preview_ctx_free(ctx);
  *rc = 2;
  return 1;
}

// Reads and parses the --trace file. Returns 0 with *trace_out set,
// or 2 after printing the usage error.
static int ccb_preview_read_trace(ccb_args *a, pv_trace **trace_out) {
  char *ttext = NULL;
  size_t tlen = 0;
  char *terr = NULL;
  pv_trace *trace = NULL;
  *trace_out = NULL;
  if (ccb_read_file(a->preview_trace, &ttext, &tlen) != CCB_OK) {
    fprintf(stderr, "error: cannot read trace file '%s'\n",
      a->preview_trace);
    return 2;
  }
  trace = pv_trace_parse(ttext, &terr);
  free(ttext);
  if (trace == NULL) {
    fprintf(stderr, "error: invalid trace file '%s': %s\n",
      a->preview_trace, terr != NULL ? terr : "parse error");
    free(terr);
    return 2;
  }
  *trace_out = trace;
  return 0;
}

// Tree-live spawns the server instead of loading a project locally,
// so it branches before the load. The scenario was already validated
// above; the trace is validated here (read + parse, exit 2 on
// failure) so a bad --trace is a usage error, not a server that dies
// mid-session. The server root is re-discovered rather than kept
// from the freed toml path, so this branch owns no extra lifetime.
static int ccb_preview_run_tree_live(ccb_args *a, ccb_preview_ctx *ctx, int *rc) {
  char *root_path = NULL;
  char *root_dir = NULL;
  const char *start = a->preview_screen != NULL ?
    a->preview_screen : a->positional;
  int trc = 0;
  if (!a->preview_tree_live) {
    return 0;
  }
  if (ccb_config_discover(ctx->cwd, a->config_path, &root_path) != CCB_OK) {
    fprintf(stderr, "error: no ccb.toml found in this directory "
      "or any parent\n");
    ccb_preview_ctx_free(ctx);
    *rc = 2;
    return 1;
  }
  root_dir = ccb_dirname(root_path);
  free(root_path);
  if (a->preview_trace != NULL) {
    pv_trace *trace = NULL;
    if (ccb_preview_read_trace(a, &trace) != 0) {
      free(root_dir);
      ccb_preview_ctx_free(ctx);
      *rc = 2;
      return 1;
    }
    pv_trace_free(trace);
  }
  trc = pv_tree_run(NULL, root_dir, a->preview_scenario,
    a->preview_trace, start);
  free(root_dir);
  ccb_preview_ctx_free(ctx);
  *rc = trc;
  return 1;
}

// Loads the project with the resolved scenario. On CCB_ERR_CONFIG
// the project is not applicable, which is exit 0 like the plain
// path. On success the config is freed and the diag list belongs to
// the project.
static int ccb_preview_load_project(ccb_args *a, ccb_preview_ctx *ctx, const ccb_scenario *scenario, pv_project **proj_out, int *rc) {
  ccb_status lrc = CCB_OK;
  pv_project *proj = NULL;
  *proj_out = NULL;
  lrc = pv_project_load_scenario(ctx->cfg, ctx->eff_width, a->preview_ascii,
    a->preview_strict, &ctx->diags, scenario, &proj);
  if (lrc == CCB_ERR_CONFIG) {
    if (a->preview_output != NULL) {
      int wrc = ccb_preview_write_file(a->preview_output,
        "preview not applicable to this project\n",
        strlen("preview not applicable to this project\n"));
      ccb_preview_ctx_free(ctx);
      *rc = wrc == 0 ? 0 : 2;
      return 1;
    }
    printf("preview not applicable to this project\n");
    ccb_preview_ctx_free(ctx);
    *rc = 0;
    return 1;
  }
  if (lrc != CCB_OK || proj == NULL) {
    ccb_print_diags(a, &ctx->diags);
    ccb_preview_ctx_free(ctx);
    *rc = 2;
    return 1;
  }
  // From here the diag list belongs to the project; the caller's list
  // was left empty by pv_project_load, so it must not be freed again.
  ctx->diags_owned = 0;
  ccb_config_free(ctx->cfg);
  ctx->cfg = NULL;
  *proj_out = proj;
  return 0;
}

// Applies the trace after load (marks and argument fills do not
// affect layout, so post-load is fine) and before diagnose, so trace
// warnings print with the rest. A missing or malformed trace file is
// a usage error (exit 2); a step that matches nothing warns
// (CCB-P1023) during apply and rendering continues. A scenario
// applied at load keeps its values: the trace only fills slots still
// unknown.
static int ccb_preview_apply_trace(ccb_args *a, ccb_preview_ctx *ctx, pv_project *proj, int *rc) {
  pv_trace *trace = NULL;
  if (a->preview_trace == NULL) {
    return 0;
  }
  if (ccb_preview_read_trace(a, &trace) != 0) {
    pv_project_free(proj);
    ccb_preview_ctx_free(ctx);
    *rc = 2;
    return 1;
  }
  pv_trace_apply(proj, trace, &proj->diags);
  pv_trace_free(trace);
  return 0;
}

// Renders the storyboard's screen journey. Always finishes the
// command when --storyboard is set.
static int ccb_preview_run_storyboard(ccb_args *a, ccb_preview_ctx *ctx, pv_project *proj, long errors, int *rc) {
  size_t njourney = 0;
  pv_screen **journey = NULL;
  int src = 0;
  if (a->preview_storyboard == NULL) {
    return 0;
  }
  journey = ccb_preview_storyboard_parse(proj, a->preview_storyboard, &njourney);
  if (journey == NULL) {
    pv_project_free(proj);
    ccb_preview_ctx_free(ctx);
    *rc = 2;
    return 1;
  }
  src = ccb_preview_render_storyboard(a, proj, journey, njourney, errors);
  free(journey);
  pv_project_free(proj);
  ccb_preview_ctx_free(ctx);
  *rc = src;
  return 1;
}

// Screen selection: --screen overrides the positional argument. An
// unknown screen is exit 1 with the available screens listed.
static int ccb_preview_select_screen(ccb_preview_ctx *ctx, pv_project *proj, const char *selected, pv_screen **screen_out, int *rc) {
  pv_screen *selected_screen = NULL;
  size_t i = 0;
  *screen_out = NULL;
  if (selected == NULL) {
    return 0;
  }
  for (i = 0; i < proj->nscreens; i++) {
    if (proj->screens[i]->name != NULL &&
        strcmp(proj->screens[i]->name, selected) == 0) {
      selected_screen = proj->screens[i];
      break;
    }
  }
  if (selected_screen != NULL) {
    *screen_out = selected_screen;
    return 0;
  }
  fprintf(stderr, "error: no screen named '%s'\n", selected);
  fprintf(stderr, "available screens:");
  for (i = 0; i < proj->nscreens; i++) {
    fprintf(stderr, " %s",
      proj->screens[i]->name != NULL ? proj->screens[i]->name : "?");
  }
  fprintf(stderr, "\nusage: ccb preview [--screen NAME] [--walk] [screen]\n");
  pv_project_free(proj);
  ccb_preview_ctx_free(ctx);
  *rc = 1;
  return 1;
}

// Interactive walk mode. Always finishes the command when --walk
// is set.
static int ccb_preview_run_walk(ccb_args *a, ccb_preview_ctx *ctx, pv_project *proj, pv_screen *selected_screen, int *rc) {
  int color = 0;
  int wrc = 0;
  if (!a->preview_walk) {
    return 0;
  }
  if (!a->preview_ascii && getenv("NO_COLOR") == NULL) {
    const char *term = getenv("TERM");
    color = term == NULL || strcmp(term, "dumb") != 0;
  }
  wrc = pv_walk_run(proj,
    selected_screen != NULL ? selected_screen->name : NULL,
    proj->frame_width, a->preview_ascii, color, a->preview_record);
  pv_project_free(proj);
  ccb_preview_ctx_free(ctx);
  *rc = wrc;
  return 1;
}

// Watch mode: the loop takes proj, and the caller's path copies are
// freed after it returns. Always finishes the command when --watch
// is set.
static int ccb_preview_run_watch(ccb_args *a, ccb_preview_ctx *ctx, pv_project *proj, long errors, const char *selected, int *rc) {
  int wrc = 0;
  if (!a->preview_watch) {
    return 0;
  }
  wrc = ccb_preview_watch_loop(a, ctx->watch_toml, ctx->watch_config_dir,
    ctx->watch_styles, ctx->watch_flow, ctx->eff_width, proj, errors, selected);
  ccb_preview_ctx_free(ctx);
  *rc = wrc;
  return 1;
}

// Golden update mode. A write failure (2) dominates; otherwise
// preview errors (1) still surface, matching a plain preview run.
// Always finishes the command when --update-goldens is set.
static int ccb_preview_run_goldens(ccb_args *a, ccb_preview_ctx *ctx, pv_project *proj, long errors, int *rc) {
  char *golden_dir = NULL;
  int grc = 0;
  if (!a->preview_update_goldens) {
    return 0;
  }
  golden_dir = ccb_preview_golden_dir(ctx->cwd);
  if (golden_dir == NULL) {
    pv_project_free(proj);
    ccb_preview_ctx_free(ctx);
    *rc = 2;
    return 1;
  }
  grc = ccb_preview_update_goldens(proj, a->preview_ascii, golden_dir);
  free(golden_dir);
  pv_project_free(proj);
  ccb_preview_ctx_free(ctx);
  *rc = grc != 0 ? grc : (errors > 0 ? 1 : 0);
  return 1;
}

int ccb_cmd_preview(ccb_args *a) {
  ccb_preview_ctx ctx;
  pv_project *proj = NULL;
  const ccb_scenario *scenario = NULL;
  const char *selected = NULL;
  pv_screen *selected_screen = NULL;
  long errors = 0;
  int rc = 0;
  memset(&ctx, 0, sizeof(ctx));
  ctx.a = a;
  // --dry-run and --explain are ignored for preview: it never builds.
  CCB_UNUSED(a->dry_run);
  CCB_UNUSED(a->explain);
  if (ccb_preview_check_flags(a, &rc)) {
    return rc;
  }
  if (ccb_preview_discover_config(a, &ctx, &rc)) {
    return rc;
  }
  if (ccb_preview_load_config(a, &ctx, &rc)) {
    return rc;
  }
  if (ccb_preview_run_diff(a, &ctx, &rc)) {
    return rc;
  }
  if (ccb_preview_check_applicable(a, &ctx, &rc)) {
    return rc;
  }
  // Effective frame width: an explicit --width always wins over
  // --device; otherwise the device preset applies (phone = the
  // default 40, tablet = 80); with neither, 0 lets load use its
  // default. Hoisted out of the block so the watch loop reuses the
  // same width on every reload.
  ctx.eff_width = a->preview_width;
  if (!a->preview_width_set && a->preview_device != NULL) {
    ctx.eff_width = strcmp(a->preview_device, "tablet") == 0 ?
      CCB_PREVIEW_TABLET_WIDTH : CCB_PREVIEW_PHONE_WIDTH;
  }
  if (ccb_preview_resolve_scenario(a, &ctx, &scenario, &rc)) {
    return rc;
  }
  if (ccb_preview_run_tree_live(a, &ctx, &rc)) {
    return rc;
  }
  if (ccb_preview_load_project(a, &ctx, scenario, &proj, &rc)) {
    return rc;
  }
  if (ccb_preview_apply_trace(a, &ctx, proj, &rc)) {
    return rc;
  }
  errors = pv_project_diagnose(proj);
  if (ccb_preview_run_storyboard(a, &ctx, proj, errors, &rc)) {
    return rc;
  }
  // Screen selection: --screen overrides the positional argument.
  selected = a->preview_screen != NULL ? a->preview_screen : a->positional;
  if (ccb_preview_select_screen(&ctx, proj, selected, &selected_screen, &rc)) {
    return rc;
  }
  if (ccb_preview_run_walk(a, &ctx, proj, selected_screen, &rc)) {
    return rc;
  }
  if (ccb_preview_run_watch(a, &ctx, proj, errors, selected, &rc)) {
    return rc;
  }
  if (ccb_preview_run_goldens(a, &ctx, proj, errors, &rc)) {
    return rc;
  }
  // Single render, shared with the watch loop's initial render.
  rc = ccb_preview_render_once(a, proj, selected_screen, errors);
  pv_project_free(proj);
  ccb_preview_ctx_free(&ctx);
  return rc;
}
