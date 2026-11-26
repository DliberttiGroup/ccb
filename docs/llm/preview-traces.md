<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview-traces.md. -->

# 轨迹：记录的运行时路径

[文档索引](index.md) · [Preview](preview.md)

轨迹是一个有序的导航步骤列表，每个步骤对应一次导航——从 walk 中记录，或（以后）由一个微型应用内日志器发出。没有内置的日志器；下面的格式是编写日志器所依据的契约。同样的合并规则也适用于 [场景](preview-scenarios.md)：标记走过的路径，填充走过边的未知参数，并选择分支条目。从不覆盖静态字面量，从不解析动态目标，也从不隐藏未选的分支条目。

## 格式（JSON，版本 1）

```json
{"version": 1, "steps": [
  {"screen": "Home", "edge": "Open note", "branch": null,
   "args": ["note-42"]},
  {"screen": "Detail", "edge": null, "branch": "is Loaded",
   "args": []},
  {"screen": "Detail", "edge": "Back", "branch": null,
   "args": ["42"]}
]}
```

| 字段 | 含义 |
| ----- | ------- |
| `screen`（必需字符串） | 导航发生的屏幕。 |
| `edge`（字符串或 null） | 导航所来自的按钮标签。Null 表示纯选择步骤：选了分支但没有导航。返回导航从不记录——轨迹是走过的前进路径，不是按键日志。 |
| `branch`（字符串或 null） | 该步骤所处的分支条目标签（`is Loaded`、`else`、`then`），无条件时为 null。 |
| `args`（字符串或 null 的数组，可以省略） | 按位置映射到所走边的参数上——步骤参数 k 在该槽未知时填充边的参数 k。Null 条目会让其槽保持未知。与场景参数（对每个带该按钮标签的边，按从左到右填充未知槽）不同，轨迹参数只按位置填充所走的边。 |

`version` 可以省略；如果存在，必须为 1。未知字段会被忽略。结构问题（无法解析的 JSON、错误的类型、缺失 `screen` 或 `steps` 数组）会以退出码 2 拒绝该文件；不匹配任何内容的步骤会告警（CCB-P1023）并被跳过。

用 `ccb preview --walk --record FILE` 记录一次 walk（walk 退出时写轨迹；写失败时退出码为 2），并用 `ccb preview --trace FILE` 以叠加方式渲染（人类可读渲染和 `--json`；`--trace` 拒绝 `--walk`、`--watch`、`--diff`、`--storyboard` 和 `--update-goldens`，并与 `--scenario` 组合——场景在加载时应用，轨迹在其后应用，轨迹只填充仍然未知的槽）。

## 渲染规则

- 走过的边会被高亮：树在边行后追加 ` [trace]`（`-> Detail("note-42") via "Open note" [trace]`），JSON 在边上带有 `"taken": true`，顶层带有 `"trace": {"steps": N}`（N 是文件中的步骤数，无论每个步骤是否匹配）。
- 分支选择在 branches 行中标记为 ` [trace]`，被选分支中轨迹未走过的条目会被灰显——在纯文本中用 `[not taken]` 标记（`branches: when state [is Loading [not taken], is Loaded [trace], else [not taken]]`）。JSON 将选择作为分支的 `"picked": {"entry": ..., "provenance": "trace"}`；不同条目的轨迹选择胜过场景选择。
- 填充的参数保留其字面文本，在树中带有 ` [trace]`（`("42" [trace])`），在 JSON 的平行 `"args_provenance"` 数组中带有 `"trace"`。一个槽最多带一个来源的标记：场景填充的槽绝不会被轨迹覆盖。
- 匹配按标签进行，与 walk 的分支规则完全相同：步骤命名一个屏幕和一个按钮标签，带分支限定的步骤会标记每个按钮标签出现在指定分支条目内的边。当多个边共享一个按钮标签时，它们都会被标记为已走过——轨迹记录的是标签，而不是边的身份。分支条目存在但其中没有该按钮的步骤仍会将边标记为已走过并告警（CCB-P1023）。
- wireframe 不受轨迹影响：走过的路径是树和 JSON 的属性，不是框的属性。

参考示例的已记录 walk 在 `tests/golden/preview/trace-walk/` 下做 golden 测试（逻辑树 + JSON）。
