<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview-scenarios.md. -->

# 场景：声明的运行时事实

[文档索引](index.md) · [Preview](preview.md)

场景是项目的运行时事实，在 ccb.toml 中声明。它从不求值任何东西：它是在渲染时合并到静态树之上的数据输入。它只填充未知项（未知参数、未选分支、占位行、空字段）并选择分支。它从不覆盖静态字面量，从不解析动态目标，也从不隐藏未选的分支条目。同样的规则也适用于 [轨迹](preview-traces.md)——轨迹只按位置填充所走的边；场景则按从左到右填充每个匹配边的未知槽。

TOML 子集没有内联表，因此四个映射是 `[preview.scenario.<name>]` 下的子表（每个条目一个键；场景名是单个段，不含点）：

```toml
[preview.scenario.loaded.branches]
Detail = "is Loaded"      # screen -> when/if entry label
Settings = "else"
[preview.scenario.loaded.lists]
notes = ["Buy milk", "Call Ana"]  # list label -> row strings
[preview.scenario.loaded.fields]
Title = "Milk"            # textfield label -> text
[preview.scenario.loaded.args]
Back = ["42"]             # button label -> fill unknown arguments
```

用 `ccb preview --scenario NAME` 渲染（人类可读渲染、`--json` 和逻辑树都接受它；`--walk` 在后续 chunk 中接受它，而 `--watch`、`--diff`、`--storyboard` 和 `--update-goldens` 会拒绝它，而不是静默地在没有它的情况下渲染）。未知的场景名是用法错误（退出码 2），会列出可用的场景。

## 合并规则

- 分支：指定屏幕的指定条目被标记为已选。每个条目仍然渲染；选择是一个标记，而不是过滤。树：`branches: when state [is Loading, is Loaded [scenario], else]`；JSON：分支的 `"picked": {"entry": ..., "provenance": "scenario"}`。
- 列表：行只替换无子项列表的 4 个占位行。已有静态子项的列表保持原样。行在 wireframe 中渲染为普通行，并在 JSON 中带有 `"provenance": "scenario"`。因为行是普通行，超出其框宽的行会被裁剪并告警 CCB-P1009，每行一个——在完整场景下，裁剪告警可能淹没真正的诊断，因此请保持行字符串简短或加宽列表。
- 字段：内容是附加的，绝不是替换——静态标签保留，wireframe 显示 `[Title: Milk]`。JSON 带有 `"value"` 和 `"value_provenance": "scenario"`。
- 参数：值按从左到右的顺序填充未知（NULL）参数槽，每个匹配的边独立填充——同一按钮标签在两个屏幕上时，会用相同的值填充每个屏幕的边。静态字面量绝不触碰，动态目标绝不解析，超出每条边槽位的值会被忽略（各屏幕的元数本来就可能不同）。树：`("42" [scenario])`；JSON：平行的 `"args_provenance"` 数组（静态条目为 null，填充的为 `"scenario"`）。JSON 顶层还带有场景名 `"scenario"`。
- 键按渲染后的标签匹配（框经过 @string/@color 解析后；边源标签按树中显示的为准）。
- 来源汇总：场景提供的值在树中标记为 `[scenario]`（分支选择和填充的参数以内联方式标记；列表行和字段内容在每个屏幕的 `scenario:` 行上标记，因为它们没有自己的树行）。wireframe 将场景值显示为内容；树和 JSON 带有标记。静态值保持无标记，没有场景时输出与之前逐字节相同。
- 不匹配是告警，绝不是错误，也绝不静默：当场景命名了未知的屏幕、分支条目、列表或字段；当列表已有静态行；当按钮标签不匹配任何边；当按钮的边没有未知参数可填充时，触发 CCB-P1022。该条目会被跳过，渲染继续。场景绝不会导致构建门禁失败。
- 配置错误（值类型错误、未知的场景小节、直接位于 `[preview.scenario.<name>]` 下的键）会像任何其他配置错误一样使配置加载失败（CCB-E1007 / CCB-E1001）。

参考示例自带一个场景：`examples/preview-sample` 声明了 `[preview.scenario.loaded]`，其场景渲染在 `tests/golden/preview/scenario-loaded/` 下做 golden 测试。
