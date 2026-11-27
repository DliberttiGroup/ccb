# CCB

[![CI](https://github.com/DliberttiGroup/ccb/actions/workflows/ci.yml/badge.svg)](https://github.com/DliberttiGroup/ccb/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![C17](https://img.shields.io/badge/C-C17-blue.svg)](CONTRIBUTING.md#code-rules)

CCB is a fast, headless build system for Android apps. One static binary,
no Gradle, no daemon. It turns a project described by a single `ccb.toml`
file into a signed, installable APK — Kotlin first, Java second, native
libs via the NDK — with per-stage caching, plain-language errors, and a
JSON event stream (`--json`) an agent can drive from a terminal.

## Install

### Standalone binary

Download `ccb-linux-x86_64` and `ccb-linux-x86_64.sha256` from the
[Releases page](https://github.com/DliberttiGroup/ccb/releases), then:

```sh
sha256sum -c ccb-linux-x86_64.sha256
chmod +x ccb-linux-x86_64
mv ccb-linux-x86_64 ~/.local/bin/ccb   # any directory on your PATH
ccb version
```

The first release (`v0.1.0-prototype`) is not published yet — until it
lands, there is nothing to download. Build from source below in the
meantime.

### Build from source

```sh
cmake --preset dev
cmake --build build
./build/ccb version        # ccb 0.1.0
```

## Quickstart

```sh
ccb init myapp
cd myapp
ccb setup                  # installs the JDK, SDK, and kotlinc if missing
ccb build                  # → build/outputs/debug/app.apk
ccb install                # needs a connected device
```

Without `--package` / `--name`, `init` scaffolds name `hello` and package
`com.example.hello` regardless of the directory name. The same hello
project ships in the repo as [`examples/hello`](examples/hello) —
`cd examples/hello && ccb build` builds it.

## Links

- [Documentation](docs/index.md)
- [LLM docs](docs/llm/index.md)
- [Specification](requirement.md)

## License

[Apache License 2.0](LICENSE).
