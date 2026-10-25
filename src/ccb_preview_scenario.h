// ccb_preview_scenario: Chunk 1 scenario merge for preview.
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
#ifndef CCB_PREVIEW_SCENARIO_H
#define CCB_PREVIEW_SCENARIO_H

#include "ccb_preview.h"

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
