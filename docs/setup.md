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
components actually landed before continuing — an install that
silently installed nothing (e.g. licenses not yet accepted on
a fresh SDK) is reported as missing components, never as success;
(6) install kotlinc when no usable 2.x resolves (see "Installing a
missing kotlinc"); (7) install the NDK revision when the project
declares `[ndk]` (see "Installing the NDK"); (8) install r8 when
the project is minify-enabled without an explicit jar (see
"Installing r8"); (9) install missing host tools from the platform
package manager (see "Host tools"); (10) run a final verification
pass (see "Final verification pass"); (11) print "SDK ready".
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

`ccb setup --upgrade` reinstalls the setup-managed (SDK-home)
kotlinc when it is not the pinned version. A user-managed kotlinc
(config, `KOTLIN_HOME`, PATH) is never touched by `--upgrade`, and
SDK component versions stay pinned by config — `--upgrade` is not
a "newest everything" switch.

## Installing the NDK

When the project declares `[ndk]` with `[sdk] ndk_version`, setup
installs that revision through sdkmanager (`ndk;<version>`) when
`<sdk-home>/ndk/<version>` is missing, and verifies the directory
landed. A project without `[ndk]` skips this step; a project with
`[ndk]` but no `ndk_version` cannot be installed (the build
reports the missing version) and setup skips it too.

## Installing r8

When the project has `[build] minify = true` without an explicit
`[sdk] r8_jar`, setup downloads the official r8 jar 9.5.28 from
Google's Maven repository into `<sdk-home>/r8/r8.jar`, verified
against a pinned SHA-256, and verifies it runs (`java -cp <jar>
com.android.tools.r8.R8 --version` — the jar's manifest Main-Class
is broken, so the class is invoked explicitly, as the build does).
The build picks up the setup-managed copy when `[sdk] r8_jar` is
unset; an explicit jar always wins. Overrides: `CCB_R8_URL` and
`CCB_R8_SHA256`, same semantics as the kotlinc ones. With
`--upgrade`, a setup-managed jar whose hash differs from the
pinned one is reinstalled; custom URLs/hashes are not
upgrade-managed.

## Host tools (Termux and Linux)

The SDK's native tools can exist yet not run: on Termux (aarch64)
the build-tools `aapt2` and `zipalign` and the platform-tools `adb`
are x86-64-only. After the SDK steps, setup checks each host tool:
a runnable SDK copy wins, else a runnable copy on PATH wins, else
— when no override pins the choice — setup installs the platform
package:

- Termux (`$PREFIX/bin/pkg`): `aapt2` and `android-tools` (for adb)
  from the official repos; `zipalign` from the rendiix repo (see
  below), used only when the official repos have no candidate.
- Regular Linux: `adb` from the distro package manager (`adb` on
  apt, `android-tools` elsewhere, same installer detection and
  sudo rules as the JDK install). `aapt2`/`zipalign` on Linux are
  SDK-native on x86-64 and have no package fix; on aarch64 Linux
  the final pass warns instead.

The install is gated like the JDK install: `--yes` (or `--upgrade`,
which implies `--yes` for these prompts) installs without asking;
on a TTY setup asks once, naming the tools (and the rendiix repo
when zipalign needs it); without a TTY and without `--yes`, setup
prints a note and continues — host tools are best effort, unlike
the JDK, which setup cannot proceed without. Declining also just
skips the installs. A failed install errors (setup promised it);
after each install setup verifies a runnable copy is on PATH.
Overrides (`[sdk]` key or `CCB_*` env) pin the choice: setup never
installs for an overridden tool.

### Third-party Termux repos (rendiix)

The official Termux repos do not carry `zipalign`. When setup
needs it on Termux, it first checks `apt-cache policy zipalign`:
a candidate means the official (or an already-added) repo carries
it and setup installs directly. Only when there is no candidate
does setup add the rendiix repo — list file
`$PREFIX/etc/apt/sources.list.d/rendiix.list` (`deb
https://rendiix.github.io android-tools termux`), key
`https://rendiix.github.io/rendiix.gpg` into
`$PREFIX/etc/apt/trusted.gpg.d/`, then `apt update` — and install
`zipalign` from it. Adding the repo is idempotent (an existing
list file skips the whole add) and a failed add removes the list
file again so a retry redoes it. Trust note: rendiix is a
third-party repo with a self-asserted key, not an official Termux
source; the prompt names it before anything is added.

## Final verification pass

After every install step, setup resolves the toolchain (plus adb)
and reports what still needs attention, as warnings — for example
an old kotlinc on PATH shadowing the SDK-home install, or a host
tool with no package fix. Warnings do not fail setup: setup already
did what it can, and the remaining items need user action. When
everything resolves, setup prints `toolchain check: ok`. "SDK
ready" still means the SDK components are ready; the final pass is
what makes it mean the toolchain is usable.

## `--upgrade` and `--print-env`

- `--upgrade`: reinstalls setup-managed tools (SDK-home kotlinc,
  SDK-home r8) when they differ from the pinned versions, and
  implies `--yes` for the JDK and host-tool prompts. It never
  touches user-managed tools and never bumps config-pinned SDK
  versions.
- `--print-env`: after a successful setup, prints shell exports
  for the provisioned toolchain on stdout instead of the "SDK
  ready" line (which moves to stderr, so stdout stays eval-safe):
  `ANDROID_HOME`, `JAVA_HOME` (the effective value, derived when
  unset), and a `PATH` prepend for the SDK-home kotlinc bin when
  it exists. No `CCB_*` overrides are printed: with the PATH
  fallbacks, CCB finds Termux tools on PATH without them.

## Download cache

Pinned downloads (cmdline-tools, kotlinc, r8) are cached under
`$XDG_CACHE_HOME/ccb/downloads` (else `~/.cache/ccb/downloads`),
keyed by URL basename and reused when the cached copy's hash
matches — so the ~250MB of setup downloads is fetched once per
version even across SDK homes. A partial download is resumed on
the next run (curl `-C -`); a server that rejects ranges falls
back to a fresh download. Unpinned downloads and `file://` URLs
are never cached. When no cache directory is available, setup
downloads directly and still verifies the hash.

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

`aapt2`, `zipalign`, and `adb` do not have to come from the SDK.
Set `[sdk] aapt2` / `[sdk] zipalign` / `[sdk] adb` in `ccb.toml`,
or the env vars `CCB_AAPT2` / `CCB_ZIPALIGN` / `CCB_ADB`.
Precedence, highest first: `[sdk]` key, env var, default
(`<sdk-home>/build-tools/<version>/<tool>` for aapt2/zipalign,
`<sdk-home>/platform-tools/adb` for adb), then a runnable copy on
PATH. An empty env value counts as unset. A relative value is used
exactly as written (relative to the working directory when the tool
runs, not to the project root or SDK home). A set-but-broken
override fails before any build stage with `CCB-E2009` naming the
source and the path — CCB never silently falls back from an
explicit choice. But a default SDK copy that is missing or cannot
run (wrong architecture) does fall back to PATH: "runnable" is
proved by spawning the tool (`aapt2 version`, `adb version`,
`zipalign` with no arguments — any exit except 127, the
exec-failure exit, counts). `ccb doctor` and `ccb check` print the
effective path and its source (`config`, `env`, `default`, or
`path`) for aapt2 and zipalign, in text and `--json` output.

Motivating case: on Termux (aarch64), the SDK's build-tools `aapt2`
is an x86-64-only binary (2.19) that cannot run natively, while the
Termux package `aapt2` (16.0.0.4-4, reports aapt 2.20) runs natively.
Point CCB at it with `[sdk] aapt2 =
"/data/data/com.termux/files/usr/bin/aapt2"` (the expanded form of
`$PREFIX/bin/aapt2`; TOML values are not shell-expanded) or
`CCB_AAPT2=$PREFIX/bin/aapt2` in the shell — or just install the
Termux package and let the PATH fallback pick it up (see "Host
tools"). The same fallback covers `zipalign` (rendiix repo) and
`adb` (`android-tools` package).

## r8 (release minification)

r8 does not ship in build-tools; it is distributed via Google's Maven
repository (`com.android.tools:r8`). Set `[sdk] r8_jar` to a downloaded
r8 jar (e.g. `r8-9.5.28.jar` from
`https://dl.google.com/dl/android/maven2/com/android/tools/r8/`), or
run `ccb setup` in a minify project to install the official jar into
`<sdk-home>/r8/r8.jar` (see "Installing r8"). `[build] minify = true`
with `[build] proguard_rules` pointing at your rules file. Release
builds with `minify = true` and no `r8_jar` (explicit or
setup-managed) fail with `CCB-E3014`.

Unlike AGP, CCB does not auto-keep manifest components: your rules
must keep every entry point (activities, services, receivers,
providers), or r8 will shrink them away and the app will break at
runtime even though the APK still verifies.
