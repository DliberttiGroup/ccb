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
| CCB-E2009 | A required tool binary is missing or not executable |

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

## Device (exit 5)

| Code | Meaning |
| ---- | ------- |
| CCB-E5001 | adb command failed or no device connected (see docs/device-setup.md) |
| CCB-E5002 | No APK to install (run `ccb build` first) |

Tool failures include the exact command that failed and the tail of
its stderr. Keystore passwords are never included: release signing
passes them to apksigner as `env:VAR` references only.
