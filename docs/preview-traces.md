# Traces: recorded runtime paths

[Docs index](index.md) · [Preview](preview.md)

*ccb-preview-traces*

A trace is an ordered list of navigation steps, one per navigation taken —
recorded from a walk, or (later) emitted by a tiny in-app logger. No logger
is built in; the format below is the contract one is written against. Same
merge rules as [scenarios](preview-scenarios.md): marks the taken path,
fills unknown arguments of taken edges, picks branch entries. Never
overrides a static literal, never resolves a dynamic target, never hides
an unpicked branch entry.

## Format (JSON, version 1)

*ccb-preview-trace-format*

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

| Field | Meaning |
| ----- | ------- |
| `screen` (required string) | The screen the navigation happened on. |
| `edge` (string or null) | The button label the navigation came from. Null means a pick-only step: a branch picked without navigating. Back navigation is never recorded — a trace is the forward path taken, not a key log. |
| `branch` (string or null) | The branch-entry label the step ran under (`is Loaded`, `else`, `then`), or null when unconditional. |
| `args` (array of string-or-null, may be omitted) | Maps positionally onto the taken edge's arguments — step arg k fills edge arg k when that slot is unknown. A null entry leaves its slot unknown. Unlike scenario args (which fill unknown slots left-to-right for every edge with a button label), trace args fill only the taken edge, positionally. |

`version` may be omitted; when present it must be 1. Unknown fields are
ignored. Structural problems (unparseable JSON, wrong types, a missing
`screen` or `steps` array) reject the file with exit 2; a step that
matches nothing warns (CCB-P1023) and is skipped instead.

Record a walk with `ccb preview --walk --record FILE` (the walk writes the
trace when it exits; a write failure is exit 2), and render with the
overlay using `ccb preview --trace FILE` (human render and `--json`;
`--trace` rejects `--walk`, `--watch`, `--diff`, `--storyboard`, and
`--update-goldens`, and composes with `--scenario` — the scenario applies
at load, the trace after, and the trace only fills slots still unknown).

## Render rules

*ccb-preview-trace-render*

- Taken edges are highlighted: the tree appends ` [trace]` to the edge
  line (`-> Detail("note-42") via "Open note" [trace]`), and JSON carries
  `"taken": true` on the edge plus `"trace": {"steps": N}` top-level (N is
  the steps in the file, whether or not every step matched).
- Branch picks are marked ` [trace]` in the branches line, and entries of
  a picked branch that the trace did not take are greyed — in plain text,
  the `[not taken]` mark (`branches: when state [is Loading [not taken],
  is Loaded [trace], else [not taken]]`). JSON carries the pick as the
  branch's `"picked": {"entry": ..., "provenance": "trace"}`; a trace pick
  wins over a scenario pick of a different entry.
- Filled arguments keep their literal text and carry ` [trace]` in the
  tree (`("42" [trace])`) and `"trace"` in the parallel
  `"args_provenance"` array in JSON. A slot carries at most one source's
  mark: scenario-filled slots are never overwritten by a trace.
- Matching is by label, exactly like the walk's branch rule: a step names
  a screen and a button label, and a branch-qualified step marks every
  edge whose button label appears inside the named branch entry. When
  several edges share a button label, all of them are marked taken — the
  trace records labels, not edge identities. A step whose branch entry
  exists but contains no such button still marks the edge taken and warns
  (CCB-P1023).
- The wireframe is unchanged by a trace: the taken path is a property of
  the tree and JSON, not of the boxes.

The reference sample's recorded walk is golden-tested under
`tests/golden/preview/trace-walk/` (logic tree + JSON).
