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
defaults: platform android-35, build-tools 35.0.0) via `sdkmanager`,
and accepts the SDK licenses. If no `cmdline-tools` are present, it
bootstraps them from Google's official archive
(`commandlinetools-linux-11076708_latest.zip`), verified against a
pinned SHA-256 before unzipping.

## Known limitation: authenticated egress proxies

`sdkmanager` has no flags for proxy authentication, and its Java HTTP
stack fails (a `NoSuchElementException` in `HttpURLConnection`
tunneling) against authenticated egress proxies. In such an
environment, `ccb setup` fails at the sdkmanager step with the
repository error; the SDK must then be provisioned by direct download
(curl handles proxy auth fine), which is how the SDK on this VM was
provisioned. This is an environment limitation, not a CCB bug: on a
machine with direct repository access, `ccb setup` works normally.

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
