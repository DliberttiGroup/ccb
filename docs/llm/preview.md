<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview.md. -->

# CCB Preview——UI 约定

[文档索引](index.md)

`ccb preview` 为遵循本约定的项目渲染终端线框图。它是一个带图的 linter，而不是模拟器：它从不执行 Kotlin、不求值条件、不取数据、不做动画。约定之外的任何内容都会告警并回退；它从不崩溃，也从不猜测。

子页面：[处理函数](preview-handlers.md) · [真实代码](preview-real-code.md) · [场景](preview-scenarios.md) · [轨迹](preview-traces.md) · [样式 + 布局](preview-styles.md) · [模式](preview-modes.md) · [LSP](preview-lsp.md)

## 配置

```toml
[preview]
styles = "styles"   # directory of CSS files
flow = "flow"       # directory of Kotlin flow files
resources = "src/main/res/values"   # optional: values directory holding strings.xml / colors.xml
layout = "res/layout"     # optional: layout XML dir or file
nav_graph = "res/navigation"  # optional: nav-graph XML file or dir
phone_width = 390   # dp, informational: scales px values to columns
phone_height = 844  # dp, informational
strict = false      # true: subset warnings become errors
gate = false        # true: preview errors fail ccb check / ccb build
```

- `layout` / `nav_graph` 是覆盖项。没有该键时，preview 按顺序尝试：`<paths res>/layout` 和 `<paths res>/navigation`，然后是 `<resources>/layout` 和 `<resources>/navigation`。每个候选都会做存在性检查；缺失的候选会落到下一个。没有任何候选意味着输入集为空，而不是错误。已配置但不存在的路径是配置错误（CCB-E1006）。值可以是一个文件，也可以是一个目录（其下所有的 `*.xml`，按排序顺序）。注意这个陷阱：`<resources>/layout` 只有在 `resources` 是 res 根目录时才存在——用标准的 values 目录值时它永远不会匹配，因此发现过程依赖 `[paths] res`（它没有默认值；请设置它，例如 `src/main/res`）或显式键。两者都没有时，preview 会静默地找不到任何 layout 或 graph，仍以退出码 0 退出，graph 导航未知，layout 绑定的屏幕不可达。
- `resources` 如果设置，必须是已存在的目录。它存放 `strings.xml` / `colors.xml`，用子集解析器解析：`<string name="n">v</string>`、`<color name="n">#rrggbb</color>`。注释会被跳过；字符串值中的 `&amp; &lt; &gt; &quot; &apos;` 会被解码。缺失的文件就是一张空表。格式错误的条目会被跳过，并发出 CCB-P1016 告警（注明文件）；非十六进制的颜色值同样如此。解析在加载时、布局之前进行：标签整个就是 `@string/name` 时会被替换为解析出的字符串；`color` 或 `background-color` 整个就是 `@color/name` 时会在规则加载时被替换为解析出的十六进制值，并在 CCB-P1018 对比度检查中算作已解析。未解析的引用会发出 CCB-P1016 告警，字面文本会按原样保留。
- `styles` 和 `flow` 各自都是可选的，但两者都没有的项目属于“preview 不适用”（退出码 0）——除非它有真实的 Kotlin 源码：一个没有 styles 也没有 flow 的 `[preview]` 小节，只要 `[paths] kotlin`（或其回退项）存在，仍然可以 preview。完全没有 `[preview]` 小节时，`ccb preview` 会打印 "preview not applicable to this project" 并退出，退出码为 0。
- 框宽度为 `--width`（默认 40 列，最小 24——更小的值会钳制并告警）。

## Flow 文件：屏幕与组件

Flow 文件是 `flow` 目录下的 Kotlin 文件。Preview 只解析结构——不做类型检查、不编译、不运行。

- 顶层 `fun Name(` 是一个名为 `Name` 的屏幕。在屏幕内，组件调用按花括号嵌套。
- 已知组件（大小写不敏感）：`Column`、`Row`、`Text`、`Button`、`List`、`TextField`、`Image`、`Box`、`Spacer`。其他任何组件都会成为通用框并告警（CCB-P1012）——它仍然会渲染，并标上其类型。
- 组件的标签是它的第一个字符串参数：`Text("My Notes")`、`Button("Save", ...)`。
- 导航是组件参数或代码块内的任何 `nav("Target")` 或 `navigateTo("Target")` 调用。边从外围组件的标签处记录。动态目标（变量、模板、任何非字面量）会成为 `unknown` 边——会显示出来，不做解析，也不是错误。
- 目标之后的参数会记录在边上：不含 `$` 的单个字符串字面量就是其字面文本，`name = <不含 $ 的字符串字面量 | 数字>` 是按原样书写的原始 `name = value` 文本（从不求值），其他任何内容（标识符、带 `$` 的模板、表达式、调用）都是 `unknown`。带参数的动态目标保持 `target: null, unknown: true`，同时仍记录其参数。边按目标、unknown 标志和参数去重。树的格式是 `-> Detail("note-42", unknown) via "Open note"`；没有额外参数的边保持旧格式。
- `when (state) { ... }` 和 `if (cond) { ... }` 会成为以其条件为标签的分支节点。分支只显示，从不求值。`when` 条目（`is Loading -> ...`、`else -> ...`）会成为分支下的条目节点。`if` 后的 `else` 会成为标签为 `else` 的条目节点。
- 入口屏幕是名为 `Home` 的屏幕（如果存在），否则是按排序后的文件顺序声明的第一个屏幕。

```kotlin
fun Home() {
  Column {
    Text("My Notes")
    List("notes")
    Row {
      Button("Open note", onClick = { nav("Detail") })
      Button("New note", onClick = { nav("Form") })
    }
    Button("Settings", onClick = { nav("Settings") })
  }
}
```

## 构建门禁：`ccb check` / `ccb build` 在 preview 错误时失败

可选，默认关闭。用 ccb.toml 中的 `[preview] gate = true` 启用，或每次调用时用 `ccb check --preview-gate` / `ccb build --preview-gate`；`--no-preview-gate` 强制关闭。优先级：`--no-preview-gate` 胜过 `--preview-gate`，胜过配置键；同时传入两个标志是用法错误（退出码 2）。

- 在 `check` 中，于工具链验证之后；在 `build` 中，于任何流水线阶段之前，因此失败的门禁绝不会启动构建。
- 严重性：任何 preview ERROR 都会导致门禁失败；仅有 WARNING 绝不会。哪些代码是错误：见 [diagnostics.md](diagnostics.md#preview-exit-1-unless-noted)。
- 失败时退出码为 1，stderr 上每个失败的诊断一行简洁消息（`CCB-P1005: navigation to unknown screen 'Ghost'`）；不打印任何屏幕渲染。在 `--json` 模式下，失败仍以这些 stderr 行加非零退出码呈现，而不是 JSON 事件。
- 没有可用 `[preview]` 小节的项目会静默通过。当配置加载本身失败时，门禁会被跳过，改为报告正常的配置错误（check 退出码为 2；build 的流水线报告它）。

## 输出与退出码

- `ccb preview`——每个屏幕的 wireframe，然后是逻辑树，然后是诊断。`ccb preview <screen>`（或 `--screen NAME`）——一个屏幕。
- `--json`——同样的树和诊断，以结构化数据呈现（见下文）。`--ascii`——用 ASCII 框线字符代替 Unicode。颜色来自 CSS 颜色近似到 ANSI 256 色码，除非设置了 `NO_COLOR`、终端是 dumb 终端，或给出了 `--ascii`。
- `--output FILE`——将渲染写入 FILE 而不是 stdout：人类可读渲染，或同时给出 `--json` 时的 JSON 对象。Stdout 什么都不输出；诊断仍输出到 stderr，退出码不变（preview 错误仍以退出码 1 退出且渲染已写入；无法写入的输出文件退出码为 2）。
- `--device phone|tablet`——在 `--width` 之上的框宽预设：phone = 40 列（默认值），tablet = 80 列。显式的 `--width` 总是胜过 `--device`；任何其他值都是用法错误。
- `--storyboard "A,B,C"`——渲染一段旅程，而不是整个应用：按旅程顺序排列的屏幕名（允许重复，每次出现都会再次渲染）。人类可读输出在每个框之前打印 `Journey N/M: <Name>`，然后是正常的逻辑树，然后是诊断。`--json` 在 `screens` 数组中只保留旅程屏幕，按旅程顺序排列（重复项作为单独条目）。名字会去掉周围空白。错误（退出码 2）：未知的屏幕名、空列表、空元素（`"Home,,Detail"`）或纯空白元素。冲突（退出码 2）：`--storyboard` 与 `--screen`、`--walk`、`--watch`、`--diff` 或 `--update-goldens` 组合。它与 `--ascii`、`--width`、`--device`、`--output`、`--strict` 和 `--json` 组合。
- `--update-goldens`——从当前渲染重新生成 `tests/golden/preview/` 下已检入的 golden 文件（每个屏幕一个 `<Screen>.txt` 框、`logic-tree.txt`、`preview.json`）。框 golden 使用当前的 `--ascii` 设置且无颜色。将 `CCB_GOLDEN_DIR` 设置为临时目录，可在不触碰真实 golden 的情况下测试；否则通过从工作目录向上走到 `tests/golden/preview/` 来找到该目录。

退出码：见 [diagnostics.md](diagnostics.md)（0 正常，允许告警；1 preview 错误；2 配置错误）。在 `--strict` 下，子集告警会成为错误，因此它们同样以退出码 1 退出。

### JSON 形态（`--json`）

一个 JSON 对象，无尾随换行，确定性的（稳定顺序，无时间戳）。`screens` 按 flow 文件顺序排列；`boxes` 是屏幕的框树，根框为树的根；`branches` 从树中按深度优先收集；`diagnostics` 按发出顺序排列：

```json
{"event":"preview","version":2,
 "screens":[{"name":"Home","entry":true,"reachable":true,
   "boxes":{"type":"screen","label":null,"x":0,"y":0,"w":38,"h":18,
     "w_dp":390,"h_dp":180,
     "children":[{"type":"text","label":"My Notes","x":2,"y":2,
       "w":34,"h":1,"w_dp":349,"h_dp":10,"children":[]}]},
   "edges":[{"from":"Open note","target":"Detail","unknown":false,
     "args":["note-42"]}],
   "branches":[{"label":"when state",
     "entries":["is Loading","is Loaded","else"]}]}],
 "diagnostics":[{"code":"CCB-P1005","phase":"preview",
   "message":"navigation to unknown screen 'Ghost'","file":null,
   "line":-1}]}
```

`label`、`target` 和 `file` 在缺失时为 `null`；`line` 对于没有源码位置的诊断为 -1。JSON 模式下 stdout 只输出这个对象；人类可读模式的文字和诊断输出到 stderr 或被抑制。版本 2 相对版本 1 增加了每条边的 `"args"`（字符串或 null 的数组）和 `"branch"`（字符串或 null），以及每框的 `"w_dp"`/`"h_dp"`。使用 `--storyboard` 时，`screens` 数组只包含旅程屏幕，按旅程顺序排列。

## 实际示例

参考示例应用位于 `examples/preview-sample/`。运行它：

```sh
cd examples/preview-sample
ccb preview
ccb preview Detail
ccb preview --json
```

它已检入的渲染是 `tests/golden/preview/` 下的 golden 文件——测试套件将实际输出与它们做 diff，因此任何改变输出的渲染器更改都必须有意更新 golden，绝不能静默更新。
