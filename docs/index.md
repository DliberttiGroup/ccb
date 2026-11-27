# CCB documentation

*ccb-docs*

Human docs, English. Agent docs (Chinese) live in `docs/llm/` — see
[docs/llm/index.md](llm/index.md). The specification is `requirement.md`.

## Start here

| Doc | What it is |
| --- | --- |
| [setup.md](setup.md) | Toolchain: JDK, SDK, kotlinc, NDK, r8, host tools |
| [setup-tools.md](setup-tools.md) | Installing tools: JDK, kotlinc, NDK, r8, host tools, custom paths |
| [build.md](build.md) | Pipeline stages, dex, AAB, caching, reproducible builds, limits |
| [graph.md](graph.md) | Task graph engine: task model, validation, plan, build summary |
| [architecture-audit.md](architecture-audit.md) | Phase 0 audit: layer map, coupling, correctness risks, migration plan per phase, scope freeze, benchmark baseline |
| [build-daemon.md](build-daemon.md) | Opt-in kotlinc daemon |
| [build-release.md](build-release.md) | Static + stripped release binary |
| [build-tests.md](build-tests.md) | Running the test suite |

## Reference

| Doc | What it is |
| --- | --- |
| [diagnostics.md](diagnostics.md) | Every `CCB-E` / `CCB-P` code and exit code |
| [json-protocol.md](json-protocol.md) | `--json` output, `--explain`, `--dry-run` |
| [modules.md](modules.md) | Library modules in `ccb.toml` |
| [library.md](library.md) | Standalone library projects: `[library]`, path/Maven consumption, resource namespacing, `check` report |
| [maven.md](maven.md) | Remote Maven dependencies: declare, plugins, repos, packaging |
| [maven-resolve.md](maven-resolve.md) | Maven resolution: transitive dependencies, profiles |
| [maven-cache.md](maven-cache.md) | Maven cache, hermeticity, lockfile |
| [maven-troubleshooting.md](maven-troubleshooting.md) | Maven troubleshooting by diagnostic code |
| [preview.md](preview.md) | Terminal UI preview: config, flow files, gate, output, JSON |
| [preview-handlers.md](preview-handlers.md) | Click handlers: derived edges, bindings, precedence, outcomes |
| [preview-real-code.md](preview-real-code.md) | Screens and edges from real Kotlin Activities |
| [preview-scenarios.md](preview-scenarios.md) | Scenarios: declared runtime facts |
| [preview-traces.md](preview-traces.md) | Traces: recorded runtime paths |
| [preview-styles.md](preview-styles.md) | CSS styles, layout model, lint diagnostics |
| [preview-modes.md](preview-modes.md) | Diff, walk, watch modes |
| [preview-lsp.md](preview-lsp.md) | LSP server, tree-live mode |
| [tui.md](tui.md) | Terminal UI library: cells, diffing, input, terminal |
| [device-setup.md](device-setup.md) | Connecting a device over wireless debugging |

## Where facts live

One home per fact. Toolchain → [setup.md](setup.md). Installing tools →
[setup-tools.md](setup-tools.md). Build commands → [README](../README.md).
Pipeline → [build.md](build.md). Daemon → [build-daemon.md](build-daemon.md).
Release binary → [build-release.md](build-release.md). Test counts →
[build-tests.md](build-tests.md). Terminal UI library → [tui.md](tui.md). Error-code meanings and exit codes →
[diagnostics.md](diagnostics.md). Measured numbers → [build.md](build.md).
Architecture audit, scope freeze, migration plan, benchmark baseline →
[architecture-audit.md](architecture-audit.md).
Maven resolution → [maven-resolve.md](maven-resolve.md). Maven cache and
lockfile → [maven-cache.md](maven-cache.md). Maven troubleshooting →
[maven-troubleshooting.md](maven-troubleshooting.md).

## Tags

Every section carries a `*ccb-<slug>*` tag. Find a section by tag here.

| Tag | Page | Section |
| --- | --- | --- |
| `*ccb-preview*` | preview.md | Preview |
| `*ccb-preview-config*` | preview.md | Config |
| `*ccb-preview-flow*` | preview.md | Flow files |
| `*ccb-preview-gate*` | preview.md | Build gate |
| `*ccb-preview-output*` | preview.md | Output and exit codes |
| `*ccb-preview-json*` | preview.md | JSON shape |
| `*ccb-preview-example*` | preview.md | Worked example |
| `*ccb-preview-handlers*` | preview-handlers.md | Click handlers |
| `*ccb-preview-branch-edges*` | preview-handlers.md | Conditional nav |
| `*ccb-preview-xml-onclick*` | preview-handlers.md | XML android:onClick |
| `*ccb-preview-navcontroller*` | preview-handlers.md | NavController + nav graph |
| `*ccb-preview-bindings*` | preview-handlers.md | Bindings |
| `*ccb-preview-precedence*` | preview-handlers.md | Precedence |
| `*ccb-preview-unknown*` | preview-handlers.md | Unknown cases |
| `*ccb-preview-scope*` | preview-handlers.md | Scope limits |
| `*ccb-preview-real-code*` | preview-real-code.md | Real code |
| `*ccb-preview-real-merge*` | preview-real-code.md | Merge with a sketch |
| `*ccb-preview-real-wireframe*` | preview-real-code.md | Wireframe, no sketch |
| `*ccb-preview-real-limits*` | preview-real-code.md | Limits |
| `*ccb-preview-scenarios*` | preview-scenarios.md | Scenarios |
| `*ccb-preview-scenario-merge*` | preview-scenarios.md | Merge rules |
| `*ccb-preview-traces*` | preview-traces.md | Traces |
| `*ccb-preview-trace-format*` | preview-traces.md | Format |
| `*ccb-preview-trace-render*` | preview-traces.md | Render rules |
| `*ccb-preview-styles*` | preview-styles.md | Styles |
| `*ccb-preview-layout*` | preview-styles.md | Layout model |
| `*ccb-preview-lint*` | preview-styles.md | Lint diagnostics |
| `*ccb-preview-modes*` | preview-modes.md | Modes |
| `*ccb-preview-diff*` | preview-modes.md | Diff mode |
| `*ccb-preview-walk*` | preview-modes.md | Walk mode |
| `*ccb-preview-watch*` | preview-modes.md | Watch mode |
| `*ccb-preview-lsp*` | preview-lsp.md | LSP server |
| `*ccb-preview-tree-live*` | preview-lsp.md | Tree-live mode |
| `*ccb-build*` | build.md | Build pipeline |
| `*ccb-architecture-audit*` | architecture-audit.md | Architecture audit and migration plan |
| `*ccb-architecture-map*` | architecture-audit.md | Layer map: what exists today |
| `*ccb-architecture-coupling*` | architecture-audit.md | Coupling findings |
| `*ccb-architecture-duplicates*` | architecture-audit.md | Duplicate responsibilities |
| `*ccb-architecture-risks*` | architecture-audit.md | Correctness risks |
| `*ccb-architecture-phases*` | architecture-audit.md | FINAL DIRECTIVE answers, per phase |
| `*ccb-architecture-scope*` | architecture-audit.md | Scope freeze list |
| `*ccb-architecture-benchmarks*` | architecture-audit.md | Benchmark baseline |
| `*ccb-architecture-next*` | architecture-audit.md | What Phase 1 should start with |
| `*ccb-build-stages*` | build.md | Stages |
| `*ccb-build-dex*` | build.md | Dex |
| `*ccb-build-aab*` | build.md | AAB output |
| `*ccb-build-cache*` | build.md | Caching |
| `*ccb-build-reproducible*` | build.md | Reproducible builds |
| `*ccb-build-limits*` | build.md | Known limits |
| `*ccb-build-daemon*` | build-daemon.md | Kotlinc daemon |
| `*ccb-build-release*` | build-release.md | Release binary |
| `*ccb-build-tests*` | build-tests.md | Running the tests |
| `*ccb-setup*` | setup.md | Toolchain setup |
| `*ccb-setup-needs*` | setup.md | What CCB needs |
| `*ccb-setup-resolution*` | setup.md | Resolution order |
| `*ccb-setup-steps*` | setup.md | ccb setup |
| `*ccb-setup-verify*` | setup.md | Final verification pass |
| `*ccb-setup-upgrade*` | setup.md | --upgrade and --print-env |
| `*ccb-setup-proxy*` | setup.md | Authenticated egress proxies |
| `*ccb-setup-tools*` | setup-tools.md | Installing tools |
| `*ccb-setup-jdk*` | setup-tools.md | Installing a missing JDK |
| `*ccb-setup-kotlinc*` | setup-tools.md | Installing a missing kotlinc |
| `*ccb-setup-ndk*` | setup-tools.md | Installing the NDK |
| `*ccb-setup-r8*` | setup-tools.md | r8 |
| `*ccb-setup-host-tools*` | setup-tools.md | Host tools |
| `*ccb-setup-rendiix*` | setup-tools.md | Third-party Termux repos |
| `*ccb-setup-custom-paths*` | setup-tools.md | Custom tool paths |
| `*ccb-setup-download-cache*` | setup-tools.md | Download cache |
| `*ccb-maven*` | maven.md | Remote Maven dependencies |
| `*ccb-maven-declare*` | maven.md | Declaring dependencies |
| `*ccb-maven-plugins*` | maven.md | Compiler plugins |
| `*ccb-maven-repos*` | maven.md | Repositories |
| `*ccb-maven-packaging*` | maven.md | Packaging |
| `*ccb-maven-transitive*` | maven-resolve.md | Transitive dependencies |
| `*ccb-maven-profiles*` | maven-resolve.md | Profiles |
| `*ccb-maven-cache*` | maven-cache.md | Cache and checksums |
| `*ccb-maven-hermetic*` | maven-cache.md | Hermeticity |
| `*ccb-maven-lockfile*` | maven-cache.md | Lockfile |
| `*ccb-maven-limits*` | maven.md | Limitations |
| `*ccb-maven-troubleshooting*` | maven-troubleshooting.md | Troubleshooting |
| `*ccb-diagnostics*` | diagnostics.md | Diagnostic codes |
| `*ccb-diagnostics-config*` | diagnostics.md | Config |
| `*ccb-diagnostics-toolchain*` | diagnostics.md | Toolchain |
| `*ccb-diagnostics-build*` | diagnostics.md | Build |
| `*ccb-diagnostics-preview*` | diagnostics.md | Preview |
| `*ccb-diagnostics-device*` | diagnostics.md | Device |
| `*ccb-modules*` | modules.md | Library modules |
| `*ccb-modules-config*` | modules.md | Config |
| `*ccb-modules-graph*` | modules.md | Graph rules |
| `*ccb-modules-flow*` | modules.md | Build flow |
| `*ccb-modules-r*` | modules.md | R semantics |
| `*ccb-modules-incremental*` | modules.md | Incremental builds |
| `*ccb-modules-limits*` | modules.md | Limitations |
| `*ccb-library*` | library.md | Standalone library projects |
| `*ccb-library-config*` | library.md | Declaring a library project |
| `*ccb-library-aar*` | library.md | AAR contents |
| `*ccb-library-consume*` | library.md | Consuming a library from a path |
| `*ccb-library-r*` | library.md | Resource merging and namespacing |
| `*ccb-library-check*` | library.md | `ccb check` reports libraries |
| `*ccb-device*` | device-setup.md | Connecting a device |
| `*ccb-device-wireless*` | device-setup.md | Wireless debugging over Tailscale |
| `*ccb-device-no-device*` | device-setup.md | No device attached |
| `*ccb-device-notes*` | device-setup.md | Notes |
| `*ccb-json*` | json-protocol.md | JSON protocol |
| `*ccb-json-build-events*` | json-protocol.md | Build events |
| `*ccb-json-diagnostics*` | json-protocol.md | Diagnostics |
| `*ccb-json-commands*` | json-protocol.md | Other commands |
| `*ccb-json-explain*` | json-protocol.md | --explain and --dry-run |
| `*ccb-tui*` | tui.md | Terminal UI library |
| `*ccb-tui-model*` | tui.md | Frame model |
| `*ccb-tui-modules*` | tui.md | Modules |
| `*ccb-tui-pipeline*` | tui.md | Render pipeline |
| `*ccb-tui-ansi*` | tui.md | ANSI subset |
| `*ccb-tui-input*` | tui.md | Input |
| `*ccb-tui-resize*` | tui.md | Resize and signals |
| `*ccb-tui-limits*` | tui.md | What it does not do |
| `*ccb-tui-tests*` | tui.md | Tests |
