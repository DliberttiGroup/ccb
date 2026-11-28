// ccb_pipeline_graph: build the pipeline task graph (V2 §4) from a
// loaded config. One task per pipeline stage (docs/build.md stages
// 0–14), in the same order the old hardcoded driver ran them, with
// declared inputs and outputs and a dependency edge from each task to
// the previous one. Stage bodies are untouched: each task's run
// function is a thin wrapper around the existing ccb_step_* function.
//
// The graph is built from the config only — it never runs stage 0.
// Stage 0 (config load, toolchain resolve, dirs, cache) is the
// bootstrap the graph needs to exist at all, so the caller runs it
// first and marks the "config" task DONE with its duration via
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
// parallelize anything (Phase 2 owns the scheduler).
#ifndef CCB_PIPELINE_GRAPH_H
#define CCB_PIPELINE_GRAPH_H

#include "ccb_common.h"
#include "ccb_config.h"
#include "ccb_diag.h"
#include "ccb_graph.h"

// Build the task graph for this config (aab selects the package-aab
// task instead of package, and drops align). Validates the graph
// before returning; on failure returns NULL with diags filled.
ccb_graph *ccb_pipeline_graph_build(const ccb_config *cfg, int aab,
  ccb_diag_list *diags);

// Mark the "config" task DONE with stage 0's duration (ms). Called
// by the pipeline driver after stage 0 succeeds, before execution.
void ccb_pipeline_graph_config_done(ccb_graph *g, long duration_ms);

#endif
