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

Targets from the directive (cold <10 s, incremental <2 s) are
still missed by both CCB and Gradle on every measurement above.
Nothing in this step optimizes toward them: measure and document
only; optimization is a later decision.
