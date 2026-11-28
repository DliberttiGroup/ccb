// ccb_sched: the parallel task-graph executor (Step 8, V2 Phase 2).
// A deliberately simple pthread worker pool over a ccb_graph:
// centralized ready queue, per-task pending-dependency counters,
// configurable worker count, failure propagation, cancellation,
// orderly shutdown. No work stealing, no distributed scheduling,
// no priorities, no adaptive heuristics.
//
// The sequential executor (ccb_graph_execute) stays the correctness
// reference and the default: the pipeline driver still calls it.
// This executor is opt-in API only; no build path calls it yet, and
// no CLI flag or config option enables parallel builds, because
// real pipeline steps are not yet safe to run concurrently (see the
// contract below). Stage 6 — parallel execution of real build
// tasks — is NOT reached: it needs per-task diag/log/cache state
// first, which is Step 11 work.
//
// Parallel-run contract for task run functions. A run function
// executed by this scheduler must own every piece of state it
// mutates: it must NOT write the shared ccb_log (not even
// ccb_log_cache — the log's last_cache_reason stash is shared),
// the shared ccb_diag_list, the shared ccb_cache, or the pipeline
// pipe. It may read shared immutable state (config, resolved
// toolchain) and write only its own outputs (its own work dir, its
// own artifact struct, its own cache delta). The scheduler itself
// serializes task lifecycle logging (task start/done/failed events)
// under its mutex. Consequences, by design: in parallel mode a
// skipped task's skip_reason is NULL (the sequential executor reads
// it from the shared log stash, which has no per-task owner here),
// and diagnostics a failed task adds are not stamped with its task
// ID (stamping reads the shared list, which workers must not push
// to concurrently) — the failing status is still returned, and
// every task that never ran is marked CANCELLED with its state
// visible in the summary.
//
// Failure semantics: the first failure wins (its status is
// returned). WAITING and READY tasks are marked CANCELLED and never
// start; a task already RUNNING when another task fails runs to
// completion (no preemption — a running kotlinc cannot be safely
// killed mid-write) and lands DONE or FAILED on its own merit.
// ccb_sched_cancel does the same marking on request: tasks already
// running still finish; nothing new starts. Cancellation is not a
// failure: run returns CCB_OK with the cancelled states visible.
#ifndef CCB_SCHED_H
#define CCB_SCHED_H

#include "ccb_common.h"
#include "ccb_diag.h"
#include "ccb_graph.h"
#include "ccb_log.h"

// Opaque scheduler: create, optionally cancel from another thread,
// run (spawns workers, joins them), free. run must be called
// exactly once; free after run returns.
typedef struct ccb_sched ccb_sched;

// Create a scheduler for g. workers is the pool size (1 behaves
// like the sequential executor, in insertion order among ready
// tasks). Returns NULL when workers is 0, with a CCB-E1016
// diagnostic when diags is non-NULL. g, ctx, log, and diags are
// borrowed for the scheduler's lifetime; the graph must already
// be built (validation runs inside ccb_sched_run, before any
// thread spawns, exactly like the sequential executor).
ccb_sched *ccb_sched_create(ccb_graph *g, void *ctx, ccb_log *log,
  ccb_diag_list *diags, size_t workers);
void ccb_sched_free(ccb_sched *s);
// Cancel: WAITING and READY tasks become CANCELLED and never run.
// Safe to call from another thread, including while run is in
// progress; running tasks finish. Also safe before run starts.
void ccb_sched_cancel(ccb_sched *s);
// Execute the graph. Validates first (CCB_ERR_CONFIG on failure,
// no thread spawned). Returns the first failing task's status, or
// CCB_OK when every task finished, was skipped, or was cancelled.
ccb_status ccb_sched_run(ccb_sched *s);

// One-shot convenience: create, run, free.
ccb_status ccb_graph_execute_parallel(ccb_graph *g, void *ctx,
  ccb_log *log, ccb_diag_list *diags, size_t workers);

#endif
