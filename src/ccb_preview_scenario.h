// ccb_preview_scenario: Chunk 1 scenario merge for preview, shared
// with Chunk 2 trace overlay.
//
// A scenario is declared runtime facts (see docs/preview.md): branch
// picks, list rows, textfield content, and fills for unknown edge
// arguments. The merge is a DATA INPUT, never evaluation: it only
// fills unknowns and picks branches, and it never overrides a static
// literal, never resolves a dynamic target, never hides an unpicked
// branch entry. Scenario-supplied values are marked by provenance
// (see pv_box/pv_edge fields in ccb_preview.h) so human output and
// JSON can tell them from static values. Entries that match nothing
// warn (CCB-P1022); they never fail the load and are never silently
// ignored.
//
// Chunk 2 reuses this merge: a trace supplies the same kinds of
// values (branch picks, argument fills) through the same primitives
// below, with PV_PROV_TRACE provenance. Only the data source
// differs — the trace module never duplicates the merge rules. The
// two primitives that both sources share are pv_rt_mark_branch
// (pick a branch entry) and pv_rt_fill_arg (fill one unknown
// argument slot); each sets the provenance flag for its source and
// leaves static values untouched.
#ifndef CCB_PREVIEW_SCENARIO_H
#define CCB_PREVIEW_SCENARIO_H

#include "ccb_preview.h"

// Which runtime overlay supplied a value. Scenario and trace flags
// are independent: both overlays may apply, and each value keeps
// its own provenance.
typedef enum {
  PV_PROV_SCENARIO,
  PV_PROV_TRACE
} pv_prov;

// Push a warning diagnostic with the given code and a
// "label: detail" message (label is "scenario 'name'" for a
// scenario, "trace" for a trace). Shared by both overlays so a
// mismatch warns the same way from either source.
void pv_rt_warn(ccb_diag_list *diags, const char *code,
  const char *label, const char *detail);

// Mark every branch-entry box with this label in screen (a pick
// marks, it never hides). Returns the number of boxes marked; 0
// means the entry does not exist in this screen. Strings are
// borrowed; only flags change.
size_t pv_rt_mark_branch(pv_project *p, pv_screen *screen,
  const char *entry, pv_prov prov);

// Fill edge argument slot k with value when that slot is unknown
// (NULL). Returns 1 when filled, 0 when the slot already holds a
// static literal (never overridden) or k is out of range. The
// value is copied into the project arena and the slot's provenance
// flag for prov is set.
int pv_rt_fill_arg(pv_project *p, pv_edge *e, size_t k,
  const char *value, pv_prov prov);

// Apply a scenario to a loaded-but-unlaid-out project. Must run
// after parsing and @string/@color resolution (so scenario keys
// match the labels as rendered) and before layout (so scenario list
// rows size the list like static rows do). Records the scenario
// name on the project for JSON output. A NULL scenario is a no-op.
// Strings from the scenario are copied into the project arena; the
// scenario itself stays config-owned.
void pv_scenario_apply(pv_project *p, const ccb_scenario *sc,
  ccb_diag_list *diags);

#endif
