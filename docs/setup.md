# Toolchain setup

## What CCB needs

- JDK 17+ (`java`, `javac`, `keytool`)
- Android SDK: `platforms;android-34` or `android-35`,
  `build-tools;35.x`, `platform-tools` (for `adb`)
- kotlinc 2.x
- NDK r27 (only for `[ndk]` projects)
- r8 (only for release + minify; see below)

## Resolution order

- SDK home: `[sdk] home` in `ccb.toml`, then `ANDROID_HOME`, then
  `ANDROID_SDK_ROOT`, then `~/Android/Sdk`.
- Kotlin home: `[sdk] kotlin_home`, then `KOTLIN_HOME`, then `kotlinc`
  on PATH. Both the distribution root (`…/kotlinc`) and its parent are
  accepted: if `$KOTLIN_HOME/bin/kotlinc` is missing but
  `$KOTLIN_HOME/kotlinc/bin/kotlinc` exists, the nested layout is used.
- Java: `$JAVA_HOME/bin/java`, then `java` on PATH.

`ccb check` and `ccb doctor` report exactly what is missing.

## `ccb setup`

Installs the missing components for the current project (or the
defaults: platform android-35, build-tools 35.0.0) via `sdkmanager`.
Order: (1) fail fast when no JDK 17+ is available, with a message
naming Java instead of a generic sdkmanager failure; (2) bootstrap
`cmdline-tools` from Google's official archive
(`commandlinetools-linux-11076708_latest.zip`), verified against a
pinned SHA-256 before unzipping, when none are present; (3) accept
the SDK licenses; (4) install the components; (5) verify the
components actually landed before printing "SDK ready" — an install
that silently installed nothing (e.g. licenses not yet accepted on
a fresh SDK) is reported as missing components, never as success.
A config that was found but fails to load is an error, not a
silent fallback to the default SDK home; defaults apply only when
no `ccb.toml` exists at all.

## Known limitation: authenticated egress proxies

`sdkmanager` has no flags for proxy authentication, and its Java HTTP
stack fails (a `NoSuchElementException` in `HttpURLConnection`
tunneling) against authenticated egress proxies. In such an
environment, `ccb setup` fails at the sdkmanager step with the
repository error; the SDK must then be provisioned by direct download
(curl handles proxy auth fine), which is how the SDK on this VM was
provisioned. This is an environment limitation, not a CCB bug: on a
machine with direct repository access, `ccb setup` works normally.
A missing JDK is reported separately before sdkmanager ever runs,
so the proxy note does not mask it.

## Custom tool paths

`aapt2` and `zipalign` do not have to come from the SDK's build-tools
directory. Set `[sdk] aapt2` / `[sdk] zipalign` in `ccb.toml`, or the
env vars `CCB_AAPT2` / `CCB_ZIPALIGN`. Precedence, highest first:
`[sdk]` key, env var, default
`<sdk-home>/build-tools/<version>/<tool>`. An empty env value counts
as unset. A relative value is used exactly as written (relative to
the working directory when the tool runs, not to the project root or
SDK home). A set-but-broken override fails before any build stage
with `CCB-E2009` naming the source and the path — CCB never silently
falls back to the default tool. `ccb doctor` and `ccb check` print
the effective path and its source (`config`, `env`, or `default`)
for both tools, in text and `--json` output.

Motivating case: on Termux (aarch64), the SDK's build-tools `aapt2`
is an x86-64-only binary (2.19) that cannot run natively, while the
Termux package `aapt2` (16.0.0.4-4, reports aapt 2.20) runs natively.
Point CCB at it with `[sdk] aapt2 =
"/data/data/com.termux/files/usr/bin/aapt2"` (the expanded form of
`$PREFIX/bin/aapt2`; TOML values are not shell-expanded) or
`CCB_AAPT2=$PREFIX/bin/aapt2` in the shell. There is no Termux `zipalign`
package, so `zipalign` keeps its default SDK path unless it gets its
own override.

## r8 (release minification)

r8 does not ship in build-tools; it is distributed via Google's Maven
repository (`com.android.tools:r8`). Set `[sdk] r8_jar` to a downloaded
r8 jar (e.g. `r8-9.5.28.jar` from
`https://dl.google.com/dl/android/maven2/com/android/tools/r8/`) and
`[build] minify = true` with `[build] proguard_rules` pointing at your
rules file. Release builds with `minify = true` and no `r8_jar` fail
with `CCB-E3014`.

Unlike AGP, CCB does not auto-keep manifest components: your rules
must keep every entry point (activities, services, receivers,
providers), or r8 will shrink them away and the app will break at
runtime even though the APK still verifies.
