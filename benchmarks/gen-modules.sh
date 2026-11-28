#!/bin/sh
# gen-modules.sh: generate a synthetic N-module CCB project for scaling
# benchmarks (Step 10). App + N library modules in a dependency chain:
# mod00 <- mod01 <- ... <- mod(N-1); the app references the last module.
# Each module has one manifest, one Kotlin helper, and one string
# resource, mirroring the test_modules.c fixture shape. Deterministic:
# re-running with the same N regenerates identical sources.
# Usage: benchmarks/gen-modules.sh <count> [out-dir]
# Default out-dir: /tmp/ccb-bench-mod<N>.
# Counts 1..100; module names are zero-padded (mod00..mod99).

set -e
N=$1
OUT=${2:-"/tmp/ccb-bench-mod$N"}

case "$N" in
  ''|*[!0-9]*) echo "usage: gen-modules.sh <count 1..100> [out-dir]" >&2; exit 2 ;;
esac
if [ "$N" -lt 1 ] || [ "$N" -gt 100 ]; then
  echo "count must be 1..100" >&2
  exit 2
fi

rm -rf "$OUT"
mkdir -p "$OUT/app/src/main/kotlin/com/example/hello"
mkdir -p "$OUT/app/src/main/res/values"
mkdir -p "$OUT/app/src/main/res/layout"

LAST=$(printf "mod%02d" $((N - 1)))
LAST_PKG=$(printf "com.example.mod%02d" $((N - 1)))

cat > "$OUT/ccb.toml" <<EOF
ccb_version = 1

[project]
name = "modbench"
package = "com.example.hello"
version = "1.0"
version_code = 1
min_sdk = 26
target_sdk = 35
compile_sdk = 35

[paths]
manifest = "app/src/main/AndroidManifest.xml"
res = "app/src/main/res"
kotlin = "app/src/main/kotlin"
EOF

i=0
while [ "$i" -lt "$N" ]; do
  MOD=$(printf "mod%02d" "$i")
  PKG=$(printf "com.example.mod%02d" "$i")
  mkdir -p "$OUT/lib$MOD/src/main/kotlin/com/example/$MOD"
  mkdir -p "$OUT/lib$MOD/src/main/res/values"
  cat >> "$OUT/ccb.toml" <<EOF

[modules.$MOD]
dir = "lib$MOD"
EOF
  if [ "$i" -gt 0 ]; then
    PREV=$(printf "mod%02d" $((i - 1)))
    cat >> "$OUT/ccb.toml" <<EOF
depends = ["$PREV"]
EOF
  fi
  cat > "$OUT/lib$MOD/src/main/AndroidManifest.xml" <<EOF
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
  package="$PKG">
</manifest>
EOF
  cat > "$OUT/lib$MOD/src/main/kotlin/com/example/$MOD/Helper.kt" <<EOF
package com.example.$MOD

object Helper {
  fun greeting(): String {
    return "$MOD"
  }
}
EOF
  cat > "$OUT/lib$MOD/src/main/res/values/strings.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<resources>
  <string name="${MOD}_text">from $MOD</string>
</resources>
EOF
  i=$((i + 1))
done

cat > "$OUT/app/src/main/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
  package="com.example.hello">
  <application android:label="@string/app_name" android:theme="@style/AppTheme">
    <activity android:name=".MainActivity" android:exported="true">
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
    </activity>
  </application>
</manifest>
EOF

cat > "$OUT/app/src/main/kotlin/com/example/hello/MainActivity.kt" <<EOF
package com.example.hello

import android.app.Activity
import android.os.Bundle
import $LAST_PKG.Helper

class MainActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_main)
    val unused = Helper.greeting()
  }
}
EOF

cat > "$OUT/app/src/main/res/values/strings.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<resources>
  <string name="app_name">modbench</string>
  <string name="hello">Hello from CCB!</string>
</resources>
EOF

cat > "$OUT/app/src/main/res/layout/activity_main.xml" <<EOF
<LinearLayout xmlns:android="http://schemas.android.com/apk/res/android"
  android:layout_width="match_parent"
  android:layout_height="match_parent"
  android:gravity="center"
  android:orientation="vertical">
  <TextView android:layout_width="wrap_content"
    android:layout_height="wrap_content"
    android:text="@string/hello" />
</LinearLayout>
EOF

cat > "$OUT/app/src/main/res/values/themes.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<resources>
  <style name="AppTheme" parent="@android:style/Theme.Material.Light.NoActionBar" />
</resources>
EOF

echo "generated $N-module project at $OUT"
