<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/json-protocol.md. -->

# JSON 协议（`--json`）

使用 `--json` 时，stdout 上绝不会出现人类可读的散文。进度以 JSONL 事件的形式输出到 stdout；诊断以 `diagnostic` 事件输出到 stdout；最终结果是一个 JSON 对象。

## 构建事件

```json
{"event":"phase","phase":"kotlinc","status":"started"}
{"event":"cache","stage":"compile-code","hit":true,"reason":"source inputs unchanged"}
{"event":"build_done","apk":"build/outputs/debug/app.apk","steps_run":2,"steps_skipped":8,"up_to_date":false}
```

详细模式把工具输出作为事件流式输出：

```json
{"event":"tool_output","phase":"kotlinc","stdout":"...","stderr":"..."}
```

## 诊断

```json
{"event":"diagnostic","code":"CCB-E1001","phase":"config","message":"unknown key 'surprise' in table [project]","file":"ccb.toml","line":9,"command":"...","suggestion":"..."}
```

`file`、`line`、`command` 和 `suggestion` 仅在设置时出现。

## 其他命令

- `ccb --json version` → `{"event":"version","version":"0.1.0"}`
- `ccb --json check` → `{"event":"check","ok":true,"project":"hello","package":"com.example.hello","variant":"debug","sdk":"…","build_tools":"35.0.0","kotlin":"2.4.21","java":17}`
- `ccb --json init` → `{"event":"init","dir":"myapp","created":8,"skipped":0}`
- `ccb --json clean` → `{"event":"clean","removed":true}`
- `ccb --json doctor`（在项目内）→ `{"event":"doctor","project":"hello","package":"com.example.hello","sdk":"…","build_tools":"35.0.0","kotlin":"2.4.21","java":17,"ndk":null}`
- `ccb --json doctor`（没有有效项目）→ `{"event":"doctor","config":null,"java":"…","kotlinc":"…","sdk":"…"}`（当存在 `ccb.toml` 但校验失败时，`"config":"invalid"`）

## `--explain` 与 `--dry-run`

`--explain` 在 stdout 上为每一步打印一行 `step: run|skip (reason)`（即使使用 `--json` 也不输出 JSON——它是一份人类可读的计划）。`--dry-run` 打印将要运行的、经过 shell 引用的确切命令，每行一条，不执行任何命令。

Explain 在 `link` 的下游是保守的：当 link 将要运行时，后续步骤会报告 "will change"，即使它们的输入最终证明是相同的（例如字符串值变化会重新生成 `linked.ap_`，但 `R.java` 不变，因此实际构建会跳过 kotlinc，而 explain 预测的是一次运行）。Explain 从不把一次跳过承诺得过满；它可能多预测运行。
