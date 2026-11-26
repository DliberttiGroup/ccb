<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview-styles.md. -->

# 样式：每个屏幕一个 CSS 文件，外加 common.css

[文档索引](index.md) · [Preview](preview.md)

- 作用域：`common.css`（如果存在于 styles 目录中）适用于每个屏幕。`<Screen>.css` 仅适用于该屏幕，且仅当 flow 文件中存在同名屏幕时才适用。任何其他 CSS 文件会被忽略并发出 CCB-P1015 告警，且从不解析。文件按排序顺序解析，`common.css` 在先，然后是各屏幕文件。各屏幕文件重新声明某个选择器时，会为其他屏幕保留早先的规则，并为自己的屏幕添加一份带作用域的副本：该副本带有早先的属性加上该屏幕文件的声明，布局优先选择精确作用域的规则而非 common 规则。
- 选择器是 `screen`（根框）或小写的组件类型（`column`、`row`、`text`、`button`、`list`、`textfield`、`image`、`box`、`spacer`）。同一选择器的后声明会逐属性覆盖早先的声明。
- 支持的属性：`display`（block、flex）、`flex-direction`（row、column）、`width`/`height`（px、%）、`padding`、`margin`（px，1–4 个值，与 CSS 相同）、`background-color`、`color`（十六进制或命名颜色，或通过 `[preview] resources` 解析的 `@color/name` 引用）、`font-size`（px，设置文本叶的行高——round(font-size * 1.2 / 10) 行，最小 1 行，没有规则设置时为 1 行——按 font-size 分桶缩放 `text-align` 宽度（小 < 14px：4/5，基准 14–23px：1，大 ≥ 24px：5/4），并选择 CCB-P1018 对比度阈值）、`position`（static、absolute）、`top`/`left`（px，仅 absolute）、`gap`（px）、`text-align`（left、center、right）、`border-width`（px）、`border-color`（十六进制或命名颜色）、`flex-wrap`（nowrap、wrap；仅行容器）。
- 特意不支持的属性（每个都会作为未知属性告警，CCB-P1001）：`justify-content`、`align-items`、`border-radius`、`line-height`，以及 `border` 简写（请使用 `border-width` + `border-color`）。
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
- 文本叶（`text`、`button`、`textfield`）的高度等于其行高：round(font-size * 1.2 / 10) 行，最小 1 行，没有规则设置 `font-size` 时为 1 行——因此 16px 标签高 2 行，24px 标签高 3 行。绘制的文本本身仍位于叶内的一行上（顶部对齐），裁剪到其框内（裁剪会告警，CCB-P1009）。按钮渲染为 `[ label ]`，文本字段渲染为 `[label]`，列表在 List 框有子项时每个子项渲染一行 `• <label>`（没有标签的子项渲染为 `• item`）；没有子项的裸 List 渲染 4 行 `• item` 占位行，且占位行不产生诊断。有子项的列表恰好每个子项高一行（另加 padding）；裸列表高 4 行（另加 padding）；图片渲染为 `[ image ]` 块，高 3 行，或规则声明的 `height`（声明的 `width` 与任何框一样会被满足）。未知组件渲染为 `[TypeName]`。
- 叶内容按叶自身缩放后的 padding 内缩：叶在（框 x + 左 padding，框 y + 上 padding）处绘制，裁剪到框宽减去左右 padding。父 padding 以同样方式通过父内容框内缩子项。
- `text-align` 在叶的内容宽度内偏移绘制的字符串（left = 内容边缘，center = 居中，right = 右对齐，字符串更宽时钳制在内容边缘）；它适用于 `text`、`button`、`textfield` 和列表行。对于 `text`、`button` 和 `textfield`，偏移使用按 font-size 缩放的字符串宽度（上述分桶），而不是原始绘制宽度——只有偏移会移动，绘制仍在框边缘处裁剪。
- 每个框的尺寸也会在 JSON 中以 dp 报告（`w_dp`、`h_dp`）：w_dp = round(w * phone_width / interior)，h_dp = h * 10（一行约 10dp）。这是从单元格网格估算的，不是测量值。
- 缩放后至少为 1 单元格的 `border-width` 会在框周围绘制 1 单元格轮廓（Unicode 框线字符，或 `--ascii` 下的 ASCII 字符），颜色为 `border-color`，否则为规则的 `color`，否则为终端默认值。内容在各边内缩 1 单元格，因此带边框的框的内容框每边缩小 1 单元格，高度增加 2 行（顶部 + 底部边框）；对于容器，轮廓画在容器框周围，子项在内缩后的内容框内布局。`border-width: 0`（或任何缩放后为 0 的值）表示没有边框；负宽度或无效的边框颜色会告警（CCB-P1014）。
- 在 `flex-wrap: wrap` 的行容器中，子项从左到右放置，超出内容宽度的子项会另起一行；新行的 y 按上一行的高度推进（其最高的已占子项高度）。行总高度不受换行影响：仍像 nowrap 模型一样对每个子项高度求和。非行容器上的 `flex-wrap` 会被忽略，并发出注明选择器的 CCB-P1014 告警。
- 绝对定位的框不影响父高度。重叠的框（CCB-P1003）或超出框体的框（CCB-P1004）会告警。

## Lint 诊断（布局之后）

- CCB-P1017——点按目标过小（告警）。仅适用于 `button` 和 `textfield` 框，在布局之后。当 `w < 8` 列，或（`h < 2` 行且高度已知）时告警，以 wireframe 单元格计。高度只有在生效规则设置了 `font-size` 时才算数（没有它时，1 行默认值不是测量值）。完全没有任何尺寸规则的按钮默认是 38x1，不再告警。消息会同时报告 dp 尺寸和单元格，例如 `tap target too small: button "Go" is 5x1 cells (51dp x 10dp; minimum 8x2 cells)`。默认框下：8 列约 82dp 宽，2 行 = 20dp 高。
- CCB-P1018——文本对比度低于阈值（告警）。适用于 `text`、`button` 和 `textfield` 叶，在布局之后。前景 = 叶规则的 `color`；背景 = 叶规则的 `background-color`，否则为 `screen` 规则的 `background-color`。如果任一侧缺失或无法解析为 RGB（十六进制 `#rgb`/`#rrggbb` 或渲染器的命名颜色表），检查会静默跳过。解析出的 `@color` 值算作已解析。比率是 WCAG 相对亮度，对比度 = (L1 + 0.05) / (L2 + 0.05)。阈值：通常为 **4.5:1**，叶规则的 `font-size` ≥ 24px 时为 **3.0:1**（WCAG 大文本阈值）。消息包含保留一位小数的比率，例如 `text contrast 2.1:1 below 4.5:1 for text "Note detail"`。

两者始终是告警——即使在 `--strict` 下也是如此——因此它们绝不会导致构建门禁失败，门禁只在错误时失败。
