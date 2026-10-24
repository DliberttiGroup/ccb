// ccb_preview_gate: opt-in build gate over preview diagnostics.
// The gate answers one question for `ccb check` / `ccb build`: does
// this project's preview have ERROR diagnostics? Warnings never fail
// the gate. The gate never forces preview onto a project that does
// not use it: a project with no usable [preview] section passes
// silently (return 0, nothing appended to failed_out).
//
// Two pieces, kept separate on purpose:
//   pv_gate_enabled -- the SWITCH: should the gate run at all?
//     Precedence, highest first: flag_off beats flag_on beats config.
//     flag_off forces 0, else flag_on forces 1, else the config value
//     ([preview] gate, default false). Both flags set is a caller
//     error the CLI must reject, but enabled() still resolves it
//     deterministically: flag_off wins.
//   pv_gate_run -- the CHECKER: load the project, diagnose it, and
//     collect the ERROR diagnostics. It does NOT consult enabled():
//     the caller (the CLI wiring) checks pv_gate_enabled first and
//     only calls pv_gate_run when the gate is on. Calling pv_gate_run
//     directly on a gate-off project still reports its errors; that
//     is by design, so the checker stays testable without the CLI.
//
// Strict: the gate's effective strict is cfg->preview_strict OR the
// strict argument (the CLI --strict flag). pv_project_load already
// ORs them into p->strict, and the gate classifies with p->strict,
// so the config flag and the CLI flag both promote CCB-P1001 and
// CCB-P1014 to errors.
//
// Return codes from pv_gate_run: 0 = pass (including preview-not-
// applicable, which passes silently); 1 = fail, with one concise
// line per failing diagnostic ("CCB-P1005: navigation to unknown
// screen 'Ghost'") appended to *failed_out, one line each, newline
// terminated; 2 = genuine failure that is not not-applicable: no
// config at all, or pv_project_load failing on a project that DOES
// have a usable [preview] section. (With the current loader the
// second case is defensive only: the loader returns CCB_ERR_CONFIG
// exactly for the not-applicable shape, which pv_gate_run screens
// out before loading, and otherwise tolerates unreadable files as
// diagnostics, not failures. The 2 exists so a future loader failure
// mode cannot masquerade as a pass.) The CLI maps gate failure to
// exit 1 and prints the failed_out lines on stderr; it skips the
// gate entirely only when config load itself fails, so the pipeline
// reports normally in that case.
//
// Ownership: failed_out is caller-owned; pv_gate_run only appends.
// A NULL failed_out is allowed: the return code is still computed
// and the lines are dropped.
#ifndef CCB_PREVIEW_GATE_H
#define CCB_PREVIEW_GATE_H

#include "ccb_common.h"
#include "ccb_config.h"
#include "ccb_str.h"

// Switch: 1 when the gate should run, 0 when it should not.
int pv_gate_enabled(const ccb_config *cfg, int flag_on, int flag_off);
// Checker: 0 pass, 1 fail (lines appended to failed_out), 2 failure.
int pv_gate_run(const ccb_config *cfg, long frame_width, int ascii,
  int strict, ccb_str *failed_out);

#endif
