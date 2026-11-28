# Advanced features reassessment (Step 12)

[Docs index](index.md)

*ccb-advanced-features-reassessment*

Step 12 of the Engineering Remediation Directive — the closing
step. The core work (Steps 1–11) is done and green; this document
reassesses every advanced feature that was frozen during the
remediation against the now-demonstrated state of the core, and
recommends what (if anything) comes next. It is an assessment, not
an implementation round: no feature below is implemented here.

Method: each candidate was checked against the code and the step
outcomes recorded in [remediation-baseline.md](remediation-baseline.md),
[graph.md](graph.md), [benchmarks/README.md](../benchmarks/README.md),
and [reproducibility.md](reproducibility.md), with the cited
file-and-line claims re-verified by inspection in this step rather
than trusted from summaries. Where a claim could not be re-verified
it is marked as such.

Job state at this writing: `test` head `d123fb1` (Step 11), CI run
#29 green. Suite: 62 tests total, 51 fast (`ctest -N` and
`ctest --test-dir build -L fast`, this step: 51/51 passed in 17.02 s
on the shared VM). All twelve remediation steps landed green; the
per-step history is in §8 of the baseline.

## How to read a verdict

- **Do next** — concrete, bounded, and justified by evidence from
  Steps 1–11. Small enough for one step.
- **Defer** — a real idea with a stated trigger: the condition that
  would make it the right next step. Not a no.
- **Drop** — no demonstrated need and no trigger on the horizon;
  reopen only on a concrete diagnosed need, not as a restructuring
  goal.
- **Standing rule** — not a feature at all, but a constraint future
  work must obey.

## 1. Scheduler stage 6 (parallel execution of real builds)

**Current state.** The scheduler (`src/ccb_sched.{h,c}`) is proven
through stage 5 and unused by any build path: no `--jobs` flag
exists, and the sequential executor remains the only way a real
build runs. Stage 6 — parallel module tasks with a
sequential-vs-parallel byte-identical APK equivalence test — was
audited in Step 11 and stopped with verdict STOP
([graph.md](graph.md), "Stage 6 audit"). The overlap invariant is
pinned by a test (`tests/test_graph.c`, 454 checks, mutation-checked):
only sibling module tasks can ever run concurrently, because every
non-module task is chained to its predecessor and `merge-manifest`
depends on every module task.

**What the remediation proved.** Three things, and they point in
different directions:

1. Parallelism is *possible in principle*: stages 1–5 prove the
   scheduler mechanics (gating, failure propagation, cancellation,
   cache deltas) deterministically, and the overlap invariant
   bounds the concurrency surface to module tasks only.
2. Parallelism is *not yet safe*: seven shared-state items are
   enumerated in `graph.md` (shared log stash, diag list realloc,
   cache map realloc during fingerprint reads, pipe AAR lists
   iterated while appended, run/skip counters read as a
   before/after delta, stdout interleaving with the JSON stream,
   lazy zip CRC init). Worse, mutexes alone do not restore
   correctness: module classpaths and fingerprints read the global
   AAR lists, so under parallel completion order they would vary
   run to run — nondeterministic skips, not just reordered output.
3. Parallelism is *not the scaling fix*: Step 11 measured the
   bottleneck as per-module compile cost growing with N — each
   module's kotlinc invocation carries every upstream module's
   classes jar on its classpath (O(N) per module, O(N²) per
   project), plus one JVM spawn per module — and the build is
   CPU-bound (cpu/wall ≈ 2.2 at N=15–25, 1.9 at N=50). Parallel
   execution would use the phone's idle CPUs, but it cannot shrink
   the O(N²) work itself; and the N=50 cold median (917.3 s, 73%
   above the linear prediction) shows the per-module cost itself
   rising with N.

**Prerequisites still missing.** Exactly (a)–(f) in `graph.md`, in
order: (a) scope module classpath and fingerprint to the module's
upstream set; (b) return the task-owned delta and artifact from
`ccb_build_module_at` and have the driver merge both at the join
in topological order; (c) report run/skip from the task and
aggregate counters in the driver; (d) per-task log and diag lists;
(e) one-time zip CRC init before workers start; (f) wire the
driver behind an explicit opt-in plus the equivalence test —
which also needs an independent-modules fixture, since the Step 11
scaling matrix chains its modules and can never have two module
tasks READY. The kotlinc daemon is off by default and needs its
own concurrency audit before use with parallel module tasks.

**Risk if done now.** A rushed parallel executor trades a measured
scaling record for an unproven one: nondeterministic skips from
completion-order-dependent fingerprints are a *silent* correctness
failure (a build that reports success from stale steps), strictly
worse than the slow sequential builds it would replace.

**Recommendation: split it — do (a)+(b) next, defer (c)–(f).**
(a)+(b) are correctness work in their own right, not just
parallelism prep: today a module's fingerprint depends on which
unrelated modules happened to finish before it, which is a latent
determinism defect in the sequential build too, and the two
comments in `src/ccb_cache.h` and `ccb_pipeline_modules.c:220`
already describe the driver-owned design the code does not
implement. Landing (a)+(b) sequentially — with the existing
byte-identical rebuild tests as the gate — removes that defect
and leaves the remaining stage-6 work as pure plumbing. (c)–(f)
then follow as one step with the equivalence test. Do not wire
the driver to real builds before (a)–(f) all land.

## 2. N=100 retry

**Current state.** Infeasible, stopped with evidence, no faked
numbers ([benchmarks/README.md](../benchmarks/README.md), Step 11
runs). Cold run 1 built 26 of 100 modules in ~62 min (~150 s/module,
8x the N=50 rate of ~18 s/module); the full 12-run matrix would
need ~12–15 h. Measured stop conditions: swap 5.4 of 5.9 GiB used
(92%), MemFree 666 MiB of 11,468 MiB; no single process hogs memory
or CPU, so the stall is memory pressure across the system, not one
runaway process — but that cause is the most plausible, not proven.

**What the remediation proved.** The N=100 failure is a property
of the *harness conditions*, not yet of the build: per-module cost
at N=100 is 8x the N=50 rate on the same binary, phone, and
harness, which is inconsistent with the O(N²) model alone (that
predicts ~2x). Until the memory-pressure confound is removed, no
N=100 number means anything.

**Prerequisites still missing.** A phone with memory pressure
relieved (reboot, or the swap hog identified) and a fresh
single-cold-run rate check before committing to the matrix. The
phone-doze artifact is also recorded: an idle `sleep` must not be
used as a cool-down timer on this phone again.

**Risk if done now.** Another 12–15 h of phone time producing
numbers that cannot be compared to the N=15/25/50 medians, and
thermal drift on top of memory pressure.

**Recommendation: defer, with a cheap trigger.** Retry only after
the memory-pressure relief + single-cold-run rate check, and only
if the question still matters: a chain of 100 modules is a
pathological fixture, and the more useful scaling question after
§1(a) lands is independent modules (the stage-6 fixture), not a
longer chain. If the ~150 s/module rate persists on a quiet phone,
the bottleneck is in the build itself and N=100 stays infeasible
for this harness — record that, do not keep re-running it.

## 3. Remote cache

**Current state.** None exists. No remote-cache code is in `src/`
(the only "remote" code is Maven artifact fetching). The two local
caches converged in Steps 2–3 on one engine shape: the main cache
publishes atomically (`ccb_write_file_atomic`,
`src/ccb_cache.c:170`) and loads corrupt-or-wrong-version as empty;
the stdlib dex cache validates on read (dex magic + header size).
The third cache-like file, `tool-probes.json`, still writes
non-atomically — see §9.

**What the remediation proved.** The *prerequisites* for a remote
cache are now demonstrated: fingerprints are audited per task
(Step 3 table + mutation tests), cache publication is atomic and
single-writer by design (Step 8's delta-merge rule, Step 10's
canonical save order), and reproducibility is pinned end to end
(two clean builds byte-identical). A remote cache built on the
Step 3 fingerprint table would at least key correctly — which
could not have been said at Step 1, when fingerprint-key selection
was unaudited and inline per step.

**Prerequisites still missing.** Everything that makes it a
feature rather than a protocol: a cache-entry format with
versioning and eviction, a transport (the codebase is libc-only
by the frozen dependency rule, so this needs a written
justification for any new dependency or a hand-rolled client),
authentication/secret handling (signing passwords are env-var
names only, never values — a remote cache must not become a
secret leak), trust/validation of downloaded artifacts (existence
≠ validity was finding R-1; a remote entry needs the same
validation tier Step 2 defined locally), and a demonstrated need:
no user, CI job, or second machine currently shares builds.

**Risk if done now.** A networked, authenticated, trust-bearing
subsystem landing on a core whose own scaling bottleneck is still
unfixed, with no consumer waiting for it. It would also be the
first part of CCB that can fail a build because of the network
during a build — the hermeticity rule (`setup` prefetches, builds
never download) would need an explicit exception.

**Recommendation: defer.** Trigger: a second machine or CI job
that demonstrably repeats the same builds, plus a scope note and
a decision on the transport/dependency question. It is the most
credible of the deferred features — the fingerprint groundwork is
real — but need comes before protocol.

## 4. Custom task API / public plugin API

**Current state.** None exists. There is no task-registration API,
no plugin loading, and no extension point of any kind: tasks are
declared in `src/ccb_pipeline_graph.c`, providers are the shared
internal operations consolidated in Step 6
(`src/ccb_pipeline_compile.c` and friends), and the only "plugins"
in the tree are Kotlin compiler plugin jars and Maven plugins —
both data, loaded by kotlinc, not code CCB executes. Step 5 moved
fingerprint-key selection into task declarations, which is the
shape a task API would build on, but it did not create one.

**What the remediation proved.** The internal side is ready: tasks
now have declared inputs/outputs/deps, explicit results, and
per-task diagnostics with IDs. What is *not* proven is any external
need: no third-party task, no user request, and no internal task
that cannot be expressed in the current declarations.

**Prerequisites still missing.** A versioned API surface and its
stability promise, a decision on what a custom task may touch
(it cannot take `ccb_pipe *` — Step 7 split the god-struct
precisely so tasks do not share mutable state), sandboxing or at
least a failure-isolation contract (a crashing custom task must
not take down the build), and a first real consumer to design
against. An API designed without a consumer is a guess.

**Risk if done now.** A public surface that freezes today's
internal shapes — including the parts Step 7 deliberately left
unsplit — and then constrains every later core change. It is also
the feature most likely to be used to load native code into the
build process, which collides directly with §5.

**Recommendation: defer.** Trigger: one concrete custom task that
cannot be expressed as a declared task today, written first as an
internal task to prove the shape. No API without a consumer.

## 5. Plugin isolation

**Current state.** There is nothing to isolate: CCB loads no
plugins of its own, and the only native code it runs is the
toolchain (kotlinc, aapt2, d8, clang) as child processes — which
is already process isolation, not in-process loading.

**What the remediation proved.** The failure mode this rule guards
against is real and now measured: a kotlinc JVM peaks at ~550 MiB
and a bad one can stall a build for an hour (Step 11 N=100). Child
processes bound that blast radius; in-process loading would not.

**Prerequisites still missing.** None — this is not a feature to
build, it is a constraint to record before §4 ever lands.

**Risk if done now.** None, because there is nothing to do; the
risk is entirely in *forgetting* it later, when a plugin API
arrives and in-process loading looks like the easy path.

**Recommendation: standing rule, not a feature.** Record it here
and in the baseline: CCB must never load arbitrary native C
plugins into the main process by default. Any future plugin API
must execute plugin code out-of-process (child process or daemon)
or justify in-process loading in writing, per plugin, with the
failure-isolation contract from §4. No work is authorized by this
recommendation.

## 6. External providers

**Current state.** None exist and there is no provider extension
point. Step 6 consolidated the *internal* providers — the compile
operations (aapt2 compile/link argv, classpath, kotlinc, javac)
are now shared code used by both the app steps and the module
builder, with `test_pipeline_shared` pinning the shared argv and
`test_modules` pinning byte-identical rebuilds. "External
provider" here means a third-party implementation of one of those
operations (a different compiler, packager, signer).

**What the remediation proved.** Consolidation proved the internal
operations are factorable — the shared-provider shape exists and
is tested. It also proved the cost of a second implementation:
the module build is still one fused step with its own fingerprint
keys, and output equivalence across implementations is pinned by
byte-identical tests. Any external provider would need the same
equivalence gate.

**Prerequisites still missing.** A provider interface (today the
providers are plain C functions, not a vtable), a selection
mechanism (config? CLI? discovery?), and — as with §4 — a first
real external provider to design against. The manifest-merge
verdict from Step 6 is a caution here too: the supported set is
frozen and expansion is a feature, not a fix.

**Risk if done now.** A second implementation of a core operation
with no consumer and no equivalence test, doubling the
maintenance surface of the most load-bearing code in the tree.

**Recommendation: drop as a standalone goal.** Reopen only on a
concrete diagnosed need for a specific external provider (e.g. a
real alternative signer), designed against that provider. Do not
build a provider framework speculatively.

## 7. AAB as an independent provider

**Current state.** AAB works on the shared packaging path and is
frozen by design (baseline D-7). APK (`ccb_step_package`) and AAB
(`ccb_step_package_aab` + `ccb_bundle.c`, 135 lines) share the ZIP
writer but lay out different trees with different verify steps;
link runs with `--proto-format` into `linked-aab.ap_` on its own
cache key; `bundletool validate` passes on CCB-built bundles.
`test_aab` (88 s, slow suite) pins it. The candidate was to split
AAB out into an independent provider instead of sharing the
packaging path.

**What the remediation proved.** The shared path is not a
correctness liability: Step 6's consolidation kept APK output
byte-identical, Step 10's reproducibility checks cover the
packaging code, and no AAB defect surfaced in any step. The split
would be a restructuring, not a fix — the exact thing the
remediation's root-cause rule forbids.

**Prerequisites still missing.** A demonstrated need: a second
bundle consumer, a packaging behavior the shared writer cannot
express, or a defect attributable to the sharing. None exists.
The frozen list also explicitly bars new bundle features (no Play
Asset Delivery, no bundle signing changes) until the roadmap
decides them.

**Risk if done now.** Two packaging paths to keep byte-identical
and bug-compatible, for a format whose only current requirement
(bundletool validates, output is correct) is already met.

**Recommendation: drop as a standalone goal.** Keep AAB on the
shared path. Reopen only if a diagnosed AAB need cannot be met
there.

## 8. Preview feature expansion

**Current state.** Frozen during the remediation, and correctly so.
Preview remains the largest subsystem: ~20,043 of 50,728 source
lines (~40%), 29 of 62 tests, with its own parsers, modes (diff /
walk / watch / tree-live / LSP), handlers, scenarios, and traces.
It is opt-in behind the `check`/`build` gate, and every step that
touched shared code (`ccb_config`, CLI dispatch, `ccb_tui`) kept
it green — the fast suite's 29 preview tests are the tripwire.

**What the remediation proved.** The maintenance cost is real but
bounded and, importantly, *not growing*: no step had to expand
preview to do its work, and the freeze held for all twelve steps
without blocking any of them. The cost is the test share and the
shared-code coupling, not active breakage — preview caused no
remediation failure. What the remediation did *not* prove is that
preview's maintenance cost is low: 29 tests exist because the
surface is large, and any future change to shared code still pays
the preview gate.

**Prerequisites still missing.** For any expansion: a scope note
naming the specific capability, its tests, and a verified critical
need — the freeze's own terms. For a *cost reduction* (as opposed
to expansion): a decision about whether preview's size is a problem
worth solving at all, which nobody has made.

**Risk if done now.** Expansion without a need repeats the
pre-remediation pattern: the largest, least load-bearing subsystem
growing while the build core's measured bottleneck (Step 11)
stays unfixed.

**Recommendation: defer expansion; keep the freeze.** Trigger:
a verified critical need with a scope note, as the freeze
requires. Separately, note as a future question — not a step —
whether preview should be trimmed or split out; that question
needs a user decision, not an engineering assessment, and this
step does not make it.

## 9. Smaller recorded follow-ups

These were recorded during Steps 2–11 as not-fixed-here. Each is
small; none is a feature. Assessed together because they share a
shape: one root cause each, one step each (or one batch).

### 9a. Winning-branch CCB-E3021 prefetch gap (Step 9)

**State.** Cache-only resolution can still fail with CCB-E3021
for a *winning* branch whose metadata was never prefetched
([reproducibility.md](reproducibility.md), "Known gaps"). Step 9
fixed the losing-branch case (nearest-wins is decided before
version-spec resolution, so a losing branch never fails the build
for metadata it does not need). The winning case is a resolver
behavior question, left for a later step; no Maven code was
changed in Step 10.

**Readiness.** Step 9 proved the resolver's verdict machinery
works (support-or-diagnose per semantic, one test per rule) and
that the losing-branch fix is safe. The winning-branch fix is
the same machinery applied to the other side: prefetch the
winning branch's metadata, or diagnose precisely which coordinate
is missing and why.

**Prerequisites.** A failing fixture: a project whose winning
branch metadata is absent from the cache, reproducing E3021.
That fixture is the whole design question — without it, the fix
is a guess about which metadata the resolver needs.

**Risk if done now.** Low: resolver-only, gated by the Maven
test suite. The risk is doing it without the fixture and
"fixing" the wrong branch.

**Recommendation: do next, in the small-follow-ups batch (§10).**
Smallest real correctness gap in this list; it fails a build for
metadata the build needs, which is the opposite of a silent pass.

### 9b. `tool-probes.json` non-atomic writes (Step 2 follow-up)

**State.** Verified in this step: `ccb_sdk_probe_cache_store`
(`src/ccb_sdk.c:438-501`) serializes the probe cache and writes it
with `ccb_write_file` (`src/ccb_sdk.c:498`) — `fopen(path, "wb")` +
`fwrite` directly onto the live file (`src/ccb_fs.c:122-140`).
The main cache converged on atomic publication in Step 2
(`ccb_write_file_atomic`, `src/ccb_cache.c:170`); the probe cache
did not. Impact is bounded by design: the load side treats a
missing or corrupt file as a miss and re-probes ("A failed write
is silent: the next build just probes"), so the worst case is two
redundant version probes (~0.2 s of hot no-op time), never a wrong
build.

**Readiness.** The atomic writer already exists and is already
used by the main cache; this is a one-call change plus a test
(interrupt the store, assert the old file survives or the load
side re-probes).

**Prerequisites.** None beyond the test.

**Risk if done now.** Negligible — but it is also negligible if
deferred, which is why it sat since Step 2.

**Recommendation: do next, in the small-follow-ups batch (§10).**
One call, one test, closes the last non-atomic cache-like write
in the tree. Do not expand scope: no probe-cache redesign.

### 9c. `ccb_cache_delta` adoption in the remaining steps

**State.** Verified in this step: only the module path uses
`ccb_cache_delta` (`src/ccb_pipeline_module.c:423-524`; the
scheduler tests pin merge semantics in `tests/test_sched.c`).
Every other step still records fingerprints directly into the
shared cache. The design intent — driver merges each task's delta
at the join — is already described by the comments in
`src/ccb_cache.h`, and Step 11 found the module path itself does
not yet live up to it (the merge happens inside the task body,
`ccb_pipeline_module.c:524`).

**Readiness.** The delta type and its merge semantics are proven
by tests, but the adoption is blocked behind §1: the delta only
pays off when tasks run under the scheduler, and the module path
— the only user — still merges inline. Adopting it in the
remaining steps before (a)+(b) land would spread the inline-merge
pattern further instead of fixing it.

**Prerequisites.** Stage 6 (a)+(b): task-owned deltas merged by
the driver at the join. Until the module path demonstrates that
shape, there is no proven pattern for the other steps to adopt.

**Risk if done now.** Moderate: a tree-wide fingerprint-recording
change with no scheduler consumer, duplicating the exact
inline-merge defect Step 11 documented.

**Recommendation: defer until §1(a)+(b) land.** Then it becomes
mechanical: each step returns its delta, the driver merges.
Do not adopt it step-by-step beforehand.

### 9d. CI job display names drifting stale

**State.** Fixed in this step: the CI job names said "61 tests"
(`.github/workflows/ci.yml`, both jobs) while `ctest -N` reports
62. Updated to the true count, verified by `ctest -N` before
editing. The same drift existed in `docs/build-tests.md`
(47 fast / 58 total vs the true 51 fast / 62 total); the current
claims there are updated in this step too. One stale count
remains and is *not* fixed here: `requirement.md:861` still says
"55 tests total (46 fast + 9 slow)" — that is the spec, and
updating a spec line belongs to a spec change, not a docs tidy.
Recorded here so the next spec touch fixes it.

**Recommendation: done in this step.** The lesson is worth
recording: a count written as a literal in a doc or a job name
drifts on the next test added. If the count is ever pinned by a
test or generated, it stops drifting; until then, treat every
count literal as stale until verified.

### 9e. Compile-code re-run quirk after every clean build

**State.** Recorded in Step 11, not fixed
([benchmarks/README.md](../benchmarks/README.md), Step 11): after
every clean build, compile-code re-runs once on the next build —
a 22 s probe at N=15 — after which the explain counts flip from
7 run / 21 skip to 1 run / 27 skip. The Step 2 fix (fingerprint
match alone never skips; the classes dir must also be valid,
`ccb_classes_dir_valid`, `src/ccb_pipeline_code.c:122-147` and the
skip at `:284-291`) is in place, so the re-run means either the
fingerprint or the classes validation disagrees with the clean
build's own output exactly once. Root cause is unknown: it was
recorded, not diagnosed.

**Readiness.** Highly diagnosable: compare the recorded
fingerprint and the classes-dir validation result immediately
after a clean build versus after the probe build. The two states
differ in exactly one of those two inputs, and the diff names the
cause. No new fixture is needed — the hello project reproduces it.

**Prerequisites.** One focused investigation on the hello fixture,
not a benchmark run. The fix, once the cause is known, is likely
in the fingerprint inputs or in what the clean build records —
small either way.

**Risk if done now.** Low. The risk is in *not* doing it: the
quirk costs a full kotlinc invocation on the first rebuild after
every clean build, and it pollutes benchmark readings (the warm
explain at N=15 was measured in the pre-probe state). Every
future measurement inherits the noise until this is fixed.

**Recommendation: do next — first in the batch (§10).** It is the
highest-value small item in this document: a measured, recurring
cost with a known diagnosis path, on the hello fixture, with no
new infrastructure.

## 10. Prioritized recommendation for the next step overall

In order. Each item is one step; do not batch the first two with
the third — the third changes the module fingerprint shape, and
the first two must be green before it so a regression has nowhere
to hide.

1. **Compile-code re-run quirk (§9e) + the small-follow-ups batch
   (§9a, §9b).** Diagnose the quirk on the hello fixture (compare
   fingerprint + classes validation before/after the probe build),
   fix the cause; fix the winning-branch E3021 gap behind a
   reproducing fixture; switch the probe-cache store to
   `ccb_write_file_atomic` with an interruption test. Three small
   root causes, three small fixes, one step. This is first because
   it removes a measured recurring cost and benchmark noise, and
   because all three are diagnosable now with fixtures in hand.
2. **Stage 6 (a)+(b) (§1).** Scope the module classpath and
   fingerprint to the module's upstream set; return the task-owned
   delta and artifact from `ccb_build_module_at` and merge both in
   the driver at the join, in topological order. Gate on the
   existing byte-identical rebuild tests. This is second because it
   is the determinism prerequisite for everything parallel, and it
   fixes a latent sequential defect (fingerprints that depend on
   completion order) rather than adding a feature.
3. **Stage 6 (c)–(f) + equivalence test (§1).** Per-task log/diag
   lists, driver-aggregated counters, one-time CRC init, opt-in
   driver wiring, and the sequential-vs-parallel byte-identical APK
   equivalence test against a new independent-modules fixture.
   Only after (a)+(b) are green.
4. **N=100 retry (§2), only if its trigger fires.** Phone memory
   pressure relieved + a fresh single-cold-run rate check, and
   only if the chained-100 question still matters after the
   independent-modules fixture exists.
5. **Deferred, in trigger order:** remote cache (§3) when a second
   consumer demonstrably repeats builds; custom task API (§4) when
   one concrete custom task exists; preview expansion (§8) when a
   verified critical need arrives with a scope note.
   `ccb_cache_delta` adoption (§9c) rides with item 2, not before.
6. **Dropped as standalone goals:** external providers (§6) and
   the AAB split (§7). Reopen only on a concrete diagnosed need.
   **Standing rule:** plugin isolation (§5) — never load arbitrary
   native C plugins into the main process by default; any future
   plugin API executes plugin code out-of-process.

What this order deliberately does *not* do: it does not optimize
toward the directive's cold <10 s / incremental <2 s targets. Both
CCB and Gradle miss those targets on every measurement, and Step 11
showed the gap is per-module compiler work (one JVM spawn per
module, O(N²) classpath), not scheduling overhead. Chasing the
targets before (a)+(b) and the quirk fix would be optimizing
against noisy numbers. Measure again after items 1–3, then decide
whether the targets are reachable at all — and by what.

## 11. What this step changed

- This document (`docs/advanced-features-reassessment.md`).
- `docs/remediation-baseline.md` §8: job completion record.
- `docs/index.md`: link to this document.
- `.github/workflows/ci.yml`: job display names "61 tests" →
  "62 tests" (both jobs), verified against `ctest -N` (62).
- `docs/build-tests.md`: fast-suite "47 tests" → "51 tests",
  full-suite "58 tests" → "62 tests", verified against `ctest -N`
  and `ctest -N -L fast`. The historical "(2026-11-25, 47 tests)"
  run note is untouched — that count was true for that run.

No source code changed. No build behavior changed. No feature was
implemented, wired, or optimized. Because `.github/workflows/ci.yml`
is not in the CI `paths-ignore` list, this commit *does* trigger a
CI run (unlike the docs-only steps); see the final report for its
number and conclusion.

## 12. Limitations

- Every recommendation above rests on the Step 1–11 evidence as
  recorded; the re-verification in this step covered the cited
  file-and-line claims, not a re-run of the benchmarks or the full
  suite. The full suite runs in CI on this commit.
- The compile-code quirk (§9e) root cause is unknown — the
  diagnosis path is stated, the cause is not claimed.
- The N=100 memory-pressure cause is "most plausible, not proven"
  (Step 11's own wording); this document does not upgrade it.
- `requirement.md:861` (55-test suite) is still stale; updating a
  spec line is left to a spec change.
- Preview's size (~40% of `src/`) is reported as a maintenance-cost
  fact, not judged: whether it should be trimmed is a user
  decision this step does not make.
