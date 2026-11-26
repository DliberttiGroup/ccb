<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file explains the layout of docs/llm/. -->

# CCB LLM 文档布局

本文档说明 `docs/llm/` 目录的布局。人类可读的英文文档位于 `docs/`；本目录是其中文版本，内容与英文版相同，供 LLM / AI 代理阅读。每个中文文档开头都有一行英文注释，注明它对应的英文原文。

## 文件对照

| LLM 文档（中文） | 人类文档（英文） | 内容 |
|---|---|---|
| `docs/llm/README.md` | `README.md`（仓库根目录） | 本布局说明；项目总览见英文 README |
| `docs/llm/build.md` | `docs/build.md` | 构建流水线、缓存、可复现构建、kotlinc daemon、静态发布二进制、运行测试 |
| `docs/llm/setup.md` | `docs/setup.md` | 通过 `ccb setup` 配置工具链、JDK / kotlinc / NDK / r8 安装、主机工具、代理限制、自定义工具路径 |
| `docs/llm/maven.md` | `docs/maven.md` | 声明 Maven 坐标、传递依赖解析、缓存与校验和、锁文件、限制、故障排查 |
| `docs/llm/modules.md` | `docs/modules.md` | 声明库模块、图规则、构建流程、R 语义、增量构建、限制 |
| `docs/llm/preview.md` | `docs/preview.md` | `ccb preview` 界面约定：flow 文件、点击处理器、真实代码、场景、轨迹、样式、布局模型、lint 诊断、输出模式、构建门禁、diff、walk、watch、LSP 服务器、tree-live |
| `docs/llm/diagnostics.md` | `docs/diagnostics.md` | CCB-E、CCB-P 诊断代码完整目录与退出码 |
| `docs/llm/json-protocol.md` | `docs/json-protocol.md` | `--json` 事件流、`--explain` 与 `--dry-run` |
| `docs/llm/device-setup.md` | `docs/device-setup.md` | 通过 Tailscale 无线调试连接设备、adb 安全、无设备时的行为 |

## 未翻译的文件

- `requirement.md`（仓库根目录）：唯一的英文规范，特意不翻译；它是权威的规格说明。
- `NOTES.md`：代理的工作笔记，不是用户文档，不翻译。
- `docs/` 下的计划文档（`deeper-coverage-plan.md`、`fidelity-behavior-plan.md`、`m5-plan.md`、`preview-correctness-plan.md`、`preview-runtime-plan.md`、`real-world-testing.md`）：工作计划，不是用户文档，不翻译。

## 约定

- 每个中文文档开头都有一行英文注释，说明它是 LLM 工具文档，以及它对应的英文原文。
- 正文为简体中文；代码块、命令、标识符、文件路径和错误代码（如 `CCB-E1001`）保持英文原文，不翻译。
- 标题与英文原文一一对应，顺序相同；英文原文删除的小节，中文版同样删除；英文原文新增的小节，中文版同样新增。
- 工具术语按标识符保留英文（如 daemon、minify、dexcheck）；`minification` 译为精简。
- 如中英文版本出现不一致，以英文人类文档和 `requirement.md` 为准。
