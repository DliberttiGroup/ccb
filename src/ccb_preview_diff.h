// ccb_preview_diff: compare a live preview render against a snapshot
// directory holding golden-format files (one <Screen>.txt frame per
// screen, logic-tree.txt, preview.json), as written by
// --update-goldens. The current render is produced with exactly the
// same calls --update-goldens uses (pv_render_screen with no color,
// pv_render_logic_tree, pv_project_json), so a snapshot created by
// --update-goldens diffs identical under --diff.
//
// Return codes (stable, mirroring the preview exit codes):
//   0 = snapshot identical to the current render
//   1 = differences found (different, missing, or stale files)
//   2 = usage/config error: unreadable snapshot directory, a project
//       with no usable [preview] section (a diff needs something to
//       compare against, so not-applicable is exit 2 here, NOT the
//       exit 0 a plain `ccb preview` gives), or an unknown --screen
//       name. Diagnostics for exit 2 go to err_out; warnings about
//       individual unreadable snapshot files go to err_out too but
//       keep the result at 1, never a crash.
// Ownership: out and err_out are caller-initialized ccb_str values
// (fresh/empty); this function appends to them and never frees them.
// out carries the report: the human report, or, when json is nonzero,
// exactly one JSON object with no trailing newline:
//   {"event":"preview-diff","identical":bool,
//    "files":[{"name":...,"status":"identical|different|missing|stale",
//              "diff":string-or-null}]}
// The diff string is present only for "different" files, and holds
// that file's unified diff. The caller decides where out goes
// (stdout, or a --output file); this module never writes files or
// stdout itself, and never shells out to an external diff binary:
// the line-based LCS diff is implemented in this module.
#ifndef CCB_PREVIEW_DIFF_H
#define CCB_PREVIEW_DIFF_H

#include "ccb_common.h"
#include "ccb_config.h"
#include "ccb_str.h"

// Compare the current preview render against snapshot_dir. With
// screen_or_null set, compare ONLY that screen's <Screen>.txt file
// (logic-tree.txt and preview.json are not compared, and other
// snapshot files are not reported stale). frame_width follows
// pv_project_load (0 = default 40, smaller values clamp); ascii is
// passed to the frame renderer (goldens use no color either way);
// strict is passed to pv_project_load (which also honors [preview]
// strict = true); json nonzero selects the JSON report.
int pv_diff_run(const ccb_config *cfg, const char *snapshot_dir,
  const char *screen_or_null, long frame_width, int ascii, int strict,
  int json, ccb_str *out, ccb_str *err_out);

#endif
