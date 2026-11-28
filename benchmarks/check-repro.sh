#!/bin/sh
# check-repro.sh: verify the Step 10 reproducibility contract
# (docs/reproducibility.md) empirically for one project: two clean
# builds on the same machine and toolchain must produce byte-identical
# outputs. Compares the signed APK, the cache manifest, and the APK's
# ZIP entry list (names in order). Usage:
#   benchmarks/check-repro.sh [project-dir]
# Default project: examples/hello. Exits 0 when everything matches,
# 1 otherwise. Slow: two clean builds (see benchmarks/README.md).
# Env overrides: CCB (ccb binary), as in bench-builds.sh.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CCB=${CCB:-"$ROOT/build/ccb"}
PROJ=${1:-"$ROOT/examples/hello"}

[ -z "$JAVA_HOME" ] && [ -d "$HOME/jdk17" ] && JAVA_HOME="$HOME/jdk17"
[ -z "$ANDROID_HOME" ] && [ -d "$HOME/android-sdk" ] && ANDROID_HOME="$HOME/android-sdk"
[ -z "$KOTLIN_HOME" ] && [ -d "$HOME/kotlin-2.4.21/kotlinc" ] && KOTLIN_HOME="$HOME/kotlin-2.4.21/kotlinc"
export JAVA_HOME ANDROID_HOME KOTLIN_HOME
PATH="$JAVA_HOME/bin:$PATH"
export PATH

APK="build/outputs/debug/app.apk"
CACHE="build/ccb-cache.json"
TMP=$(mktemp -d /tmp/ccb-repro-XXXXXX)
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "reproducibility check: $PROJ"
echo "ccb: $($CCB version 2>&1 | head -1) ($CCB)"
echo "java: $(java -version 2>&1 | head -1)"

zip_entries() {
  # $1 = apk path. Prints entry names in archive order, one per line.
  python3 -c 'import sys, zipfile; [print(i.filename) for i in zipfile.ZipFile(sys.argv[1]).infolist()]' "$1"
}

fail=0
i=1
while [ "$i" -le 2 ]; do
  (cd "$PROJ" && "$CCB" clean > /dev/null 2>&1)
  if ! (cd "$PROJ" && "$CCB" build > "$TMP/build$i.log" 2>&1); then
    echo "build $i: FAILED (see $TMP/build$i.log)"
    exit 1
  fi
  if [ ! -f "$PROJ/$APK" ]; then
    echo "build $i: no APK at $APK"
    exit 1
  fi
  sha256sum "$PROJ/$APK" | awk '{print $1}' > "$TMP/apk$i.sha"
  sha256sum "$PROJ/$CACHE" | awk '{print $1}' > "$TMP/cache$i.sha"
  zip_entries "$PROJ/$APK" > "$TMP/entries$i.txt"
  echo "build $i: apk=$(cat "$TMP/apk$i.sha" | cut -c1-16)... cache=$(cat "$TMP/cache$i.sha" | cut -c1-16)... entries=$(wc -l < "$TMP/entries$i.txt")"
  i=$((i + 1))
done

check() {
  # $1 = label, $2/$3 = files to compare.
  if cmp -s "$2" "$3"; then
    echo "PASS: $1 identical"
  else
    echo "FAIL: $1 differs"
    fail=1
  fi
}

check "APK bytes" "$TMP/apk1.sha" "$TMP/apk2.sha"
check "cache manifest bytes" "$TMP/cache1.sha" "$TMP/cache2.sha"
check "APK entry order" "$TMP/entries1.txt" "$TMP/entries2.txt"

if [ "$fail" -eq 0 ]; then
  echo "reproducibility: PASS (two clean builds byte-identical)"
else
  echo "reproducibility: FAIL (see above; keep $TMP for inspection)"
  trap - EXIT INT TERM
  echo "artifacts kept at $TMP"
fi
exit "$fail"
