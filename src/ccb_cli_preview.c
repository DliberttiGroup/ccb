// ccb_cli_preview: `ccb preview` file helpers + watch mode.
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

// ---------- preview ----------

// Device width presets for `ccb preview --device`: phone is the
// existing default frame width; tablet is a wider frame. An explicit
// --width always wins over --device (resolved by the caller).

// Write data to a file for --output. Returns 0 on success, -1 on
// failure (message already printed to stderr).
int ccb_preview_write_file(const char *path, const char *data,
    size_t len) {
  if (ccb_write_file(path, data, len) != CCB_OK) {
    fprintf(stderr, "error: cannot write %s\n", path);
    return -1;
  }
  return 0;
}

// Resolve the golden directory for --update-goldens. The test-only
// CCB_GOLDEN_DIR override wins when set; otherwise walk up from cwd
// looking for tests/golden/preview. Returns a malloc'd path or NULL
// (message already printed to stderr).
char *ccb_preview_golden_dir(const char *cwd) {
  const char *env = getenv("CCB_GOLDEN_DIR");
  if (env != NULL && env[0] != '\0') {
    return ccb_xstrdup(env);
  }
  {
    char *dir = ccb_xstrdup(cwd);
    for (;;) {
      char *cand = ccb_path_join3(dir, "tests", "golden/preview");
      // ccb_path_join3 joins with single separators, so the third
      // argument's embedded slash survives as a path component.
      if (ccb_path_exists(cand)) {
        free(dir);
        return cand;
      }
      free(cand);
      {
        char *parent = ccb_dirname(dir);
        if (strcmp(parent, dir) == 0) {
          free(parent);
          break;
        }
        free(dir);
        dir = parent;
      }
    }
    free(dir);
  }
  fprintf(stderr, "error: cannot locate golden directory "
    "(run from inside the CCB tree, or set CCB_GOLDEN_DIR)\n");
  return NULL;
}

// Regenerate the golden files from the current render: one frame file
// per screen (<Screen>.txt), logic-tree.txt, and preview.json (the
// JSON form with no trailing newline, matching the checked-in file).
// Frames use the --ascii flag and no color, matching the goldens.
// Returns 0 on success, 2 on any write failure.
int ccb_preview_update_goldens(pv_project *proj, int ascii,
    const char *golden_dir) {
  size_t i = 0;
  ccb_str out;
  ccb_str_init(&out);
  for (i = 0; i < proj->nscreens; i++) {
    ccb_str name;
    char *path = NULL;
    ccb_str_init(&out);
    pv_render_screen(&out, proj->screens[i], proj->frame_width, ascii, 0);
    ccb_str_init(&name);
    ccb_str_append(&name, proj->screens[i]->name != NULL ?
      proj->screens[i]->name : "?");
    ccb_str_append(&name, ".txt");
    path = ccb_path_join(golden_dir, name.buf);
    ccb_str_free(&name);
    if (ccb_preview_write_file(path, out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : "")) != 0) {
      free(path);
      ccb_str_free(&out);
      return 2;
    }
    free(path);
    ccb_str_free(&out);
  }
  ccb_str_init(&out);
  pv_render_logic_tree(&out, proj);
  {
    char *path = ccb_path_join(golden_dir, "logic-tree.txt");
    int rc = ccb_preview_write_file(path,
      out.buf != NULL ? out.buf : "",
      strlen(out.buf != NULL ? out.buf : ""));
    free(path);
    ccb_str_free(&out);
    if (rc != 0) {
      return 2;
    }
  }
  ccb_str_init(&out);
  pv_project_json(&out, proj);
  {
    char *path = ccb_path_join(golden_dir, "preview.json");
    int rc = ccb_preview_write_file(path,
      out.buf != NULL ? out.buf : "",
      strlen(out.buf != NULL ? out.buf : ""));
    free(path);
    ccb_str_free(&out);
    if (rc != 0) {
      return 2;
    }
  }
  printf("updated goldens in %s\n", golden_dir);
  return 0;
}

// ---------- preview watch ----------

// Free the watch path copies (all NULL-safe). The watch loop takes
// its own copies of the dirs, so the caller's copies are freed at the
// watch branch; the early exits above free them instead.
void ccb_preview_watch_paths_free(char *toml, char *dir,
    char *styles, char *flow) {
  free(toml);
  free(dir);
  free(styles);
  free(flow);
}

// Render one preview, exactly as a single `ccb preview` does: human
// frames + logic tree, or the JSON object (with its trailing newline).
// --output writes the file and leaves stdout empty. Diagnostics go to
// stderr in human mode and ride inside the JSON object in JSON mode.
// Returns 0 (ok), 1 (preview errors), or 2 (output write failure).
// The caller keeps owning proj.
int ccb_preview_render_once(ccb_args *a, pv_project *proj,
    pv_screen *screen, long errors) {
  size_t i = 0;
  if (a->json) {
    ccb_str out;
    ccb_str_init(&out);
    pv_project_json(&out, proj);
    // The file carries exactly what stdout would have carried.
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
    // A file is not a terminal: never write ANSI colors into it.
    if (!a->preview_ascii && a->preview_output == NULL &&
        getenv("NO_COLOR") == NULL) {
      term = getenv("TERM");
      color = term == NULL || strcmp(term, "dumb") != 0;
    }
    ccb_str_init(&out);
    if (screen != NULL) {
      pv_render_screen(&out, screen, proj->frame_width,
        a->preview_ascii, color);
    } else {
      for (i = 0; i < proj->nscreens; i++) {
        pv_render_screen(&out, proj->screens[i], proj->frame_width,
          a->preview_ascii, color);
      }
    }
    pv_render_logic_tree(&out, proj);
    if (a->preview_output != NULL) {
      int rc = ccb_preview_write_file(a->preview_output,
        out.buf != NULL ? out.buf : "",
        strlen(out.buf != NULL ? out.buf : ""));
      ccb_str_free(&out);
      // Diagnostics in human form, to stderr like other commands.
      ccb_print_diags_human(&proj->diags);
      return rc == 0 ? (errors > 0 ? 1 : 0) : 2;
    }
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
    // Diagnostics in human form, to stderr like other commands.
    ccb_print_diags_human(&proj->diags);
  }
  return errors > 0 ? 1 : 0;
}

// One watch render, with the refresh framing for re-renders (first is
// 0): TTY stdout clears first, piped human mode gets a delimiter line
// on stdout, JSONL mode gets neither (each render stays one JSON
// object + newline), and --output mode reports on stderr instead
// (stdout stays empty). A selected screen that vanished since the
// last render prints a diagnostic naming it and renders nothing; the
// loop keeps watching. Returns the single-render exit code; the loop
// ignores it (preview errors surface via diagnostics, and a signal
// stop still exits 0).
static int ccb_preview_watch_render(ccb_args *a, pv_project *proj,
    const char *selected_name, long errors, int first) {
  pv_screen *screen = NULL;
  size_t i = 0;
  if (selected_name != NULL) {
    for (i = 0; i < proj->nscreens; i++) {
      if (proj->screens[i]->name != NULL &&
          strcmp(proj->screens[i]->name, selected_name) == 0) {
        screen = proj->screens[i];
        break;
      }
    }
    if (screen == NULL) {
      fprintf(stderr, "error: screen '%s' no longer exists; still watching\n",
        selected_name);
      return 0;
    }
  }
  if (!first) {
    if (a->preview_output != NULL) {
      fprintf(stderr, "preview refreshed: %s\n", a->preview_output);
    } else if (isatty(STDOUT_FILENO)) {
      fputs("\033[2J\033[H", stdout);
    } else if (!a->json) {
      fputs("--- ccb preview refresh ---\n", stdout);
    }
  }
  return ccb_preview_render_once(a, proj, screen, errors);
}

// NULL-aware dir comparison for the watched-dirs-moved check.
static int ccb_preview_watch_dirs_changed(const char *old_dir,
    const char *new_dir) {
  if (old_dir == NULL || new_dir == NULL) {
    return old_dir != new_dir;
  }
  return strcmp(old_dir, new_dir) != 0;
}

// The watch loop. Borrows toml_path, config_dir, styles_dir, flow_dir
// and selected_name (NULL renders every screen); takes ownership of
// proj and frees it on every exit path. Signals are installed once
// the initial render is out: the handlers only set pv_watch_stop, the
// loop checks it every tick and exits 0 on stop (nothing terminal
// related to restore, since watch mode never enters raw mode, but
// stdout is flushed first). A reload or load failure after a change
// (a half-saved TOML, broken CSS/Kotlin, a [preview] section removed)
// prints diagnostics and keeps watching on the last good render; a
// broken save never kills the loop. Returns 0 on a signal stop, 2 on
// a watch setup or wait failure.
// Re-scan the watched dirs after a move and adopt the fresh snapshot,
// so the move itself does not trigger one spurious refresh. A failed
// scan keeps the old baseline; the next tick rescans.
static void ccb_preview_watch_rebaseline(const char *config_dir,
    const char *cur_styles, const char *cur_flow, const char *toml_path,
    pv_watch_snapshot **snap) {
  pv_watch_snapshot *fresh = NULL;
  if (pv_watch_snapshot_take(config_dir, cur_styles, cur_flow,
      toml_path, &fresh) == CCB_OK) {
    pv_watch_snapshot_free(*snap);
    *snap = fresh;
  }
}

int ccb_preview_watch_loop(ccb_args *a, const char *toml_path,
    const char *config_dir, const char *styles_dir, const char *flow_dir,
    long eff_width, pv_project *proj, long errors,
    const char *selected_name) {
  pv_watch_ctx *ctx = NULL;
  pv_watch_snapshot *snap = NULL;
  pv_watch_debounce deb;
  char *cur_styles = styles_dir == NULL ? NULL : ccb_xstrdup(styles_dir);
  char *cur_flow = flow_dir == NULL ? NULL : ccb_xstrdup(flow_dir);
  int used_inotify = 0;
  int rc = 0;
  memset(&deb, 0, sizeof(deb));
  // Initial render, exactly like a single preview.
  ccb_preview_watch_render(a, proj, selected_name, errors, 1);
  fflush(stdout);
  pv_watch_install_signals();
  if (pv_watch_stop) {
    goto watch_done;
  }
  if (pv_watch_begin(config_dir, cur_styles, cur_flow, toml_path, &ctx,
      &used_inotify) != CCB_OK) {
    fprintf(stderr, "error: cannot watch preview inputs\n");
    rc = 2;
    goto watch_done;
  }
  CCB_UNUSED(used_inotify);
  if (pv_watch_snapshot_take(config_dir, cur_styles, cur_flow, toml_path,
      &snap) != CCB_OK) {
    fprintf(stderr, "error: cannot scan preview inputs\n");
    rc = 2;
    goto watch_done;
  }
  for (;;) {
    int w = 0;
    if (pv_watch_stop) {
      break;
    }
    w = pv_watch_wait(ctx, 50);
    if (pv_watch_stop) {
      break;
    }
    if (w < 0) {
      fprintf(stderr, "error: preview watch failed\n");
      rc = 2;
      break;
    }
    if (w == 1) {
      pv_watch_debounce_event(&deb, pv_watch_now_ms());
    }
    if (!pv_watch_debounce_due(&deb, pv_watch_now_ms())) {
      continue;
    }
    pv_watch_debounce_reset(&deb);
    {
      pv_watch_snapshot *next = NULL;
      ccb_diag_list rdiags;
      ccb_config *ncfg = NULL;
      pv_project *nproj = NULL;
      long nerrors = 0;
      if (pv_watch_snapshot_take(config_dir, cur_styles, cur_flow,
          toml_path, &next) != CCB_OK) {
        // A scan failure (a directory briefly missing) is not a
        // reason to quit; the next tick rescans.
        continue;
      }
      if (!pv_watch_snapshot_changed(snap, next)) {
        pv_watch_snapshot_free(next);
        continue;
      }
      // Adopt the new baseline before reloading, so a broken save
      // does not re-report on every tick.
      pv_watch_snapshot_free(snap);
      snap = next;
      // Reload config from the watched toml path: [preview] paths
      // may themselves have changed.
      ccb_diag_list_init(&rdiags);
      if (ccb_config_load(toml_path, a->variant, &ncfg, &rdiags) != CCB_OK) {
        ccb_print_diags_human(&rdiags);
        ccb_diag_list_free(&rdiags);
        continue;
      }
      // A successful load leaves no diagnostics to print; the call
      // is harmless when the list is empty and surfaces any warning
      // the loader did emit.
      ccb_print_diags_human(&rdiags);
      ccb_diag_list_free(&rdiags);
      if (!pv_preview_applicable(ncfg)) {
        fprintf(stderr, "error: preview no longer applicable to this "
          "project; still watching\n");
        ccb_config_free(ncfg);
        continue;
      }
      {
        ccb_diag_list ldiags;
        ccb_status lrc = CCB_OK;
        ccb_diag_list_init(&ldiags);
        lrc = pv_project_load(ncfg, eff_width, a->preview_ascii,
          a->preview_strict, &ldiags, &nproj);
        if (lrc != CCB_OK || nproj == NULL) {
          ccb_print_diags_human(&ldiags);
          ccb_diag_list_free(&ldiags);
          ccb_config_free(ncfg);
          continue;
        }
        // Success: ownership of ldiags transferred to nproj; the
        // caller's list is empty and must not be freed again.
      }
      nerrors = pv_project_diagnose(nproj);
      // The watched dirs may have moved: re-point the watch at the
      // new dirs so later changes there are seen. The new context is
      // begun before the old one ends, so a begin failure keeps the
      // old watch (and the fresh render) intact.
      if (ccb_preview_watch_dirs_changed(cur_styles,
          ncfg->preview_styles) ||
          ccb_preview_watch_dirs_changed(cur_flow,
          ncfg->preview_flow)) {
        pv_watch_ctx *nctx = NULL;
        if (pv_watch_begin(config_dir, ncfg->preview_styles,
            ncfg->preview_flow, toml_path, &nctx,
            &used_inotify) == CCB_OK) {
          pv_watch_end(ctx);
          ctx = nctx;
          free(cur_styles);
          free(cur_flow);
          cur_styles = ncfg->preview_styles == NULL ? NULL :
            ccb_xstrdup(ncfg->preview_styles);
          cur_flow = ncfg->preview_flow == NULL ? NULL :
            ccb_xstrdup(ncfg->preview_flow);
          // Re-baseline against the new dirs so the move itself does
          // not trigger one spurious refresh.
          ccb_preview_watch_rebaseline(config_dir, cur_styles, cur_flow,
            toml_path, &snap);
        } else {
          fprintf(stderr, "error: cannot watch new preview directories; "
            "still watching the old ones\n");
        }
      }
      ccb_config_free(ncfg);
      pv_project_free(proj);
      proj = nproj;
      errors = nerrors;
      ccb_preview_watch_render(a, proj, selected_name, errors, 0);
      fflush(stdout);
    }
  }
watch_done:
  if (snap != NULL) {
    pv_watch_snapshot_free(snap);
  }
  if (ctx != NULL) {
    pv_watch_end(ctx);
  }
  pv_project_free(proj);
  free(cur_styles);
  free(cur_flow);
  fflush(stdout);
  return rc;
}
