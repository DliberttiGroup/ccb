#!/bin/sh
# bench-builds.sh: reproducible build-time baseline for CCB (Phase 0).
# Measures cold, warm, no-op, and incremental builds for the hello
# example and the Showcase app, 3 runs each, median reported.
# Usage: benchmarks/bench-builds.sh [all|hello|showcase] [results-file]
# Env overrides: CCB (ccb binary), SHOWCASE (showcase project dir),
# JAVA_HOME / ANDROID_HOME / KOTLIN_HOME (toolchain; defaults are the
# VM paths below, used only when the variable is unset).
# Definitions:
#   cold:        `ccb clean` + global stdlib dex cache emptied, then build.
#   warm:        `ccb clean` only (global caches intact), then build.
#   no-op:       immediate rebuild with no changes.
#   incremental: one comment line appended to the main .kt file, then
#                build; the file is restored afterwards (never measured).
# Run order per project is cold, warm, no-op, incremental: no-op relies
# on the warm build having just run, incremental on the no-op state.
# Times are wall-clock milliseconds from `date +%s%N`; medians are the
# middle value of the 3 runs. `--explain` run/skip counts are recorded
# once per mode, after that mode's last run.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CCB=${CCB:-"$ROOT/build/ccb"}
SHOWCASE=${SHOWCASE:-"$HOME/workspace/ccb-showcase"}
DEX_CACHE="${XDG_CACHE_HOME:-"$HOME/.cache"}/ccb/dex-stdlib"
OUT=${2:-"$ROOT/benchmarks/results-$(date +%Y%m%d-%H%M).txt"}

[ -z "$JAVA_HOME" ] && [ -d "$HOME/jdk17" ] && JAVA_HOME="$HOME/jdk17"
[ -z "$ANDROID_HOME" ] && [ -d "$HOME/android-sdk" ] && ANDROID_HOME="$HOME/android-sdk"
[ -z "$KOTLIN_HOME" ] && [ -d "$HOME/kotlin-2.4.21/kotlinc" ] && KOTLIN_HOME="$HOME/kotlin-2.4.21/kotlinc"
export JAVA_HOME ANDROID_HOME KOTLIN_HOME
PATH="$JAVA_HOME/bin:$PATH"
export PATH

log() { echo "$@" | tee -a "$OUT"; }

median_of_3() {
  # stdin: 3 integers, one per line. Prints the middle value.
  sort -n | sed -n '2p'
}

time_build() {
  # $1 = project dir. Prints wall ms on stdout; 0/1 exit status.
  dir=$1
  start=$(date +%s%N)
  (cd "$dir" && "$CCB" build > /tmp/ccb-bench-last.log 2>&1)
  rc=$?
  end=$(date +%s%N)
  echo $(( (end - start) / 1000000 ))
  return $rc
}

explain_counts() {
  # $1 = project dir. Prints "run=N skip=M" from --explain.
  (cd "$1" && "$CCB" build --explain 2>/dev/null \
    | awk '/: run / {r++} /: skip / {s++} END {printf "run=%d skip=%d", r+0, s+0}')
}

run_set() {
  # $1 = project dir, $2 = mode, $3 = .kt file to touch (incremental only).
  dir=$1
  mode=$2
  kt=$3
  runs=""
  i=1
  while [ "$i" -le 3 ]; do
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
    ms=$(time_build "$dir")
    rc=$?
    if [ "$mode" = "incremental" ]; then
      mv "$kt.bench-bak" "$kt"
    fi
    if [ "$rc" -ne 0 ]; then
      log "  $mode run $i: BUILD FAILED (see /tmp/ccb-bench-last.log)"
      return 1
    fi
    log "  $mode run $i: ${ms} ms"
    runs="$runs
$ms"
    i=$((i + 1))
  done
  med=$(printf '%s\n' "$runs" | grep -v '^$' | median_of_3)
  counts=$(explain_counts "$dir")
  log "  $mode median: ${med} ms ($counts)"
}

main() {
  which=$1
  {
    echo "CCB build benchmark baseline"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "machine: shared VM, 1.5 CPUs (cgroup limit; nproc reports $(nproc)), $(free -m | awk '/^Mem:/ {print $2}') MiB RAM"
    echo "ccb: $($CCB version 2>&1 | head -1) ($CCB)"
    echo "java: $(java -version 2>&1 | head -1)"
    echo "kotlinc: $( "$KOTLIN_HOME/bin/kotlinc" -version 2>&1 | head -1)"
    echo "dex-stdlib cache dir: $DEX_CACHE"
    echo ""
  } | tee "$OUT"
  if [ "$which" = "all" ] || [ "$which" = "hello" ]; then
    log "project: hello ($ROOT/examples/hello)"
    run_set "$ROOT/examples/hello" cold
    run_set "$ROOT/examples/hello" warm
    run_set "$ROOT/examples/hello" noop
    run_set "$ROOT/examples/hello" incremental \
      "$ROOT/examples/hello/src/main/kotlin/com/example/hello/MainActivity.kt"
    log ""
  fi
  if [ "$which" = "all" ] || [ "$which" = "showcase" ]; then
    log "project: showcase ($SHOWCASE)"
    run_set "$SHOWCASE" cold
    run_set "$SHOWCASE" warm
    run_set "$SHOWCASE" noop
    run_set "$SHOWCASE" incremental \
      "$SHOWCASE/src/main/kotlin/com/ccb/showcase/MainActivity.kt"
    log ""
  fi
  log "done."
}

main "${1:-all}"
