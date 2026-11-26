<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview-real-code.md. -->

# 真实代码：直接从应用本身得到屏幕和边

[文档索引](index.md) · [Preview](preview.md)

flow 草图是可选的。当项目在 `[paths] kotlin`（否则为 `[paths] java`，否则为 manifest 旁边的 `kotlin/`/`java/` 目录）下有真实的 Kotlin Activity 时，preview 会从该代码派生屏幕和边，复用处理函数机制，在每个 Activity 上合成一段 flow 文本。

| 形态 | 屏幕 / 边 |
| ----- | ------------- |
| `class XActivity : Activity()`（任何父类） | 屏幕名 = 类名去掉尾部的 `Activity`/`Fragment`（`MainActivity` -> `Main`）。作为处理函数或辅助函数被引用的顶层 fun 不是屏幕。 |
| `onCreate` -> `setContentView(R.layout.<name>)` | 将 Activity 绑定到其 layout 文件。没有 `onCreate` 或没有 `setContentView` 的类不产生屏幕（Compose 的 `setContent` 不在范围内；通向这种 Activity 的导航通过 manifest P1005 豁免仍然有效）。 |
| `findViewById<Button>(R.id.<id>).setOnClickListener { ... }`（也包括带括号的 lambda 和 `.onClick = { }` 形态）在 `onCreate` 内 | 接收者 id 映射到 layout 按钮的标签 = 其 `android:text`（通过 resources 表解析 `@string`，否则就是 id 本身），在配置的 `[preview.bindings]` 之后合并（配置优先）。函数体内的 nav 与 [处理函数](preview-handlers.md) 的形态相同：`startActivity(Intent(this, Y::class.java))`（包括 `val intent = Intent(...); startActivity(intent)` 形态）、`nav("Y")`、`navigateTo("Y")`、通过导航图表的 `navigate(R.id.y)` / `navigate("Y")`。辅助函数调用深入一层，调用点的字面量参数会带到辅助函数的边上（`openDetail("note-1")` -> 边参数 `["note-1"]`；变量参数会成为 unknown 参数条目）。 |
| Layout 的 `android:onClick="method"` | 在 Activity 类内部定义的方法（不只是顶层 flow fun）-> 其 nav 会成为从该按钮出发的边。按钮标签是配置绑定，否则是 layout 自身的 `android:text`，否则是 CCB-P1019（无绑定也无文本）。 |
| Manifest 启动 activity（MAIN/LAUNCHER intent-filter） | 在没有名为 `Home` 的屏幕时是入口屏幕。启动器的真实屏幕会合并进名为 `Home` 的草图屏幕（启动器就是入口屏幕；草图将其命名为 Home，而真实 Activity 是 `MainActivity`）；没有草图 Home 时，名为 `Home` 的导航图目标就是指启动器屏幕。 |

## 与草图合并

当 `flow/` 存在时，派生的（真实代码的）屏幕/边会合并进来：同一按钮 + 目标（带或不带 `Activity` 后缀的拼写——`Form` 和 `FormActivity` 是同一屏幕）会静默去重，保留草图边（它可能带有真实调用没有的参数）；同一按钮的目标冲突会告警 CCB-P1020，真实代码的目标保留。草图根优先于真实根：草图提供线框图，真实代码提供边。仅真实代码有的屏幕保留其 layout 派生的线框图（见下文）。

## 无草图时的线框图

框树从 layout XML 派生——`LinearLayout` 垂直 -> Column，水平 -> Row（没有给出 `android:orientation` 时水平是 Android 的默认值），`TextView` -> Text，`Button` -> Button，`EditText` -> TextField，`ImageView` -> Image，其他任何内容 -> 通用框（包括 `ConstraintLayout`：不解约束，子项仍做映射）。`android:text` 是标签（`@string` 与其他组件标签一起解析）。不需要 CSS；没有新的布局引擎，没有字体度量。无子项的通用框（Switch、裸 view）取 1 行叶高度，而不是消失。`<include>` 是没有子项的通用框（include 不解析）；作为根的 `<merge>` 将其子项垂直堆叠。

## 限制，特意如此设计

- 只提取 `onCreate` 的附加点。附加在其他任何位置的监听器、`RecyclerView` 适配器绑定和数据驱动列表（对 view id 做 `for` 循环，如 p2-catalog 产品循环）都不记录边——该循环会带真实源码行告警 CCB-P1025，因此该 nav 是可见的，而不是被静默丢弃。
- 监听器方法引用（`::handler`）在任何已扫描的 Activity 中都没有定义（顶层 fun，或拼写错误）时，会告警 CCB-P1025，每个方法一次。没有匹配 fun 的 `android:onClick` 方法保持静默。
- 真实 Kotlin 文件中的顶层 fun（在任何 Activity 类之外）不会被提取；只有 Activity 成员 fun 会被提取。
- 合成 flow 文本上的诊断会注明真实的 `.kt` 文件，但行号是合成的行号，不是真实源码行号——真实模块自身的 CCB-P1025（无法解析的监听器、被丢弃的成员 fun）除外，它们带有真实的 file:line。
- flow 草图的 onClick 分发只匹配 BUTTON 框：与按钮共享文本的 `TextView`（屏幕标题，例如 Settings 屏幕上的 `"Settings"`）是不同的组件，不再吸引该按钮的边。
