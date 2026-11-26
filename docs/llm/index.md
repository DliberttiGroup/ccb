<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/index.md. -->

# CCB 文档

人类文档，英文。代理文档（中文）位于 `docs/llm/`——见仓库根目录的 `llms.txt`。规范是 `requirement.md`。

## 从这里开始

| 文档 | 内容 |
| --- | --- |
| [setup.md](setup.md) | 工具链：JDK、SDK、kotlinc、NDK、r8、主机工具 |
| [setup-tools.md](setup-tools.md) | 安装工具：JDK、kotlinc、NDK、r8、主机工具、自定义路径 |
| [build.md](build.md) | 流水线阶段、dex、AAB、缓存、可复现构建、限制 |
| [build-daemon.md](build-daemon.md) | 可选加入的 kotlinc daemon |
| [build-release.md](build-release.md) | 静态 + stripped 发布二进制 |
| [build-tests.md](build-tests.md) | 运行测试套件 |

## 参考

| 文档 | 内容 |
| --- | --- |
| [diagnostics.md](diagnostics.md) | 每个 `CCB-E` / `CCB-P` 代码与退出码 |
| [json-protocol.md](json-protocol.md) | `--json` 输出、`--explain`、`--dry-run` |
| [modules.md](modules.md) | `ccb.toml` 中的库模块 |
| [maven.md](maven.md) | 远程 Maven 依赖、缓存、锁文件 |
| [preview.md](preview.md) | 终端 UI preview：配置、flow 文件、门禁、输出、JSON |
| [preview-handlers.md](preview-handlers.md) | 点击处理函数：派生边、绑定、优先级、结果 |
| [preview-real-code.md](preview-real-code.md) | 从真实 Kotlin Activity 得到屏幕和边 |
| [preview-scenarios.md](preview-scenarios.md) | 场景：声明的运行时事实 |
| [preview-traces.md](preview-traces.md) | 轨迹：记录的运行时路径 |
| [preview-styles.md](preview-styles.md) | CSS 样式、布局模型、lint 诊断 |
| [preview-modes.md](preview-modes.md) | Diff、walk、watch 模式 |
| [preview-lsp.md](preview-lsp.md) | LSP 服务器、tree-live 模式 |
| [device-setup.md](device-setup.md) | 通过无线调试连接设备 |

## 事实的归属

每个事实只有一个归属。工具链 → [setup.md](setup.md)。安装工具 → [setup-tools.md](setup-tools.md)。构建命令 → [README](../../README.md)。流水线 → [build.md](build.md)。Daemon → [build-daemon.md](build-daemon.md)。发布二进制 → [build-release.md](build-release.md)。测试数量 → [build-tests.md](build-tests.md)。错误代码含义与退出码 → [diagnostics.md](diagnostics.md)。实测数字 → [build.md](build.md)。状态 → [LAUNCH.md](../../LAUNCH.md)。

## 标签

每个小节都带一个 `*ccb-<slug>*` 标签。在此按标签查找小节。

| 标签 | 页面 | 小节 |
| --- | --- | --- |
| `*ccb-preview*` | preview.md | Preview |
| `*ccb-preview-config*` | preview.md | 配置 |
| `*ccb-preview-flow*` | preview.md | Flow 文件 |
| `*ccb-preview-gate*` | preview.md | 构建门禁 |
| `*ccb-preview-output*` | preview.md | 输出与退出码 |
| `*ccb-preview-json*` | preview.md | JSON 形态 |
| `*ccb-preview-example*` | preview.md | 实际示例 |
| `*ccb-preview-handlers*` | preview-handlers.md | 点击处理函数 |
| `*ccb-preview-branch-edges*` | preview-handlers.md | 条件导航 |
| `*ccb-preview-xml-onclick*` | preview-handlers.md | XML android:onClick |
| `*ccb-preview-navcontroller*` | preview-handlers.md | NavController 与导航图 |
| `*ccb-preview-bindings*` | preview-handlers.md | 绑定 |
| `*ccb-preview-precedence*` | preview-handlers.md | 优先级 |
| `*ccb-preview-unknown*` | preview-handlers.md | 未知情况 |
| `*ccb-preview-scope*` | preview-handlers.md | 作用域限制 |
| `*ccb-preview-real-code*` | preview-real-code.md | 真实代码 |
| `*ccb-preview-real-merge*` | preview-real-code.md | 与草图合并 |
| `*ccb-preview-real-wireframe*` | preview-real-code.md | 无草图时的线框图 |
| `*ccb-preview-real-limits*` | preview-real-code.md | 限制 |
| `*ccb-preview-scenarios*` | preview-scenarios.md | 场景 |
| `*ccb-preview-scenario-merge*` | preview-scenarios.md | 合并规则 |
| `*ccb-preview-traces*` | preview-traces.md | 轨迹 |
| `*ccb-preview-trace-format*` | preview-traces.md | 格式 |
| `*ccb-preview-trace-render*` | preview-traces.md | 渲染规则 |
| `*ccb-preview-styles*` | preview-styles.md | 样式 |
| `*ccb-preview-layout*` | preview-styles.md | 布局模型 |
| `*ccb-preview-lint*` | preview-styles.md | Lint 诊断 |
| `*ccb-preview-modes*` | preview-modes.md | 模式 |
| `*ccb-preview-diff*` | preview-modes.md | Diff 模式 |
| `*ccb-preview-walk*` | preview-modes.md | Walk 模式 |
| `*ccb-preview-watch*` | preview-modes.md | Watch 模式 |
| `*ccb-preview-lsp*` | preview-lsp.md | LSP 服务器 |
| `*ccb-preview-tree-live*` | preview-lsp.md | Tree-live 模式 |
| `*ccb-build*` | build.md | 构建流水线 |
| `*ccb-build-stages*` | build.md | 各阶段 |
| `*ccb-build-dex*` | build.md | Dex |
| `*ccb-build-aab*` | build.md | AAB 输出 |
| `*ccb-build-cache*` | build.md | 缓存 |
| `*ccb-build-reproducible*` | build.md | 可复现构建 |
| `*ccb-build-limits*` | build.md | 已知限制 |
| `*ccb-build-daemon*` | build-daemon.md | Kotlinc daemon |
| `*ccb-build-release*` | build-release.md | 发布二进制 |
| `*ccb-build-tests*` | build-tests.md | 运行测试 |
| `*ccb-setup*` | setup.md | 工具链设置 |
| `*ccb-setup-needs*` | setup.md | CCB 需要什么 |
| `*ccb-setup-resolution*` | setup.md | 解析顺序 |
| `*ccb-setup-steps*` | setup.md | ccb setup |
| `*ccb-setup-verify*` | setup.md | 最终验证 |
| `*ccb-setup-upgrade*` | setup.md | --upgrade 与 --print-env |
| `*ccb-setup-proxy*` | setup.md | 需要认证的出口代理 |
| `*ccb-setup-tools*` | setup-tools.md | 安装工具 |
| `*ccb-setup-jdk*` | setup-tools.md | 安装缺失的 JDK |
| `*ccb-setup-kotlinc*` | setup-tools.md | 安装缺失的 kotlinc |
| `*ccb-setup-ndk*` | setup-tools.md | 安装 NDK |
| `*ccb-setup-r8*` | setup-tools.md | r8 |
| `*ccb-setup-host-tools*` | setup-tools.md | 主机工具 |
| `*ccb-setup-rendiix*` | setup-tools.md | 第三方 Termux 仓库 |
| `*ccb-setup-custom-paths*` | setup-tools.md | 自定义工具路径 |
| `*ccb-setup-download-cache*` | setup-tools.md | 下载缓存 |
| `*ccb-maven*` | maven.md | 远程 Maven 依赖 |
| `*ccb-maven-declare*` | maven.md | 声明依赖 |
| `*ccb-maven-repos*` | maven.md | 仓库 |
| `*ccb-maven-packaging*` | maven.md | 打包类型 |
| `*ccb-maven-transitive*` | maven.md | 传递依赖 |
| `*ccb-maven-cache*` | maven.md | 缓存与校验和 |
| `*ccb-maven-hermetic*` | maven.md | 封闭性 |
| `*ccb-maven-lockfile*` | maven.md | 锁文件 |
| `*ccb-maven-limits*` | maven.md | 限制 |
| `*ccb-maven-troubleshooting*` | maven.md | 故障排查 |
| `*ccb-diagnostics*` | diagnostics.md | 诊断代码 |
| `*ccb-diagnostics-config*` | diagnostics.md | 配置 |
| `*ccb-diagnostics-toolchain*` | diagnostics.md | 工具链 |
| `*ccb-diagnostics-build*` | diagnostics.md | 构建 |
| `*ccb-diagnostics-preview*` | diagnostics.md | 预览 |
| `*ccb-diagnostics-device*` | diagnostics.md | 设备 |
| `*ccb-modules*` | modules.md | 库模块 |
| `*ccb-modules-config*` | modules.md | 配置 |
| `*ccb-modules-graph*` | modules.md | 图规则 |
| `*ccb-modules-flow*` | modules.md | 构建流程 |
| `*ccb-modules-r*` | modules.md | R 语义 |
| `*ccb-modules-incremental*` | modules.md | 增量构建 |
| `*ccb-modules-limits*` | modules.md | 限制 |
| `*ccb-device*` | device-setup.md | 连接设备 |
| `*ccb-device-wireless*` | device-setup.md | 通过 Tailscale 无线调试 |
| `*ccb-device-no-device*` | device-setup.md | 无设备连接时 |
| `*ccb-device-notes*` | device-setup.md | 说明 |
| `*ccb-json*` | json-protocol.md | JSON 协议 |
| `*ccb-json-build-events*` | json-protocol.md | 构建事件 |
| `*ccb-json-diagnostics*` | json-protocol.md | 诊断 |
| `*ccb-json-commands*` | json-protocol.md | 其他命令 |
| `*ccb-json-explain*` | json-protocol.md | --explain 与 --dry-run |
