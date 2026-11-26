// ccb_preview_gate: opt-in build gate over preview diagnostics.
// See ccb_preview_gate.h for the switch/checker split, the
// flag_off > flag_on > config precedence, and the 0/1/2 contract.
#include "ccb_common.h"
#include "ccb_preview_gate.h"
#include "ccb_preview.h"

int pv_gate_enabled(const ccb_config *cfg, int flag_on, int flag_off) {
  // Explicit flags beat config; flag_off beats flag_on if both are
  // somehow set (the CLI should reject that combination, but the
  // resolution here stays deterministic regardless).
  if (flag_off) {
    return 0;
  }
  if (flag_on) {
    return 1;
  }
  return cfg != NULL && cfg->preview_gate;
}

int pv_gate_run(const ccb_config *cfg, long frame_width, int ascii,
    int strict, ccb_str *failed_out) {
  pv_project *p = NULL;
  ccb_status st = CCB_OK;
  size_t i = 0;
  int failed = 0;
  if (cfg == NULL) {
    return 2;
  }
  // Preview not applicable: no [preview] section, or a [preview]
  // section with neither styles, flow, nor real Kotlin sources
  // (Phase 2). Passes silently; the gate never forces preview onto
  // a project that does not use it.
  if (!pv_preview_applicable(cfg)) {
    return 0;
  }
  // pv_project_load ORs strict with cfg->preview_strict into
  // p->strict, so classifying with p->strict below implements the
  // documented effective-strict rule exactly.
  st = pv_project_load(cfg, frame_width, ascii, strict, NULL, &p);
  if (st != CCB_OK || p == NULL) {
    pv_project_free(p);
    return 2;
  }
  (void)pv_project_diagnose(p);
  for (i = 0; i < p->diags.count; i++) {
    const char *code = p->diags.items[i].code;
    const char *msg = p->diags.items[i].message;
    if (!pv_diag_is_error(code, p->strict)) {
      continue;
    }
    failed = 1;
    if (failed_out != NULL) {
      ccb_str_appendf(failed_out, "%s: %s\n", code != NULL ? code : "",
        msg != NULL ? msg : "");
    }
  }
  pv_project_free(p);
  return failed ? 1 : 0;
}
