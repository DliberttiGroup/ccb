<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/preview-lsp.md. -->

# LSP 服务器：`ccb lsp [--scenario NAME] [--trace FILE]`

[文档索引](index.md) · [Preview](preview.md)

一个长期运行的服务器，通过 stdio 以 LSP 风格的 `Content-Length` 帧讲 JSON-RPC。它提供与 `ccb preview --json` 相同的逻辑树（屏幕、边、分支标签、参数，外加选中的场景/轨迹叠加），实时更新：编辑文件后树会更新，无需重启。单线程 `select()` 循环监听 stdin + inotify；没有线程。终端树客户端（`ccb preview --tree-live`，见下文）和普通 LSP 客户端都与该服务器通信；CCB 本身不附带编辑器插件。

标准方法：`initialize`、`shutdown`、`exit`、`textDocument/didOpen`、`didChange`（全量同步：取 `contentChanges[]` 最后一项的文本）、`didSave`（无操作——文本已同步）、`didClose`。自定义方法：`ccb/logicTree`（整个树对象，从 preview JSON 原样拼接）、`ccb/screenInfo`（`{"screen": <一个屏幕对象>, "incoming": [...]}`——incoming 条目带有 `from_screen, from, target, unknown, args, branch`；包含自边）、`ccb/diagnostics`（`{"diagnostics": [...]}`，与树的诊断完全一致）。`ccb/publishTree` 是推送通知，带有整棵树，在 `initialize` 之后和每次改变树 JSON 的重建之后发送（按字符串相等比较；第一次发布总是发送）。

`initialize` 结果：`{"capabilities":{"textDocumentSync":1}, "serverInfo":{"name":"ccb lsp","version":<version>}}`。根优先级：`rootUri`，然后是第一个 `workspaceFolders[].uri`，然后是 `rootPath`。第二次 `initialize` 带不同的根时会重新定根：只有在一次一次性的配置加载证明新根有可用的 `[preview]` 小节后才提交；否则忽略该根，旧树继续服务。退出码：`shutdown` 在 `exit`/EOF 之前时为 0，否则为 1。错误码：无法解析的 JSON 为 `-32700`（null id），未知方法为 `-32601`，shutdown 之后的请求为 `-32600`，未知屏幕为 `-32602`，没有项目为 `-32603`。

何时重新解析：

- Flow 和 CSS 文件每个文件解析一次，存入 staging arena，只在该文件变化时重新解析——编辑器的 `didChange` 会立即重建，磁盘变化防抖 200ms（watch 模块的防抖）。
- `ccb.toml` 只从磁盘读取：未保存的编辑器更改在保存时生效。重新加载时，bindings 变化或 nav/layout/resources 路径变化会使整个 flow 缓存失效；styles/flow 目录变化会重建监视上下文和快照。失败的重新加载（无法加载的 TOML 或没有可用的 `[preview]`）会保留旧配置和上一个良好项目，并在下一次变化时重试——损坏的保存绝不会杀死服务器。
- Nav/layout XML 在每次构建时都会重新扫描（文件很小）；nav/layout 内容变化会使 flow 缓存失效。Resources（`strings.xml`/`colors.xml`）在每次构建时作为组装的一部分重新读取，因此 resources 变化只需触发一次重建。
- 打开的 flow/CSS 文档会覆盖磁盘内容直到关闭；关闭前会忽略对已打开文档的磁盘更改。磁盘上尚不存在的文件的已打开文档会在重新同步时合并进文件列表，因此未保存的新文件仍能解析。一次重建也会消耗待处理的磁盘变化并重置防抖。

服务器的树 JSON（无场景/轨迹）与同一项目的 `ccb preview --json` 逐字节相同（除了 CLI 的尾随换行）；测试套件会针对示例 golden 固定这一点。

`lsp` 会拒绝其他 preview 标志（退出码 2）：它只接受 `--scenario` 和 `--trace`。未知场景 / 无法读取的轨迹 / 没有 `ccb.toml` / preview 不适用都是退出码 2，与 CLI 相同。项目通过从工作目录向上搜索找到，与 `preview` 相同。

## Tree-live 模式：`ccb preview --tree-live [--scenario NAME] [--trace FILE] [screen]`

终端树客户端：它将 `ccb lsp` 作为子进程启动，在项目目录初始化它，并在终端中实时渲染逻辑树。它从不自己解析 flow 或 CSS；它显示的一切都是服务器发布的树。

视图有三部分。带 `>` 光标的屏幕列表（`[entry]` 标记入口屏幕，`(unreachable)` 标记没有任何边到达的屏幕）。选中屏幕的详情窗格：其出边（`-> Target(args) via "label" [branch] [trace]`，动态目标为 `-> unknown`）、其入边（`<- Screen via "label"`——包含自边）和其分支（`when state: is Loading, is Loaded, else (picked: is Loaded [scenario])`）。诊断以内联方式显示在下方，按项目顺序（`CODE message (file:line)`；诊断没有注明文件时省略文件部分）。叠加激活时会出现 `scenario: NAME` / `trace: N steps` 行。参数来源是按槽的：场景或轨迹填充的字面量渲染为 `"value" [scenario]` / `"value" [trace]`；静态字面量保持无标记，发布的 JSON 不带具名参数标志，因此每个字面量在这里都带引号渲染。

按键：`j`/`k` 或上/下移动选择（在两端钳制，绝不环绕）；`q` 或 Ctrl-C 退出。`ccb/publishTree` 通知会重新渲染视图：当选中的屏幕仍然存在时，选择跟随该屏幕的名字，否则钳制到上一个索引。在第一次发布之前，视图显示 "waiting for logic tree..."，按键仍然有效，因此慢速服务器看起来绝不会卡住。

组合与退出码：`--scenario` 和 `--trace` 会传给启动的服务器（未知场景或无法读取/格式错误的轨迹是退出码 2，在启动前验证）；`--screen` 或位置参数屏幕在第一次发布后选择初始屏幕（未知名字是退出码 1，并列出可用屏幕）。`--tree-live` 拒绝 `--walk`、`--watch`、`--diff`、`--storyboard`、`--output` 和 `--update-goldens`（退出码 2）；`--ascii`/`--width`/`--device` 会被接受，但对树视图没有效果。stdin 或 stdout 不是 TTY 时退出码为 2 并给出诊断，绝不挂起。服务器在每条退出路径上都会被干净地关闭（shutdown + exit，有界等待，SIGKILL 兜底），终端会被恢复。如果服务器在从未发布树的情况下结束，或在会话中意外结束，客户端会打印诊断并以退出码 1 退出。状态机（选择、发布应用、帧消息流）在 `tests/test_preview_tree.c` 中用脚本化按键和预制载荷做单元测试；终端循环本身像 walk 模式一样，在手机上用脚本化 pty 会话覆盖。
