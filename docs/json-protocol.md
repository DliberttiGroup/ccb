# JSON protocol (`--json`)

[Docs index](index.md)

*ccb-json*

With `--json`, human prose never appears on stdout. Progress goes to
stdout as JSONL events; diagnostics go to stdout as `diagnostic` events;
the final result is one JSON object.

## Build events

*ccb-json-build-events*

```json
{"event":"phase","phase":"kotlinc","status":"started"}
{"event":"cache","stage":"compile-code","hit":true,"reason":"source inputs unchanged"}
{"event":"task","task":"compile-code","status":"started"}
{"event":"task","task":"compile-code","status":"finished","duration_ms":5211,"result":"done"}
{"event":"task","task":"compile-res","status":"finished","duration_ms":12,"result":"skipped","reason":"resource inputs unchanged"}
{"event":"build_done","apk":"build/outputs/debug/app.apk","steps_run":2,"steps_skipped":8,"up_to_date":false,"tasks":[{"id":"config","state":"done","duration_ms":41,"skipped":false,"skip_reason":null}]}
```

Task events (Phase 1) carry the task ID and per-task timing;
`result` is `done`, `skipped`, or `failed`. A skipped task carries
its `reason` (absent when the skip logged no reason).
`build_done` gains a `tasks` array with one
`{id, state, duration_ms, skipped, skip_reason}` object per task in
plan order (see [graph engine](graph.md)); `skip_reason` is the
reason string for a skipped task, else `null`. On a failed
build the same array goes out as a standalone
`{"event":"build_summary","tasks":[...]}` object, before the
diagnostics, so the failed and cancelled tasks are machine-readable
too. In explain
mode the plan is one `build_plan` object listing every task with its
dependencies, inputs, and outputs; each input is an object with
`name`, `kind` (`source`, `generated`, or `env`), and `required`.
`ccb graph --affected INPUT` answers with one `affected` object
listing the invalidated tasks in execution order:

```json
{"event":"affected","input":"dex","tasks":["dexcheck","native","package","align","sign","verify"]}
```

Diagnostics from a failed task
carry its ID as `"task"`.

Verbose mode streams tool output as events:

```json
{"event":"tool_output","phase":"kotlinc","stdout":"...","stderr":"..."}
```

## Diagnostics

*ccb-json-diagnostics*

```json
{"event":"diagnostic","code":"CCB-E1001","phase":"config","message":"unknown key 'surprise' in table [project]","file":"ccb.toml","line":9,"command":"...","suggestion":"..."}
```

`file`, `line`, `command`, and `suggestion` appear only when set.

## Other commands

*ccb-json-commands*

| Command | Event |
| ------- | ----- |
| `ccb --json version` | `{"event":"version","version":"0.1.0"}` |
| `ccb --json check` | `{"event":"check","ok":true,"project":"hello","package":"com.example.hello","variant":"debug","sdk":"…","build_tools":"35.0.0","kotlin":"2.4.21","java":17}` |
| `ccb --json init` | `{"event":"init","dir":"myapp","created":8,"skipped":0}` |
| `ccb --json clean` | `{"event":"clean","removed":true}` |
| `ccb --json doctor` (in a project) | `{"event":"doctor","project":"hello","package":"com.example.hello","sdk":"…","build_tools":"35.0.0","kotlin":"2.4.21","java":17,"ndk":null}` |
| `ccb --json doctor` (no valid project) | `{"event":"doctor","config":null,"java":"…","kotlinc":"…","sdk":"…"}` (`"config":"invalid"` when a `ccb.toml` exists but fails validation) |

## `--explain` and `--dry-run`

*ccb-json-explain*

`--explain` prints one `step: run|skip (reason)` line per step on stdout
(not JSON, even with `--json` — it is a human-readable plan).
`--dry-run` prints the exact shell-quoted commands that would run, one per
line, and executes nothing.

Explain is conservative downstream of `link`: when link will run, later
steps report "will change" even if their inputs turn out identical (e.g. a
string-value change regenerates `linked.ap_` but leaves `R.java` untouched,
so the real build skips kotlinc while explain predicted a run). Explain
never over-promises a skip; it may over-predict runs.
