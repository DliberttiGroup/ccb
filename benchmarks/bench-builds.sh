#!/bin/sh
# bench-builds.sh: reproducible build-time baseline for CCB (Phase 0,
# extended in Step 10 with resource metrics, cache-hit rates,
# dependency-resolution cost, and synthetic module scaling).
# Measures cold, warm, no-op, and incremental builds, 3 runs each,
# median reported. See benchmarks/README.md for methodology.
# Usage:
#   benchmarks/bench-builds.sh [all|hello|showcase] [results-file]
#   benchmarks/bench-builds.sh modules <N> [results-file]
# Env overrides: CCB (ccb binary), SHOWCASE (showcase project dir),
# BENCH_RUNS (runs per mode, default 3), JAVA_HOME / ANDROID_HOME /
# KOTLIN_HOME (toolchain; defaults are the VM paths below, used only
# when the variable is unset).
# Definitions:
#   cold:        `ccb clean` + global stdlib dex cache emptied, then build.
#   warm:        `ccb clean` only (global caches intact), then build.
#   no-op:       immediate rebuild with no changes.
#   incremental: one comment line appended to the main .kt file, then
#                build; the file is restored afterwards (never measured).
#   setup:       one `ccb setup` run (dependency-resolution cost).
#                Reported, not gated on. Even with no Maven deps this
#                costs seconds on the VM (measured 3.4 s hello,
#                6.9 s modules-5); the number is the cost, not a no-op.
# Run order per project is setup, cold, warm, no-op, incremental:
# no-op relies on the warm build having just run, incremental on the
# no-op state. Times are wall-clock milliseconds; CPU time (user+sys)
# and peak child RSS come from benchmarks/measure.py when python3 is
# available (see that file for what maxrss means); otherwise wall
# time only, from `date +%s%N`. Medians are the middle value of the
# runs. `--explain` run/skip counts are recorded once per mode, after
# that mode's last run, with the cache-hit rate skip/(run+skip).

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CCB=${CCB:-"$ROOT/build/ccb"}
SHOWCASE=${SHOWCASE:-"$HOME/workspace/ccb-showcase"}
DEX_CACHE="${XDG_CACHE_HOME:-"$HOME/.cache"}/ccb/dex-stdlib"
RUNS=${BENCH_RUNS:-3}
MEASURE="$ROOT/benchmarks/measure.py"
# Scratch dir for per-run build logs and generated module projects.
# /tmp is not writable everywhere this runs (Termux): honor TMPDIR.
BENCH_TMP="${TMPDIR:-/tmp}"
LAST_LOG="$BENCH_TMP/ccb-bench-last.log"
SETUP_LOG="$BENCH_TMP/ccb-bench-setup.log"
OUT=${2:-"$ROOT/benchmarks/results-$(date +%Y%m%d-%H%M).txt"}
# modules mode: $2 is the module count, $3 the results file.
if [ "$1" = "modules" ]; then
  OUT=${3:-"$ROOT/benchmarks/results-$(date +%Y%m%d-%H%M).txt"}
fi

[ -z "$JAVA_HOME" ] && [ -d "$HOME/jdk17" ] && JAVA_HOME="$HOME/jdk17"
[ -z "$ANDROID_HOME" ] && [ -d "$HOME/android-sdk" ] && ANDROID_HOME="$HOME/android-sdk"
[ -z "$KOTLIN_HOME" ] && [ -d "$HOME/kotlin-2.4.21/kotlinc" ] && KOTLIN_HOME="$HOME/kotlin-2.4.21/kotlinc"
export JAVA_HOME ANDROID_HOME KOTLIN_HOME
PATH="$JAVA_HOME/bin:$PATH"
export PATH

log() { echo "$@" | tee -a "$OUT"; }

median_of() {
  # stdin: integers, one per line. Prints the middle value (lower
  # middle for an even count).
  sort -n | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }'
}

max_of() {
  # stdin: integers, one per line. Prints the largest.
  sort -n | tail -1
}

use_measure() {
  command -v python3 > /dev/null 2>&1 && [ -f "$MEASURE" ]
}

time_build() {
  # $1 = project dir. Prints "wall cpu maxrss" (ms, ms, MiB) on
  # stdout; exit status is the build's. Build output goes to
  # $LAST_LOG either way.
  dir=$1
  if use_measure; then
    m=$(cd "$dir" && python3 "$MEASURE" --log "$LAST_LOG" -- "$CCB" build)
    rc=$?
    wall=$(printf '%s' "$m" | sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p')
    user=$(printf '%s' "$m" | sed -n 's/.*user_ms=\([0-9]*\).*/\1/p')
    sys=$(printf '%s' "$m" | sed -n 's/.*sys_ms=\([0-9]*\).*/\1/p')
    rss=$(printf '%s' "$m" | sed -n 's/.*maxrss_mib=\([0-9]*\).*/\1/p')
    echo "${wall:-0} $(( ${user:-0} + ${sys:-0} )) ${rss:-0}"
    return $rc
  fi
  start=$(date +%s%N)
  (cd "$dir" && "$CCB" build > "$LAST_LOG" 2>&1)
  rc=$?
  end=$(date +%s%N)
  echo "$(( (end - start) / 1000000 )) 0 0"
  return $rc
}

explain_counts() {
  # $1 = project dir. Prints "run=N skip=M hit=P%" from --explain.
  (cd "$1" && "$CCB" build --explain 2>/dev/null \
    | awk '/: run / {r++} /: skip / {s++} END {t=r+s; if (t>0) printf "run=%d skip=%d hit=%d%%", r+0, s+0, s*100/t; else printf "run=%d skip=%d hit=n/a", r+0, s+0}')
}

time_setup() {
  # $1 = project dir. One `ccb setup` run: dependency-resolution
  # cost. Informational only: a failure is logged, not fatal.
  dir=$1
  if use_measure; then
    m=$(cd "$dir" && python3 "$MEASURE" --log "$SETUP_LOG" -- "$CCB" setup)
    rc=$?
    wall=$(printf '%s' "$m" | sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p')
  else
    start=$(date +%s%N)
    (cd "$dir" && "$CCB" setup > "$SETUP_LOG" 2>&1)
    rc=$?
    end=$(date +%s%N)
    wall=$(( (end - start) / 1000000 ))
  fi
  if [ "$rc" -ne 0 ]; then
    log "  setup: FAILED (rc=$rc, see $SETUP_LOG)"
  else
    log "  setup: ${wall} ms (dependency resolution)"
  fi
}

run_set() {
  # $1 = project dir, $2 = mode, $3 = .kt file to touch (incremental only).
  dir=$1
  mode=$2
  kt=$3
  walls=""
  cpus=""
  rsses=""
  i=1
  while [ "$i" -le "$RUNS" ]; do
    case "$mode" in
      cold)
        (cd "$dir" && "$CCB" clean > /dev/null 2>&1)
        rm -rf "$DEX_CACHE"
        ;;
      warm)
        (cd "$dir" && "$CCB" clean > /dev/null 2>&1)
        ;;
      noop)
        ;;
      incremental)
        cp "$kt" "$kt.bench-bak"
        printf '// bench-touch run %s\n' "$i" >> "$kt"
        ;;
    esac
    m=$(time_build "$dir")
    rc=$?
    if [ "$mode" = "incremental" ]; then
      mv "$kt.bench-bak" "$kt"
    fi
    if [ "$rc" -ne 0 ]; then
      log "  $mode run $i: BUILD FAILED (see $LAST_LOG)"
      return 1
    fi
    set -- $m
    log "  $mode run $i: $1 ms (cpu $2 ms, maxrss $3 MiB)"
    walls="$walls
$1"
    cpus="$cpus
$2"
    rsses="$rsses
$3"
    i=$((i + 1))
  done
  med=$(printf '%s\n' "$walls" | grep -v '^$' | median_of)
  med_cpu=$(printf '%s\n' "$cpus" | grep -v '^$' | median_of)
  max_rss=$(printf '%s\n' "$rsses" | grep -v '^$' | max_of)
  counts=$(explain_counts "$dir")
  log "  $mode median: ${med} ms (cpu ${med_cpu} ms, maxrss ${max_rss} MiB; $counts)"
}

bench_project() {
  # $1 = label, $2 = project dir, $3 = main .kt file (incremental).
  log "project: $1 ($2)"
  time_setup "$2"
  run_set "$2" cold
  run_set "$2" warm
  run_set "$2" noop
  run_set "$2" incremental "$3"
  log ""
}

main() {
  {
    echo "CCB build benchmark baseline"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    # Portable machine line: arch + nproc + RAM everywhere; the
    # cgroup cpu.max quota when one applies (the shared VM's 1.5
    # CPUs); the Android device model when getprop has one (phone).
    echo "machine: $(uname -m), $(nproc) CPUs$(awk '$1 != "max" {printf " (cgroup cpu.max %s/%s)", $1, $2}' /sys/fs/cgroup/cpu.max 2>/dev/null), $(free -m | awk '/^Mem:/ {print $2}') MiB RAM"
    model=$(getprop ro.product.model 2>/dev/null)
    [ -n "$model" ] && echo "device: $model"
    echo "ccb: $($CCB version 2>&1 | head -1) ($CCB)"
    echo "java: $(java -version 2>&1 | head -1)"
    # kotlinc prints its version on stderr and, on Termux, a
    # non-fatal jansi warning first: grep the version line itself.
    echo "kotlinc: $( "$KOTLIN_HOME/bin/kotlinc" -version 2>&1 | grep -m1 'kotlinc' || echo 'version output unavailable')"
    echo "dex-stdlib cache dir: $DEX_CACHE"
    echo "runs per mode: $RUNS (BENCH_RUNS)"
    if use_measure; then
      echo "measure: python3 $MEASURE (wall + cpu + maxrss per run)"
    else
      echo "measure: date only (python3 or measure.py missing; no cpu/rss)"
    fi
    echo ""
  } | tee "$OUT"
  if [ "$1" = "modules" ]; then
    n=$2
    moddir="$BENCH_TMP/ccb-bench-mod$n"
    "$ROOT/benchmarks/gen-modules.sh" "$n" "$moddir" || return 1
    bench_project "modules-$n" "$moddir" \
      "$moddir/app/src/main/kotlin/com/example/hello/MainActivity.kt"
    log "done."
    return 0
  fi
  if [ "$1" = "all" ] || [ "$1" = "hello" ]; then
    bench_project "hello" "$ROOT/examples/hello" \
      "$ROOT/examples/hello/src/main/kotlin/com/example/hello/MainActivity.kt"
  fi
  if [ "$1" = "all" ] || [ "$1" = "showcase" ]; then
    bench_project "showcase" "$SHOWCASE" \
      "$SHOWCASE/src/main/kotlin/com/ccb/showcase/MainActivity.kt"
  fi
  log "done."
}

main "${1:-all}" "$2" "$3"
