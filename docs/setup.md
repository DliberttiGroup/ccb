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
  on PATH, then `<sdk-home>/kotlinc` (the setup-installed compiler,
  last resort). Both the distribution root (`…/kotlinc`) and its parent are
  accepted: if `$KOTLIN_HOME/bin/kotlinc` is missing but
  `$KOTLIN_HOME/kotlinc/bin/kotlinc` exists, the nested layout is used.
- Java: `$JAVA_HOME/bin/java`, then `java` on PATH.

`ccb check` and `ccb doctor` report exactly what is missing.

## `ccb setup`

Installs the missing components for the current project (or the
defaults: platform android-35, build-tools 35.0.0) via `sdkmanager`.
Order: (1) when no JDK 17+ is available, offer to install one (see
"Installing a missing JDK" below) instead of failing at the
sdkmanager step with a generic error; (2) bootstrap
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

## Installing a missing JDK

When `ccb setup` finds no JDK 17+, it installs OpenJDK 17 with the
platform's package manager instead of stopping:

- Termux: `pkg install -y openjdk-17` (detected via `$PREFIX/bin/pkg`).
- Regular Linux: `apt-get` (Debian/Ubuntu, `openjdk-17-jdk`),
  `dnf`/`yum` (`java-17-openjdk-devel`), `pacman`
  (`jdk17-openjdk`), `zypper` (`java-17-openjdk-devel`), or `apk`
  (`openjdk17`) — whichever is on PATH. When not running as root,
  the command runs under `sudo` if available; without root or sudo
  there is no usable installer and setup reports that.
- Any other platform, or no installer found: setup prints a plain
  manual-install message (install a JDK 17+, set `JAVA_HOME` or put
  `java` on PATH) and exits 3.

The install is interactive by default: on a TTY, setup asks
"Install OpenJDK 17 via <installer>? [y/N]" and installs only on
"y"/"yes"; any other answer aborts setup. Pass `--yes` to install
without asking (for scripts and non-TTY runs). Without `--yes` and
without a TTY, setup refuses to install unattended and tells you to
run it in a terminal or pass `--yes`. After installing, setup
re-resolves `java` and fails if it is still not found, so a package
manager that reports success without delivering a usable JDK is not
mistaken for success. When `JAVA_HOME` is unset, setup also points it
at the resolved JDK for its own sdkmanager invocations (Termux's
`$PREFIX/lib/jvm/java-17-openjdk` when present, else the `java`
path's parent of `bin/`).

Note: setup installs the JDK and the Android SDK, not kotlinc —
kotlinc is still a manual install (or a future setup step), and
`ccb doctor` keeps reporting it when missing.

## Installing a missing kotlinc

When `ccb setup` finds no usable kotlinc 2.x (missing, or an older
one), it installs the official Kotlin compiler 2.4.21 into the SDK
home (`<sdk-home>/kotlinc`), where the resolver picks it up as a
last resort (after `[sdk] kotlin_home`, `KOTLIN_HOME`, and PATH).
The install is automatic, like the SDK component install: the
destination is CCB's own directory, not a system package, so there
is no prompt and `--yes` is not needed for this step. The archive
comes from JetBrains' official GitHub release
(`kotlin-compiler-2.4.21.zip`) and is verified against a pinned
SHA-256 before unzipping; after installing, setup runs the new
`kotlinc -version` and fails if it does not report 2.x.

When a non-2.x kotlinc is found (e.g. an old one on PATH), setup
still installs 2.4.21 but warns that builds will keep picking the
old one until it is removed or `[sdk] kotlin_home` / `KOTLIN_HOME`
points at the SDK-home install — the SDK-home copy is last in
resolution order and never shadows an explicit config, env, or PATH
kotlinc.

Overrides (mirrors, offline testing): `CCB_KOTLIN_URL` replaces the
download source, and `CCB_KOTLIN_SHA256` replaces the expected
hash. A custom URL without a custom hash skips checksum
verification, with a notice — the pinned hash only applies to the
official release. The install needs `unzip` on PATH.

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
