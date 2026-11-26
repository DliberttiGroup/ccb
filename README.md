# CCB

[Documentation](docs/index.md) | [Specification](requirement.md) | [LLM docs](llms.txt)

[![CI](https://github.com/DliberttiGroup/ccb/actions/workflows/ci.yml/badge.svg)](https://github.com/DliberttiGroup/ccb/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/DliberttiGroup/ccb)](LICENSE)
[![Tag](https://img.shields.io/github/v/tag/DliberttiGroup/ccb)](https://github.com/DliberttiGroup/ccb/tags)

Build headless Android APKs without Gradle. One C binary, no daemon, no
runtime dependencies. Every build reproducible and explainable
(`--explain`, `--json`). Built to be driven by an AI agent from a terminal.

## Features

- Kotlin + Java apps, AAR libraries, native (NDK) libs, multi-module projects
- Incremental builds: per-step cache, tool-upgrade invalidation
- Release: R8 minification, zipalign, signing, verification
- Dex gate: missing classes fail the build before packaging
- Preview: CSS + Kotlin structure renders to a box/logic tree
- JSON event stream (`--json`), stable exit codes, dry-run mode
- Opt-in kotlinc daemon (auto-shutdown) for faster rebuilds

## Install from source

Toolchain: see [docs/setup.md](docs/setup.md).

```sh
cmake -S . -B build        # Release is the default build type
cmake --build build
./build/ccb version        # ccb 0.1.0
```

Presets (`release`, `dev`, `sanitizer`) live in `CMakePresets.json`.
Stripped, statically linked release binary:

```sh
cmake --preset release
cmake --build build-release
```

Tests: [docs/build.md](docs/build.md#running-the-tests). Run suites
sequentially, never concurrently. No packages yet — see [LAUNCH.md](LAUNCH.md).

## Transitioning from Gradle

CCB replaces the Gradle build, not the Android SDK. Same tools
(kotlinc, d8, aapt2, apksigner), driven directly. One `ccb.toml`
instead of `build.gradle` + `settings.gradle`.

```sh
ccb init myapp
cd myapp
ccb build                  # → build/outputs/debug/app.apk
ccb run                    # build → install → launch (needs a device)
ccb check                  # validate config + toolchain
ccb doctor                 # toolchain report
```

## Project layout

```
├─ src/           C17 implementation, one module per subsystem
├─ tests/         test suite (see tests/test.h)
├─ docs/          human docs, English (see docs/index.md)
├─ docs/llm/      agent docs, Chinese (see llms.txt)
├─ examples/      hello, preview-sample, real-world
├─ requirement.md the specification
├─ LAUNCH.md      prototype launch checklist
└─ LICENSE        Apache 2.0
```

## License

[Apache License 2.0](LICENSE).
