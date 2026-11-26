<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview-handlers.md. -->

# 点击处理函数：派生边

[文档索引](index.md) · [Preview](preview.md)

派生边在逻辑树、walk 模式、storyboard、diff 和 JSON 中的渲染与 flow 声明的边完全相同（输出中没有任何 `derived` 标记），构建门禁也像对待其他边一样对待它——除了同一按钮的声明边总是优先于它（见优先级）。分支边增加一个标注：树和 walk 边列表中显示为 `-> Detail via "Open note" [if ready: then]`，JSON 中则作为边的 `"branch"` 字段（字符串，无条件边为 `null`）。

识别的形态（全部在屏幕 fun 体内——见作用域限制）：

| 形态 | 示例 |
| ----- | ------- |
| 尾随 lambda | `openButton.setOnClickListener { nav("Detail") }` |
| 带括号的 lambda | `openButton.setOnClickListener({ nav("Detail") })` |
| 属性赋值 | `openButton.onClick = { nav("Detail") }` |
| 直接方法引用 | `openButton.setOnClickListener(::onOpenClick)`，其中 `fun onOpenClick(v: View) { nav("Detail") }` 是一个顶层 fun——同一文件，或任何其他 flow 文件（当名字唯一时，整个项目的函数索引会解析跨文件引用；一个名字在两个 flow 文件中都有定义时会告警 CCB-P1024 且不做解析，绝不猜测）。先把引用存入变量的形态也能解析，仅限 plain 形态：`val h = ::onOpenClick`（或 `var`），然后 `setOnClickListener(h)` 或 `onClick = h`。带类型的形态（`val h: Handler = ::onOpenClick`）无法解析。 |
| 辅助函数调用 | 处理器体内可以用裸名调用辅助函数（`openDetail()`）；辅助函数的函数体会被扫描一层深（带上当前分支标注），因此辅助函数内的 nav 会成为该按钮的边。辅助函数体内的调用不会再跟进（深度上限），所以每条链都会终止，包括自递归；带点的调用（`x.helper()`）和未知名字的调用也不会跟进。 |

处理器体内识别的导航调用：`nav("X")`、`navigateTo("X")`、`startActivity(Intent(this, DetailActivity::class.java))`（目标是类字面量的简单名，严格按书写原样——不剥离后缀，不猜测映射），以及 NavController 的 `navigate(...)`（见下文）。

## 条件导航 → 分支边

处理器体内 `if`/`when` 分支体内的 nav/startActivity/navigate 调用会成为带分支标签标注的边。条件只做标注，从不求值。标签与 flow 分支节点标签一致：`if <cond>` 的条目为 `then` / `else`，`when <cond>` 的条目为条目文本（`is Loaded`、`else`、……）；无主语的 `when { ... }` 其条目只标注在 `when` 之下。完整标注是 `"<base>: <entry>"`，例如 `if ready: then`、`when state: is Loaded`。带花括号的函数体和单个无花括号语句都算数（`if (ready) nav("Detail")`、`is Loaded -> nav("Detail")`、`else nav("Form")`）；无花括号函数体本身就是条件语句时，整个跳过，因此其中的 nav 绝不会泄漏为无条件边。嵌套条件会用 `", "` 复合（`if x: then, if y: then`）；`else if` 链独立存在（`else if b: then`、`else if b: else`）——它的 else 分支意味着“外层条件也不成立”，所以与外层 `then` 标签复合是错误的。任何其他嵌套块（嵌套 lambda/回调、协程、任何内部块）内的 nav 仍然完全不产生边。flow 声明的分支（组件树中的 `when`/`if` 节点，不在处理器体内）内的 nav 调用保持现有行为：从外围组件出发的无标注边。

处理器边上的字面量参数完全复用 flow 声明的参数机制：`nav("Detail", "note-42")` → 参数 `["note-42"]`，`nav("Detail", id = 7)` → 参数 `["id = 7"]`（具名，按书写原样显示）。非字面量参数是一个 `unknown` 参数条目，而不是被丢弃的边。非字面量目标仍是未知边（见结果表）。

## XML `android:onClick`

在 layout XML 文件中声明了 `android:onClick="methodName"` 的按钮会解析为代码中同名的处理函数：该函数内的 nav 调用会从该按钮产生派生边。layout 解析器是一个子集解析器（不用 XML 库）：它识别标签本地名以 `Button` 结尾（`Button`、`AppCompatButton`、`ImageButton`、……）且带有 `android:onClick` 和 `android:id` 属性的元素。`android:id` 的值 `@+id/x` 和 `@id/x` 都绑定在 `x` 之下。从 view id 到标签的绑定复用 `[preview.bindings]`（同一张表，以 view id 为键，例如 `open_button = "Open note"`），因此 layout 按钮和监听器接收者可以共用一个标签。处理器 fun 必须是 flow 文件中的顶层 fun（任何 flow 文件，不一定是声明该屏幕的文件）；与 `::` 处理器一样，它不是屏幕。它的派生边会附加到每个组件树中包含该绑定标签的屏幕上。Layout 文件按 `layout` 设置/回退规则定位（[preview.md](preview.md)）。XML 注释会被跳过；子集解析器不识别的标签（包括非 Button 元素上的 `onClick`）根本不是附加点——没有边，也没有告警。分发在 `@string` 解析之前比较标签，因此标签为 `@string/name` 的组件只有在绑定键就是字面量 `@string/name` 时才匹配。

## NavController 与导航图

处理器体内（仅在处理器体内——flow 中其他位置的 `navigate()` 调用遵循与任何非处理器语句相同的无边规则）的 `navigate(...)` 调用会通过导航图文件解析为边：

- `navigate("Detail")`——字符串形态，与 `nav("Detail")` 一样的字面量目标。
- `navigate(R.id.detail)`——id 形态：`detail` 在导航图表中查找；目标是它映射到的屏幕名。
- `navigate(R.id.detail, "note-42")`——额外参数复用 nav 参数分类。

导航图表来自 `nav_graph` 设置/回退规则（[preview.md](preview.md)）：任何同时带有 `android:id` 和 `android:name` 的元素都是一个目标，将 id 条目名映射到规范化后的类名。规范化会剥离 `[project] package` 前缀和任何前导点，然后取简单名，因此 `.DetailActivity`、`com.example.DetailActivity` 和 `DetailActivity` 是同一目标。没有匹配 flow 屏幕的目标会自成一个节点，在逻辑树中标记为 `[graph]`，在 JSON 中标记为 `"graph_only":true`：一个布局未知的节点，不是错误，也绝不会触发空屏幕告警。目标匹配在两个方向上都是后缀感知的：`DetailActivity` 匹配 `Detail` 屏幕，反之亦然，但 `DetailActivity` 不匹配 `DetailFragment`。重复 id 时第一个条目获胜。`<action>` 元素没有 `android:name`，不是目标，因此导航到 action id 是未知的。目标 id 在图中没有条目，或者根本没有 graph 文件时，`navigate()` 调用就是未知边——绝不猜测目标。非字面量的 `navigate()` 目标（变量、表达式）同样是未知边。导航目标只有在什么都不匹配时才是未知的（CCB-P1005）：没有屏幕、没有 graph 目标，也没有在配置的 manifest 中声明的 activity。

## 将处理函数绑定到按钮：`[preview.bindings]`

绑定只通过显式表进行——不按声明顺序猜测，不做模糊名字匹配：

```toml
[preview.bindings]
openButton = "Open note"
```

绑定键是附加点的 RECEIVER 标识符（点链中方法前的最后一个标识符，因此 `binding.openButton.setOnClickListener` 绑定在 `openButton` 之下），而不是处理函数 fun 的名字。已绑定处理函数的 nav 调用会成为 `from` = 绑定组件标签的派生边。表中的非字符串值是配置错误（CCB-E1007）。

## 优先级：声明边胜过派生边

解析完所有 flow 文件后，一次后处理会针对同一按钮（相同的 `from`）协调派生边与声明边：

- 目标相同 → 派生边被静默丢弃（去重）。
- 目标不同 → CCB-P1020（注明按钮和两个目标），声明边保留，派生边被丢弃。
- 任何种类的声明边都胜过与之不完全匹配的派生边（包括派生的未知边）。

## 未知情况：精确结果，绝不猜测

| 处理函数体 | 结果 |
| ---- | ---- |
| 接收者没有 `[preview.bindings]` 条目 | CCB-P1019（"handler found, button not bound"，注明接收者），无边——即使体内有字面量 nav 调用 |
| 处理函数体内直接有非字面量/计算出的目标的 nav（`nav(target)`、模板） | 未知边（`from` = 绑定标签，`target: null, unknown: true`），与动态的 flow 声明 nav 完全相同 |
| 处理函数体内的 `if`/`when` 分支体中的 nav（带花括号或单个无花括号语句） | 带分支标注的边：`-> Detail via "Open note" [if ready: then]`，条件只做标注，从不求值 |
| 处理函数体内任何其他嵌套花括号块中的 nav（嵌套 lambda/回调、协程、任何内部块） | 完全没有边，连未知边都没有——这是异步领域，绝不猜测——但 CCB-P1025 会带 file:line 告警，因此该 nav 是可见的，而不是被静默丢弃 |
| 处理函数体内带字面量参数的 nav（`nav("Detail", "note-42")`、`nav("Detail", id = 7)`） | 带参数的边，与带参数的 flow 声明 nav 完全相同 |
| 处理函数体内带非字面量参数的 nav（`nav("Detail", noteId)`） | 带一个 `unknown` 参数条目的边，与带非字面量参数的 flow 声明 nav 相同 |
| 处理函数体内的 `navigate(R.id.x)`，`x` 在导航图中 | 通向图中 `x` 对应屏幕名的边；名字已规范化（剥离包前缀和前导点），没有 flow 屏幕的目标会成为 `[graph]` 节点，不是错误 |
| 任何边的目标位置上没有 flow 屏幕的导航图目标 | 该目标是一个节点（`[graph]`，JSON 中 `"graph_only":true`），像任何屏幕一样可达；没有 CCB-P1005，该节点也没有 CCB-P1007 |
| 导航目标匹配 manifest activity，但没有屏幕或 graph 目标 | 没有 CCB-P1005（manifest activity 豁免了这条边）；不会为它创建节点 |
| 处理函数体内的 `navigate(R.id.x)`，`x` 不在图中，或根本没有 graph 文件 | 未知边（`from` = 绑定标签，`target: null, unknown: true`），绝不猜测目标 |
| 处理函数体内的 `navigate("Detail")` | 通向 `Detail` 的边，与 `nav("Detail")` 相同 |
| 处理函数体之外的 `navigate(...)` | 没有边——NavController 调用遵循与处理函数归属相同的规则 |
| XML `android:onClick` 按钮，已绑定，找到了处理函数 fun | 从绑定标签出发的派生边，附加到每个包含该标签的屏幕上 |
| XML `android:onClick` 按钮，其 view id 没有 `[preview.bindings]` 条目 | layout 自身的 `android:text` 命名该按钮；只有既无绑定又无文本的按钮才会告警 CCB-P1019，无边（与未绑定的监听器接收者使用同一代码） |
| XML `android:onClick` 按钮，已绑定，但其标签不在任何屏幕中 | CCB-P1021，无边 |
| XML `android:onClick` 方法没有匹配的 fun，或处理函数没有 nav | 无边，无告警 |
| 仅调用共享辅助函数的处理函数（`openDetail()`），nav 在辅助函数内 | 归属于该按钮的边：辅助函数体被扫描一层深，带上当前分支标注；辅助函数体内的调用不会再跟进，因此第二层辅助函数的 nav 不产生边——但 CCB-P1025 会为此告警，而不是静默 |
| 没有类字面量的 `startActivity`（`startActivity(intent)`） | 未知边，绝不猜测目标 |
| 引用另一个文件中的 fun 的方法引用 | 当名字在各文件间唯一时，通过整个项目的函数索引解析；同一文件内的定义优先于跨文件定义 |
| 引用在两个或更多 flow 文件中都有定义的 fun 的方法引用或辅助函数调用 | 无边，CCB-P1024（每个文件和名字一次）——绝不猜测 |
| 引用没有任何 fun 的方法引用 | 无边，无告警——在 flow 文件中。在真实代码中，监听器的 `::handler` 在任何已扫描的 Activity 中都没有定义时会告警 CCB-P1025，每个方法一次。 |

CCB-P1019、CCB-P1020、CCB-P1021、CCB-P1024 和 CCB-P1025 始终是告警——即使在 `--strict` 下也是如此——因此它们绝不会导致构建门禁失败。

## 作用域限制（特意如此设计）

- 处理函数仅在屏幕 fun 体内识别（定义屏幕的顶层 `fun Name` 函数体）。写在别处的处理函数（例如任何屏幕 fun 之外的 Activity `onCreate`）会被忽略，不告警——在 flow 文件中是如此。真实代码会在应用本身中识别 `onCreate` 处理函数（见 [真实代码](preview-real-code.md)）；flow 文件的规则不变。
- 具名处理函数必须是顶层 fun。作为处理函数或辅助函数被引用的顶层 fun 不是屏幕：通过索引做引用计数，取消任何通过 `::` 在 fun 体内任何位置、从 setOnClickListener/onClick lambda 块、或从处理函数 fun 体内被引用的 fun 的资格（迭代至不动点，因此辅助函数链也计入）。它不会出现在屏幕列表中，其 nav 目标只能通过引用它的按钮来解析。直接在屏幕 UI 体内的裸调用不会取消资格：那是屏幕组合（作为组件的 `Detail()`），被组合的屏幕仍然是屏幕。
- 被忽略调用的 lambda 块内的附加点（例如 `foo { ... }` 参数块内，解析器只在其中扫描 nav）会回退到现有的通用扫描，而不是处理函数归属。
- XML `android:onClick` 处理函数 fun 与 `::` 处理函数不同，可以位于任何 flow 文件中（附加点是 layout 文件，而不是屏幕体）；它们仍然只能是顶层 fun，绝不能是嵌套 fun。
- NavController `navigate()` 仅在处理函数体内识别（监听器 lambda、`onClick` 赋值、具名处理函数和 `android:onClick` 处理函数 fun）。任何其他位置的 `navigate()` 调用都不是边。
