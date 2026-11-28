# Benchmarks

Methodology, environment, and how to read the results. The
reproducibility contract these benchmarks support is
[docs/reproducibility.md](../docs/reproducibility.md).

## Scripts

| Script | What it does |
| --- | --- |
| `bench-builds.sh` | Cold / warm / no-op / incremental builds for hello, showcase, or a synthetic N-module project. Medians of N runs (default 3). |
| `gen-modules.sh` | Generates a synthetic N-module project (app + N library modules in a dependency chain). |
| `check-repro.sh` | Two clean builds of one project; PASS/FAIL on APK bytes, cache-manifest bytes, and APK entry order. |
| `measure.py` | Runs one command and reports wall time, CPU time (user+sys), and peak child RSS. Used by `bench-builds.sh`. |

## Modes

- **cold**: `ccb clean` + global stdlib dex cache emptied, then build.
- **warm**: `ccb clean` only (global caches intact), then build.
- **no-op**: immediate rebuild with no changes.
- **incremental**: one comment line appended to the main `.kt` file,
  then build; the file is restored afterwards (never measured).
- **setup**: one `ccb setup` run before the build modes, reporting
  dependency-resolution cost. Informational only: a failure is
  logged, not fatal. Projects with no Maven deps report a fast
  no-op here.

Run order per project is setup, cold, warm, no-op, incremental:
no-op relies on the warm build having just run, incremental on the
no-op state.

## Metrics (per run, per mode)

- **wall ms**: wall-clock time of the build.
- **cpu ms**: user + sys CPU time of the build process tree
  (`resource.getrusage(RUSAGE_CHILDREN)`). A build that spends
  most of its wall time waiting on a daemon or the network shows
  low CPU against high wall time; that split is the point of
  recording both.
- **maxrss MiB**: peak resident set of the single largest child
  process (the kernel reports a maximum, not a sum). For a CCB
  build that is usually kotlinc or d8, not the `ccb` process
  itself. Treat it as a lower bound on total memory pressure, not
  a whole-build total. Daemon processes that outlive the build
  (kotlinc daemon) are not counted.
- **run/skip + hit%**: `--explain` task counts recorded after the
  mode's last run. They describe what the *next* build from that
  state would do, not the mode's own execution: after any completed
  build the next plan is mostly skips (hello: 1 run / 12 skip),
  which validates that the cache is populated. After incremental
  the file is restored, so the next plan reruns the tasks the
  restore invalidates (hello: 7 run / 6 skip) — that is the
  restore working, not a cache miss. Do not read these as the
  mode's own hit rate.
- **median**: the middle value of the runs (lower middle for an
  even count). **maxrss** on the median line is the maximum across
  runs, not the median: peak memory is a worst case, not a
  typical case.

Per-mode lines report the median wall, median CPU, max RSS, and
the run/skip counts. `BENCH_RUNS` overrides the runs per mode
(default 3); use `BENCH_RUNS=1` for a smoke run only, never to
quote a number.

## Module scaling

`bench-builds.sh modules <N> [results-file]` generates a synthetic
project with N library modules (1..100) via `gen-modules.sh` and
benches it like a real project. The generator is deterministic:
re-running with the same N regenerates identical sources. The
modules form a dependency chain (`mod00 <- mod01 <- ...`), each
with one manifest, one Kotlin helper, and one string resource,
mirroring the `test_modules.c` fixture shape; the app references
the last module. The target matrix is 5 / 15 / 25 / 50 / 100
modules. Only small N is cheap on the shared VM (see below);
larger N belongs on the phone (6 CPUs, quieter) or in Step 11
scaling validation. No scalability claim is made without a run
at that N: the matrix below is the record, not a projection.

## Environment and noise

Every results file starts with a header: UTC date, machine (CPU
cgroup limit + `nproc`, RAM), `ccb version`, java and kotlinc
versions, the dex-stdlib cache dir, runs per mode, and whether
resource measurement was available. Quote a number only with its
header attached.

The shared VM (1.5 CPUs) is heavily contended, so absolute numbers
are noisy: consecutive identical runs vary by 2x or more (see the
Phase 0 baselines, where hello cold ranged 71–177 s across runs).
What the VM numbers support is order-of-magnitude comparison and
regression spotting across runs on the same machine, not
millisecond claims and not cross-machine comparison. The phone
(Pixel 7 Pro, 6 CPUs) is the quieter reference: phone results
live alongside these files and carry their own headers.

Benchmark runs are slow and run sequentially: never run a benchmark
concurrently with another build or test on the shared VM.

## Result files

Results are written to `benchmarks/results-<YYYYMMDD-HHMM>.txt`
by default, or to the path passed as the last argument. Result
files are untracked and are not committed: they are evidence for a
specific run, not source. The files already here are the record
(see below); a new run adds a new file, it never overwrites an
old one.

## Results so far

- `results-20261127.txt` (Phase 0, VM): hello cold median
  109,895 ms / warm 113,012 ms / no-op 5,518 ms / incremental
  79,293 ms; showcase cold 242,179 ms / warm 58,726 ms / no-op
  2,875 ms / incremental 36,674 ms. Wall time only; no CPU/RSS.
- `results-20261128-0014.txt` (VM): hello cold 91,093 ms / warm
  23,974 ms / no-op 1,484 ms / incremental 12,497 ms; showcase
  cold 60,042 ms / warm 30,948 ms / no-op 1,095 ms / incremental
  19,581 ms. Same caveat: the warm/incremental spread between
  the two VM runs is contention noise, not a binary change.
- `results-ccb-phone-latest-20261127.txt` (phone, Pixel 7 Pro):
  two runs, Step 6 and Step 7 binaries; hello cold medians
  49,445 ms (run 1) with warm 43,583 ms / no-op 445 ms /
  incremental 30,298 ms. Run 2's cold/warm first runs are
  extreme outliers (209.6 s / 169.2 s) under sustained load,
  likely thermal; see the caveats in that file.
- `results-gradle-phone-20261127.txt` (phone, Gradle 8.9 / AGP
  8.7.3 reference): hello cold 145,050 ms / warm 7,057 ms /
  no-op 2,924 ms / incremental 4,227 ms.
- Step 10 runs (this step, VM, new format with CPU/RSS/hit%):
  `results-20261128-step10-hello.txt` — hello: setup 3,448 ms;
  cold median 50,499 ms (cpu 67,657 ms, maxrss 537 MiB); warm
  21,318 ms (cpu 29,102 ms, maxrss 534 MiB); no-op 986 ms (cpu
  985 ms, maxrss 11 MiB); incremental 11,599 ms (cpu 16,145 ms,
  maxrss 528 MiB). Post-mode explain: 1 run / 12 skip after
  cold/warm/no-op, 7 run / 6 skip after incremental (restored
  file; see Metrics). `results-20261128-step10-mod5.txt` —
  synthetic 5-module project: setup 6,909 ms; cold 85,606 ms
  (cpu 117,185 ms, maxrss 532 MiB); warm 58,378 ms; no-op 4,317
  ms; incremental 15,025 ms; 18 tasks (1 run / 17 skip
  post-mode). Deltas vs hello (same VM, same day): cold +35 s,
  warm +37 s, no-op +3.3 s, incremental +3.4 s. The no-op delta
  is the scaling signal to watch: no-op cost grows with task
  count even when everything skips. 15/25/50/100 not run on the
  VM (each N=5 cold build is ~85 s; larger N belongs on the
  phone or in Step 11). Max RSS is flat (~520–540 MiB) in every
  JVM mode at both sizes: the largest single child (kotlinc/d8)
  dominates, not the module count.
- Step 11 runs (phone, Pixel 7 Pro, 6 CPUs, Step 10 binary at
  `build-step11/ccb`, medians of 3; files
  `results-step11-mod15-phone.txt`, `results-step11-mod25-phone.txt`,
  `results-step11-mod50-phone.txt`, `results-step11-mod100-phone.txt`):
  synthetic chained-module projects, one manifest + one Kotlin
  helper + one string resource per module, app referencing the
  last module. N=15: setup 10.0 s; cold 204.0 s (cpu 455.3 s,
  maxrss 553 MiB; 1 run / 27 skip, 96%); warm 187.7 s (cpu
  364.1 s, maxrss 538 MiB; 7 run / 21 skip, 75%); no-op 3.67 s;
  incremental 17.7 s (cpu 40.0 s, maxrss 543 MiB; 7 run / 21
  skip). N=25: setup 9.4 s; cold 296.8 s (cpu 656.9 s, maxrss
  549 MiB; 1 run / 37 skip, 97%); warm 302.7 s (cpu 610.7 s,
  maxrss 546 MiB; 1 run / 37 skip, 97%); no-op 4.19 s;
  incremental 20.9 s (cpu 44.6 s, maxrss 546 MiB; 7 run / 31
  skip, 81%). N=50: setup 8.8 s; cold 917.3 s (runs 917 / 1078 /
  775 s; cpu 1707.0 s, maxrss 539 MiB; 1 run / 62 skip, 98%);
  warm 660.0 s (runs 624 / 774 / 660 s; cpu 1374.4 s, maxrss
  539 MiB; 1 run / 62 skip, 98%); no-op 8.20 s (cpu 8.1 s,
  maxrss 12 MiB; 1 run / 62 skip, 98%); incremental 24.0 s
  (cpu 47.2 s, maxrss 541 MiB; 7 run / 56 skip, 88%). N=100:
  INFEASIBLE — stopped, no medians, no faked numbers (file
  `results-step11-mod100-phone.txt` is the partial record:
  header + setup 12,063 ms only; cold run 1 never finished).
  The bench started 01:09:31 PST 2026-11-28 and was stopped
  at ~02:16 after cold run 1 had built 26 of 100 modules in
  ~62 min: per-module times from the intermediates' mtimes
  run 58–225 s, mean ~150 s, with no recovery trend over the
  hour (mod00 01:16:22, mod12 01:44:47, mod24 02:14:39).
  At that sustained rate cold run 1 alone needs ~4 h and the
  full 12-run matrix ~12–15 h, which this session cannot run.
  For comparison the N=50 cold median implies ~18 s/module
  (917 s / 50) — per-module cost at N=100 is ~8x the N=50
  rate, on the same binary, phone, and harness. Measured
  conditions at the stop: swap 5.4 of 5.9 GiB used (92%),
  MemFree 666 MiB of 11,468 MiB, MemAvailable 2,688 MiB, slab
  1,191 MiB; no single process hogs memory (largest RSS is
  the kotlinc JVM at ~352 MiB) or CPU (kotlinc bursts to
  ~154% then stalls; load ~4–5; little/big core frequencies
  normal at 1.8 / 2.35 GHz; the build log kept advancing
  aapt2-compile → aapt2-link → kotlinc, so it was progressing,
  not hung). The memory pressure is the most plausible cause
  of the stall-heavy module times, but the deciding evidence
  is the measured rate itself, sustained over 24 modules —
  not a model. What N=100 needs before a retry: a phone with
  memory pressure relieved (reboot, or the swap hog
  identified) and a fresh rate check on a single cold run
  before committing to the matrix; if the ~150 s/module rate
  persists on a quiet phone, the bottleneck is in the build
  itself and N=100 stays infeasible for this harness.
- Step 11 scaling shape (cold medians): 15→25 adds 9.3 s per
  module (linear); the linear prediction at N=50 is ~529 s, but
  the measured median is 917 s — 73% above linear, i.e. the
  cost per module rises from ~9.3 s to ~18.3 s (917 s / 50).
  The N=50 cold spread (775–1078 s, 39%) and the N=25 warm
  median exceeding its cold median (302.7 s vs 296.8 s) are
  thermal drift under sustained load, not binary changes: runs
  are sequential, the phone throttles, and later runs in a
  session run slower. N=15 survived a VM restart mid-run (SSH
  drop reaped the script after warm run 3); it was resumed by
  hand in tmux and the warm median was reconstructed from the
  recorded warm runs, with explain counts re-measured in the
  unchanged post-warm-3 state — see the resume note in that
  file. The N=100 session was started with a `sleep 1800`
  cool-down, but the phone's suspend stretched it — the sleep
  was still running after 61 min elapsed. It was killed at
  ~01:09 and the bench started at once: the cool-down's purpose
  was already met in physical time (N=50 ended 00:00, the phone
  idle 69 min since, no heat generated), so no thermal confound
  was introduced. The stretched sleep is a phone-doze artifact,
  not a bench failure; an idle `sleep` must not be used as a
  cool-down timer on this phone again. No-op grows with task count (3.67 s at N=15, 4.19 s at
  N=25, 8.20 s at N=50 — roughly doubling from 25 to 50 while
  the task count goes 38 to 63); incremental likewise (17.7 s,
  20.9 s, 24.0 s). At N=50 warm (660.0 s) sits well below cold
  (917.3 s), the reverse of N=25 — both orderings are thermal
  drift, not a binary change: warm run 2 (774 s) was the
  slowest of the three warm runs.
  Max RSS is flat (~530–550 MiB) at every N: the largest
  single child (one kotlinc JVM, ~550 MiB peak) dominates, not
  the module count — memory is not the bottleneck. What the
  numbers support as the bottleneck, measured not guessed:
  per-module compile cost grows with N because each module's
  kotlinc invocation carries every upstream module's classes
  jar on its classpath (the N=50 run's process list shows the
  mod44 compile with 44 upstream jars plus android.jar), and
  the module classpath and fingerprint read the global AAR
  lists (`ccb_build_classpath`,
  `ccb_fp_add_aar_class_jars`; see docs/graph.md) — so both
  the compile itself and the fingerprinting are O(N) per
  module, O(N²) for the project, on top of one JVM spawn per
  module. CPU time grows with wall time (cpu/wall ≈ 2.2 at
  N=15–25, 1.9 at N=50), so the build is CPU-bound in the
  compilers, not waiting on I/O or a daemon. Two quirks
  recorded, not fixed here: after every clean build,
  compile-code re-runs once on the next build (a 22 s probe at
  N=15; fingerprint-vs-classes quirk — after that probe the
  explain counts flip from 7 run / 21 skip to 1 run / 27
  skip), and the warm explain at N=15 shows that 7-run state
  because it was measured before the probe. No parallel
  execution was used for any of these numbers: the build is
  fully sequential, and the chained-module fixture could not
  exercise concurrency anyway (at most one module task is ever
  READY there; see docs/graph.md).

Targets from the directive (cold <10 s, incremental <2 s) are
still missed by both CCB and Gradle on every measurement above.
Nothing in this step optimizes toward them: measure and document
only; optimization is a later decision.
