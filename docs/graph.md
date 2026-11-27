# Task graph engine (Phase 1)

[Docs index](index.md) · [Build](build.md) · [Diagnostics](diagnostics.md) · [JSON protocol](json-protocol.md)

*ccb-graph*

CCB executes builds as a task graph (DAG), not a hardcoded stage
list. Each pipeline stage is a task with an explicit ID, type,
declared inputs and outputs, dependencies, execution state, and
cache policy. The graph engine (`src/ccb_graph.{h,c}`) validates the
graph, computes the execution order, and runs it; the pipeline
(`src/ccb_pipeline_graph.{h,c}`) builds one task per stage from the
loaded config. Stage bodies are unchanged — the graph only decides
the order tasks run in.

## Task model

*ccb-graph-task*

| Field | Meaning |
| ----- | ------- |
| ID | Stable, unique within the graph; used in diagnostics and the build summary. Stage tasks use the stage name (`deps`, `compile-code`, `dexcheck`, …). |
| Type | Provider/kind (`aapt2`, `kotlin`, `dex`, `manifest`, …). |
| Inputs | Logical names. `SOURCE` inputs are project files (config paths); `GENERATED` inputs are produced by another task in the graph. |
| Outputs | Logical names other tasks consume (`linked-apk`, `r-java`, `classes`, `dex`, …). Two tasks may not claim the same output. |
| Dependencies | Task IDs that must finish first. The pipeline graph chains each stage after the previous one, so the order is exactly the legacy stage order. |
| State | `WAITING → READY → RUNNING → DONE`, plus `FAILED` and `CANCELLED` (see below). |
| Cache policy | `FINGERPRINT` (may skip on unchanged inputs) or `ALWAYS_RUN` (runs every build; `dexcheck`). |
| Run function | The existing stage function behind a thin wrapper. `config` has no run function: stage 0 is the bootstrap that loads the project model, so the driver marks it `DONE` with stage 0's duration before execution. |

Inter-task artifacts use shared logical names, not file paths:
`project-model` (config → every task), `aar-extracted`,
`library-aars`, `module-aars`, `merged-manifest`, `compiled-res`,
`linked-apk`, `r-java`, `classes`, `dex`, `native-libs`,
`unsigned-artifact`, `aligned-artifact`, `signed-artifact`. A task
with no outputs (`dexcheck`, `verify`) is a check, not a producer.

## Validation

*ccb-graph-validation*

`ccb_graph_validate` reports every problem it finds:

| Code | Meaning |
| ---- | ------- |
| CCB-E1012 | Duplicate task ID. |
| CCB-E1013 | Unknown dependency, GENERATED input produced by no task, or two tasks claiming the same output. |
| CCB-E1014 | Task dependency cycle, naming the cycle path (e.g. `a -> c -> b -> a`), following the CCB-E1010 module-cycle pattern. |
| CCB-E1015 | Missing SOURCE input file (reported by `ccb_graph_check_source_inputs`, which the pipeline does not run — its steps diagnose missing files themselves with their own codes). |

The cycle check runs only when every dependency resolves; a graph
with an unknown dependency reports that instead.

## Execution

*ccb-graph-execution*

The executor is sequential: it repeatedly marks `WAITING` tasks
whose dependencies are all `DONE` as `READY`, runs the first ready
task, and repeats. On the first failure the failing task is marked
`FAILED`, every task that has not run is marked `CANCELLED`, and the
failing status is returned. Diagnostics added during the failed task
gain its task ID. Parallel scheduling is Phase 2 and replaces only
the executor, not the graph.

A task that skips on a cache hit sets its `skipped` flag; the
executor records `duration_ms` per task.

## Inspecting the plan

*ccb-graph-plan*

`ccb build --explain` prints the plan first, then the per-step
run/skip lines as before:

```
build plan (15 tasks, in execution order):
  config
  deps (after: config)
  libraries (after: deps)
  ...
  verify (after: sign)
```

`ccb graph [--aab]` prints the same plan without building anything
(human table by default; one `build_plan` JSON object with
`--json`). It loads the config read-only and resolves no toolchain,
so it works without an SDK installed. `--aab` shows the AAB plan
(`package-aab` instead of `package`, no `align`).

Affected tasks — which tasks a changed input invalidates — are
computed by `ccb_graph_affected`: a task is affected when one of its
inputs names a changed input, or when it transitively depends on an
affected task. This is the graph primitive Phase 3 (incremental
cache) and Phase 5 (multi-module) build on; no CLI exposes it yet.

## Build summary and task events

*ccb-graph-summary*

In `--json` mode each task emits lifecycle events:

```json
{"event":"task","task":"compile-code","status":"started"}
{"event":"task","task":"compile-code","status":"finished","duration_ms":5211,"result":"done"}
```

`result` is `done`, `skipped`, or `failed`. Human mode stays
unchanged: phase lines already show progress.

The final `build_done` object gains a `tasks` array, one object per
task in plan order:

```json
{"event":"build_done","apk":"build/outputs/debug/app.apk","steps_run":2,"steps_skipped":8,"up_to_date":false,"tasks":[{"id":"config","state":"done","duration_ms":41,"skipped":false}, …]}
```

Tasks that never run (cancelled after a failure) report
`duration_ms: -1`. On a failed build the same array goes out as a
standalone `{"event":"build_summary","tasks":[...]}` object before
the diagnostics. Diagnostics from a failed task carry its ID as
`"task"` in JSON and `[task <id>]` in human output.

## Equivalence proof

*ccb-graph-equivalence*

The graph-driven build is proven equivalent to the old driver by:

1. `tests/test_graph.c`: the pipeline graph's topological order
   equals the legacy stage order exactly (app, AAB, and library
   graphs).
2. A real `examples/hello` build before and after the change:
   identical `--explain` run/skip lines and byte-identical APKs
   (reproducible builds: fixed timestamps, deterministic order).
   Baseline (pre-change binary): APK
   `9736bbae9c5bfdad383418d96ba8dddf6a10a96a68e7e3ffa809031e42ff5c3f`;
   the graph-driven build reproduces the same hash.

## What Phase 1 deliberately does not do

*ccb-graph-not-done*

- No parallelism: one task runs at a time (Phase 2 scheduler).
- No new cache design: skip decisions are still the steps' own
  fingerprint comparisons (Phase 3).
- No provider extraction: Android stages are still `ccb_step_*`
  functions wrapped as tasks (Phase 4).
- No `ccb_pipe` redesign: the god-struct stays the shared context
  the steps read; splitting it is a Phase 4 concern
  (see [architecture audit](architecture-audit.md)).
- No module ID on diagnostics: tasks carry IDs; module IDs arrive
  with the multi-module model (Phase 5).
