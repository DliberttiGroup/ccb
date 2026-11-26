<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview.md. -->

# CCB Preview——UI 约定

Preview（`ccb preview`）为遵循本约定的项目渲染终端线框图。它是一个带图的 linter，而不是模拟器：它从不执行 Kotlin、不求值条件、不取数据、不做动画。约定之外的任何内容都会告警并回退；它从不崩溃，也从不静默猜测。

## 在 ccb.toml 中声明 preview

```toml
[preview]
styles = "styles"   # directory of CSS files
flow = "flow"       # directory of Kotlin flow files
resources = "res"   # optional: directory of strings.xml / colors.xml
layout = "res/layout"     # optional: directory (or file) of layout XML, for android:onClick (Slice 3)
nav_graph = "res/navigation"  # optional: nav-graph XML file or directory (Slice 3)
phone_width = 390   # dp, informational: scales px values to columns
phone_height = 844  # dp, informational
strict = false      # true: subset warnings become errors
gate = false        # true: preview errors fail ccb check / ccb build
```

`layout` 和 `nav_graph` 是覆盖项，不是必需项。当某个键缺失时，preview 按顺序回退：`[paths] res` 已设置时用 `<paths res>/layout` / `<paths res>/navigation`，然后 `resources` 已设置时用 `<resources>/layout` / `<resources>/navigation`。每个回退候选都会做存在性检查：不存在的候选会落到下一个，只有当所有候选都不存在时输入集才为空（没有 graph 文件或没有 layout 文件不是错误——见下面的结果表）。这一点很重要，因为 `resources` 通常指向 values 目录（`src/main/res/values`），而不是 res 根目录，所以 `<resources>/layout` 不会存在，也绝不能遮住正确的 `<paths res>` 回退。显式配置的键保持严格：配置的路径不存在就是配置错误（CCB-E1006）。`layout`/`nav_graph` 的值可以是单个文件，也可以是目录（目录会贡献其下所有的 `*.xml`，按排序顺序）。

`resources` 如果设置，必须是已存在的目录（与 `styles` 和 `flow` 一样）。它存放 Android values 文件 `strings.xml` 和 `colors.xml`，用一个极小的子集解析器解析（不用 XML 库）：`<string name="n">v</string>` 和 `<color name="n">#rrggbb</color>`。XML 注释会被跳过；字符串值中的 `&amp; &lt; &gt; &quot; &apos;` 会被解码。缺失的文件就是一张空表（项目可以只定义 strings、只定义 colors，或者两者都不定义）；格式错误的条目会被跳过，并发出 CCB-P1016 告警（注明文件），非十六进制的颜色值同样会被跳过并发出 CCB-P1016。解析在加载时、布局之前进行：

- 组件标签如果整个就是 `@string/name`（整个标签，不是内嵌文本），会被替换为解析出的字符串，因此裁剪宽度按解析后的文本计算。
- `color` 或 `background-color` 的值如果整个就是 `@color/name`，会在规则加载时被替换为解析出的十六进制值。解析出的 `@color` 值在 CCB-P1018 对比度检查中算作已解析。

未解析的引用（名字不在表中，或未配置 resources 目录）会发出 CCB-P1016 告警（注明该引用），字面文本 `@string/name` / `@color/name` 会被保留——按原样渲染，绝不崩溃，也绝不凭空编造值。

`styles` 和 `flow` 各自都是可选的，但两者都没有的项目属于“preview 不适用”（退出码 0）——除非它有真实的 Kotlin 源码（Phase 2）：一个没有 styles 也没有 flow 的 `[preview]` 小节，只要 `[paths] kotlin`（或其回退项）存在，仍然可以 preview。完全没有 `[preview]` 小节时，`ccb preview` 会打印 "preview not applicable to this project" 并退出，退出码为 0。终端框宽度为 `--width`（默认 40 列，最小 24——更小的值会钳制并告警）。

## Flow 文件：屏幕与组件

Flow 文件是 `flow` 目录下的 Kotlin 文件。Preview 只解析结构——不做类型检查、不编译、不运行任何代码。

- 顶层 `fun Name(` 函数就是一个名为 `Name` 的屏幕。
- 在屏幕内，组件调用按花括号嵌套。已知组件（按调用名大小写不敏感匹配）：`Column`、`Row`、`Text`、`Button`、`List`、`TextField`、`Image`、`Box`、`Spacer`。其他任何组件都会成为通用框并告警（CCB-P1012）——它仍然会渲染，并标上其类型。
- 组件的标签是它的第一个字符串参数：`Text("My Notes")`、`Button("Save", ...)`。
- 导航是组件参数或代码块内的任何 `nav("Target")` 或 `navigateTo("Target")` 调用。边从外围组件的标签处记录。动态目标（变量、模板、任何非字面量）会成为 `unknown` 边——会显示出来，不做解析，也不是错误。
- 目标之后的参数会记录在边上，并在逻辑树、walk 模式的边列表和 JSON 中显示：不含 `$` 的单个字符串字面量就是其字面文本，`name = <不含 $ 的字符串字面量 | 数字>` 是按原样书写的原始 `name = value` 文本（从不求值），其他任何内容（标识符、带 `$` 的模板、表达式、调用）都是 `unknown`。带参数的动态目标保持 `target: null, unknown: true`，同时仍记录其参数。边按目标、unknown 标志和参数去重。树的格式是 `-> Detail("note-42", unknown) via "Open note"`；没有额外参数的边保持旧格式。
- `when (state) { ... }` 和 `if (cond) { ... }` 会成为以其条件为标签的分支节点。分支只显示，从不求值。`when` 条目（`is Loading -> ...`、`else -> ...`）会成为分支下的条目节点。`if` 后的 `else` 会成为标签为 `else` 的条目节点。
- 入口屏幕是名为 `Home` 的屏幕（如果存在），否则是按排序后的文件顺序声明的第一个屏幕。

## Flow 文件：点击处理器（Slices 1–3，派生边）

生产 Kotlin 很少写 `Button { nav() }`；它写的是监听器。Slices 1–3 识别点击处理器形态并从中派生边。派生边在逻辑树、walk 模式、storyboard、diff 和 JSON 中的渲染与 flow 声明的边完全相同（输出中没有任何 `derived` 标记），构建门禁也像对待其他边一样对待它——除了同一按钮的声明边总是优先于它（见下面的优先级）。分支边（Slice 2）增加一个可见标注：树和 walk 边列表中显示为 `-> Detail via "Open note" [if ready: then]`，JSON 中则作为边的 `"branch"` 字段（字符串，无条件边为 `null`）。

识别的形态（全部在屏幕 fun 体内——见下面的作用域限制）：

- 尾随 lambda：`openButton.setOnClickListener { nav("Detail") }`
- 带括号的 lambda：`openButton.setOnClickListener({ nav("Detail") })`
- 属性赋值：`openButton.onClick = { nav("Detail") }`
- 直接方法引用：`openButton.setOnClickListener(::onOpenClick)`，其中 `fun onOpenClick(v: View) { nav("Detail") }` 是一个顶层 fun——在同一文件内，或在任何其他 flow 文件内（当名字唯一时，整个项目的函数索引会解析跨文件引用；一个名字在两个 flow 文件中都有定义时会告警 CCB-P1024 且不做解析，绝不猜测）。先把引用存入变量的形态也能解析，仅限 plain 形态：`val h = ::onOpenClick`（或 `var`），然后 `setOnClickListener(h)` 或 `onClick = h`。带类型的形态（`val h: Handler = ::onOpenClick`）无法解析——这是已记录的限制。
- 辅助函数调用：处理器体内可以用裸名调用辅助函数（`openDetail()`），辅助函数的函数体会被扫描一层深（带上当前分支标注），因此辅助函数内的 nav 会成为该按钮的边。辅助函数体内的调用不会再跟进（深度上限），所以每条链都会终止，包括自递归；带点的调用（`x.helper()`）和未知名字的调用也不会跟进。

处理器体内识别的导航调用：照常有 `nav("X")` 和 `navigateTo("X")`，还有 `startActivity(Intent(this, DetailActivity::class.java))`，其中目标是类字面量的简单名，严格按书写原样（不剥离后缀，不猜测映射），以及（Slice 3）NavController 的 `navigate(...)`——见下文。

### Slice 2：条件导航 → 分支边

Slice 2 仅对 `if`/`when` 分支放宽 Slice 1 的 depth-0 规则：处理器体内 `if`/`when` 分支体内的 nav/startActivity/navigate 调用会成为带分支标签标注的边。条件只做标注，从不求值。分支标签与 flow 分支节点标签一致：`if <cond>` 的条目为 `then` / `else`，`when <cond>` 的条目为条目文本（`is Loaded`、`else`、……）；无主语的 `when { ... }` 其条目只标注在 `when` 之下。完整标注是 `"<base>: <entry>"`，例如 `if ready: then` 或 `when state: is Loaded`。带花括号的函数体和单个无花括号语句都算数（`if (ready) nav("Detail")`、`is Loaded -> nav("Detail")`、`else nav("Form")`）；无花括号函数体本身就是条件语句时，整个跳过，因此其中的 nav 绝不会泄漏为无条件边。嵌套条件会用 `", "` 复合标签（`if x: then, if y: then`）；`else if` 链独立存在（`else if b: then`、`else if b: else`）——它的 else 分支意味着“外层条件也不成立”，所以与外层 `then` 标签复合是错误的。任何其他嵌套块（嵌套 lambda/回调、协程、任何内部块）内的 nav 仍然完全不产生边。flow 声明的分支（组件树中的 `when`/`if` 节点，不在处理器体内）内的 nav 调用保持现有行为：从外围组件出发的无标注边。

### Slice 2（续）：处理器边上的字面量参数

带字面量参数的处理器 nav 调用会产生带参数的边，完全复用 flow 声明的参数机制：`nav("Detail", "note-42")` → 参数 `["note-42"]`，`nav("Detail", id = 7)` → 参数 `["id = 7"]`（具名，按书写原样显示）。非字面量参数是一个 `unknown` 参数条目，而不是被丢弃的边——与 flow 声明的 nav 规则相同。非字面量目标仍是未知边（见结果表）。

### Slice 3：XML android:onClick

在 layout XML 文件中声明了 `android:onClick="methodName"` 的按钮会解析为代码中同名的处理器函数：该函数内的 nav 调用会从该按钮产生派生边。layout 解析器是一个极小的子集解析器（不用 XML 库）：它识别标签本地名以 `Button` 结尾（`Button`、`AppCompatButton`、`ImageButton`、……）且带有 `android:onClick` 和 `android:id` 属性的元素。`android:id` 的值 `@+id/x` 和 `@id/x` 都绑定在 `x` 之下。从 view id 到 flow 组件标签的绑定复用 `[preview.bindings]`——同一张表，以 view id 为键（例如 `open_button = "Open note"`），因此 layout 按钮和监听器接收者可以共用一个标签。处理器函数必须是 flow 文件中的顶层 fun（任何 flow 文件，不一定是声明该屏幕的文件）；与 `::` 处理器一样，它不是屏幕。它的派生边会附加到每个组件树中包含该绑定标签的组件的屏幕上（即 layout 声明的那个按钮）。Layout 文件按上述 `layout` 设置/回退规则定位。XML 注释会被跳过；子集解析器不识别的标签（包括非 Button 元素上的 `onClick`）根本不是附加点——没有边，也没有告警。没有 `[preview.bindings]` 条目的 XML `android:onClick` 按钮由 layout 自身的 `android:text` 命名（Phase 2）；只有既无绑定又无文本的按钮才会告警 CCB-P1019，无边（与未绑定的监听器接收者使用同一代码）。分发在 `@string` 解析之前比较标签，与 Slice 1 的协调比较一致——因此标签为 `@string/name` 的组件只有在绑定键就是字面量 `@string/name` 时才匹配。

### Slice 3：NavController + 导航图

处理器体内（仅在处理器体内——flow 中其他位置的 `navigate()` 调用遵循与任何非处理器语句相同的无边规则）的 `navigate(...)` 调用会通过导航图文件解析为边：

- `navigate("Detail")`——字符串形态，与 `nav("Detail")` 一样的字面量目标。
- `navigate(R.id.detail)`——id 形态：`detail` 在导航图表中查找；目标是它映射到的屏幕名。
- `navigate(R.id.detail, "note-42")`——额外参数复用 nav 参数分类。

导航图表来自上述 `nav_graph` 设置/回退规则：任何同时带有 `android:id` 和 `android:name` 的元素都是一个目标，将 id 条目名映射到规范化后的类名。规范化会剥离 `[project] package` 前缀和任何前导点，然后取简单名，因此 `.DetailActivity`、`com.example.DetailActivity` 和 `DetailActivity` 是同一目标。没有匹配 flow 屏幕的目标会自成一个节点，在逻辑树中标记为 `[graph]`，在 JSON 中标记为 `"graph_only":true`：一个布局未知的节点，不是错误，也绝不会触发空屏幕告警。目标匹配在两个方向上都是后缀感知的：`DetailActivity` 匹配 `Detail` 屏幕，反之亦然，但 `DetailActivity` 不匹配 `DetailFragment`。重复 id 时第一个条目获胜。`<action>` 元素没有 `android:name`，不是目标，因此导航到 action id 是未知的。目标 id 在图中没有条目，或者根本没有 graph 文件时，`navigate()` 调用就是未知边——绝不猜测目标。非字面量的 `navigate()` 目标（变量、表达式）同样是未知边。导航目标只有在什么都不匹配时才是未知的（CCB-P1005）：没有屏幕、没有 graph 目标，也没有在配置的 manifest 中声明的 activity。

### 将处理器绑定到按钮：`[preview.bindings]`

flow 文件的组件标签是目标；代码的 view 标识符是来源。绑定只通过显式表进行——不按声明顺序猜测，不做模糊名字匹配：

```toml
[preview.bindings]
openButton = "Open note"
```

绑定键是附加点的 RECEIVER 标识符（点链中方法前的最后一个标识符，因此 `binding.openButton.setOnClickListener` 绑定在 `openButton` 之下），而不是处理器 fun 的名字。已绑定处理器的 nav 调用会成为 `from` = 绑定组件标签的派生边。表中的非字符串值是配置错误（CCB-E1007）。

### 优先级：声明边胜过派生边

解析完所有 flow 文件后，一次后处理会针对同一按钮（相同的 `from`）协调派生边与声明边：

- 目标相同 → 派生边被静默丢弃（去重）。
- 目标不同 → CCB-P1020（注明按钮和两个目标），声明边保留，派生边被丢弃。
- 任何种类的声明边都胜过与之不完全匹配的派生边（包括派生的未知边）。

### 未知情况：精确结果，绝不猜测

| 处理器体 | 结果 |
| ---- | ---- |
| 接收者没有 `[preview.bindings]` 条目 | CCB-P1019（"handler found, button not bound"，注明接收者），无边——即使体内有字面量 nav 调用 |
| 处理器体内直接有非字面量/计算出的目标的 nav（`nav(target)`、模板） | 未知边（`from` = 绑定标签，`target: null, unknown: true`），与动态的 flow 声明 nav 完全相同 |
| 处理器体内的 `if`/`when` 分支体中的 nav（带花括号或单个无花括号语句） | 带分支标注的边（Slice 2）：`-> Detail via "Open note" [if ready: then]`，条件只做标注，从不求值 |
| 处理器体内任何其他嵌套花括号块中的 nav（嵌套 lambda/回调、协程、任何内部块） | 完全没有边，连未知边都没有——这是异步领域，绝不猜测——但 CCB-P1025 会带 file:line 告警（Chunk C），因此该 nav 是可见的，而不是被静默丢弃 |
| 处理器体内带字面量参数的 nav（`nav("Detail", "note-42")`、`nav("Detail", id = 7)`） | 带参数的边，与带参数的 flow 声明 nav 完全相同（Slice 2） |
| 处理器体内带非字面量参数的 nav（`nav("Detail", noteId)`） | 带一个 `unknown` 参数条目的边，与带非字面量参数的 flow 声明 nav 相同 |
| 处理器体内的 `navigate(R.id.x)`，`x` 在导航图中 | 通向图中 `x` 对应屏幕名的边（Slice 3）；名字已规范化（剥离包前缀和前导点），没有 flow 屏幕的目标会成为 `[graph]` 节点，不是错误 |
| 任何边的目标位置上没有 flow 屏幕的导航图目标 | 该目标是一个节点（`[graph]`，JSON 中 `"graph_only":true`），像任何屏幕一样可达；没有 CCB-P1005，该节点也没有 CCB-P1007 |
| 导航目标匹配 manifest activity，但没有屏幕或 graph 目标 | 没有 CCB-P1005（manifest activity 豁免了这条边）；不会为它创建节点 |
| 处理器体内的 `navigate(R.id.x)`，`x` 不在图中，或根本没有 graph 文件 | 未知边（`from` = 绑定标签，`target: null, unknown: true`），绝不猜测目标 |
| 处理器体内的 `navigate("Detail")` | 通向 `Detail` 的边，与 `nav("Detail")` 相同（Slice 3） |
| 处理器体之外的 `navigate(...)` | 没有边——NavController 调用遵循与 Slice 1 相同的处理器归属规则 |
| XML `android:onClick` 按钮，已绑定，找到了处理器 fun | 从绑定标签出发的派生边，附加到每个包含该标签的屏幕上（Slice 3） |
| XML `android:onClick` 按钮，其 view id 没有 `[preview.bindings]` 条目 | layout 自身的 `android:text` 命名该按钮（Phase 2）；只有既无绑定又无文本的按钮才会告警 CCB-P1019，无边（与未绑定的监听器接收者使用同一代码） |
| XML `android:onClick` 按钮，已绑定，但其标签不在任何屏幕中 | CCB-P1021，无边 |
| XML `android:onClick` 方法没有匹配的 fun，或处理器没有 nav | 无边，无告警 |
| 仅调用共享辅助函数的处理器（`openDetail()`），nav 在辅助函数内 | 归属于该按钮的边：辅助函数体被扫描一层深（Chunk B），带上当前分支标注；辅助函数体内的调用不会再跟进，因此第二层辅助函数的 nav 不产生边——但 CCB-P1025 会为此告警（Chunk C），而不是静默 |
| 没有类字面量的 `startActivity`（`startActivity(intent)`） | 未知边，绝不猜测目标 |
| 引用另一个文件中的 fun 的方法引用 | 当名字在各文件间唯一时，通过整个项目的函数索引解析（Chunk B）；同一文件内的定义优先于跨文件定义 |
| 引用在两个或更多 flow 文件中都有定义的 fun 的方法引用或辅助函数调用 | 无边，CCB-P1024（每个文件和名字一次）——绝不猜测 |
| 引用没有任何 fun 的方法引用 | 无边，无告警——在 flow 文件中。在真实代码（Phase 2）中，监听器的 `::handler` 在任何已扫描的 Activity 中都没有定义时会告警 CCB-P1025，每个方法一次。 |

CCB-P1019、CCB-P1020、CCB-P1021、CCB-P1024 和 CCB-P1025 始终是告警——即使在 `--strict` 下也是如此——因此它们绝不会导致构建门禁失败。

### 作用域限制（Slices 1–3，特意如此设计）

- 处理器仅在屏幕 fun 体内识别（定义屏幕的顶层 `fun Name` 函数体）。写在别处的处理器（例如任何屏幕 fun 之外的 Activity `onCreate`）会被忽略，不告警——在 flow 文件中是如此。Phase 2 会在真实代码本身中识别 `onCreate` 处理器（见下文“真实代码”）；flow 文件的规则不变。
- 具名处理器必须是顶层 fun。作为处理器或辅助函数被引用的顶层 fun 不是屏幕：Chunk C 通过索引做引用计数，取消任何通过 `::` 在 fun 体内任何位置、从 setOnClickListener/onClick lambda 块、或从处理器 fun 体内被引用的 fun 的资格（迭代至不动点，因此辅助函数链也计入）。它不会出现在屏幕列表中，其 nav 目标只能通过引用它的按钮来解析。直接在屏幕 UI 体内的裸调用不会取消资格：那是屏幕组合（作为组件的 `Detail()`），被组合的屏幕仍然是屏幕。
- 被忽略调用的 lambda 块内的附加点（例如 `foo { ... }` 参数块内，解析器只在其中扫描 nav）会回退到现有的通用扫描，而不是 Slice 1 归属。
- XML `android:onClick` 处理器 fun 与 `::` 处理器不同，可以位于任何 flow 文件中（附加点是 layout 文件，而不是屏幕体）；它们仍然只能是顶层 fun，绝不能是嵌套 fun。
- NavController `navigate()` 仅在处理器体内识别（监听器 lambda、`onClick` 赋值、具名处理器和 `android:onClick` 处理器 fun）。任何其他位置的 `navigate()` 调用都不是边。

## 真实代码（Phase 2）：直接从应用本身得到屏幕和边

flow 草图是可选的。当项目在 `[paths] kotlin`（否则为 `[paths] java`，否则为 manifest 旁边的 `kotlin/`/`java/` 目录）下有真实的 Kotlin Activity 时，preview 会从该代码派生屏幕和边，复用 Slice 1–3 和 Chunk A–C 的机制，在每个 Activity 上合成一段 flow 文本——没有第二个 flow 解析器，也没有第二个处理器模型。

只识别以下形态；其他一切都会告警并回退，绝不崩溃，绝不猜测：

- **Activity 类** `class XActivity : Activity()`（任何父类）。屏幕名 = 类名去掉尾部的 `Activity`/`Fragment`（`MainActivity` -> `Main`）。作为处理器或辅助函数被引用的顶层 fun 不是屏幕（Chunk C 规则）。
- **`onCreate` -> `setContentView(R.layout.<name>)`** 将 Activity 绑定到其 layout 文件。没有 `onCreate` 或没有 `setContentView` 的类不产生屏幕（Compose 的 `setContent` 不在范围内；通向这种 Activity 的导航通过 manifest P1005 豁免仍然有效）。
- **`findViewById<Button>(R.id.<id>).setOnClickListener { ... }`**（也包括带括号的 lambda 和 `.onClick = { }` 形态）在 `onCreate` 内。接收者 id 映射到 layout 按钮的标签 = 其 `android:text`（通过 resources 表解析 `@string`，否则就是 id 本身），在配置的 `[preview.bindings]` 之后合并（配置优先）。函数体内的 nav 与 Slice 1–3 的形态相同：`startActivity(Intent(this, Y::class.java))`（包括 `val intent = Intent(...); startActivity(intent)` 形态）、`nav("Y")`、`navigateTo("Y")`、通过导航图表的 `navigate(R.id.y)` / `navigate("Y")`。辅助函数调用深入一层，调用点的字面量参数会带到辅助函数的边上（`openDetail("note-1")` -> 边参数 `["note-1"]`；变量参数会成为 unknown 参数条目）。
- **Layout 的 `android:onClick="method"`** -> 在 Activity 类内部定义的方法（不只是顶层 flow fun）-> 其 nav 会成为从该按钮出发的边。按钮标签是配置绑定，否则是 layout 自身的 `android:text`，否则是 CCB-P1019（无绑定也无文本）。
- **Manifest 启动 activity**（MAIN/LAUNCHER intent-filter）在没有名为 `Home` 的屏幕时是入口屏幕。启动器的真实屏幕会合并进名为 `Home` 的草图屏幕（启动器就是入口屏幕；草图将其命名为 Home，而真实 Activity 是 `MainActivity`），没有草图 Home 时，名为 `Home` 的导航图目标就是指启动器屏幕。

**与草图合并**（当 `flow/` 存在时）：派生的（真实代码的）屏幕/边会合并进来；同一按钮 + 目标（带或不带 `Activity` 后缀的拼写——`Form` 和 `FormActivity` 是同一屏幕）会静默去重，保留草图边（它可能带有真实调用没有的参数）；同一按钮的目标冲突会告警 CCB-P1020，真实代码的目标保留。草图根优先于真实根：草图提供线框图，真实代码提供边。仅真实代码有的屏幕保留其 layout 派生的线框图（见下文）。

**没有草图时的线框图**：框树从 layout XML 派生——`LinearLayout` 垂直 -> Column，水平 -> Row（没有给出 `android:orientation` 时水平是 Android 的默认值），`TextView` -> Text，`Button` -> Button，`EditText` -> TextField，`ImageView` -> Image，其他任何内容 -> 通用框（包括 `ConstraintLayout`：不解约束，子项仍做映射）。`android:text` 是标签（`@string` 与其他组件标签一起解析）。不需要 CSS；没有新的布局引擎，没有字体度量。无子项的通用框（Switch、裸 view）取 1 行叶高度，而不是消失。`<include>` 是没有子项的通用框（include 不解析）；作为根的 `<merge>` 将其子项垂直堆叠。

**限制，特意如此设计**：

- 只提取 `onCreate` 的附加点。附加在其他任何位置的监听器、`RecyclerView` 适配器绑定和数据驱动列表（对 view id 做 `for` 循环，如 p2-catalog 产品循环）都不记录边——该循环会带真实源码行告警 CCB-P1025，因此该 nav 是可见的，而不是被静默丢弃。
- 监听器方法引用（`::handler`）在任何已扫描的 Activity 中都没有定义（顶层 fun，或拼写错误）时，会告警 CCB-P1025，每个方法一次。没有匹配 fun 的 `android:onClick` 方法保持静默，与 Slice 3 相同。
- 真实 Kotlin 文件中的顶层 fun（在任何 Activity 类之外）不会被提取；只有 Activity 成员 fun 会被提取。
- 合成 flow 文本上的诊断会注明真实的 `.kt` 文件，但行号是合成的行号，不是真实源码行号——真实模块自身的 CCB-P1025（无法解析的监听器、被丢弃的成员 fun）除外，它们带有真实的 file:line。把合成行号当作真实行号是一个已知限制，不是需要静默绕过的问题。
- flow 草图的 onClick 分发只匹配 BUTTON 框：与按钮共享文本的 `TextView`（屏幕标题，例如 Settings 屏幕上的 `"Settings"`）是不同的组件，不再吸引该按钮的边。

## Flow 文件示例

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

## 场景：声明的运行时事实

场景是项目的运行时事实，在 ccb.toml 中声明——即“没有解释器的真实运行时行为”的答案。场景从不求值任何东西：它是在渲染时合并到静态树之上的数据输入。它只填充未知项（未知参数、未选分支、占位行、空字段）并选择分支。它从不覆盖静态字面量，从不解析动态目标，也从不隐藏未选的分支条目。同样的规则也适用于轨迹（见下文）——轨迹只按位置填充所走的边；场景则按从左到右填充每个匹配边的未知槽。

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

合并规则：

- 分支：指定屏幕的指定条目被标记为已选。每个条目仍然渲染；选择是一个标记，而不是过滤。树显示为 `branches: when state [is Loading, is Loaded [scenario], else]`，JSON 中则作为分支的 `"picked": {"entry": ..., "provenance": "scenario"}`。
- 列表：行只替换无子项列表的 4 个占位行。已有静态子项的列表保持原样。行在 wireframe 中渲染为普通行，并在 JSON 中带有 `"provenance": "scenario"`。
- 字段：内容是附加的，绝不是替换——静态标签保留，wireframe 显示 `[Title: Milk]`。JSON 带有 `"value"` 和 `"value_provenance": "scenario"`。
- 参数：值按从左到右的顺序填充未知（NULL）参数槽，每个匹配的边独立填充——同一按钮标签在两个屏幕上时，会用相同的值填充每个屏幕的边。静态字面量绝不触碰，动态目标绝不解析，超出每条边槽位的值会被忽略（各屏幕的元数本来就可能不同）。树将填充的参数标记为 `("42" [scenario])`，JSON 则带有一个平行的 `"args_provenance"` 数组（静态条目为 null，填充的为 `"scenario"`）。JSON 顶层还带有场景名 `"scenario"`。
- 键按渲染后的标签匹配（框经过 @string/@color 解析后；边源标签按树中显示的为准）。
- 来源汇总：场景提供的值在树中标记为 `[scenario]`（分支选择和填充的参数以内联方式标记；列表行和字段内容在每个屏幕的 `scenario:` 行上标记，因为它们没有自己的树行）。wireframe 将场景值显示为内容；树和 JSON 带有标记。静态值保持无标记，没有场景时输出与之前逐字节相同。
- 不匹配是告警，绝不是错误，也绝不静默：当场景命名了未知的屏幕、分支条目、列表或字段；当列表已有静态行；当按钮标签不匹配任何边；当按钮的边没有未知参数可填充时，触发 CCB-P1022。该条目会被跳过，渲染继续。场景绝不会导致构建门禁失败。
- 配置错误（值类型错误、未知的场景小节、直接位于 `[preview.scenario.<name>]` 下的键）会像任何其他配置错误一样使配置加载失败（CCB-E1007 / CCB-E1001）。

参考示例自带一个场景：`examples/preview-sample` 声明了 `[preview.scenario.loaded]`，其场景渲染在 `tests/golden/preview/scenario-loaded/` 下做 golden 测试。

## 轨迹：记录的运行时路径

轨迹是一个有序的导航步骤列表，每个步骤对应一次导航——从 walk 中记录，或（以后）由一个微型应用内日志器发出。没有内置的日志器；下面的格式是编写日志器所依据的契约。同样的合并规则也适用于场景（见上文）：标记走过的路径，填充走过边的未知参数，并选择分支条目。从不覆盖静态字面量，从不解析动态目标，也从不隐藏未选的分支条目。

轨迹格式（JSON，版本 1）：

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

- `screen`（必需字符串）：导航发生的屏幕。
- `edge`（字符串或 null）：导航所来自的按钮标签。Null 表示纯选择步骤：选了分支但没有导航。返回导航从不记录——轨迹是走过的前进路径，不是按键日志。
- `branch`（字符串或 null）：该步骤所处的分支条目标签（`is Loaded`、`else`、`then`），无条件时为 null。
- `args`（字符串或 null 的数组，可以省略）：按位置映射到所走边的参数上——步骤参数 k 在该槽未知时填充边的参数 k。Null 条目会让其槽保持未知。与场景参数（对每个带该按钮标签的边，按从左到右填充未知槽）不同，轨迹参数只按位置填充所走的边。
- `version` 可以省略；如果存在，必须为 1。未知字段会被忽略。结构问题（无法解析的 JSON、错误的类型、缺失 `screen` 或 `steps` 数组）会以退出码 2 拒绝该文件；不匹配任何内容的步骤会告警（CCB-P1023）并被跳过。

用 `ccb preview --walk --record FILE` 记录一次 walk（walk 退出时写轨迹；写失败时退出码为 2），并用 `ccb preview --trace FILE` 以叠加方式渲染（人类可读渲染和 `--json`；`--trace` 拒绝 `--walk`、`--watch`、`--diff`、`--storyboard` 和 `--update-goldens`，并与 `--scenario` 组合——场景在加载时应用，轨迹在其后应用，轨迹只填充仍然未知的槽）。

渲染规则：

- 走过的边会被高亮：树在边行后追加 ` [trace]`（`-> Detail("note-42") via "Open note" [trace]`），JSON 在边上带有 `"taken": true`，顶层带有 `"trace": {"steps": N}`（N 是文件中的步骤数，无论每个步骤是否匹配）。
- 分支选择在 branches 行中标记为 ` [trace]`，被选分支中轨迹未走过的条目会被灰显——在纯文本中用 `[not taken]` 标记（`branches: when state [is Loading [not taken], is Loaded [trace], else [not taken]]`）。JSON 将选择作为分支的 `"picked": {"entry": ..., "provenance": "trace"}`；不同条目的轨迹选择胜过场景选择。
- 填充的参数保留其字面文本，在树中带有 ` [trace]`（`("42" [trace])`），在 JSON 的平行 `"args_provenance"` 数组中带有 `"trace"`。一个槽最多带一个来源的标记：场景填充的槽绝不会被轨迹覆盖。
- 匹配按标签进行，与 walk 的分支规则完全相同：步骤命名一个屏幕和一个按钮标签，带分支限定的步骤会标记每个按钮标签出现在指定分支条目内的边。当多个边共享一个按钮标签时，它们都会被标记为已走过——轨迹记录的是标签，而不是边的身份。分支条目存在但其中没有该按钮的步骤仍会将边标记为已走过并告警（CCB-P1023）。
- wireframe 不受轨迹影响：走过的路径是树和 JSON 的属性，不是框的属性。

参考示例的已记录 walk 在 `tests/golden/preview/trace-walk/` 下做 golden 测试（逻辑树 + JSON）。

## 样式：每个屏幕一个 CSS 文件，外加 common.css

- 作用域规则：`common.css`（如果存在于 styles 目录中）适用于每个屏幕。`<Screen>.css` 仅适用于该屏幕，且仅当 flow 文件中存在同名屏幕时才适用。任何其他 CSS 文件会被忽略并发出 CCB-P1015 告警，且从不解析。文件按排序顺序解析，`common.css` 在先，然后是各屏幕文件。重新声明某个选择器的各屏幕文件会为其他屏幕保留早先的规则，并为自己的屏幕添加一份带作用域的副本：该副本带有早先的属性加上该屏幕文件的声明，布局优先选择精确作用域的规则而非 common 规则。因此 `common.css` 的属性仍然处处适用，各屏幕的属性只适用于该屏幕，两个各屏幕文件声明同一选择器时（例如 `Empty.css` 和 `Settings.css` 都声明 `column`）各自得到自己的带作用域规则。
- 选择器是 `screen`（根框）或小写的组件类型（`column`、`row`、`text`、`button`、`list`、`textfield`、`image`、`box`、`spacer`）。同一选择器的后声明会逐属性覆盖早先的声明。
- 支持的属性：`display`（block、flex）、`flex-direction`（row、column）、`width`/`height`（px、%）、`padding`、`margin`（px，1–4 个值，与 CSS 相同）、`background-color`、`color`（十六进制或命名颜色，或通过 `[preview] resources` 解析的 `@color/name` 引用）、`font-size`（px，设置文本叶的行高——round(font-size * 1.2 / 10) 行，最小 1 行，没有规则设置时为 1 行——按 font-size 分桶缩放 `text-align` 宽度（小 < 14px：4/5，基准 14–23px：1，大 ≥ 24px：5/4），并选择 CCB-P1018 对比度阈值）、`position`（static、absolute）、`top`/`left`（px，仅 absolute）、`gap`（px）、`text-align`（left、center、right）、`border-width`（px）、`border-color`（十六进制或命名颜色）、`flex-wrap`（nowrap、wrap；仅行容器）。
- 特意不支持的属性（它们像子集之外的任何其他内容一样，作为未知属性告警，CCB-P1001）：`justify-content` / `align-items`（基于求和的行/列模型无法诚实表达的分布语义）、`border-radius`（在 1 单元格尺度上没有线框图可见的圆角）、`line-height`（文本按设计是单行；它只会填充）、以及 `border` 简写（请使用上面两个完整写法）。
- 其他任何内容都会带属性名和位置告警（CCB-P1001 未知属性、CCB-P1014 不支持的值），渲染继续并使用回退值。在 `--strict`（或 `[preview] strict = true`）下，这些告警会成为错误。

示例：

```css
screen { background-color: #ffffff; padding: 16px; }
text { color: #111111; font-size: 16px; }
button { background-color: #6200ee; color: #ffffff; margin: 8px; }
```

## 布局模型（终端列与行）

- 手机框宽为 `--width` 列（默认 40）；内容内部是框减去两列边框。
- px 值按 `interior / phone_width` 缩放为列/行（因此 390dp 手机上 40 列时 16px 的 padding 约为 2 行）。`%` 宽高相对于父内容尺寸。
- 列容器将子项垂直堆叠，每个子项为全宽减去 margin。行容器平分宽度：先满足固定尺寸，剩余部分由自动尺寸的子项平分。
- 文本叶（`text`、`button`、`textfield`）的高度等于其行高：round(font-size * 1.2 / 10) 行，最小 1 行，没有规则设置 `font-size` 时为 1 行——因此 16px 标签高 2 行，24px 标签高 3 行。绘制的文本本身仍位于叶内的一行上（顶部对齐），裁剪到其框内（裁剪会告警，CCB-P1009）。按钮渲染为 `[ label ]`，文本字段渲染为 `[label]`，列表在 List 框有子项时每个子项渲染一行 `• <label>`（没有标签的子项渲染为 `• item`）；没有子项的裸 List——其数量确实未知——渲染 4 行 `• item` 占位行，且占位行不产生诊断。有子项的列表恰好每个子项高一行（另加 padding）；裸列表高 4 行（另加 padding）。图片渲染为 `[ image ]` 块，高 3 行，或规则声明的 `height`（声明的 `width` 与任何框一样会被满足；由于约定中没有内在尺寸，只声明一边无法保持宽高比）。未知组件渲染为 `[TypeName]`。
- 叶内容按叶自身缩放后的 padding 内缩：叶在（框 x + 左 padding，框 y + 上 padding）处绘制，裁剪到框宽减去左右 padding。父 padding 以同样方式通过父内容框内缩子项。
- `text-align` 在叶的内容宽度内偏移绘制的字符串（left = 内容边缘，center = 居中，right = 右对齐，字符串更宽时钳制在内容边缘）；它适用于 `text`、`button`、`textfield` 和列表行。对于 `text`、`button` 和 `textfield`，偏移使用按 font-size 缩放的字符串宽度（上述 F1 分桶），而不是原始绘制宽度——只有偏移会移动，绘制仍在框边缘处裁剪。
- 每个框的尺寸也会在 JSON 中以 dp 报告（`w_dp`、`h_dp`）：w_dp = round(w * phone_width / interior)，h_dp = h * 10（一行约 10dp）。这是从单元格网格估算的，不是测量值。
- 缩放后至少为 1 单元格的 `border-width` 会在框周围绘制 1 单元格轮廓（Unicode 框线字符，或 `--ascii` 下的 ASCII 字符），颜色为 `border-color`，否则为规则的 `color`，否则为终端默认值。内容在各边内缩 1 单元格，因此带边框的框的内容框每边缩小 1 单元格，高度增加 2 行（顶部 + 底部边框）；对于容器，轮廓画在容器框周围，子项在内缩后的内容框内布局。`border-width: 0`（或任何缩放后为 0 的值）表示没有边框；负宽度或无效的边框颜色会告警（CCB-P1014）。
- 在 `flex-wrap: wrap` 的行容器中，子项从左到右放置，超出内容宽度的子项会另起一行；新行的 y 按上一行的高度推进（其最高的已占子项高度）。行总高度不受换行影响：仍像 nowrap 模型一样对每个子项高度求和。非行容器上的 `flex-wrap` 会被忽略，并发出注明选择器的 CCB-P1014 告警。
- 绝对定位的框不影响父高度。重叠的框（CCB-P1003）或超出框体的框（CCB-P1004）会告警。

## Lint 诊断（布局之后）

- CCB-P1017——点击目标过小（告警）。仅适用于 `button` 和 `textfield` 框，在布局之后。当 `w < 8` 列，或（`h < 2` 行且高度已知）时告警，以 wireframe 单元格计。已知高度规则（Chunk C）：宽度总是由布局算出，因此 `w < 8` 总是算数，但没有 `font-size` 规则时的高度是 1 行默认值，不是测量值，因此 `h < 2` 只有在生效规则设置了 `font-size` 时才算数。报告已知的情况，对未知的情况保持沉默：完全没有任何尺寸规则的按钮默认是 38x1，不再告警。阈值特意保留在单元格中，尽管 F2 报告 dp：48dp 的高度规则会标记几乎每个按钮——F1 行高对典型文本是 20–30dp（16px → 2 行，24px → 3 行），因此达到 48dp 还需要在顶部加上充足的垂直 padding——而 48dp 的宽度规则（约 5 列）会比当前的 8 列（约 82dp）更宽松，会放过被行拆分挤压的按钮。单元格规则针对的是真正退化的情况（被行拆分挤到几列的按钮、零 padding 的 1 行按钮）。消息会同时报告 dp 尺寸和单元格，例如 `tap target too small: button "Go" is 5x1 cells (51dp x 10dp; minimum 8x2 cells)`。默认框下的已记录 dp 等价：8 列约 82dp 宽，2 行 = 20dp 高。
- CCB-P1018——文本对比度低于阈值（告警）。适用于 `text`、`button` 和 `textfield` 叶，在布局之后。前景 = 叶规则的 `color`；背景 = 叶规则的 `background-color`，否则为 `screen` 规则的 `background-color`。如果任一侧缺失或无法解析为 RGB（十六进制 `#rgb`/`#rrggbb` 或渲染器的命名颜色表），检查会静默跳过——对比度无法判断。解析出的 `@color` 值算作已解析。比率是 WCAG 相对亮度，对比度 = (L1 + 0.05) / (L2 + 0.05)。阈值：通常为 **4.5:1**，叶规则的 `font-size` ≥ 24px 时为 **3.0:1**（WCAG 大文本阈值）。消息包含保留一位小数的比率，例如 `text contrast 2.1:1 below 4.5:1 for text "Note detail"`。

两者始终是告警——即使在 `--strict` 下也是如此——因此它们绝不会导致构建门禁失败，门禁只在错误时失败。

## 输出与退出码

- `ccb preview`——每个屏幕的 wireframe，然后是逻辑树，然后是诊断。`ccb preview <screen>`（或 `--screen NAME`）——一个屏幕的 wireframe，然后是逻辑树，然后是诊断。
- `--json`——同样的框树、逻辑树和诊断，以结构化数据呈现。`--ascii`——用 ASCII 框线字符代替 Unicode。颜色来自 CSS 颜色近似到 ANSI 256 色码，除非设置了 `NO_COLOR`、终端是 dumb 终端，或给出了 `--ascii`。
- `--output FILE`——将 preview 渲染写入 FILE 而不是 stdout：人类可读渲染（框 + 逻辑树），或同时给出 `--json` 时的 JSON 对象。这种情况下 stdout 什么都不输出；诊断仍输出到 stderr，退出码不变（preview 错误仍以退出码 1 退出且渲染已写入；无法写入的输出文件退出码为 2）。
- `--device phone|tablet`——在 `--width` 之上的框宽预设：phone = 40 列（默认值），tablet = 80 列。显式的 `--width` 总是胜过 `--device`；任何其他值都是用法错误。
- `--storyboard "A,B,C"`——渲染一段旅程，而不是整个应用：参数是以逗号分隔的屏幕名列表，按旅程顺序。允许重复（旅程可以重访某个屏幕）；每次出现都会再次渲染。人类可读输出在每个屏幕的框之前打印一行标题行 `Journey N/M: <Name>`（N = 从 1 开始的位置，M = 旅程长度），框按顺序排列，然后是正常的逻辑树，然后是诊断——与多屏幕渲染完全相同，只是增加了标题行。`--json` 在 `screens` 数组中只保留旅程屏幕，按旅程顺序排列（重复项作为单独条目）。名字会去掉周围空白（`"Home, Detail"` 可用）。错误（退出码 2）：列表中有未知的屏幕名、空列表、空元素（`"Home,,Detail"`）或纯空白元素。冲突（退出码 2）：`--storyboard` 与 `--screen`（位置参数屏幕算作 `--screen`）、`--walk`、`--watch`、`--diff` 或 `--update-goldens` 组合。它与 `--ascii`、`--width`、`--device`、`--output`、`--strict` 和 `--json` 组合。Walk/watch 不受影响：storyboard 是渲染模式，不是循环。
- `--update-goldens`——从当前渲染重新生成 `tests/golden/preview/` 下已检入的 golden 文件（每个屏幕一个 `<Screen>.txt` 框、`logic-tree.txt`、`preview.json`）。框 golden 使用当前的 `--ascii` 设置且无颜色。如果不想触碰真实 golden 而做测试，请将 `CCB_GOLDEN_DIR` 设置为临时目录（仅测试用覆盖）；否则通过从工作目录向上走到 `tests/golden/preview/` 来找到该目录。

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

`label`、`target` 和 `file` 在缺失时为 `null`；`line` 对于没有源码位置的诊断为 -1。JSON 模式下 stdout 只输出这个对象；人类可读模式的文字和诊断输出到 stderr 或被抑制。该对象在 `"event":"preview"` 旁带有 `"version":2`（版本 2 相对版本 1 的形态变化是每条边的 `"args"` 数组，字符串或 null，nav 调用没有额外参数时为 `[]`，以及每条边的 `"branch"` 字段，字符串或 null，无条件边为 `null`——两者都是加法变化，因此版本保持 2；解析出的 `@string` 标签显示为普通的 `label` 值，是值变化而非形态变化，且只在使用 `@string` 的项目中出现）。每框的 `"w_dp"`/`"h_dp"` 字段同样是加法变化，因此版本保持 2。使用 `--storyboard` 时，`screens` 数组只包含旅程屏幕，按旅程顺序排列。

- 退出码：见 [docs/diagnostics.md](diagnostics.md)（0 正常，允许告警；1 preview 错误；2 配置错误）。在 `--strict` 下，子集告警会成为错误，因此它们同样以退出码 1 退出。

## 构建门禁：`ccb check` / `ccb build` 在 preview 错误时失败

可选，默认关闭。用 ccb.toml 中的 `[preview] gate = true` 启用，或每次调用时用 `ccb check --preview-gate` / `ccb build --preview-gate`；`--no-preview-gate` 强制关闭。优先级：`--no-preview-gate` 胜过 `--preview-gate`，胜过配置键；同时传入两个标志是用法错误（退出码 2）。

启用后，门禁会为该项目运行 preview 诊断：

- 在 `check` 中，于工具链验证之后；在 `build` 中，于任何流水线阶段之前，因此失败的门禁绝不会启动构建。
- 严重性规则：任何 preview ERROR 诊断都会导致门禁失败（缺失导航目标 CCB-P1005、重复屏幕 CCB-P1008，以及 strict 下的 CCB-P1001/CCB-P1014）；仅有 WARNING 诊断绝不会导致失败。
- 失败时退出码为 1，stderr 上每个失败的诊断一行简洁消息（`CCB-P1005: navigation to unknown screen 'Ghost'`）；不打印任何屏幕渲染。在 `--json` 模式下，失败仍以这些 stderr 行加非零退出码呈现，而不是 JSON 事件。
- 没有可用 `[preview]` 小节的项目会静默通过——门禁绝不会把 preview 强加给不使用它的项目。当配置加载本身失败时，门禁会被跳过，改为报告正常的配置错误（check 退出码为 2；build 的流水线报告它）。

## Diff 模式：`ccb preview --diff <snapshot-dir>`

将当前渲染与 golden 文件格式的已保存快照目录比较（每个屏幕一个 `<Screen>.txt` 框、`logic-tree.txt`、`preview.json`——与 `--update-goldens` 写入的相同文件，用相同的调用渲染，因此由 `--update-goldens` 制作的快照 diff 结果相同）。

- 相同时退出码为 0。存在差异时：退出码为 1，每个不同的文件一个 unified diff，以文件名为标题，外加一行汇总行，列出所有不同的文件（`differing files (2): Home.txt, preview.json`）。
- 快照中缺失的当前屏幕报告为 `missing: <file> (not in snapshot)`；没有匹配当前屏幕的快照文件报告为 `stale: <file> (no matching current screen)`。无法读取的快照目录、没有可用 `[preview]` 小节的项目，或未知的 `--screen` 名以退出码 2 退出并给出清晰诊断；在任何项目之外，`--diff` 是用法错误（退出码 2），而不是普通 preview 的“不适用”。项目本身的 preview 错误不会改变 diff 退出码——渲染仍会比较。
- `--diff` + `--screen NAME` 只 diff 该屏幕的 `<Screen>.txt` 文件。`--diff` + `--output FILE` 将 diff 报告写入文件而不是 stdout。`--diff` + `--json` 发出一个机器可读对象而不是人类可读 diff 文本：`{"event":"preview-diff","identical":bool,"files":[{"name":...,"status":"identical|different|missing|stale","diff":string-or-null}]}`（diff 字符串仅在 "different" 文件时出现）。
- 冲突（退出码 2）：`--diff` 不能与 `--watch`、`--walk` 或 `--update-goldens` 组合。

## Walk 模式：`ccb preview --walk [screen]`

Walk 模式是对逻辑树的静态探索，不是模拟器：它沿着项目已声明的屏幕和边移动。它从不执行 Kotlin，从不求值 `when`/`if` 条件，从不取数据，从不做动画。

`ccb preview --walk` 从入口屏幕开始（存在时为 `Home`，否则为第一个屏幕）；`ccb preview --walk Detail`（或 `--screen Detail`）从指定屏幕开始。每个视图显示屏幕的 wireframe、其出边列表（显示与逻辑树相同的参数文本，例如 `-> Detail("note-42")`）并带 `>` 光标、返回栈和页脚提示。

按键：上/下箭头或 `j`/`k` 移动选择（选择在两端钳制，绝不环绕）；Enter 跟随选中的边；`b` 或 Backspace 返回；`q` 退出。`t` 进入当前屏幕的分支选择模式，列出其分支条目，如 `when state: is Loaded`；上/下移动，Enter 选择。选择从不求值条件：它跟随第一条源标签命名了所选条目内组件的边，或者，当该条目没有出边时，记录选择并停留在当前屏幕。Esc（或 `b`）不做选择退出分支模式。

跟随未知边——动态目标，或不存在的目标屏幕——会显示为未知并停留在当前屏幕；walk 模式从不凭空编造目标。在根屏幕返回是无操作。Ctrl-C 会退出并恢复终端，SIGINT/SIGTERM 也是如此。Walk 模式需要终端：stdin 或 stdout 不是 TTY 时，它会打印错误并以退出码 2 退出，而不是挂起。

`ccb preview --walk --record FILE` 将 walk 记录为轨迹（见上文“轨迹”），在 walk 退出时写入：每次前进导航一个步骤，外加未导航的分支选择的纯选择步骤。返回导航和未知跟随不记录任何内容。没有 `--walk` 的 `--record` 是用法错误。

## Watch 模式：`ccb preview --watch [screen]`

Watch 模式立即渲染一次，然后在每次 preview 输入变化时重新渲染——无需重启，无需重建。它与 `--screen NAME`（或位置参数屏幕）、`--ascii`、`--width`、`--device`、`--json`、`--strict` 和 `--output` 组合；所有这些选项在每次重新渲染时保持其单次渲染的含义。

监视集合：styles 目录下的每个 CSS 文件、flow 目录下的每个文件，以及 `ccb.toml` 本身。新建和删除的文件都算作变化，因为每次重新扫描都会与上一次做 diff。`ccb.toml` 在每次变化时都会重新加载，因此编辑 `[preview]` 路径会将监视重新指向新目录。

刷新帧取决于渲染输出到哪里：

- TTY stdout：先清屏（`ESC[2J ESC[H`），然后输出新渲染。初始渲染不清屏。
- 管道人类可读模式：渲染之间用单独一行的分隔线分隔，`--- ccb preview refresh ---`，在第一次之后的每次重新渲染之前。
- `--json` 管道模式：无分隔符——每次渲染是一个 JSON 对象加一个换行，即有效的 JSONL，每次渲染后刷新。
- `--output FILE`：文件在每次变化时重写，stdout 保持为空，刷新通知和诊断输出到 stderr。

变化会防抖约 200ms：一连串保存（或一次触及多个文件的保存）会在输入安静 200ms 后只产生一次重新渲染。损坏的保存绝不会杀死循环：写了一半的 TOML、损坏的 CSS 或损坏的 Kotlin 会打印诊断，循环继续在上一个良好渲染上监视，并在输入再次有效时重新渲染。如果所选屏幕在变化后消失，循环会打印注明它的诊断并继续监视，而不是崩溃或退出。

用 Ctrl-C（或 SIGTERM）退出：循环以退出码 0 退出，先刷新 stdout。Watch 模式从不进入 raw 模式，因此没有需要恢复的终端状态。

冲突（退出码 2，与其他 preview 冲突的风格相同）：`--watch` 不能与 `--update-goldens` 组合，也不能与 `--walk` 组合——交互循环和监视循环不能共享 stdout 语义。允许 `--watch` 与 `--output` 组合。

## LSP 服务器：`ccb lsp [--scenario NAME] [--trace FILE]`

一个长期运行的服务器，通过 stdio 以 LSP 风格的 `Content-Length` 帧讲 JSON-RPC。它提供与 `ccb preview --json` 相同的逻辑树（屏幕、边、分支标签、参数，外加选中的场景/轨迹叠加），实时更新：编辑文件后树会更新，无需重启。单线程 `select()` 循环监听 stdin + inotify；没有线程（v1 规则）。终端树客户端（`ccb preview --tree-live`，Chunk 4）和普通 LSP 客户端都与该服务器通信；CCB 本身不附带编辑器插件。

标准方法：`initialize`、`shutdown`、`exit`、`textDocument/didOpen`、`didChange`（全量同步：取 `contentChanges[]` 最后一项的文本）、`didSave`（无操作——文本已同步）、`didClose`。自定义方法：`ccb/logicTree`（整个树对象，从 preview JSON 原样拼接）、`ccb/screenInfo`（`{"screen": <一个屏幕对象>, "incoming": [...]}`——incoming 条目带有 `from_screen, from, target, unknown, args, branch`；包含自边）、`ccb/diagnostics`（`{"diagnostics": [...]}`，与树的诊断完全一致）。`ccb/publishTree` 是推送通知，带有整棵树，在 `initialize` 之后和每次改变树 JSON 的重建之后发送（按字符串相等比较；第一次发布总是发送）。

`initialize` 结果：`{"capabilities":{"textDocumentSync":1}, "serverInfo":{"name":"ccb lsp","version":<version>}}`。根优先级：`rootUri`，然后是第一个 `workspaceFolders[].uri`，然后是 `rootPath`。第二次 `initialize` 带不同的根时会重新定根：只有在一次一次性的配置加载证明新根有可用的 `[preview]` 小节后才提交；否则忽略该根，旧树继续服务。退出码：`shutdown` 在 `exit`/EOF 之前时为 0，否则为 1。错误码：无法解析的 JSON 为 `-32700`（null id），未知方法为 `-32601`，shutdown 之后的请求为 `-32600`，未知屏幕为 `-32602`，没有项目为 `-32603`。

何时重新解析：flow 和 CSS 文件每个文件解析一次，存入 staging arena，只在该文件变化时重新解析——编辑器的 `didChange` 会立即重建，磁盘变化防抖 200ms（watch 模块的防抖）。`ccb.toml` 只从磁盘读取：未保存的编辑器更改在保存时生效。重新加载时，bindings 变化或 nav/layout/resources 路径变化会使整个 flow 缓存失效（bindings 和 graph/layout 是 flow 解析的输入）；styles/flow 目录变化会重建监视上下文和快照。失败的重新加载（无法加载的 TOML 或没有可用的 `[preview]`）会保留旧配置和上一个良好项目，并在下一次变化时重试——损坏的保存绝不会杀死服务器。Nav/layout XML 在每次构建时都会重新扫描（文件很小）；nav/layout 内容变化会使 flow 缓存失效。Resources（`strings.xml`/`colors.xml`）在每次构建时作为组装的一部分重新读取，因此 resources 变化只需触发一次重建。打开的 flow/CSS 文档会覆盖磁盘内容直到关闭；关闭前会忽略对已打开文档的磁盘更改。磁盘上尚不存在的文件的已打开文档会在重新同步时合并进文件列表，因此未保存的新文件仍能解析。一次重建也会消耗待处理的磁盘变化并重置防抖。

增量是结构性的，不是一个标志：服务器在每次构建时将原始的每文件解析深拷贝到一个新的项目 arena 中（场景/轨迹应用和 resources 解析会修改框，因此解析出的框绝不在构建之间复用），然后运行与加载路径相同的组装——`pv_project_assemble` 是共享的，不是分叉的。服务器的树 JSON（无场景/轨迹）与同一项目的 `ccb preview --json` 逐字节相同（除了 CLI 的尾随换行）；测试套件会针对示例 golden 固定这一点。CSS 合并同样是共享的：每文件解析用 `pv_css_rule_merge` 合并，它精确复制 `pv_css_parse` 的跨文件语义（同一文件/同一作用域就地合并；各屏幕重新声明会拆分一个带有新文件/作用域和新声明行的副本；common 文件合并进第一个匹配规则）。

`lsp` 会拒绝其他 preview 标志（退出码 2）：它只接受 `--scenario` 和 `--trace`。未知场景 / 无法读取的轨迹 / 没有 `ccb.toml` / preview 不适用都是退出码 2，与 CLI 相同。项目通过从工作目录向上搜索找到，与 `preview` 相同。

## Tree-live 模式：`ccb preview --tree-live [--scenario NAME] [--trace FILE] [screen]`

终端树客户端：它将 `ccb lsp` 作为子进程启动，在项目目录初始化它，并在终端中实时渲染逻辑树——即运行时计划中“直接渲染到终端”的客户端。它从不自己解析 flow 或 CSS；它显示的一切都是服务器发布的树。

视图有三部分。带 `>` 光标的屏幕列表（`[entry]` 标记入口屏幕，`(unreachable)` 标记没有任何边到达的屏幕）。选中屏幕的详情窗格：其出边（`-> Target(args) via "label" [branch] [trace]`，动态目标为 `-> unknown`）、其入边（`<- Screen via "label"`——包含自边）和其分支（`when state: is Loading, is Loaded, else (picked: is Loaded [scenario])`）。诊断以内联方式显示在下方，按项目顺序（`CODE message (file:line)`；诊断没有注明文件时省略文件部分）。叠加激活时会出现 `scenario: NAME` / `trace: N steps` 行。参数来源是按槽的：场景或轨迹填充的字面量渲染为 `"value" [scenario]` / `"value" [trace]`；静态字面量保持无标记，发布的 JSON 不带具名参数标志，因此每个字面量在这里都带引号渲染。

按键：`j`/`k` 或上/下移动选择（在两端钳制，绝不环绕）；`q` 或 Ctrl-C 退出。`ccb/publishTree` 通知会重新渲染视图：当选中的屏幕仍然存在时，选择跟随该屏幕的名字，否则钳制到上一个索引。在第一次发布之前，视图显示 "waiting for logic tree..."，按键仍然有效，因此慢速服务器看起来绝不会卡住。

组合与退出码：`--scenario` 和 `--trace` 会传给启动的服务器（未知场景或无法读取/格式错误的轨迹是退出码 2，在启动前验证）；`--screen` 或位置参数屏幕在第一次发布后选择初始屏幕（未知名字是退出码 1，并列出可用屏幕）。`--tree-live` 拒绝 `--walk`、`--watch`、`--diff`、`--storyboard`、`--output` 和 `--update-goldens`（退出码 2）；`--ascii`/`--width`/`--device` 会被接受，但对树视图没有效果。stdin 或 stdout 不是 TTY 时退出码为 2 并给出诊断，绝不挂起。服务器在每条退出路径上都会被干净地关闭（shutdown + exit，有界等待，SIGKILL 兜底），终端会被恢复。如果服务器在从未发布树的情况下结束，或在会话中意外结束，客户端会打印诊断并以退出码 1 退出。状态机（选择、发布应用、帧消息流）在 `tests/test_preview_tree.c` 中用脚本化按键和预制载荷做单元测试；终端循环本身像 walk 模式一样，在手机上用脚本化 pty 会话覆盖。

## 实际示例

参考示例应用位于 `examples/preview-sample/`。运行它：

```sh
cd examples/preview-sample
ccb preview
ccb preview Detail
ccb preview --json
```

它已检入的渲染是 `tests/golden/preview/` 下的 golden 文件；测试套件将实际输出与它们做 diff，因此任何改变输出的渲染器更改都必须有意更新 golden，绝不能静默更新。
