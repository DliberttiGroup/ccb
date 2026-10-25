# Diagnostic codes

Every failure carries a stable code. Codes are grouped by phase; the
exit code follows spec §6.4 (0 success, 1 build failure, 2 config
error, 3 toolchain missing/wrong version, 4 signing error, 5 device
error).

## Config (exit 2)

| Code | Meaning |
| ---- | ------- |
| CCB-E1001 | Unknown key in `ccb.toml` (hard error; the key and table are named) |
| CCB-E1002 | Missing required `ccb_version` |
| CCB-E1003 | Unsupported `ccb_version` (only 1 is supported) |
| CCB-E1004 | Missing required key |
| CCB-E1005 | Invalid value (bad variant, SDK range, min > target, manifest package mismatch, minify on debug, …) |
| CCB-E1006 | Declared path does not exist |
| CCB-E1007 | Wrong value type for a key |
| CCB-E1008 | TOML parse error (with line number) |
| CCB-E1009 | Config file not found |
| CCB-E1010 | Module dependency cycle (names the cycle path, e.g. `a -> b -> a`) |
| CCB-E1011 | Module depends on an unknown module (names the module and the dependency) |

`ccb check` reports every config problem at once, not just the first.

## Toolchain (exit 3)

| Code | Meaning |
| ---- | ------- |
| CCB-E2001 | Android SDK not found (set `[sdk] home` or `ANDROID_HOME`) |
| CCB-E2002 | Requested build-tools version not installed |
| CCB-E2003 | Requested platform `android.jar` not found |
| CCB-E2004 | kotlinc not found (set `[sdk] kotlin_home` or `KOTLIN_HOME`) |
| CCB-E2005 | java missing or older than 17 |
| CCB-E2006 | kotlinc is not 2.x |
| CCB-E2007 | NDK not installed for the requested version |
| CCB-E2008 | NDK revision mismatch |
| CCB-E2009 | A required tool binary is missing or not executable. Also covers a broken explicit tool override: a `[sdk] aapt2` / `[sdk] zipalign` key or a `CCB_AAPT2` / `CCB_ZIPALIGN` env var pointing at a missing or non-executable path fails with this code, and the message names the override source (e.g. `[sdk] aapt2`, `CCB_ZIPALIGN`) and the path. No silent fallback to the default tool. |

## Build (exit 1 unless noted)

| Code | Meaning |
| ---- | ------- |
| CCB-E3001 | Release signing pre-check failed (exit 4) |
| CCB-E3002 | aapt2 compile failed |
| CCB-E3003 | aapt2 link failed |
| CCB-E3004 | kotlinc failed |
| CCB-E3005 | javac failed |
| CCB-E3006 | d8 failed |
| CCB-E3007 | APK packaging failed (includes duplicate native libraries) |
| CCB-E3008 | zipalign failed |
| CCB-E3009 | Signing failed (exit 4) |
| CCB-E3010 | apksigner verify failed |
| CCB-E3011 | aapt2 dump badging failed |
| CCB-E3012 | NDK compile failed |
| CCB-E3013 | NDK link failed |
| CCB-E3014 | r8 requested but no `[sdk] r8_jar` set (release + minify) |
| CCB-E3015 | AAR extraction failed |
| CCB-E3016 | Cannot create or write a build directory or file |
| CCB-E3017 | r8 failed |
| CCB-E3018 | Malformed manifest during merge (names the AAR or the app manifest) |
| CCB-E3019 | Maven resolution failed: unresolvable coordinate, malformed or unsupported POM, or artifact not found in any configured repository |
| CCB-E3020 | Maven checksum mismatch: downloaded or cached file does not match its published or recorded checksum |
| CCB-E3021 | Maven artifact not in the local cache; run `ccb setup` to prefetch it (builds never download) |
| CCB-E3022 | Library module name collides with a declared AAR's base name (their extraction paths would overwrite each other) |
| CCB-E3023 | Maven lockfile mismatch: the resolved tree deviates from `ccb.lock` (version, packaging, hash, missing, or stale entry), or the lockfile is malformed; run `ccb setup` to regenerate it |
| CCB-E3024 | Maven parent POM chain error: a cycle in the chain, or more than 10 ancestors, naming the chain |

## Preview (exit 1 unless noted)

| Code | Meaning |
| ---- | ------- |
| CCB-P1001 | Unknown CSS property (warning; error under `--strict` / `[preview] strict`) |
| CCB-P1002 | CSS parse error, partial parse kept (warning) |
| CCB-P1003 | Overlapping boxes (warning) |
| CCB-P1004 | Box extends past the interior bounds (warning) |
| CCB-P1005 | Navigation to an unknown screen (error) |
| CCB-P1006 | Unreachable screen, nothing navigates to it (warning) |
| CCB-P1007 | Empty screen, no components (warning) |
| CCB-P1008 | Duplicate screen name, 2nd and later occurrences (error) |
| CCB-P1009 | Text clipped to its box (warning) |
| CCB-P1010 | Flow parse error, partial parse kept (warning) |
| CCB-P1011 | No screens found (warning) |
| CCB-P1012 | Unknown Kotlin construct, kept as a generic box (warning) |
| CCB-P1013 | Frame width clamped to the minimum of 24 columns (warning) |
| CCB-P1014 | CSS value outside the subset, e.g. unsupported unit or display value (warning; error under `--strict` / `[preview] strict`) |
| CCB-P1015 | CSS file ignored: neither `common.css` nor a `<Screen>.css` file for a screen in the project (warning) |
| CCB-P1016 | Unresolved `@string/` or `@color/` reference, or a malformed values entry / non-hex color value skipped (warning; literal kept) |
| CCB-P1017 | Tap target too small: `button` / `textfield` under 8 columns or 2 rows, wireframe cells; message also reports the size in dp (warning, always) |
| CCB-P1018 | Text contrast below threshold: WCAG ratio under 4.5:1, or 3.0:1 at `font-size` ≥ 24px (warning, always) |
| CCB-P1019 | Handler found, button not bound: a click handler whose receiver has no `[preview.bindings]` entry (warning, always; no edge recorded) |
| CCB-P1020 | Declared navigation disagrees with a derived edge for the same button; the declared edge stands, the derived edge is dropped (warning, always) |
| CCB-P1021 | XML `android:onClick` button not found in any screen: the bound component label appears in no screen's component tree, so its derived edges have nowhere to attach (warning, always; no edge recorded) |
| CCB-P1022 | Scenario entry does not match the project: a scenario names an unknown screen, branch entry, list, or field; a list that already has static rows; a button label matching no edge; or a button whose edges have no unknown argument to fill. The entry is skipped, rendering continues (warning, always; a scenario never fails the build gate) |
| CCB-P1023 | Trace step does not match the project: a trace names an unknown screen; an edge label matching no edge in the screen; a branch entry that does not exist in the screen; a step naming neither an edge nor a branch; or a branch entry that contains no component with the step's button label (the edge is still marked taken). The step (or its unmatched part) is skipped, rendering continues (warning, always; a trace never fails the build gate) |

Preview warnings exit 0; only CCB-P1005, CCB-P1008, and (under
`--strict`) CCB-P1001/CCB-P1014 exit 1. CCB-P1016/P1017/P1018/P1019/P1020/P1021/P1022/P1023 are
warnings even under `--strict`, so they never fail the build gate.
A project with no usable
`[preview]` section is not a diagnostic at all: `ccb preview` prints
"preview not applicable to this project" and exits 0.

## Device (exit 5)

| Code | Meaning |
| ---- | ------- |
| CCB-E5001 | adb command failed or no device connected (see docs/device-setup.md) |
| CCB-E5002 | No APK to install (run `ccb build` first) |

Tool failures include the exact command that failed and the tail of
its stderr. Keystore passwords are never included: release signing
passes them to apksigner as `env:VAR` references only.
