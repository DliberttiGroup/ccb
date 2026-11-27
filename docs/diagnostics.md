# Diagnostic codes

[Docs index](index.md)

*ccb-diagnostics*

Every failure carries a stable code. Exit codes: 0 success, 1 build
failure, 2 config error, 3 toolchain missing/wrong version, 4 signing
error, 5 device error.

Tool failures include the exact command that failed and the tail of its
stderr. Keystore passwords never appear: release signing passes them to
apksigner as `env:VAR` references only.

Human-mode diagnostics all print with an `error[...]` prefix,
regardless of severity: a warning such as CCB-P1017 prints as
`error[CCB-P1017] ...` on stderr. Judge pass/fail by the diagnostic's
severity in the tables below and by the exit code, not by the prefix.

## Config (exit 2)

*ccb-diagnostics-config*

`ccb check` reports every config problem at once, not just the first.

| Code | Meaning |
| ---- | ------- |
| CCB-E1001 | Unknown key in `ccb.toml` (hard error; key and table named) |
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

## Toolchain (exit 3)

*ccb-diagnostics-toolchain*

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
| CCB-E2009 | Required tool binary missing or not executable. Also a broken explicit tool override: `[sdk] aapt2` / `[sdk] zipalign` key or `CCB_AAPT2` / `CCB_ZIPALIGN` env var pointing at a missing or non-executable path fails with this code, message names the override source and the path. No silent fallback to the default tool. |

## Build (exit 1 unless noted)

*ccb-diagnostics-build*

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
| CCB-E3022 | Library module or path-library name collides with a declared AAR's base name (their extraction paths would overwrite each other) |
| CCB-E3023 | Maven lockfile mismatch: resolved tree deviates from `ccb.lock` (version, packaging, hash, missing, or stale entry), or the lockfile is malformed; run `ccb setup` to regenerate it |
| CCB-E3024 | Maven parent POM chain error: a cycle in the chain, or more than 10 ancestors, naming the chain |
| CCB-E3025 | Dex check failed: the dex references one or more classes it does not define (first 10 named, then "+ N more"), or a dex file is malformed/truncated; the build stops before packaging |

## Preview (exit 1 unless noted)

*ccb-diagnostics-preview*

Preview warnings exit 0; only CCB-P1005, CCB-P1008, and (under `--strict`)
CCB-P1001/CCB-P1014 exit 1. CCB-P1016–P1025 are warnings even under
`--strict`, so they never fail the build gate. A project with no usable
`[preview]` section is not a diagnostic at all: `ccb preview` prints
"preview not applicable to this project" and exits 0.

| Code | Meaning |
| ---- | ------- |
| CCB-P1001 | Unknown CSS property (warning; error under `--strict` / `[preview] strict`) |
| CCB-P1002 | CSS parse error, partial parse kept (warning) |
| CCB-P1003 | Overlapping boxes (warning) |
| CCB-P1004 | Box extends past the interior bounds (warning) |
| CCB-P1005 | Navigation to a target matching nothing: no screen, no nav-graph destination, and no manifest activity (error) |
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
| CCB-P1017 | Tap target too small: `button` / `textfield` under 8 columns or 2 rows (warning, always). Full rule: [preview-styles.md](preview-styles.md#lint-diagnostics-after-layout) |
| CCB-P1018 | Text contrast below threshold: WCAG ratio under 4.5:1, or 3.0:1 at `font-size` ≥ 24px (warning, always). Full rule: [preview-styles.md](preview-styles.md#lint-diagnostics-after-layout) |
| CCB-P1019 | Handler found, button not bound: a click handler whose receiver has no `[preview.bindings]` entry (warning, always; no edge recorded) |
| CCB-P1020 | Declared navigation disagrees with a derived edge for the same button; the declared edge stands, the derived edge is dropped (warning, always) |
| CCB-P1021 | XML `android:onClick` button not found in any screen: the bound component label appears in no screen's component tree, so its derived edges have nowhere to attach (warning, always; no edge recorded) |
| CCB-P1022 | Scenario entry does not match the project (warning, always; a scenario never fails the build gate) |
| CCB-P1023 | Trace step does not match the project (warning, always; a trace never fails the build gate) |
| CCB-P1024 | Ambiguous function reference: a click-handler method reference or a helper call names a top-level fun defined in more than one flow file, so no edge is recorded for it — the reference is never guessed. Warns once per (file, name) (warning, always; never fails the build gate) |
| CCB-P1025 | Unattributed navigation: a nav call in a flow file that no parse recorded as an edge — inside a nested block, a skipped statement, or a helper never scanned from a button. Reported with file:line so it is never silently dropped; never invents an edge. Navs already reported by CCB-P1019 (unbound handler) or CCB-P1024 (ambiguous reference) are not repeated here (warning, always; never fails the build gate) |

## Device (exit 5)

*ccb-diagnostics-device*

| Code | Meaning |
| ---- | ------- |
| CCB-E5001 | adb command failed or no device connected (see [device-setup.md](device-setup.md)) |
| CCB-E5002 | No APK to install (run `ccb build` first) |
