// ccb_graph: the build task graph (DAG) engine — V2 §3 layer 3, §4.
// A task is a unit of work with an explicit ID, type, declared inputs
// and outputs, dependencies on other tasks, an execution state, a
// cache policy, and a run function. The graph is validated (duplicate
// IDs, unknown dependencies, unproduced inputs, cycles), planned
// (topological order), and executed in order by a sequential executor.
// The executor is deliberately sequential: parallel scheduling is
// Phase 2, which replaces only the executor, not this module.
//
// What this module does NOT do: it knows nothing about Android, the
// pipeline stages, or the cache. Callers (ccb_pipeline_graph) build a
// graph of pipeline stages on top of it. It owns no global state: all
// state lives in the ccb_graph the caller passes in.
//
// Inputs are logical names, not necessarily file paths. A SOURCE input
// is a project file the task reads (validated on disk only by
// ccb_graph_check_source_inputs, which callers run when it applies —
// a build graph must not fail validation for an intermediate that a
// later task has not produced yet). A GENERATED input must be claimed
// as an output by another task in the graph, else validation fails
// with CCB-E1013: an input with no producer is a missing input.
//
// Diagnostics: CCB-E1012 duplicate task ID, CCB-E1013 unknown
// dependency / unproduced generated input / duplicate output /
// duplicate or conflicting declaration / GENERATED input whose
// producer the consumer does not depend on, CCB-E1014 task
// dependency cycle (names the cycle path, following the CCB-E1010
// module-cycle pattern), CCB-E1015 missing source input.
#ifndef CCB_GRAPH_H
#define CCB_GRAPH_H

#include "ccb_common.h"
#include "ccb_diag.h"
#include "ccb_log.h"
#include "ccb_str.h"

// Task lifecycle (V2 §5). WAITING -> READY -> RUNNING -> DONE;
// RUNNING -> FAILED; WAITING/READY/RUNNING -> CANCELLED when a task
// the build depends on fails (Phase 1 cancels, it never resumes).
typedef enum {
  CCB_TASK_WAITING = 0,
  CCB_TASK_READY,
  CCB_TASK_RUNNING,
  CCB_TASK_DONE,
  CCB_TASK_FAILED,
  CCB_TASK_CANCELLED
} ccb_task_state;

const char *ccb_task_state_name(ccb_task_state s);

// Cache policy: FINGERPRINT means the task may skip when its inputs
// are unchanged (the run function decides and sets skipped);
// ALWAYS_RUN means the task runs on every build (e.g. dexcheck).
typedef enum {
  CCB_TASK_CACHE_FINGERPRINT = 0,
  CCB_TASK_CACHE_ALWAYS_RUN
} ccb_task_cache;

// Input kind: SOURCE is a project file, GENERATED is produced by
// another task in the graph, ENV is an environment variable read by
// name (the value is never recorded, only the name, so a signing
// password variable is a declared input without leaking the secret
// into the plan, the summary, or affected-task matching).
typedef enum {
  CCB_TASK_INPUT_SOURCE = 0,
  CCB_TASK_INPUT_GENERATED,
  CCB_TASK_INPUT_ENV
} ccb_task_input_kind;

const char *ccb_task_input_kind_name(ccb_task_input_kind kind);

typedef struct ccb_task ccb_task;

// Run a task. ctx is the caller's execution context (for the
// pipeline, a ccb_pipe *). The run function sets task->skipped when
// it did no work because its inputs were unchanged, and returns
// CCB_OK or a spec §6.4 status. A NULL run function is a no-op that
// succeeds (used for tasks completed before execution, e.g. config).
typedef ccb_status (*ccb_task_run_fn)(void *ctx, ccb_task *task);

typedef struct {
  char *name;
  ccb_task_input_kind kind;
  // Required inputs must exist for ccb_graph_check_source_inputs;
  // an optional input may be absent (the task tolerates it, e.g. a
  // debug keystore the step generates when missing). Required is
  // the default: ccb_task_add_input declares a required input.
  int required;
} ccb_task_input;

struct ccb_task {
  char *id; // stable, unique within the graph; used in diagnostics
  char *type; // provider/type, e.g. "aapt2", "kotlin", "dex"
  ccb_task_input *inputs;
  size_t inputs_count;
  size_t inputs_cap;
  char **outputs;
  size_t outputs_count;
  size_t outputs_cap;
  char **deps; // task IDs this task depends on, in declared order
  size_t deps_count;
  size_t deps_cap;
  ccb_task_state state;
  ccb_task_cache cache;
  ccb_task_run_fn run;
  long duration_ms; // set by the executor; -1 when never run
  int skipped; // set by the run function on a cache-hit skip
  // Why the task skipped, when it did. Borrowed: either a string
  // literal or the log's last cache reason, both outliving the
  // task; never freed. NULL when the task ran or when its skip
  // logged no reason. Set by the executor, not the run function.
  const char *skip_reason;
};

typedef struct {
  ccb_task **tasks; // insertion order; the plan reorders by topology
  size_t count;
  size_t cap;
} ccb_graph;

ccb_graph *ccb_graph_create(void);
void ccb_graph_free(ccb_graph *g);
ccb_task *ccb_graph_find(ccb_graph *g, const char *id);

// Add a task. Returns NULL (with a CCB-E1012 diag when diags is
// non-NULL) when a task with this ID already exists.
ccb_task *ccb_graph_add_task(ccb_graph *g, const char *id,
  const char *type, ccb_task_cache cache, ccb_task_run_fn run,
  ccb_diag_list *diags);
void ccb_task_add_input(ccb_task *t, const char *name,
  ccb_task_input_kind kind);
// Declare an input with an explicit required flag. A required
// SOURCE input must exist for ccb_graph_check_source_inputs; an
// optional one may be absent. ccb_task_add_input is this with
// required = 1.
void ccb_task_add_input_opt(ccb_task *t, const char *name,
  ccb_task_input_kind kind, int required);
void ccb_task_add_output(ccb_task *t, const char *name);
void ccb_task_add_dep(ccb_task *t, const char *dep_id);

// Validate the graph: duplicate IDs, duplicate or conflicting
// declarations (a dependency, input, or output listed twice; one
// input declared with conflicting kinds; a SOURCE input that is
// also the task's own output), unknown dependencies, duplicate
// outputs, GENERATED inputs with no producer or whose producer the
// consumer does not depend on (directly or transitively), and
// cycles.
// Reports every problem found (except the producer-dependency and
// cycle checks, which need resolvable dependencies and are skipped
// when any earlier check fails). Returns CCB_OK or CCB_ERR_CONFIG;
// diags filled on failure.
ccb_status ccb_graph_validate(ccb_graph *g, ccb_diag_list *diags);

// Check required SOURCE inputs against the filesystem. A missing
// required source input is CCB-E1015 naming the task and the input.
// Optional SOURCE inputs are skipped, as are GENERATED and ENV
// inputs. Callers that build graphs over generated intermediates
// run this only when the sources are expected to exist (the
// pipeline does not: config load already rejects missing declared
// paths, and its steps diagnose the rest with their own codes).
ccb_status ccb_graph_check_source_inputs(ccb_graph *g,
  ccb_diag_list *diags);
ccb_status ccb_graph_check_source_inputs(ccb_graph *g,
  ccb_diag_list *diags);

// Topological execution order (Kahn, insertion order breaks ties).
// *order_out is a malloc'd array of borrowed task pointers; the
// caller frees the array (not the tasks). Returns CCB_ERR_CONFIG
// when the graph has a cycle.
ccb_status ccb_graph_plan(ccb_graph *g, ccb_task ***order_out,
  size_t *count_out);

// Move every WAITING task whose dependencies are all DONE to READY.
// Returns the number of tasks newly marked READY.
size_t ccb_graph_mark_ready(ccb_graph *g);

// Execute the graph sequentially in topological order. Tasks already
// DONE (e.g. completed before execution) are skipped. On the first
// failure the failing task is marked FAILED, every task that has not
// run is marked CANCELLED, diagnostics added during the failed task
// gain its task ID, and the failing status is returned. Task start /
// finish events go to the log stream (JSONL task events in --json
// mode; silent in human mode, where phase lines already show
// progress). Per-task timings land on the tasks for the summary.
ccb_status ccb_graph_execute(ccb_graph *g, void *ctx, ccb_log *log,
  ccb_diag_list *diags);

// Affected tasks: every task whose inputs name a changed input, plus
// every task that transitively depends on one. *out_ids is a malloc'd
// array of strdup'd IDs in topological order; free it with
// ccb_free_str_array. Returns CCB_OK.
ccb_status ccb_graph_affected(ccb_graph *g, const char **changed,
  size_t n_changed, char ***out_ids, size_t *out_count);

// True when any task declares an input with this exact name.
// (The CLI uses it to resolve a relative --affected path against
// the project root before matching.)
int ccb_graph_has_input(ccb_graph *g, const char *name);

// Append a machine-readable build summary: a JSON array of one
// object per task ({id, state, duration_ms, skipped, skip_reason}),
// in plan order. skip_reason is the reason string when the task
// skipped, else null. Tasks never run have duration_ms -1.
void ccb_graph_summary_json(ccb_graph *g, ccb_str *out);

// Inspect the plan. Human: one line per task in execution order
// ("  <id> (after: <dep>, <dep>)" or "  <id>" when it has no deps).
// JSON: one {"event":"build_plan","tasks":[...]} line, each task
// with its type, deps, inputs (objects with name, kind, and
// required), and outputs. Both print to the given stream; on a
// cycle they print nothing and return CCB_ERR_CONFIG.
ccb_status ccb_graph_emit_plan_human(ccb_graph *g, FILE *f);
ccb_status ccb_graph_emit_plan_json(ccb_graph *g, FILE *f);

// Inspect affected tasks. Human: one line per affected task in
// execution order, or a "no tasks affected" line. JSON: one
// {"event":"affected","input":...,"tasks":[...]} line. The changed
// input is matched by declared input name (a file path, a logical
// name such as "dex", or an environment variable name). Both print
// to the given stream; on a cycle they print nothing and return
// CCB_ERR_CONFIG.
ccb_status ccb_graph_emit_affected_human(ccb_graph *g,
  const char *changed, FILE *f);
ccb_status ccb_graph_emit_affected_json(ccb_graph *g,
  const char *changed, FILE *f);

// Milliseconds since an arbitrary point (CLOCK_MONOTONIC). Exposed
// for callers that time work done outside the executor (e.g. the
// pipeline's stage 0, which the "config" task stands for).
long ccb_graph_now_ms(void);

#endif
