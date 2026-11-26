<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview-modes.md. -->

# Preview 模式：diff、walk、watch

[文档索引](index.md) · [Preview](preview.md)

## Diff 模式：`ccb preview --diff <snapshot-dir>`

将当前渲染与 golden 文件格式的已保存快照目录比较（每个屏幕一个 `<Screen>.txt` 框、`logic-tree.txt`、`preview.json`——与 `--update-goldens` 写入的相同文件，用相同的调用渲染，因此由 `--update-goldens` 制作的快照 diff 结果相同）。

- 相同时退出码为 0。存在差异时：退出码为 1，每个不同的文件一个 unified diff，以文件名为标题，外加一行汇总行，列出所有不同的文件（`differing files (2): Home.txt, preview.json`）。
- 快照中缺失的当前屏幕报告为 `missing: <file> (not in snapshot)`；没有匹配当前屏幕的快照文件报告为 `stale: <file> (no matching current screen)`。无法读取的快照目录、没有可用 `[preview]` 小节的项目，或未知的 `--screen` 名以退出码 2 退出并给出清晰诊断；在任何项目之外，`--diff` 是用法错误（退出码 2），而不是普通 preview 的“不适用”。项目本身的 preview 错误不会改变 diff 退出码——渲染仍会比较。
- `--diff` + `--screen NAME` 只 diff 该屏幕的 `<Screen>.txt` 文件。`--diff` + `--output FILE` 将 diff 报告写入文件而不是 stdout。`--diff` + `--json` 发出一个机器可读对象而不是人类可读 diff 文本：`{"event":"preview-diff","identical":bool,"files":[{"name":...,"status":"identical|different|missing|stale","diff":string-or-null}]}`（diff 字符串仅在 "different" 文件时出现）。
- 冲突（退出码 2）：`--diff` 不能与 `--watch`、`--walk` 或 `--update-goldens` 组合。

## Walk 模式：`ccb preview --walk [screen]`

对逻辑树的静态探索，不是模拟器：它沿着项目已声明的屏幕和边移动。它从不执行 Kotlin，从不求值 `when`/`if` 条件，从不取数据，从不做动画。

`ccb preview --walk` 从入口屏幕开始（存在时为 `Home`，否则为第一个屏幕）；`ccb preview --walk Detail`（或 `--screen Detail`）从指定屏幕开始。每个视图显示屏幕的 wireframe、其出边列表（显示与逻辑树相同的参数文本，例如 `-> Detail("note-42")`）并带 `>` 光标、返回栈和页脚提示。

按键：上/下箭头或 `j`/`k` 移动选择（选择在两端钳制，绝不环绕）；Enter 跟随选中的边；`b` 或 Backspace 返回；`q` 退出。`t` 进入当前屏幕的分支选择模式，列出其分支条目，如 `when state: is Loaded`；上/下移动，Enter 选择。选择从不求值条件：它跟随第一条源标签命名了所选条目内组件的边，或者，当该条目没有出边时，记录选择并停留在当前屏幕。Esc（或 `b`）不做选择退出分支模式。

跟随未知边——动态目标，或不存在的目标屏幕——会显示为未知并停留在当前屏幕；walk 模式从不凭空编造目标。在根屏幕返回是无操作。Ctrl-C 会退出并恢复终端，SIGINT/SIGTERM 也是如此。Walk 模式需要终端：stdin 或 stdout 不是 TTY 时，它会打印错误并以退出码 2 退出，而不是挂起。

`ccb preview --walk --record FILE` 将 walk 记录为 [轨迹](preview-traces.md)，在 walk 退出时写入：每次前进导航一个步骤，外加未导航的分支选择的纯选择步骤。返回导航和未知跟随不记录任何内容。没有 `--walk` 的 `--record` 是用法错误。

## Watch 模式：`ccb preview --watch [screen]`

立即渲染一次，然后在每次 preview 输入变化时重新渲染——无需重启，无需重建。它与 `--screen NAME`（或位置参数屏幕）、`--ascii`、`--width`、`--device`、`--json`、`--strict` 和 `--output` 组合；所有这些选项在每次重新渲染时保持其单次渲染的含义。

监视集合：styles 目录下的每个 CSS 文件、flow 目录下的每个文件，以及 `ccb.toml` 本身。新建和删除的文件都算作变化，因为每次重新扫描都会与上一次做 diff。`ccb.toml` 在每次变化时都会重新加载，因此编辑 `[preview]` 路径会将监视重新指向新目录。

刷新帧取决于渲染输出到哪里：

| 输出位置 | 帧 |
| ----------- | ------- |
| TTY stdout | 先清屏（`ESC[2J ESC[H`），然后输出新渲染。初始渲染不清屏。 |
| 管道人类可读模式 | 渲染之间用单独一行的分隔线分隔，`--- ccb preview refresh ---`，在第一次之后的每次重新渲染之前。 |
| `--json` 管道模式 | 无分隔符——每次渲染是一个 JSON 对象加一个换行，即有效的 JSONL，每次渲染后刷新。 |
| `--output FILE` | 文件在每次变化时重写，stdout 保持为空，刷新通知和诊断输出到 stderr。 |

变化会防抖约 200ms：一连串保存（或一次触及多个文件的保存）会在输入安静 200ms 后只产生一次重新渲染。损坏的保存绝不会杀死循环：写了一半的 TOML、损坏的 CSS 或损坏的 Kotlin 会打印诊断，循环继续在上一个良好渲染上监视，并在输入再次有效时重新渲染。如果所选屏幕在变化后消失，循环会打印注明它的诊断并继续监视，而不是崩溃或退出。

用 Ctrl-C（或 SIGTERM）退出：循环以退出码 0 退出，先刷新 stdout。Watch 模式从不进入 raw 模式，因此没有需要恢复的终端状态。

冲突（退出码 2）：`--watch` 不能与 `--update-goldens` 组合，也不能与 `--walk` 组合。`--watch` 与 `--output` 组合是允许的。
