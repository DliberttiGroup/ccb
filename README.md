# CCB — Custom C Builder

Build headless Android APKs without Gradle. C17, no build-system
dependencies beyond a C compiler and CMake.

## Build CCB

```sh
cmake -S . -B build        # Release is the default build type
cmake --build build
./build/ccb version        # ccb 0.1.0
```

Run the unit tests (sanitizer build):

```sh
cmake -S . -B build-san -DCCB_SANITIZE=ON
cmake --build build-san
ctest --test-dir build-san
```

## Use it

```sh
ccb init myapp
cd myapp
ccb build                  # → build/outputs/debug/app.apk
ccb run                    # build → install → launch (needs a device)
ccb install --dry-run      # show the adb command without running it
ccb check                  # validate config + toolchain
ccb doctor                 # toolchain report
ccb clean
```

Toolchain: JDK 17+, Android SDK (platform android-34/35,
build-tools 35.x, platform-tools), kotlinc 2.x, NDK r27 for native
projects, r8 for minified release builds (see `docs/setup.md`).

`ccb build` requires `ANDROID_HOME` (or `[sdk] home` in `ccb.toml`),
`JAVA_HOME`, and `KOTLIN_HOME` unless the tools are on PATH.

## Status

- M0 skeleton, config, toolchain resolution: done.
- M1 first real APK (Kotlin hello-world, debug-signed, zipaligned,
  verified): done.
- M2 incremental builds (step cache, `--explain`, tool-upgrade
  invalidation): done.
- M3 native libs + AARs: done.
- M4 release readiness (fail-fast signing, r8 minification,
  byte-identical rebuilds, JSON protocol): done.
- M5 preview (CSS + Kotlin structure → box/logic tree): stretch,
  not started.
- Not done: fuzz harnesses for the TOML/ZIP/CSS/Kotlin-structure
  parsers (§9.3); `ccb setup`'s sdkmanager step cannot run behind
  authenticated egress proxies (see `docs/setup.md`); install/launch
  against a real device (see `docs/device-setup.md`).

## Docs

- `docs/build.md` — pipeline, caching, reproducibility, known costs.
- `docs/setup.md` — toolchain provisioning, r8, proxy limitation.
- `docs/device-setup.md` — connecting a device (Tailscale wireless
  debugging).
- `docs/diagnostics.md` — diagnostic code catalog.
- `docs/json-protocol.md` — `--json` event stream.
- `requirement.md` — the full specification.

No license has been chosen for CCB itself (open question 9); nothing
is published anywhere.
