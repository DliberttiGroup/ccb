// ccb_preview_trace: Chunk 2 trace overlay for preview.
//
// A trace is an ordered list of navigation steps, one per navigation
// taken — recorded from a walk (`ccb preview --walk --record FILE`)
// or, later, emitted by a tiny in-app logger (no logger is built in
// this chunk; the format is documented in docs/preview.md so one can
// be written against it). A trace is a DATA INPUT like a scenario:
// applying it only marks the taken path, fills unknown arguments of
// taken edges, and picks branch entries. It never overrides a static
// literal, never resolves a dynamic target, never hides an unpicked
// branch entry. Trace-supplied values carry "trace" provenance,
// parallel to chunk 1's "scenario" provenance. Steps that match
// nothing warn (CCB-P1023); they never fail the load.
//
// Step ownership: a parsed trace owns its strings (pv_trace_free
// frees them); walk-recorded steps borrow strings from the project
// arena (the trace must be written before pv_project_free). The
// writer only reads, so both uses share it.
#ifndef CCB_PREVIEW_TRACE_H
#define CCB_PREVIEW_TRACE_H

#include "ccb_preview.h"

// One navigation step. screen is required; edge is the button label
// the navigation came from, or NULL for a pick-only step (a branch
// picked without navigating); branch is the branch-entry label the
// step ran under, or NULL when unconditional; args maps positionally
// onto the taken edge's arguments (a NULL entry leaves that slot
// unknown). Back navigation is never recorded: a trace is the
// forward path taken, not a key log.
typedef struct {
  const char *screen;
  const char *edge;
  const char *branch;
  const char **args;
  size_t nargs;
} pv_trace_step;

typedef struct {
  pv_trace_step *steps;
  size_t nsteps, capsteps;
} pv_trace;

// Parse trace JSON text (see docs/preview.md for the format).
// Returns a malloc'd trace, or NULL with *err set to a malloc'd
// message (caller frees) when err is non-NULL. Structural problems
// (unparseable JSON, wrong types, a missing screen or steps array,
// a version other than 1) are errors; semantic mismatches (a step
// naming an unknown screen, edge, or branch) are NOT parse errors —
// they warn (CCB-P1023) at apply time instead.
pv_trace *pv_trace_parse(const char *text, char **err);
void pv_trace_free(pv_trace *t);

// Serialize steps as trace JSON: {"version":1,"steps":[...]}, no
// trailing newline. A NULL arg entry serializes as JSON null.
void pv_trace_write(ccb_str *out, const pv_trace_step *steps,
  size_t nsteps);

// Apply a trace to a loaded project (before or after layout: marks
// and argument fills do not affect layout). Sets
// p->trace_applied and p->trace_step_count (steps in the file).
// Taken edges are marked trace_taken; branch picks go through the
// chunk 1 shared primitive (pv_rt_mark_branch) and argument fills
// through pv_rt_fill_arg, so the merge rules are identical and only
// the data source differs. A NULL trace is a no-op.
void pv_trace_apply(pv_project *p, const pv_trace *t,
  ccb_diag_list *diags);

#endif
