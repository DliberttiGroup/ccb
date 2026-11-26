# CCB documentation

Human docs, English. Agent docs (Chinese) live in `docs/llm/` — see
`llms.txt` at the repo root. The specification is `requirement.md`.

## Start here

| Doc | What it is |
| --- | --- |
| [setup.md](setup.md) | Toolchain: JDK, SDK, kotlinc, NDK, r8, host tools |
| [build.md](build.md) | Pipeline stages, caching, daemon, release binary, tests |

## Reference

| Doc | What it is |
| --- | --- |
| [diagnostics.md](diagnostics.md) | Every `CCB-E` / `CCB-P` code and exit code |
| [json-protocol.md](json-protocol.md) | `--json` output, `--explain`, `--dry-run` |
| [modules.md](modules.md) | Library modules in `ccb.toml` |
| [maven.md](maven.md) | Remote Maven dependencies, cache, lockfile |
| [preview.md](preview.md) | Terminal UI preview: flow, CSS, scenarios, traces, LSP |
| [device-setup.md](device-setup.md) | Connecting a device over wireless debugging |

## History

| Doc | What it is |
| --- | --- |
| [real-world-testing.md](real-world-testing.md) | 2026-10-24 testing round: 4 apps, findings, verdicts |

## Where facts live

One home per fact. Toolchain → [setup.md](setup.md). Build commands →
[README](../README.md). Test counts → [build.md](build.md). Error-code
meanings and exit codes → [diagnostics.md](diagnostics.md). Measured
numbers → [build.md](build.md). Status → [LAUNCH.md](../LAUNCH.md).
