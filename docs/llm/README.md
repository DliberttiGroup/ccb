<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file explains the layout of docs/llm/. -->

# CCB LLM 文档布局

本文档说明 `docs/llm/` 目录的布局。人类可读的英文文档位于 `docs/`；本目录是其中文版本，内容与英文版相同，供 LLM / AI 代理阅读。每个中文文档开头都有一行英文注释，注明它对应的英文原文。

## 文件对照

| LLM 文档（中文） | 人类文档（英文） | 内容 |
|---|---|---|
| `docs/llm/README.md` | `README.md`（仓库根目录） | 本布局说明；项目总览见英文 README。英文 README 中的 init 默认值：未传 `--package` / `--name` 时，`init` 脚手架的名称为 `hello`、包为 `com.example.hello`，与目录名无关 |
| `docs/llm/index.md` | `docs/index.md` | 文档索引、入口与参考表、全部小节标签 |
| `docs/llm/build.md` | `docs/build.md` | 构建流水线、dex、AAB、缓存、可复现构建、已知限制 |
| `docs/llm/build-daemon.md` | `docs/build-daemon.md` | 可选加入的 kotlinc daemon、空闲关闭、回退、实测耗时 |
| `docs/llm/build-release.md` | `docs/build-release.md` | 静态 + stripped 发布二进制、实测大小 |
| `docs/llm/build-tests.md` | `docs/build-tests.md` | 运行测试套件、fast/slow 标签、CI |
| `docs/llm/setup.md` | `docs/setup.md` | 通过 `ccb setup` 配置工具链、解析顺序、setup 步骤、最终验证、`--upgrade`/`--print-env`、代理限制 |
| `docs/llm/setup-tools.md` | `docs/setup-tools.md` | 安装缺失的 JDK、kotlinc、NDK、r8、主机工具、第三方 Termux 仓库、自定义工具路径、下载缓存 |
| `docs/llm/maven.md` | `docs/maven.md` | 声明 Maven 坐标、传递依赖解析、缓存与校验和、锁文件、限制、故障排查 |
| `docs/llm/modules.md` | `docs/modules.md` | 声明库模块、图规则、构建流程、R 语义、增量构建、限制 |
| `docs/llm/library.md` | `docs/library.md` | 独立库项目、路径/Maven 消费、资源合并与命名空间、`check` 报告 |
| `docs/llm/preview.md` | `docs/preview.md` | `ccb preview` 界面约定：配置、flow 文件、构建门禁、输出与退出码、JSON 形态、实际示例 |
| `docs/llm/preview-handlers.md` | `docs/preview-handlers.md` | 点击处理函数：派生边、条件导航、XML android:onClick、NavController 与导航图、绑定、优先级、未知情况、作用域限制 |
| `docs/llm/preview-real-code.md` | `docs/preview-real-code.md` | 从真实 Kotlin Activity 得到屏幕和边、与草图合并、无草图时的线框图、限制 |
| `docs/llm/preview-scenarios.md` | `docs/preview-scenarios.md` | 场景：声明的运行时事实与合并规则 |
| `docs/llm/preview-traces.md` | `docs/preview-traces.md` | 轨迹：记录的运行时路径、格式、渲染规则 |
| `docs/llm/preview-styles.md` | `docs/preview-styles.md` | CSS 样式、布局模型、lint 诊断 |
| `docs/llm/preview-modes.md` | `docs/preview-modes.md` | Diff、walk、watch 模式 |
| `docs/llm/preview-lsp.md` | `docs/preview-lsp.md` | LSP 服务器、tree-live 模式 |
| `docs/llm/diagnostics.md` | `docs/diagnostics.md` | CCB-E、CCB-P 诊断代码完整目录与退出码 |
| `docs/llm/json-protocol.md` | `docs/json-protocol.md` | `--json` 事件流、`--explain` 与 `--dry-run` |
| `docs/llm/device-setup.md` | `docs/device-setup.md` | 通过 Tailscale 无线调试连接设备、adb 安全、无设备时的行为 |

## 未翻译的文件

- `requirement.md`（仓库根目录）：唯一的英文规范，特意不翻译；它是权威的规格说明。

## 约定

- 每个中文文档开头都有一行英文注释，说明它是 LLM 工具文档，以及它对应的英文原文。
- 正文为简体中文；代码块、命令、标识符、文件路径和错误代码（如 `CCB-E1001`）保持英文原文，不翻译。
- 标题与英文原文一一对应，顺序相同；英文原文删除的小节，中文版同样删除；英文原文新增的小节，中文版同样新增。
- 工具术语按标识符保留英文（如 daemon、minify、dexcheck）；`minification` 译为精简。
- 如中英文版本出现不一致，以英文人类文档和 `requirement.md` 为准。
