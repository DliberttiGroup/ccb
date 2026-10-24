// ccb_pipeline: the build pipeline (spec §5, stages in order).
// Stage 0 load/validate -> deps/AAR extraction -> AAR manifest merge ->
// aapt2 compile -> aapt2 link -> kotlinc (+ javac) -> d8 (or r8 for
// release+minify) -> NDK libs -> package (CCB's own ZIP writer) ->
// zipalign -> apksigner -> verify.
// Steps are cached by input fingerprint: an unchanged step is skipped and
// its previous outputs reused. A changed tool fingerprint invalidates all.
#ifndef CCB_PIPELINE_H
#define CCB_PIPELINE_H

#include "ccb_common.h"
#include "ccb_diag.h"
#include "ccb_log.h"

typedef struct {
  int json;
  int dry_run;
  int verbose;
  int explain;
  const char *variant_override;
  const char *config_path;
  const char *workdir;
} ccb_build_opts;

typedef struct {
  int steps_run;
  int steps_skipped;
  int up_to_date;
  char *apk_path;
} ccb_build_result;

// Run a build. Returns CCB_OK or a spec §6.4 status; diags filled on
// failure. In explain mode nothing is executed and result is empty.
ccb_status ccb_pipeline_build(const ccb_build_opts *opts, ccb_build_result *result, ccb_diag_list *diags);
void ccb_build_result_free(ccb_build_result *r);

#endif
