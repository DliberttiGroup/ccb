# Remediation baseline (Step 1)

[Docs index](index.md)

*ccb-remediation-baseline*

Step 1 of the Engineering Remediation Directive
(`~/workspace/user/files/CCB_Engineering_Remediation_Directive_8_9tma.md`,
§0 and §12 item 1). Baseline, build/test setup, and verified findings
at `test` head `665ec57` ("Phase 1: core graph engine (DAG) drives the
build", CI run #17 green). The V2 roadmap is CLOSED at Phase 1; this
document governs the remediation work queue (Steps 2–12).

Method: read the code and the Phase 0 audit
(`docs/architecture-audit.md`, written at head `f41ef5e`), re-verified
every finding this step depends on against the current tree, and ran
the live checks below. Anything not re-verified is marked as such —
an audit claim carried forward is not a Step 1 finding.

## 1. Baseline confirmation

- Branch: `test` (phone clone `~/ccb`, verified over SSH).
- Head: `665ec57` "Phase 1: core graph engine (DAG) drives the build".
- Previous commits: `25c7797` (Phase 0 audit), `f41ef5e` (preview onto
  `ccb_tui`), `6831f1c` (TUI library), `61e0039` (test_native LSan fix).
- Phone working tree: clean except untracked `ccb-test.tar.gz`
  (unrelated leftover, not touched).
- VM tree (`~/workspace/ccb`): snapshot copy, NOT a git repo (by
  design). No drift from the phone head was found by inspection of
  the files this step touches; no unrelated work was overwritten.
  Full-tree diff VM-vs-phone was not run (no git in the VM tree);
  the sync in §5 is the check.
- Binary: `ccb 0.1.0`, dev build (`build/ccb`, dynamic, unstripped).

## 2. Fast test suite (local verification)

Exact command (sequential, as required on this shared VM):

```
cd ~/workspace/ccb && cmake --build build -j1
ctest --test-dir build -L fast
```

Result: **46/46 passed, 0 failed** (total test time 15.93 s;
`ctest -N` reports 55 entries: 46 fast + 9 slow, matching the
directive's counts). Label summary: config 2, graph 1, misc 5,
pipeline 6, preview 29, setup 2, tui 1. The full suite (including the
9 slow tests) runs in CI after a push; it was not run locally in this
step, per the task.

No pre-existing local failures. Nothing in this step changes source
code, so no failure can be attributed to this step.

## 3. Benchmark setup status

`benchmarks/bench-builds.sh` is intact (usage header, cold/warm/
no-op/incremental definitions, median-of-3, `--explain` counts) and
its inputs exist: `examples/hello` and the Showcase fixture
`~/workspace/ccb-showcase`. The global stdlib dex cache is populated
(`~/.cache/ccb/dex-stdlib` holds entries). The full benchmark suite
was NOT re-run in this step (many minutes on this shared VM, per the
task); the Phase 0 baseline numbers in
`docs/architecture-audit.md` §7 stand as the recorded baseline:

- hello: cold 109.9 s, warm 113.0 s, no-op 5.5 s, incremental 79.3 s
  (medians, 2026-11-27, contended shared VM).
- Showcase: cold 242.2 s, warm 58.7 s, no-op 2.9 s, incremental 36.7 s.
- Open question carried from Phase 0 §7, still unexplained: a no-op
  rebuild costs 5.5 s (hello) / 2.9 s (Showcase) with everything
  skipped — far above the documented ~0.2 s. Step 10 owns explaining
  it before claiming any no-op win.

One live build timing from this step (not a benchmark, single run,
same contended VM): hello cold-ish build (global dex cache warm,
project `build/` removed) took 22.8 s wall — consistent with the
documented ~19–23 s hello cold range, and inconsistent with the
Phase 0 §7 medians, which the audit itself attributes to VM
contention that day. Do not compare later measurements against the
§7 medians unless machine class and load are comparable; re-run the
script on a quiet VM before Phase 7 (Step 10).

## 4. What changed since the Phase 0 audit (verified at head)

The audit was written at `f41ef5e`; Phase 1 (`665ec57`) landed since.
These audit items are now wholly or partly addressed — later steps
must not re-litigate them as if they were still open:

- **Graph engine exists (audit §1 layer 3, C2, C8).**
  `src/ccb_graph.{h,c}` + `src/ccb_pipeline_graph.{h,c}`: task struct
  with declared inputs/outputs/deps (`ccb_graph.h:81-87`), builder
  declaring them (`ccb_pipeline_graph.c:116-139`, `265-267`),
  validation for duplicate task IDs (`CCB-E1012`,
  `ccb_graph.c:94`), unknown dependencies (`CCB-E1013`,
  `ccb_graph.c:191,225,238`), cycles (`CCB-E1014`,
  `ccb_graph.c:296-321`), and duplicate outputs (`CCB-E1013`,
  `ccb_graph.c:201-222`, `366`). `ccb_pipeline_build`
  (`src/ccb_pipeline.c:603-652`) builds the graph and executes it;
  the stage order is exactly the legacy order
  (`docs/graph.md:26,131`).
- **Most skip decisions now check outputs exist (audit R1, partly).**
  Existence checks verified at head: compile-res
  (`src/ccb_pipeline_res.c:92-100`, every compiled zip must exist),
  link (`ccb_pipeline_res.c:274`, `link_out` must exist), dex
  (`ccb_pipeline_dex.c:495`, `dex_dir` exists and holds ≥ 1 `.dex`),
  package (`ccb_pipeline_package.c:431`, APK must exist), align/sign
  (`ccb_pipeline_sign.c:59,174`, aligned/signed APK must exist),
  merge-manifest (`ccb_pipeline_deps.c:327`, merged manifest must
  exist), module AAR (`ccb_pipeline_module.c:589-591`, AAR and
  classes jar must exist), native `.so`
  (`ccb_pipeline_native.c:250,344`, `.so` must exist). The two
  exceptions are B-1 and B-2 below.
- **Corrupt cache is safe (audit D2/R1, load side).**
  `ccb_cache_load` (`src/ccb_cache.c:44-56`) treats a missing,
  unparseable, non-object, or wrong-version cache as empty
  ("rebuild everything"); version must be exactly `1`
  (`ccb_cache.c:58-61`).
- **Failed build still saves cache — confirmed intended.**
  `src/ccb_pipeline.c:623-629` saves on failure so a retry resumes
  ("Save progress: completed steps stay cached"). Only steps that
  ran to completion record a fingerprint (e.g. compile-code sets
  its step fp after success, `ccb_pipeline_code.c:327`), so this is
  sound as designed; it is not a bug.
- **Doc drift fixed.** `docs/build-tests.md:16` now says "full suite:
  55 tests", matching `ctest -N` (the audit §8 noted it said 52).

## 5. Prioritized verified findings

Severity order within each class. Every item cites the file +
line/function verified in this step and the evidence. "Code-verified"
means read at head; "live-verified" means run. Items marked
**unverified** were not checked in this step — they are audit claims
carried forward, not Step 1 findings.

### (a) Confirmed bugs

- **B-1 — compile-code skip is fingerprint-only; deleting the classes
  directory yields a false "up to date" build (exit 0, stale APK).**
  `src/ccb_pipeline_code.c:226`: `if (!p->opts->dry_run && old != NULL
  && strcmp(old, fp) == 0)` — no output-existence check, unlike
  every other step in §4. Live-verified on a copy of
  `examples/hello` (`/tmp/ccb-r1-hello`, `ccb 0.1.0`, env
  `JAVA_HOME=$HOME/jdk17`, `ANDROID_HOME=$HOME/android-sdk`,
  `KOTLIN_HOME=$HOME/kotlin-2.4.21/kotlinc`):
  1. First build: exit 0 in 22.8 s, `build/outputs/debug/app.apk`
     produced.
  2. `rm -rf build/intermediates/classes` with `build/ccb-cache.json`
     intact.
  3. Rebuild: exit 0 in ~1.0 s, reporting `cache compile-code: hit
     (source inputs unchanged)`, `no classes or jars, skipping dex`,
     `no classes or jars, skipping dexcheck`, `cache package/align/
     sign: hit`, and `up to date: .../app.apk`. `build/intermediates/
     classes` still did not exist afterwards; the APK timestamp was
     unchanged from the first build — the "up to date" APK is stale
     by construction (its classes are gone, yet the build claims
     success). This is the audit's R1, confirmed live and worse than
     the audit's wording: the failure does not even surface later,
     because dex and dexcheck silently skip too (see B-1a).
  - **B-1a — dex and dexcheck silently skip when there are no classes
    or jars.** `src/ccb_pipeline_dex.c:422-424` ("no classes or jars,
    skipping dex") and `src/ccb_pipeline_dex.c:665-674` (same for
    dexcheck). Legitimate for a resources-only project, but it turns
    B-1's missing-classes state into a silent pass instead of an
    error. Step 2 must decide: missing classes after a compile-code
    skip is an error (or forces a rerun), not a skip.
- **B-2 — deps step skip is fingerprint-only; no check that extracted
  AAR outputs still exist.** `src/ccb_pipeline_deps.c:137`:
  `strcmp(old, fp) == 0` with no `ccb_path_exists` guard (contrast
  merge-manifest at `:327`, which does check). Code-verified at head;
  live behavior not exercised in this step (no AAR fixture build was
  run). Impact: deleting an extracted AAR directory under
  `build/intermediates/` with an intact cache skips extraction; the
  failure surfaces later, if at all.
- **B-3 — cache save is not atomic; an interrupted save truncates
  `ccb-cache.json` in place.** `ccb_cache_save`
  (`src/ccb_cache.c:91`) builds the JSON string and calls
  `ccb_write_file` (`src/ccb_fs.c:122-140`), which is `fopen(path,
  "wb")` + `fwrite` directly onto the live file — no temp file, no
  rename. Code-verified. Impact is bounded: the load side treats a
  truncated cache as empty (§4), so the worst case is losing all
  incremental state and rebuilding everything, not a wrong build.
  The stdlib dex cache already publishes by atomic rename (audit
  D2) — the main cache should converge on the same pattern. Step 2.

### (b) Correctness risks

- **R-1 — No output-integrity validation anywhere: existence ≠
  validity.** The §4 existence checks catch deletion, not
  truncation or corruption: dex only requires `ndex > 0`
  (`ccb_pipeline_dex.c:495-503`), package/align/sign only require
  the file to exist. A truncated `.dex` or APK under
  `build/intermediates/` is still skipped and packaged. (Dexcheck
  runs even on a dex skip, which catches some dex corruption — but
  see B-1a: it silently skips when there are no classes at all.)
  Step 2 must define a validation tier per output and justify it
  (directive §1: avoid validation that makes cache hits slower than
  necessary).
- **R-2 — Whole-step granularity + unaudited fingerprint key
  selection.** Each step builds its fingerprint keys inline
  (`ccb_fp_add*` calls scattered across `ccb_pipeline_res.c`,
  `ccb_pipeline_code.c`, `ccb_pipeline_dex.c`,
  `ccb_pipeline_native.c`, `ccb_pipeline_sign.c`,
  `ccb_pipeline_package.c`, `ccb_pipeline_module.c`); there is no
  audited table of task → fingerprint inputs and no test that
  enumerates config keys × steps (audit D3/R2, still true at head).
  A missing key silently reuses a stale step. Step 2/3 (fingerprint
  table + per-task mutation tests) and Step 5 (keys move into task
  declarations).
- **R-3 — `--explain` can over-predict runs downstream of link.**
  `src/ccb_pipeline_code.c:213-216` (compile-code reports `run`
  when `link_will_run`), `ccb_pipeline_dex.c:482-486`,
  `ccb_pipeline_package.c:418-422` — conservative "will change"
  predictions from the `*_will_run` flags on `ccb_pipe`, not facts
  computed from real fingerprints. `docs/graph.md` does not label
  the plan as an upper bound (no "upper bound"/"conservative"
  wording found). An agent planning from `--explain` sees runs that
  never happen. Step 4/5: plan from real fingerprints, or label it
  explicitly.
- **R-4 — Silent ignores (audit R4, all still true at head).**
  - **R-4a — Manifest merge drops library-manifest entries outside
    the merged set with no diagnostic.** `src/ccb_manifest.c:599`:
    "queries, ...) is not merged; the app manifest owns it";
    `docs/build.md:47`: "Everything else in a library manifest is
    ignored." `warns_push` is only called for conflicts
    (`ccb_manifest.c:901,944`), never for ignored entry kinds. A
    legal APK can behave differently from what the library author
    declared. Step 6 (directive §6-style verdict: support or
    diagnose).
  - **R-4b — Maven exclusions: parent-declared `<exclusions>` are
    ignored; per-dependency POM `<exclusions>` unsupported.**
    `src/ccb_maven_resolve.c:364-365` ("exclusions are not
    inherited (CCB has no per-dependency exclusions)");
    `src/ccb_maven_pom.c:258` ("Anything deeper (e.g. text inside
    `<exclusions>`) is [not parsed]"). Project-level exclusions
    ARE supported (`mv_exclusions`, `ccb_maven_resolve.c:124`).
    Step 9.
  - **R-4c — Profile activation by id only.**
    `src/ccb_maven_pom.c:25-27` ("Other activation forms (jdk, os,
    property, file) [are not evaluated]"), `:225`. Step 9.
- **R-5 — Environment inputs are not fingerprinted (audit R8, still
  true).** The tool fingerprint (`src/ccb_pipeline.c:498-513`)
  records build-tools version, aapt2/zipalign paths, kotlinc
  version, java major, compile_sdk — but toolchain resolution reads
  `PATH`, `JAVA_HOME`, `KOTLIN_HOME`, `ANDROID_HOME`,
  `ANDROID_SDK_ROOT` (`src/ccb_sdk.c:20,154,492,495,667`), and
  release signing reads passwords from `env:VAR`
  (`src/ccb_pipeline_sign.c:263-275`). Changing `JAVA_HOME` between
  two identical-version JDKs invalidates nothing. Low risk today
  (same-version JDKs produce the same output), but Step 2/3 must
  decide explicitly which environment inputs are declared inputs.
  Note: password *values* must never be fingerprinted or stored —
  only the variable *name* (or nothing) is a candidate.
- **R-6 — No resource limits of any kind (audit R6, carried).**
  No memory cap, no concurrency cap, no d8 argfile fallback for
  very large projects (`.class` files pass as individual arguments;
  the OS argument limit is the known cliff). **Unverified in this
  step** (d8 invocation not re-read); carried from the audit. Step 8
  (worker limits before parallelism) and Step 11 own it.

### (c) Architectural debt

- **D-1 — `ccb_pipe` is still a god-struct (audit C1, still true).**
  `src/ccb_pipeline_internal.h:28-112`: one struct carrying options,
  log, diags, config, SDK, cache, 8 directory paths, 6 parallel AAR
  arrays, merge state, module/library work dirs, 8 `*_will_run`
  flags, step counters. Every step takes `ccb_pipe *` and mutates
  shared state. Mitigated, not fixed, by Phase 1: the graph layer
  now declares per-task inputs/outputs, but execution still flows
  through the pipe. Step 7.
- **D-2 — The pipeline mutates the project model at stage 0 (audit
  C3, still true).** `src/ccb_pipeline.c:437-456`: library `.aar`
  paths are appended to `p->cfg->aars`, and
  `p->cfg->kotlin_plugin_jars` is filled by resolving Maven plugins
  in stage 0. A task graph built from the config must treat the
  config as read-only input; plugin resolution belongs in a resolve
  step. Step 5/7.
- **D-3 — Toolchain identity is split (audit C4, still true).**
  Global tool fingerprint (`ccb_pipeline.c:498-513`) has no NDK/
  clang identity; the native step adds its own `ndk` version key
  (`ccb_pipeline_native.c:173`) and the dex step keys on the stdlib
  jar hash + min-api + build-tools version. Each step keys what it
  needs (currently sound), but there is no single toolchain identity
  a task fingerprint can reference. Step 7.
- **D-4 — The module build is a second mini-pipeline (audit D1,
  still true).** `src/ccb_pipeline_module.c` re-implements resource
  compile, kotlinc, classes-jar write, and AAR assembly:
  `ccb_mod_compile_resources` (`:197`), `ccb_mod_run_kotlinc`
  (`:332`), `ccb_mod_compile_sources` (`:414`),
  `ccb_mod_write_classes_jar` (`:424`), `ccb_mod_assemble_aar`
  (`:474`), with its own fingerprint keys
  (`ccb_mod_fingerprint`, `:135`). Any compile-behavior fix must
  still be applied twice — the most likely source of module/app
  divergence bugs. Step 6, one shared operation at a time.
- **D-5 — Two caches with different rules (audit D2, still true).**
  `build/ccb-cache.json` (per-step fingerprints, corrupt →
  rebuild-everything, non-atomic save per B-3) vs the global stdlib
  dex cache (atomic rename on publish, validated on read by dex
  magic + header size, corrupt → fall back to the jar). Plus a
  third cache-like file, `build/intermediates/tool-probes.json`
  (tool version probes, keyed by path + mtime + size). Step 2/3:
  converge on one cache engine with the dex cache's properties.
- **D-6 — Fingerprint construction is copy-pasted per step (audit
  D3).** See R-2. Step 5.
- **D-7 — Two packaging paths (audit D4, still true by design).**
  APK (`ccb_step_package`) and AAB (`ccb_step_package_aab` +
  `ccb_bundle.c`) share the ZIP writer but lay out different trees
  with different verify steps. Frozen: no new packaging behavior
  on either path until Step 12. No action before then.
- **D-8 — R-class handling in three places (audit D5).**
  **Unverified in this step** (not re-read at head); carried from
  the audit: R.java generated by aapt2 link, compiled by javac in
  the code step, "definalized" for modules. Step 6 owns confirming
  it when the resource provider is extracted.
- **D-9 — Diagnostics are not yet machine-observable per task
  (audit R5, partly addressed).** The graph adds task/module IDs to
  validation diags (`CCB-E1012`–`CCB-E1015`, §4), and the build
  summary object now exists (`ccb_graph_summary_json`,
  `ccb_pipeline.c:617-621`). Whether per-*step* diags carry task
  IDs, and whether JSONL events are per-task rather than per-phase,
  was **not verified in this step**. Step 5.
- **D-10 — The kotlinc daemon is a second execution path (audit C7).
  Unverified in this step** (not re-read at head); carried from the
  audit. Step 6 owns folding it into the kotlinc provider.

### (d) Premature / out-of-scope ideas

Per directive §11, the following must NOT be started in this
remediation, and none was found in the tree at head (no evidence of
any of them in the files inspected; the scope freeze in
`docs/architecture-audit.md` §6 still stands):

remote cache, distributed scheduling / work stealing, public
plugin/custom-task API, preview feature expansion (bug fixes only),
AAB feature expansion, Maven rewrite, graph-engine replacement,
Rust/other-language rewrite, process-per-task scheduling, new config
options without a concrete use case, a framework emulating Gradle,
optimizing without benchmarks.

Preview remains the largest subsystem (~42% of `src/` at audit time)
and the least load-bearing for builds; its tests are 29 of the 46
fast tests. Any refactor touching shared code (`ccb_config`, CLI
dispatch, `ccb_tui`) must keep preview green. The preview gate in
`check`/`build` stays opt-in.

## 6. Work queue for Steps 2–12

Derived from §5, in directive §12 order. Each item names its step
and the findings it addresses.

1. **Step 2 — Cache output validation and safe publication.**
   B-1, B-1a, B-2, B-3, R-1, R-5 (decision), D-5. Add
   output-existence checks to compile-code and deps skips; make
   dex/dexcheck treat "no classes" after a compile skip as an
   error (or force a rerun), not a silent skip; publish
   `ccb-cache.json` atomically (tmp + rename, matching the dex
   cache); define and justify a per-output validation tier
   (existence now, hashes where cheap); decide explicitly which
   environment inputs are declared fingerprint inputs.
2. **Step 3 — Cache/fingerprint regression tests.** R-2, B-1, B-2,
   B-3, R-1. Tests for: missing/truncated/corrupt step outputs,
   corrupt/incomplete cache metadata, incompatible cache version,
   interruption during publication, upstream invalidation,
   no-change rebuild, and a per-task fingerprint-mutation test
   (change exactly one input, assert exactly the expected tasks
   rerun). Plus the audited fingerprint table (task → inputs) as a
   doc + a test enumerating config keys × tasks.
3. **Step 4 — Graph validation and deterministic planning.** R-3,
   D-9 (validation side). Verify the Phase 1 validators' coverage
   with tests (empty/single-node/linear/branching/convergent
   graphs, cycles, unknown deps, duplicate IDs/deps, output
   conflicts, GENERATED inputs with no producer); prove planning is
   deterministic; make `--explain` exact or label it an upper
   bound.
4. **Step 5 — Explicit task inputs, outputs, dependencies, results.**
   D-2, D-6, R-2, R-3, D-9. Move fingerprint-key selection into
   task declarations one step at a time; config becomes read-only
   input to the graph (plugin resolution moves out of stage 0);
   step diags gain task/module IDs; JSONL events extend from
   phases to task lifecycle.
5. **Step 6 — Consolidate duplicated pipeline providers.** D-4,
   D-8, D-10, R-4a, D-7 (frozen except the R-4a diagnostic
   verdict). One shared operation at a time (resource → code →
   dex → manifest merge → package/sign/verify → native/cmake →
   deps/AAR → modules, delete the mini-pipeline last); each
   landing keeps the APK byte-identical. Manifest-merge ignored
   entries get a support-or-diagnose verdict.
6. **Step 7 — Configuration/runtime-state boundaries and ownership.**
   D-1, D-3. Split `ccb_pipe` into validated immutable config,
   per-task execution context, toolchain resolution results,
   artifact ownership, diagnostics, cache/task state — only where
   justified, no split for aesthetics; define the single toolchain
   identity. Prerequisite for Step 8.
7. **Step 8 — Scheduler, only after Steps 2–7 prerequisites.**
   R-6, D-1 (ownership). Deliberately simple: pthread worker pool,
   central ready queue, dependency counters, configurable worker
   count, failure propagation, cancellation, orderly shutdown,
   single-writer atomic cache publication. Staged: single-worker
   first, then two independent tasks, then gating/failure/
   cancellation/stress, then real build tasks. No work stealing,
   no distributed scheduling. Sequential-vs-parallel output
   equivalence is the acceptance test.
8. **Step 9 — Maven semantic audit and diagnostics.** R-4b, R-4c.
   Write the resolver spec from the code as it is; one test per
   rule; each unsupported semantic (parent/per-dependency
   exclusions, jdk/os/property/file activation) gets a
   support-or-diagnose verdict with an actionable diagnostic. No
   Maven rewrite.
9. **Step 10 — Reproducibility and benchmark suite.** §3 open
   question (no-op 5.5 s), R-6 (d8 argfile). Re-run
   `benchmarks/bench-builds.sh` on a quiet VM before and after any
   performance change; compare medians; add medium/large fixtures
   per directive §8 (5/15/25/50/100 modules where feasible);
   define the reproducibility contract (ZIP ordering/timestamps,
   fingerprints, lockfile consistency).
10. **Step 11 — Real multi-module scaling validation.** D-4, D-5
    (per-module incrementality). Two-module fixture: change module
    A, assert B's tasks rerun and dex changes; change B only,
    assert A's tasks skip. Parallel module execution only on the
    proven Step 8 scheduler.
11. **Step 12 — Reassess advanced features only after the above is
    demonstrably stable.** §5(d) items stay frozen until then; each
    needs its own scope note + tests + a verified critical need
    before implementation.

## 7. Limitations of this step

- Full suite (9 slow tests) not run locally; CI covers it, but this
  commit is docs-only so no CI run is created (docs paths are in
  `paths-ignore` in `.github/workflows/ci.yml`).
- R-1's live test used the hello fixture only; the deps-step variant
  (B-2) and corrupt-output variants were code-verified, not run.
- The benchmark suite was not re-run; §3 numbers are Phase 0's.
- No VM-vs-phone full-tree diff was run (VM tree has no git); sync
  verification is `git status` on the phone after the sync.
- Items marked unverified in §5 (R-6, D-8, D-9 partial, D-10) are
  audit claims, not Step 1 findings; the owning steps must verify
  them before acting on them.
