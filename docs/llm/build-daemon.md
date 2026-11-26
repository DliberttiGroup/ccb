<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/build-daemon.md. -->

# Kotlinc daemon（可选加入）

[文档索引](index.md) · [构建](build.md)

默认：普通的 `kotlinc` 子进程。一个二进制文件、一条命令、一个结果，没有后台进程。选择加入后，同一项目的多次构建会复用一个 kotlinc 编译 daemon——热重建可以跳过 kotlinc 的 JVM 启动。

```toml
[build]
kotlinc_daemon = true            # default false
kotlinc_daemon_idle_seconds = 300 # default 300, allowed 60-3600
```

```sh
ccb build --kotlinc-daemon      # opt in for this build (overrides config)
ccb build --no-kotlinc-daemon   # opt out for this build (overrides config)
ccb daemon stop                 # ask the project's daemon to shut down
```

同时使用两个标志是错误。`ccb clean` 会在删除 `build/` 之前尽力停止 daemon（仅当配置中启用了 daemon 模式时）。

- 按项目隔离：运行文件位于 `build/kotlin-daemon` 下。两个项目永远不会共用一个 daemon；`ccb clean` 会随 `build/` 的其余部分一起删除它。
- CCB 直接调用 daemon 客户端（`org.jetbrains.kotlin.daemon.client.KotlinCompilerClient`），并使用显式 classpath——客户端 7 个 jar，编译器 6 个 jar（包括 `kotlin-daemon.jar`）——绝不使用 `lib/*` 通配符。缺少 jar 时会静默回退到普通 kotlinc。
- 客户端和 daemon 的 JVM 都会获得 `-Djava.net.preferIPv4Stack=true`。不要删除它：没有它时，daemon 握手会挂起。
- 两个 JVM 的堆都限制为 512 MB（`-Xmx512m`，不可配置）。
- 空闲关闭需要两个选项，两个都需要：`--daemon-autoshutdownIdleSeconds=<idle>` 和 `--daemon-shutdownDelayMilliseconds=<idle * 1000>`。没有关闭延迟的话，daemon 会在第一次构建后约 1 s 退出（客户端在每次构建后退出），永远不会被复用。
- `ccb daemon stop` 会要求 daemon 关闭，但 daemon 仍会遵守关闭延迟：进程在空闲超时内退出，而不是立即退出。如果需要等待，请轮询它。
- 即使编译失败，daemon 客户端也会以 0 退出，因此 CCB 不信任其退出码：它会解析捕获的输出，查找 `result code: 0`（成功）和行首的 `ERROR\t`（编译错误，报告为 `CCB-E3004`）。
- 过期或崩溃的 daemon 永远不会导致构建失败：客户端无法连接（或退出时既没有结果码也没有错误行）→ CCB 会记录一条回退说明并运行普通 kotlinc。只有普通 kotlinc 也失败时，构建才会失败。
- daemon 日志中可能包含脚本插件（`ScriptingK2CompilerPluginRegistrar`）的 `ClassNotFoundException`：这是良性的，脚本插件不在 daemon classpath 上，而且 CCB 从不编译脚本。
- 开关 daemon 不会使输出失效：相同的编译器、相同的参数，compile-code 指纹不变。

实测（2026-11-25，kotlinc 2.4.21，单次运行，共享虚拟机，约 1.5 CPU——数据有噪声，请把冷构建数字视为范围，而非承诺）：

| 项目 | 冷构建，daemon 关闭 | 冷构建，daemon 开启 | 单文件重建，关闭 | 单文件重建，开启 |
|---|---|---|---|---|
| hello | 23–29 s | 25 s | 13.6 s | 5.2 s |
| Showcase（副本） | 34–64 s | 66–77 s | 21.6 s | 17.8 s |

冷构建基本打平。热重建会受益——kotlinc 步骤本身的耗时从约 8 s 降至约 2.5 s。
