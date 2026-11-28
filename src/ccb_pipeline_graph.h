// ccb_pipeline_graph: build the pipeline task graph (V2 §4) from a
// loaded config. One task per pipeline stage (docs/build.md stages
// 0–14), in the same order the old hardcoded driver ran them, with
// declared inputs and outputs and a dependency edge from each task to
// the previous one. Stage bodies are untouched: each task's run
// function is a thin wrapper around the existing ccb_step_* function.
//
// The graph is built from the config plus the resolved runtime state
// (ccb_resolved: declared + Maven-resolved + path-library AARs/JARs
// and plugin jars). It never runs stage 0: stage 0 (config load,
// toolchain resolve, dirs, cache) is the bootstrap the graph needs
// to exist at all, so the caller runs it first and marks the
// "config" task DONE with its duration via
// ccb_pipeline_graph_config_done. That keeps the plan complete (all
// 15 stages) while staying honest about what ran when.
//
// Inputs are logical names (see ccb_graph.h). Inter-task artifacts
// use shared names ("aar-extracted", "merged-manifest", "linked-apk",
// "r-java", "classes", "dex", "native-libs", "unsigned-artifact",
// "aligned-artifact", "signed-artifact"); project files use their
// config paths as SOURCE inputs. Every task also declares the
// "project-model" and "toolchain" GENERATED inputs produced by
// "config", so a config or toolchain change invalidates the whole
// graph in affected-task terms. File inputs the steps read are
// declared on the task that reads them (classpath jars and plugin
// jars on compile-code, jars and ProGuard rules on dex, the
// keystore on sign); the release signing password variables are
// declared on sign as ENV inputs, by name only. A task declares an
// input only when the step reads it: the native task declares NDK
// sources only with [ndk] and CMakeLists.txt only with [cmake].
// The one deliberate gap is the default debug keystore, which the
// sign step generates when absent, so it is not a declared input.
//
// What this does NOT do: it does not split ccb_pipe (the shared
// context the step functions read — that is Phase 4), it does not
// change any step's fingerprint or skip decision, and it does not
// run anything in parallel: the pipeline driver still executes the
// graph sequentially. The parallel executor (ccb_sched) exists and
// is proven on synthetic graphs (Step 8, stages 1–5), but no build
// path calls it yet — real steps still share one log, one diag
// list, one cache, and one pipe, so parallel execution of real
// build tasks stays off until per-task ownership of those exists.
// Modules already fan out in the graph itself: one task per module
// ("module-<name>", output "module-aar-<name>"), each depending on
// deps + libraries and on its own upstream modules, with
// merge-manifest joining them; with no modules there is no module
// task.
#ifndef CCB_PIPELINE_GRAPH_H
#define CCB_PIPELINE_GRAPH_H

#include "ccb_common.h"
#include "ccb_config.h"
#include "ccb_diag.h"
#include "ccb_graph.h"
#include "ccb_resolved.h"

// Build the task graph from the config + resolved runtime state
// (the pipeline's call: stage 0 already resolved dependencies).
// Validates the graph before returning; on failure returns NULL
// with diags filled.
ccb_graph *ccb_pipeline_graph_build_resolved(const ccb_config *cfg,
  const ccb_resolved *resolved, int aab, ccb_diag_list *diags);

// Build the task graph from the config alone, declaring the
// config's own AAR/JAR lists (no resolution ran: ccb check, tests).
// Same validation as the resolved variant.
ccb_graph *ccb_pipeline_graph_build(const ccb_config *cfg, int aab,
  ccb_diag_list *diags);

// Mark the "config" task DONE with stage 0's duration (ms). Called
// by the pipeline driver after stage 0 succeeds, before execution.
void ccb_pipeline_graph_config_done(ccb_graph *g, long duration_ms);

#endif
