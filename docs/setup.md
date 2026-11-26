# Toolchain setup

[Docs index](index.md)

*ccb-setup*

Sub-page: [installing tools](setup-tools.md) (JDK, kotlinc, NDK, r8, host tools, custom paths)

## What CCB needs

*ccb-setup-needs*

| Tool | Version |
| ---- | ------- |
| JDK (`java`, `javac`, `keytool`) | 17+ |
| Android SDK | `platforms;android-34`, `-35`, or `-36`; `build-tools;35.x` or `36.x`; `platform-tools` (for `adb`) |
| kotlinc | 2.x |
| NDK | r27, only for `[ndk]` projects |
| r8 | only for release + minify ([setup-tools.md](setup-tools.md#r8)) |

## Resolution order

*ccb-setup-resolution*

- SDK home: `[sdk] home`, then `ANDROID_HOME`, then `ANDROID_SDK_ROOT`,
  then `~/Android/Sdk`.
- Kotlin home: `[sdk] kotlin_home`, then `KOTLIN_HOME`, then `kotlinc` on
  PATH, then `<sdk-home>/kotlinc` (setup-installed compiler, last
  resort). Distribution root (`…/kotlinc`) and its parent both accepted:
  if `$KOTLIN_HOME/bin/kotlinc` is missing but
  `$KOTLIN_HOME/kotlinc/bin/kotlinc` exists, the nested layout is used.
- Java: `$JAVA_HOME/bin/java`, then `java` on PATH.

`ccb check` and `ccb doctor` report exactly what is missing.

## `ccb setup`

*ccb-setup-steps*

Installs missing components for the current project (or the defaults:
platform android-35, build-tools 35.0.0) via `sdkmanager`. When platform,
build-tools, and platform-tools are already present, setup skips licenses
and sdkmanager entirely and proceeds to the remaining steps — so it works
behind a proxy that blocks sdkmanager, as long as the SDK is already
provisioned. A config that was found but fails to load is an error, not a
silent fallback to the default SDK home; defaults apply only when no
`ccb.toml` exists at all.

| # | Step |
| - | ---- |
| 1 | Missing JDK 17+: offer to install one (see [setup-tools.md](setup-tools.md#installing-a-missing-jdk)) instead of failing at the sdkmanager step with a generic error. |
| 2 | Bootstrap `cmdline-tools` from Google's official archive (`commandlinetools-linux-11076708_latest.zip`), verified against a pinned SHA-256 before unzipping, when none are present. |
| 3 | Accept the SDK licenses. |
| 4 | Install the components. |
| 5 | Verify the components actually landed before continuing — an install that silently installed nothing (e.g. licenses not yet accepted on a fresh SDK) is reported as missing components, never as success. |
| 6 | Install kotlinc when no usable 2.x resolves. |
| 7 | Install the NDK revision when the project declares `[ndk]`. |
| 8 | Install r8 when the project is minify-enabled without an explicit jar. |
| 9 | Install missing host tools from the platform package manager. |
| 10 | Prefetch `[deps] maven` artifacts into the local Maven cache (see [maven.md](maven.md)). |
| 11 | Final verification pass (below). |
| 12 | Print "SDK ready". |

## Final verification pass

*ccb-setup-verify*

After every install step, setup resolves the toolchain (plus adb) and
reports what still needs attention, as warnings — e.g. an old kotlinc on
PATH shadowing the SDK-home install, or a host tool with no package fix.
Warnings don't fail setup: setup already did what it can, the remaining
items need user action. When everything resolves, setup prints `toolchain
check: ok`. "SDK ready" means the SDK components are ready; the final pass
is what makes it mean the toolchain is usable.

## `--upgrade` and `--print-env`

*ccb-setup-upgrade*

- `--upgrade`: reinstalls setup-managed tools (SDK-home kotlinc, SDK-home
  r8) when they differ from the pinned versions, and implies `--yes` for
  the JDK and host-tool prompts. Never touches user-managed tools, never
  bumps config-pinned SDK versions.
- `--print-env`: after a successful setup, prints shell exports for the
  provisioned toolchain on stdout instead of the "SDK ready" line (which
  moves to stderr, so stdout stays eval-safe): `ANDROID_HOME`,
  `JAVA_HOME` (effective value, derived when unset), and a `PATH` prepend
  for the SDK-home kotlinc bin when it exists. No `CCB_*` overrides are
  printed: with the PATH fallbacks, CCB finds Termux tools on PATH without
  them.

## Known limitation: authenticated egress proxies

*ccb-setup-proxy*

`sdkmanager` has no flags for proxy authentication, and its Java HTTP
stack fails against authenticated egress proxies. In such an environment,
`ccb setup` fails at the sdkmanager step with the repository error; the
SDK must then be provisioned by direct download (curl handles proxy auth
fine). A missing JDK is reported separately before sdkmanager ever runs,
so the proxy note doesn't mask it.
