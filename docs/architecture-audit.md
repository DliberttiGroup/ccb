# Architecture audit and migration plan (Phase 0)

[Docs index](index.md)

*ccb-architecture-audit*

Audit of the CCB implementation at `test` head `f41ef5e` ("Swap preview
render engine onto ccb_tui", CI run #15 green), against the target
architecture in the Architecture Scope Revision V2 (8 layers, §3; DAG
core, §4; roadmap §14). This document is the Phase 0 deliverable: it
maps what exists, names the coupling and correctness risks concretely,
answers the FINAL DIRECTIVE's 7 questions for each later phase, freezes
scope, and records the benchmark baseline the later phases measure
against.

Method: read the code, not memory. Line counts are `wc -l` over
`src/` at this head (47,214 lines total). Stage numbers are the
`docs/build.md` stage table (0–14). Test counts are `ctest -N`
(54 entries: 52 `test_*.c` binaries + 2 `fuzz_preview_*` harnesses;
45 fast + 9 slow).

## 1. Layer map: what exists today

*ccb-architecture-map*

| # | Target layer (§3) | What exists | Files (lines) | Status |
| - | ----------------- | ----------- | ------------- | ------ |
| 1 | CLI / Automation Interface | 13 commands (`init setup check build clean daemon doctor install run logcat preview lsp version`, `ccb_cli.c` dispatch at `ccb_cli.c:556-590`); `--json`, `--explain`, `--dry-run`; human + JSON diag output; JSONL phase events (`ccb_log`: phase started/finished + `duration_ms`, warning, cache-hit with reason) | `ccb_cli*.c` (5,255), `ccb_log.c`, `ccb_json.c` (446) | Exists. Events are per-*phase*, not per-*task* (no tasks yet); no build summary object, no aggregate cache statistics (§8 gap, R5). |
| 2 | Project Model | `ccb_config` (2,378 lines): one struct holds everything — `[project]`, `[paths]`, `[sdk]`, `[signing]`, `[build]`, `[deps]`, `[ndk]`, `[cmake]`, `[preview]`, `[modules]`, `[library]`. Module topo-sort + cycle detection at load (`ccb_config.c:617-710`, `CCB-E1010`/`CCB-E1011`). Load reports every problem at once. | `ccb_config.{h,c}`, `ccb_toml.{h,c}` (858) | Exists, over-wide. No module identity beyond name/dir, no variants, no per-module `[deps]`/`[ndk]` (see §6). |
| 3 | Graph Engine | None. The "graph" is the fixed stage order in `ccb_pipeline_build` (`ccb_pipeline.c:588-660`) plus module topo-sort at config load. No task abstraction, no declared inputs/outputs per task, no build plan object. | — | Missing. Phase 1. |
| 4 | Scheduler | None. Builds run strictly sequentially in one thread; the only concurrency in the tree is the opt-in kotlinc daemon (a warm JVM, not a scheduler). No ready queue, no worker pool, no failure propagation beyond "first error aborts the build", no cancellation. | — | Missing. Phase 2. |
| 5 | Cache Engine | Per-step whole-step fingerprints in `build/ccb-cache.json` (`ccb_cache.{h,c}`, 220 lines) + a separate global stdlib dex cache (`$XDG_CACHE_HOME/ccb/dex-stdlib`). Tool fingerprint invalidates all steps. `--explain` prints the run/skip plan. | `ccb_cache.{h,c}`, `ccb_pipeline.c:498-530` (tool fp), `ccb_pipeline_dex.c` (stdlib cache) | Partial. Fingerprint contents are sound for file inputs (see §4) but the design is step-granular, fingerprint-only, and split across two caches (R1, R2, D2). |
| 6 | Diagnostics / Observability | `ccb_diag` (195 lines): stable `CCB-E`/`CCB-P` codes, phase, message, source position, failing command, suggestion; human + JSON emitters. Exit codes map 1:1 to spec §6.4 (`ccb_common.h`). Phase timing + cache-hit reasons already flow as JSONL via `ccb_log`. | `ccb_diag.{h,c}`, `docs/diagnostics.md` | Partial. Diag has no task ID, no module ID; no build summary object, no aggregate cache statistics (§8 gap, R5). |
| 7 | Task Providers | Not separated. All Android work is pipeline steps in `ccb_pipeline*.c` (5,593 lines, 11 files): `ccb_step_deps`, `ccb_step_libraries`, `ccb_step_modules`, `ccb_step_merge_manifest`, `ccb_step_compile_res`, `ccb_step_link`, `ccb_step_code`, `ccb_step_dex`, `ccb_step_dexcheck`, `ccb_step_native` (+`ccb_step_cmake`), `ccb_step_package[_aab]`, `ccb_step_align`, `ccb_step_sign`, `ccb_step_verify`. | `ccb_pipeline*.{h,c}`, `ccb_manifest.c` (1,194), `ccb_maven*.c` (3,572), `ccb_bundle.c` (163), `ccb_dexcheck.c` (495) | To be extracted (Phase 4). Maven is the closest to a provider already (own internal header, two modes). |
| 8 | Execution Infrastructure | Solid and reusable: process spawn without `system()` (`ccb_proc.{h,c}` — child inherits env + `env_extra` overrides, stdin `/dev/null`); arena allocation (`ccb_alloc.{h,c}`); SHA-256 (`ccb_hash.c`); own ZIP reader/writer (`ccb_zip.c`, 843 lines, fixed 1980 timestamps, `..`/absolute-name rejection in `ccb_zip_name_ok`); toolchain discovery (`ccb_sdk.{h,c}`, 952 lines); kotlinc daemon (`ccb_daemon.{h,c}`, 312 lines). | `ccb_proc`, `ccb_alloc`, `ccb_hash`, `ccb_zip`, `ccb_sdk`, `ccb_daemon`, `ccb_fs`, `ccb_str`, `ccb_slice` | Exists, reusable as-is. Two load-bearing quirks to preserve: daemon RMI needs `-Djava.net.preferIPv4Stack=true`; the daemon client exits 0 on compile failure, so the caller must parse `result code: N` (`ccb_daemon.h` header comment). |

Cross-cutting subsystems that do not fit one layer:

- **Preview / UI analysis — 20,043 lines, 42% of `src/`.** `ccb_preview*.c`
  (parsers for CSS and Kotlin structure, flow files, handlers, scenarios,
  traces, styles, diff/walk/watch modes, LSP server, tree-live) plus the
  `ccb_tui` library (2,811 lines, 6 modules: cell/screen/diff/term/input/
  style) that now renders preview output. Its own CLI surface (`preview`,
  `lsp` commands; `ccb_cli_preview*.c`). It is already static analysis,
  not an emulator — the V2 §12 repositioning is mostly a positioning and
  scope statement, not a rewrite. It shares the Project Model (`[preview]`
  tables in `ccb_config.h`) and the CLI, and the optional preview gate
  hooks into `check`/`build`.
- **AAB — `ccb_bundle.c` (163) + `ccb_step_package_aab`** (hand-encoded
  `BundleConfig.pb`, `bundletool validate` passes). Works, but is a second
  packaging path inside the pipeline (D4).
- **Setup / device — `ccb_cli_setup*.c` (1,833), `ccb_cli_device.c` (433),
  `ccb_net.c`.** Toolchain install + `adb install/run/logcat`. Belongs to
  layers 1 and 8; no change needed for Phases 1–3.

## 2. Coupling findings

*ccb-architecture-coupling*

- **C1 — `ccb_pipe` is a god-struct.** `ccb_pipeline_internal.h` defines
  one struct (~120 lines, 20+ fields) carrying options, log, diags,
  config, SDK, cache, 8 directory paths, 6 parallel AAR arrays
  (`aar_res_dirs`/`aar_res_zips`/`aar_class_jars`/`aar_packages`/
  `aar_assets_dirs`/`aar_jni_dirs`), merge state (`merge_aar_paths`/
  `merge_aar_srcs`/`merge_aar_names`, `merged_manifest`, `merge_fp`),
  module/library work dirs, 8 `*_will_run` flags, and step counters.
  Every step function takes `ccb_pipe *` and mutates shared state; the
  11 split files include the same internal header. Nothing about a step's
  inputs or outputs is declared anywhere except inside its own function
  body. Phase 1 replaces this with a task struct (declared inputs,
  outputs, fingerprint keys); the per-step fingerprint builders
  (`ccb_fp_add`, `ccb_fp_add_file`, `ccb_fp_addl`, `ccb_pipeline.c:100-160`)
  are the reusable core of the new task fingerprint.
- **C2 — Order is a hardcoded `if`-chain, not a graph.**
  `ccb_pipeline_build` (`ccb_pipeline.c:588-660`) runs 14 steps as
  `if (rc == CCB_OK) rc = ccb_step_X(&p);` in fixed order, with a special
  early branch for library projects (stages 0–2, then stop). There is no
  way to run a subset, reorder, skip a failed-but-optional step, or run
  two independent steps concurrently. A module change rebuilds the app
  end-to-end (module AARs are steps 2–3 of a single linear run). Any
  Phase 1 graph must reproduce exactly this order for single-module
  projects — the order is correct, only its encoding is wrong.
- **C3 — The pipeline mutates the project model.** Stage 0 fills
  `cfg->kotlin_plugin_jars` by resolving Maven plugins
  (`ccb_pipeline.c:447-456`) and `cfg->modules` is topo-sorted at load.
  A task graph built from the config must treat the config as read-only
  input; plugin resolution belongs in a resolve step (or a provider
  setup), not in stage 0's side effects. Related: `[preview]` scenario
  tables live in the same config struct as build settings, so every
  build parses preview config it never uses.
- **C4 — Toolchain identity is split in two places.** The global tool
  fingerprint (`ccb_pipeline.c:498-513`: build-tools version, aapt2/
  zipalign paths, kotlinc version, java major, compile_sdk) invalidates
  all steps on change — but NDK/clang identity is not in it; the native
  step adds its own `ndk` version key (`ccb_pipeline_native.c:173`) and
  the dex step keys on stdlib jar hash + min-api + build-tools version.
  The split is currently sound (each step keys what it needs) but there
  is no single "toolchain identity" a task fingerprint can reference, so
  Phase 3 must define one place where toolchain identity lives and make
  steps reference it instead of re-deriving it.
- **C5 — Failure handling is "first error wins, everything after is
  skipped".** A step failure aborts the build; the cache is still saved
  (`ccb_pipeline.c:655-657`) so a retry resumes — that part is good.
  But there is no failure context beyond one diag, no record of which
  steps completed in the output, and no cancellation path. The `verify`
  step is the only post-hoc correctness check (dexcheck, stage 9, runs
  even when dex was cache-skipped — good, keep that invariant).
- **C6 — Preview gate couples preview to the build core.**
  `check`/`build` accept `--preview-gate` and the gate runs preview
  analysis as part of validation. The gate must stay optional and must
  not become a build step: preview stays a separate command whose
  diagnostics the gate surfaces, never a stage in the task graph.
- **C7 — The kotlinc daemon is a second execution path, not a provider
  option.** `ccb_step_code` branches on daemon on/off
  (`ccb_pipeline_code.c`) with client-output parsing and fallback in the
  step. Phase 4 must fold the daemon into the kotlinc provider (one
  provider, two execution modes) so the graph sees one compile task.
- **C8 — Config load is the only graph validation.** Module cycles are
  caught at load (`CCB-E1010`, names the cycle path — this is good and
  reusable as the pattern for task-graph cycle diagnostics). But there
  is no validation of the build plan itself: nothing checks that a
  step's declared inputs exist before it runs, or that two steps do not
  claim the same output path.
- **C9 — Almost no hidden global state (good).** A grep over `src/*.c`
  finds only two file-scope mutables: `pv_watch_stop`
  (`ccb_preview_watch.c:54`, a `sig_atomic_t` stop flag) and
  `tui_active_term` (`ccb_tui_term.c:19`). The no-hidden-global-state
  rule (§15) is already nearly satisfied; Phase 2's scheduler must keep
  it that way (ready queue and counters live in a scheduler struct, not
  in globals).

## 3. Duplicate responsibilities

*ccb-architecture-duplicates*

- **D1 — The module build is a second, smaller pipeline.**
  `ccb_pipeline_module.c` re-implements resource compile, kotlinc,
  classes-jar write, and AAR assembly (`ccb_mod_compile_resources`,
  `ccb_mod_run_kotlinc`, `ccb_mod_compile_sources`,
  `ccb_mod_write_classes_jar`, `ccb_mod_assemble_aar`) with its own
  fingerprint keys, duplicating stage 5/7/11 logic for modules. The
  library-project path (`ccb_step_library`) reuses the same module
  builder. Phase 4 must make modules run the same providers as the app
  (same compile task, different inputs), not a parallel implementation.
  Until then, any fix to compile behavior has to be applied twice —
  this is the single most likely source of module/app divergence bugs.
- **D2 — Two caches with different rules.** `build/ccb-cache.json`
  (per-step fingerprints, whole-step granularity, corrupt = "rebuild
  everything") and the global stdlib dex cache (single `<key>.dex` file,
  atomic rename on publish, validated on read by dex magic + header
  size, corrupt = fall back to the jar). The dex cache is the better
  design; Phase 3 should converge on one cache engine with the dex
  cache's validation/atomicity properties, not two ad-hoc mechanisms.
  A third cache-like file, `build/intermediates/tool-probes.json`
  (tool version probes, keyed by path + mtime + size), feeds only the
  tool fingerprint.
- **D3 — Fingerprint construction is copy-pasted per step.** Each step
  builds its own key list inline (`ccb_fp_add*` calls scattered across
  `ccb_pipeline_res.c`, `ccb_pipeline_code.c`, `ccb_pipeline_dex.c`,
  `ccb_pipeline_native.c`, `ccb_pipeline_sign.c`, `ccb_pipeline_package.c`,
  `ccb_pipeline_module.c`). The helpers are shared; the key *selection*
  is not reviewed anywhere as a whole. Phase 3 needs one audited table
  of "what each task's fingerprint covers" — this document's §4 is the
  starting list, not the table.
- **D4 — Two packaging paths.** APK (`ccb_step_package`) and AAB
  (`ccb_step_package_aab` + `ccb_bundle.c`) share the zip writer but
  lay out different trees with different verify steps (apksigner verify
  vs `jarsigner -verify` + structural check). AAB is a provider in the
  target architecture (§9); until Phase 8 it stays as-is, but no new
  packaging behavior may be added to either path.
- **D5 — R-class handling in three places.** `R.java` is generated by
  aapt2 link (`ccb_pipeline_res.c:226`), compiled by javac in the code
  step (kotlinc emits no R.class files, so javac must compile them —
  `ccb_pipeline_code.c`), and "definalized" for modules
  (`ccb_mod_definalize_r`, `ccb_pipeline_modules.c:72`). The semantics
  (which R symbols exist, who owns them) are implicit in three
  implementations. Phase 4's resource provider must own R semantics in
  one place.

## 4. Correctness risks

*ccb-architecture-risks*

- **R1 — Skips are fingerprint-only; outputs are never re-validated.**
  Every step's skip path is `old != NULL && strcmp(old, fp) == 0`
  (e.g. `ccb_pipeline_code.c:226-233` for compile-code), with no check
  that the step's outputs still exist on disk. Deleting or corrupting a
  file under `build/intermediates/` while `ccb-cache.json` is intact
  yields a "successful" build from missing inputs; the failure surfaces
  later (or never, if the corrupted file is packaged). Phase 3 must add
  output-existence (and, for cheap outputs, output-hash) validation to
  every skip decision. This is the highest-value cache-correctness fix
  in the roadmap and it does not need the DAG first.
- **R2 — Whole-step granularity hides partial staleness.** A one-line
  Kotlin change re-runs all of kotlinc; a one-string change re-runs
  link + package + align + sign + verify. Documented and intended
  today, but it means the fingerprint table (D3) is the only thing
  standing between "incremental" and "wrong": a missing key silently
  reuses a stale step. The known gap to audit in Phase 3 is whether
  every config value that affects a step is in that step's fingerprint
  (spot-check so far: plugin jars/options are in the compile
  fingerprint, `merge_fp` covers manifests, R8 keep-rule text is in the
  dex fingerprint — the pattern is followed, but there is no test that
  enumerates config keys × steps).
- **R3 — `--explain` can over-predict runs.** Documented in
  `docs/json-protocol.md`: explain is conservative downstream of `link`
  ("will change" even when inputs turn out identical). An agent that
  plans from `--explain` will see runs that never happen. Phase 1's
  build-plan output must be exact (computed from real fingerprints, not
  from will-run flags), or it must be clearly labeled as an upper bound.
  The `*_will_run` flags on `ccb_pipe` (C1) are the mechanism behind
  the imprecision — they are predictions, not facts.
- **R4 — Silent ignores.** Three documented-or-undocumented cases: (a)
  manifest merge ignores every library-manifest entry outside the
  merged set (components, permissions, features — `docs/build.md`);
  (b) Maven parent-declared `<exclusions>` are ignored and per-dependency
  POM `<exclusions>` are unsupported (`docs/maven.md` Limitations);
  (c) profile activation is by id only (`<jdk>`/`<os>`/`<property>`/
  `<file>` activation not evaluated). (a) and (b) can produce a legal
  APK that behaves differently from what the dependency author declared.
  V2 §10/§8 require explicit diagnostics for unsupported semantics —
  today these are silent. Phase 6 must turn each into a diag or a
  supported feature, starting with the manifest-merge case in Phase 4.
- **R5 — Diagnostics are not yet machine-observable per task.**
  `ccb_diag` has code/phase/message/file/line/col/command/workdir/
  suggestion but no task ID, no module ID. The JSONL event layer
  itself exists and is good (`ccb_log.{h,c}`: phase started/finished
  with `duration_ms`, warnings, cache-hit events with a reason string,
  all as JSONL on stdout in `--json` mode) — but it reports *phases*,
  and there is no build summary object (no task list, no aggregate
  cache statistics). An agent cannot currently tell *which* task
  failed without parsing human text, because there are no tasks yet.
  Phase 1 must add task/module IDs to diags; Phase 2 extends the
  existing JSONL stream from phases to task lifecycle.
- **R6 — No resource limits of any kind.** No memory cap, no
  concurrency cap (nothing is concurrent yet), no d8 argfile fallback
  for very large projects (`docs/build.md`: `.class` files pass as
  individual arguments; the OS argument limit is the known cliff).
  Phase 2 must add worker limits before any parallelism lands; Phase 7
  owns the argfile fix.
- **R7 — Preview is the largest subsystem and the least load-bearing
  for builds.** 20,043 lines (42% of `src/`) for static UI analysis,
  vs 5,593 for the whole build pipeline. Its tests are 29 of 45 fast
  tests. None of that is wrong — but it means the majority of future
  maintenance cost sits outside the build core, and any Phase 1–4
  refactor that touches shared code (`ccb_config`, CLI dispatch, `ccb_tui`)
  must keep preview green. The preview gate in `check`/`build` is the
  one place where preview can break a build; keep it opt-in.
- **R8 — Environment inputs are not fingerprinted.** Toolchain
  resolution reads `PATH`, `JAVA_HOME`, `KOTLIN_HOME`, `ANDROID_HOME`,
  `ANDROID_SDK_ROOT` (`ccb_sdk.c`) and release signing reads
  `env:VAR` passwords (`ccb_pipeline.c:374-383`), but the tool
  fingerprint records only versions/paths, not the environment that
  selected them. Changing `JAVA_HOME` between two identical-version
  JDKs does not invalidate anything (same major version → same
  fingerprint). Low risk today (same-version JDKs produce the same
  output), but Phase 3's fingerprint spec must decide explicitly which
  environment inputs are declared inputs and which are ignored.

## 5. FINAL DIRECTIVE answers, per phase

*ccb-architecture-phases*

Each phase lists: (1) reuse, (2) refactor, (3) redesign, (4) remove /
postpone, (5) missing, (6) migration sequence, (7) risks + tests.
"Do not rewrite blindly" is applied as: the stage functions, their
fingerprint helpers, and the execution infrastructure survive; only the
driver (`ccb_pipeline_build` + `ccb_pipe`) and the step interfaces change.

### Phase 1 — Core graph engine

1. **Reuse:** all 14 stage functions and their bodies; `ccb_fp_add*`
   fingerprint helpers; module topo-sort + `CCB-E1010` cycle diag as the
   cycle-error pattern; `ccb_diag` struct and emitters; `--explain`
   output shape (but see 3).
2. **Refactor:** `ccb_pipeline_build` into graph construction (one task
   node per stage, edges = the current fixed order) + validation +
   execution of the graph in topological order; `ccb_pipe` into a
   per-task context (inputs/outputs/fingerprint owned by the task, not
   by a shared struct); `ccb_config` split so the graph builder takes a
   read-only project model (plugin resolution moves out of stage 0).
3. **Redesign:** the task abstraction itself (ID, type, inputs, outputs,
   deps, cache policy, diagnostics — V2 §4); `--explain` into an exact
   build plan (fix R3: plan from real fingerprints, drop `*_will_run`
   prediction flags); diag gains task ID + module ID (R5).
4. **Remove/postpone:** nothing in Phase 1. Do not add parallelism,
   custom tasks, or a new cache design here.
5. **Missing:** task struct, graph builder/validator, cycle detection
   for tasks (module cycles exist; task cycles do not), build-plan
   output (human + JSON).
6. **Sequence:** (a) define `ccb_task` + graph types alongside the
   pipeline, no behavior change; (b) build the graph from the current
   stage list and execute it in the same order, behind the existing
   `ccb_pipeline_build` entry point; (c) move fingerprint-key selection
   into task declarations one step at a time (compile-res first, it has
   the fewest dependents); (d) switch `--explain` to the plan; (e)
   delete `ccb_pipe` only when no step references it. Each step keeps
   the full suite green.
7. **Risks/tests:** the main risk is behavioral drift in step order or
   skip decisions — guard with a test that runs `--explain` + a full
   build on hello + Showcase before and after and diffs the run/skip
   plan and the output APK hashes (reproducible builds make this a
   byte comparison). New tests: graph validation (cycle, missing input,
   duplicate output), plan exactness (explain == actual run/skip for a
   no-op and a one-file change). Slow tests (`test_dex_contents`,
   `test_modules`, `test_aab`) are the regression net; run them in CI,
   not on the VM.

### Phase 2 — Scheduler

1. **Reuse:** task graph + task states from Phase 1; `ccb_proc`
   (already spawns without `system()`); `ccb_log`/`ccb_diag`.
2. **Refactor:** the graph executor from "topological loop" to "ready
   queue + dependency counters + worker pool". Step functions do not
   change — only how they are invoked.
3. **Redesign:** the scheduler itself (V2 §5: central ready queue,
   pthread worker pool, failure propagation, cancellation, basic CPU/
   worker limits). This is greenfield, not a refactor: nothing
   concurrent exists today.
4. **Remove/postpone:** work-stealing, priorities, distributed
   execution, remote execution (V2 §5 explicitly defers these);
   parallel module builds stay sequential until the scheduler is
   proven on single-module builds.
5. **Missing:** worker pool, task state machine (WAITING→READY→RUNNING→
   DONE/FAILED/CANCELLED), failure propagation (fail-fast vs
   continue-on-error policy), cancellation, extension of the existing
   JSONL phase events to task lifecycle (R5), per-task timing.
6. **Sequence:** (a) scheduler with worker count 1 — same behavior as
   Phase 1, proves the state machine; (b) worker count = CPU count
   with only provably independent tasks parallel (dexcheck vs package
   is the wrong example — they are dependent; the first real parallel
   pair is module builds, Phase 5, so Phase 2 proves parallelism with
   a synthetic two-task fixture + the existing test suite); (c) failure
   propagation + cancellation; (d) resource limits.
7. **Risks/tests:** races in cache writes (two tasks writing
   `ccb-cache.json` — the cache save must become atomic + single-writer
   before any parallelism); partial outputs mistaken for valid outputs
   (atomic publish per task, V2 §5); flaky tests from timing. New
   tests: scheduler unit tests (dependency counters, failure
   propagation, cancellation), a stress test with a synthetic wide
   graph, and the full slow suite under worker counts 1 and N with
   byte-identical APKs. Do not benchmark until correctness is proven.

### Phase 3 — Correct incremental cache

1. **Reuse:** `ccb_cache` load/save + `ccb_cache_fingerprint` (SHA-256
   over `key=value` lines); the stdlib dex cache's validation (magic +
   size) and atomic-rename publish as the pattern for all cache writes;
   per-step fingerprint key *selection* (audited, see 3).
2. **Refactor:** the two caches (D2) into one cache engine with one
   backend interface (local first; remote is Phase 8); `ccb-cache.json`
   schema to carry per-task fingerprints + output hashes + toolchain
   identity, with a version field and a corrupt-cache fallback that
   stays "rebuild everything, never error".
3. **Redesign:** the fingerprint spec itself — one audited table of
   task → fingerprint inputs, covering dependency outputs (not just
   source files), toolchain identity (single definition, fixing C4),
   declared environment inputs (decide R8 explicitly), and provider
   versions; skip decisions gain output-existence validation (R1) and
   cache-corruption recovery; `--explain` gains cache hit/miss reasons.
4. **Remove/postpone:** remote cache (Phase 8); content-addressed
   artifact cache as a separate store (design the interface now, build
   it only if the local cache proves insufficient — the dex cache
   already is a content-addressed store for one artifact).
5. **Missing:** output hashes in the cache, cache inspection/clear
   commands (`ccb cache` surface does not exist), cache diagnostics
   (hit/miss + reason per task, machine-readable).
6. **Sequence:** (a) write the fingerprint table as a doc + a test
   that enumerates config keys × tasks and fails on unlisted keys;
   (b) add output-existence checks to skip decisions (R1 — safe,
   behavior-preserving except for the corrupt-intermediates case);
   (c) unify the dex cache into the cache engine; (d) add output
   hashes; (e) add cache commands + diagnostics. (b) can land before
   Phase 1 finishes — it touches only step skip paths.
7. **Risks/tests:** the dominant risk is a false cache hit shipping a
   stale APK — guard with a "fingerprint mutation" test per task
   (change exactly one input, assert exactly the expected tasks rerun,
   via `--explain` + a real build), a corrupt-intermediates test
   (delete an intermediate, assert rebuild), and a corrupt-cache test.
   The existing `test_cache` (fast) + `test_dex_contents` (slow) are
   the base; both need the new cases. Benchmark after, not during:
   no-op and incremental times before/after (§7 baseline).

### Phase 4 — Refactor Android tasks into providers

1. **Reuse:** every stage body, unchanged in behavior; `ccb_manifest`
   merge rules (documented subset); `ccb_maven` resolver as-is;
   `ccb_bundle` AAB layout; `ccb_dexcheck` invariant (runs even on
   cache skip); daemon quirks (C7, §1 layer 8).
2. **Refactor:** each `ccb_step_*` into a provider with declared
   inputs/outputs/toolchain/deps/cache behavior/diagnostics/limitations
   (V2 §9); the module mini-pipeline (D1) into the same providers the
   app uses; R semantics (D5) into the resource provider; the daemon
   into the kotlinc provider's execution modes.
3. **Redesign:** nothing in the providers themselves. The provider
   *interface* is new, the implementations are moved.
4. **Remove/postpone:** AAB stays as-is (provider-shaped, but no new
   AAB work — Phase 8); manifest-merge expansion stays frozen (R4a
   becomes a diag, not a feature); no new resource processing.
5. **Missing:** the provider interface + registry; per-provider
   limitation declarations (each provider states what it does not do,
   machine-readably); manifest-merge diagnostics for ignored entries
   (R4a); fixture-based merge tests (V2 §10).
6. **Sequence:** providers one at a time, in dependency order:
   resource (link/compile-res + R) → code (kotlinc/javac + daemon) →
   dex (+ dexcheck, r8) → manifest merge → package/sign/verify →
   native/cmake → deps/AAR extraction → modules (delete the
   mini-pipeline last, only when the app providers cover modules).
   Each provider lands with its stage's existing tests still passing
   and the APK byte-identical.
7. **Risks/tests:** module/app divergence during the D1 deletion
   (guard: build the same code as an app and as a module, compare dex
   contents — `test_dex_contents` pattern); manifest-merge behavior
   change from new diagnostics (guard: golden merged manifests in
   `tests/golden/`); signing correctness (never change sign/verify
   behavior without `test_aab` + a real-device install check).

### Phase 5 — Multi-module

1. **Reuse:** `[modules.<name>]` config + topo-sort + cycle diag
   (`CCB-E1010`); module AAR build (now via providers, Phase 4);
   `docs/modules.md` graph rules.
2. **Refactor:** modules from "steps 2–3 of the app pipeline" into
   first-class graph nodes: each module owns a task subgraph, module
   deps map to task-graph edges, module outputs are explicit artifacts.
3. **Redesign:** the module model (V2 §7): module identity, source
   sets, declared deps, per-module diagnostics. The current model has
   no per-module `[deps]`/`[ndk]`/signing and builds every module on
   every build (§6) — those are design decisions to revisit here, not
   bugs to fix earlier.
4. **Remove/postpone:** dynamic feature modules (already excluded in
   `docs/modules.md`); publishing modules to Maven.
5. **Missing:** per-module incremental builds (a changed module must
   not invalidate unrelated modules — today any module change reruns
   the linear pipeline); parallel module builds (needs Phase 2);
   module-output caching across builds.
6. **Sequence:** (a) module subgraphs in the graph (still executed in
   topo order, sequentially — behavior identical); (b) per-module
   fingerprints + skip (a module whose inputs are unchanged is skipped
   as a unit); (c) parallel module execution on the Phase 2 scheduler;
   (d) per-module `[deps]`/`[ndk]` if a real project needs them.
7. **Risks/tests:** a module change silently not rebuilding a dependent
   (guard: two-module fixture, change module A, assert module B's
   tasks rerun and the app's dex changes; change module B only, assert
   A's tasks skip); cycle detection across module + task edges.
   `test_modules` (slow, ~450 s) is the regression net — extend it,
   do not replace it.

### Phase 6 — Dependency resolver hardening

1. **Reuse:** `ccb_maven*.c` (3,572 lines) nearly whole: coordinates,
   transitive BFS, nearest-wins, parent POMs, profiles by id,
   exclusions (project-level), checksums (.sha256 else .sha1, sidecar
   fallback), lockfile (`ccb.lock`, enforced with `CCB-E3023`),
   hermetic builds (cache-only in builds, prefetch in `setup`),
   version ranges / LATEST / RELEASE / snapshots via metadata.
   This is the most complete subsystem in the tree; treat it as proven
   and do not rewrite it.
2. **Refactor:** the resolver's boundary with the graph (it becomes a
   resolve task/provider input, not a stage-0 side effect); error paths
   into the unified diag format with task/module IDs.
3. **Redesign:** the resolver *specification* (V2 §8): a written spec
   with a test per rule, stating exactly what is supported. The code
   mostly implements the spec already; the spec document is the missing
   piece.
4. **Remove/postpone:** PGP verification (documented as out of scope in
   `docs/maven.md`; revisit only with a real demand); per-dependency
   POM `<exclusions>` and parent-declared `<exclusions>` (R4b — either
   support or diagnose, but do not silently ignore; the decision belongs
   here, with a test either way); `<jdk>`/`<os>`/`<property>`/`<file>`
   profile activation (same treatment).
5. **Missing:** resolver spec doc; real-repository fixtures; offline
   tests; conflict-policy tests; checksum-failure tests (some exist in
   `test_maven` — audit coverage against the spec and fill gaps).
6. **Sequence:** (a) write the spec from the code (describe what it
   does today, honestly); (b) add a test per spec rule; (c) decide the
   R4b/R4c items one by one with a diag-or-support verdict each;
   (d) locking/offline hardening only where a test shows a gap.
7. **Risks/tests:** a resolver change that silently resolves a different
   artifact version is a correctness failure, not a performance issue —
   guard with lockfile golden tests (resolved tree byte-compared) and a
   network-failure/offline test matrix. `test_maven` (slow, ~82 s) is
   the base.

### Phase 7 — Large-project reliability

1. **Reuse:** everything; the benchmark fixtures from §7 (extended,
   not replaced).
2. **Refactor:** d8 invocation to argfiles (R6) when a project hits the
   argument limit; `ccb_log` into structured task tracing.
3. **Redesign:** build profiling / task tracing output; performance
   regression tests (fail CI when a benchmark regresses beyond a
   threshold — thresholds defined here, not earlier).
4. **Remove/postpone:** nothing — but this phase starts only when
   Phases 1–6 are done; do not pull its work forward.
5. **Missing:** medium (multi-module, hundreds of files) and large
   (dozens of modules, thousands of files) synthetic fixtures (V2 §13);
   memory-limit enforcement; cache stress tests; concurrency tests.
6. **Sequence:** (a) build the medium + large fixtures; (b) measure
   (throughput, memory, cache hit rate, tasks executed/skipped);
   (c) fix what the numbers show, one bottleneck at a time, benchmark
   before and after each fix (§15).
7. **Risks/tests:** optimizing the wrong thing (guard: no performance
   work without a benchmark showing the bottleneck); flaky
   performance tests (guard: thresholds with margin, medians of 3+,
   same-machine comparisons only). Real-application tests: the
   Showcase app + an install-and-run check on a real device.

### Phase 8 — Advanced features

Only after Phases 1–7. Each item is separately gated on core stability
+ real demand: AAB as an independent provider (it already is nearly
one — D4); remote cache (interface designed in Phase 3, backend built
here); declarative custom command tasks (Stage 2 of §11, explicit
inputs/outputs); external providers / plugin isolation (Stages 3–4 of
§11 — never load arbitrary native C plugins into the main process by
default, V2 §12); advanced variants; additional UI analysis. No
Phase 8 work may start while any earlier phase is red, and each item
needs its own scope note + tests before implementation.

## 6. Scope freeze list

*ccb-architecture-scope*

Frozen from this commit until the roadmap completes (V2 §14 + §15).
"Frozen" means: no new non-essential feature, no new subsystem, no
expansion of an existing subsystem's supported semantics. Bug fixes,
test additions, documentation, and the roadmap phases themselves are
not frozen. Any exception needs an explicit user decision recorded in
this section.

**Frozen (do not start):**

- AAB feature work beyond bug fixes (no new bundle features, no Play
  Asset Delivery, no bundle signing changes).
- Remote cache, custom task API, external providers, plugin system
  (V2 §11 Stages 2–4 — all Phase 8).
- New preview capabilities (no new parsers, modes, or handlers; no
  runtime simulation — preview stays static analysis per §10/V2 §12).
- New Maven semantics (no PGP verification, no per-dependency POM
  exclusions, no new profile activation kinds, no new repository
  types) until Phase 6 decides each one.
- New manifest-merge rules (the documented subset in `docs/build.md`
  is the supported set).
- Multi-module expansion (no per-module `[deps]`/`[ndk]`, no dynamic
  feature modules, no Maven publishing of modules) until Phase 5.
- New CLI commands, new output formats beyond what Phases 1–2 specify
  (task IDs in diags, build plan, JSONL events).
- New dependencies: libc only stays (V2 §13 — no new libraries
  without a written justification; the in-tree TOML/ZIP/JSON/SHA-256
  stay in-tree).

**Supported behavior right now (honest snapshot, 2026-11-27):**

- Single-module and library-module Android builds (Kotlin and/or Java)
  via a fixed 15-stage pipeline (stages 0–14, `docs/build.md`);
  library projects build an `.aar` and stop after deps.
- Maven dependencies: exact versions, ranges, LATEST/RELEASE,
  snapshots; transitive (compile + runtime scopes); nearest-wins;
  parent POMs (properties, dependencyManagement, dependencies,
  profiles); profiles by id; project-level exclusions; checksums;
  lockfile; hermetic builds (prefetch in `setup`, cache-only in
  builds). Compiler plugins via `[build] kotlin_plugins`.
- Manifest merging for the documented subset (components, permissions,
  features; app wins conflicts with a warning).
- DEX via d8, R8 minify for release, stdlib dex cache, dexcheck
  (referenced-but-undefined class fails the build, `CCB-E3025`).
- Native: per-source NDK clang per ABI (depfile-based header
  fingerprints, 16 KB page size on arm64) or a `[cmake]` table driving
  the project's own `CMakeLists.txt`; `java_extra` source roots.
- APK packaging with CCB's own ZIP writer (reproducible: fixed
  timestamps, deterministic order, byte-identical clean builds);
  zipalign, apksigner sign + verify; AAB output (`--aab`, bundletool
  validate passes).
- Opt-in kotlinc daemon (warm rebuilds only); tool-probe cache (warm
  no-op ~0.2 s).
- Preview as static UI analysis: flow files, real-code screens, CSS
  styles, handlers, scenarios, traces, diff/walk/watch modes, LSP
  server + tree-live, `ccb_tui` renderer. It is not an emulator and
  does not simulate the Android runtime, device metrics, density,
  Material components, themes, lifecycle, or dynamic layout.
- 54 ctest entries (45 fast + 9 slow), plain + sanitizer CI jobs;
  docs-only pushes skip CI by design (`.github/workflows/ci.yml`
  `paths-ignore`).

**Known limitations (do not claim otherwise):**

- Performance targets in `requirement.md` §8 are not met: cold
  hello-world ~19–23 s vs the <10 s target; one-file Kotlin rebuild
  ~14 s (5.2 s with the daemon) vs the <2 s target. JVM startup in
  kotlinc/d8/apksigner dominates. See §7 for the measured baseline.
- Walk mode is unverified on Termux (real terminal behavior unproven;
  needs a user device check, not a VM check).
- Maven Limitations (`docs/maven.md`): no PGP verification; no
  per-dependency POM `<exclusions>`; parent-declared `<exclusions>`
  ignored; profile activation by id only. See R4.
- Modules Limitations (`docs/modules.md`): no per-module `[deps]` /
  `[ndk]` / signing; no dynamic feature modules; no Maven publishing;
  all declared modules are built on every build.
- Preview is static analysis, not an emulator (see above); layout
  bounds are estimates; navigation analysis covers only recognized
  handler positions.
- d8 argument limit (R6); no device/emulator verification in CI
  (install/launch verified only up to the device boundary).
- Cold-build wall time is JVM-dominated, so scheduler parallelism
  (Phase 2) will not fix cold builds — it helps multi-module and
  large-project throughput, not hello-world.

## 7. Benchmark baseline

*ccb-architecture-benchmarks*

Measured 2026-11-27 with `benchmarks/bench-builds.sh` (3 runs per
mode, median reported; script + raw results committed alongside this
document). Machine: shared VM, 1.5 CPUs (cgroup limit; `nproc`
reports 4 — do not compare against `nproc`-based numbers), 7935 MiB
RAM. Toolchain: JDK 17.0.20.1 (`~/jdk17`), kotlinc 2.4.21, Android SDK
`~/android-sdk` (platform android-35, build-tools 35.0.0) for hello;
Showcase uses its own `[sdk]` home (`~/Android/Sdk`, platform
android-36, build-tools 36.0.0). CCB: dev build (`cmake --preset dev`,
dynamic, unstripped) at head `f41ef5e`, version 0.1.0.

Definitions: **cold** = `ccb clean` + global stdlib dex cache emptied,
then build. **warm** = `ccb clean` only (global caches intact), then
build. **no-op** = immediate rebuild with no changes. **incremental**
= one comment line appended to the main `.kt` file, then build (file
restored afterwards; the comment change re-runs kotlinc but skips dex
and downstream, per `docs/build.md`). `--explain` counts are recorded
after each mode's last run (note: `dexcheck` always reports `run` and
does not count toward `steps_run`).

<!-- BENCHMARK RESULTS — filled in by the Phase 0 run below. -->

| Project | Mode | Run 1 | Run 2 | Run 3 | Median | `--explain` after mode* |
| ------- | ---- | ----- | ----- | ----- | ------ | ----------------------- |
| hello | cold | 98.6 s | 109.9 s | 177.2 s | 109.9 s | run=1 skip=13 |
| hello | warm | 135.7 s | 113.0 s | 103.0 s | 113.0 s | run=1 skip=13 |
| hello | no-op | 5.0 s | 5.5 s | 7.3 s | 5.5 s | run=1 skip=13 |
| hello | incremental | 78.2 s | 81.5 s | 79.3 s | 79.3 s | run=7 skip=7 |
| Showcase | cold | 242.2 s | 268.3 s | 150.2 s | 242.2 s | run=1 skip=13 |
| Showcase | warm | 69.4 s | 58.7 s | 56.5 s | 58.7 s | run=1 skip=13 |
| Showcase | no-op | 2.9 s | 2.9 s | 2.8 s | 2.9 s | run=1 skip=13 |
| Showcase | incremental | 34.4 s | 36.7 s | 44.9 s | 36.7 s | run=7 skip=7 |

\* `--explain` is recorded once per mode, after that mode's last run —
it is the plan for a rebuild *in the resulting state*, not the plan the
mode itself executed. For cold/warm/no-op the tree is up to date at that
point, so it shows the no-op plan (only `dexcheck` reports `run`; it
always runs and does not count toward `steps_run`). For incremental the
touched file has been restored, so the plan shows the 7 steps a
one-file change reruns (compile-code, dex, dexcheck, package, align,
sign, verify).

**Reading these numbers.** Every figure is far above the prior numbers
in `docs/build.md` (hello cold ~19–23 s, one-file rebuild ~14 s, warm
no-op ~0.2 s; Showcase clean 57 s, warm 33–34 s). The spread inside a
single mode is wide (hello cold 98.6–177.2 s, Showcase cold 150.2–
268.3 s), which says the shared VM was heavily contended during this
run — these medians are a baseline for *this* machine on *this* day,
not a claim about CCB on a quiet machine. Do not compare later
measurements against them unless the machine class and load are
comparable; re-run the script on a quiet VM to get a cleaner baseline
before Phase 7. Three structural facts survive the noise and are worth
carrying forward: (1) for hello, warm ≈ cold (113.0 s vs 109.9 s) — the
stdlib dex cache saves little; JVM boots dominate both. (2) No-op
rebuilds cost 5.5 s (hello) / 2.9 s (showcase) with everything skipped
and the tool-probe cache hit (`build/intermediates/tool-probes.json`
present) — far above the documented ~0.2 s; the cause was not
established in Phase 0 (a no-op build still burns ~2.7 s of user CPU
with no step running; dexcheck's dex parsing is an unconfirmed
suspect). Phase 3 should explain it before claiming any no-op win.
(3) A one-file comment change reruns 7 of 14 steps and costs 79.3 s
(hello) / 36.7 s (Showcase) — the incremental path is dominated by
kotlinc + d8 JVM boots, which is exactly what the kotlinc daemon
(measured 13.6 s → 5.2 s on hello, 2026-11-25) and the Phase 2
scheduler have to attack. Raw results:
`benchmarks/results-20261127.txt`.

Fixtures: hello = `examples/hello` (1 Kotlin file, 1 activity,
`compile_sdk = 35`); Showcase = `~/workspace/ccb-showcase`
(`com.ccb.showcase`, 7 Kotlin files, 23 XML files, `compile_sdk = 36`).
Prior numbers for comparison (from `docs/build.md`, same VM class):
Showcase clean 57 s before dex cache, 63 s cold cache, 33–34 s warm;
hello cold ~19–23 s, one-file rebuild ~14 s (5.2 s with daemon).

Rules for later phases: re-run the same script on the same machine
class before and after any performance change; compare medians, not
single runs; a regression beyond noise in cold/warm/no-op/incremental
blocks the change (§15). Do not optimize solely for hello-world —
medium/large fixtures arrive in Phase 7.

## 8. What Phase 1 should start with

*ccb-architecture-next*

- The graph must reproduce the current stage order exactly for
  single-module projects; the byte-identical-APK check (§5 Phase 1.7)
  is the acceptance test.
- Start the `ccb_task` type alongside the pipeline (no behavior
  change), then move fingerprint-key selection into task declarations
  one step at a time — compile-res first.
- Fix R1 (output-existence on skip) as a Phase 3 early win; it does
  not need the DAG and is the cheapest correctness improvement in
  this audit.
- Decide R4a (manifest-merge ignored entries → diag) in Phase 4;
  decide R4b/R4c (Maven exclusions, profile activation) in Phase 6 —
  each needs a user-visible verdict (support or diagnose), recorded
  in §6 of this document when decided.
- Chinese docs mirrors (`docs/llm/`) drift with every English doc
  change: this document has no Chinese mirror yet. Add the mirror as
  a follow-up, not as a blocker for Phase 0 — but do not let the
  mirror drift silently; the follow-up should cover this document
  and the `docs/build-tests.md` test-count drift (52 vs 54) found
  during this audit.
- Two doc drifts found during this audit (not fixed here, to keep
  Phase 0 to one commit with a reviewable scope — file as follow-ups):
  `docs/build-tests.md` says "52 tests" and "full suite: 52 tests"
  while `ctest -N` reports 54 (52 test binaries + 2 fuzz harnesses);
  and the fast/slow split (45/9) is correct in `CMakeLists.txt` but
  the "52" figure in the prose is stale.
