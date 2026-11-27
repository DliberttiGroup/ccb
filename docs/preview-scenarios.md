# Scenarios: declared runtime facts

[Docs index](index.md) · [Preview](preview.md)

*ccb-preview-scenarios*

A scenario is runtime facts for a project, declared in ccb.toml. It never
evaluates anything: it is a data input merged at render time, on top of
the static tree. It only fills unknowns (unknown arguments, unpicked
branches, placeholder rows, empty fields) and picks branches. It never
overrides a static literal, never resolves a dynamic target, and never
hides an unpicked branch entry. The same rules apply to
[traces](preview-traces.md) — a trace fills only the taken edge,
positionally; a scenario fills every matching edge's unknown slots
left-to-right.

The TOML subset has no inline tables, so the four maps are sub-tables
under `[preview.scenario.<name>]` (one key per entry; scenario names are
single segments, no dots):

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

Render with `ccb preview --scenario NAME` (human render, `--json`, and the
logic tree all accept it). `--walk`, `--watch`, `--diff`, `--storyboard`,
and `--update-goldens` reject it (exit 2) rather than silently rendering
without it. An unknown scenario name is a
usage error (exit 2) listing the available scenarios.

## Merge rules

*ccb-preview-scenario-merge*

- Branches: the named entry of the named screen is marked picked. Every
  entry still renders; the pick is a mark, not a filter. Tree:
  `branches: when state [is Loading, is Loaded [scenario], else]`; JSON:
  the branch's `"picked": {"entry": ..., "provenance": "scenario"}`.
- Lists: rows replace the 4 placeholder rows of a childless list only. A
  list that already has static children keeps them. Rows render as ordinary
  rows in the wireframe and carry `"provenance": "scenario"` in JSON.
  A row wider than its box clips and
  warns CCB-P1009, one per row — keep row strings short or widen the
  list.
- Fields: content is additional, never a replacement — the static label
  stays and the wireframe shows `[Title: Milk]`. JSON carries `"value"`
  plus `"value_provenance": "scenario"`.
- Args: values fill unknown (NULL) argument slots left-to-right, per
  matching edge independently — the same button label on two screens fills
  each screen's edge from the same values. Static literals are never
  touched, dynamic targets are never resolved, and values beyond every
  edge's slots are ignored (arities legitimately differ per screen). Tree:
  `("42" [scenario])`; JSON: parallel `"args_provenance"` array (null for
  static entries, `"scenario"` for filled ones). JSON also carries the
  scenario name top-level as `"scenario"`.
- Keys match the labels as rendered (after @string/@color resolution for
  boxes; edge source labels as shown in the tree).
- Provenance summary: scenario-supplied values are marked `[scenario]` in
  the tree (branch picks and filled args inline; list rows and field
  content on a per-screen `scenario:` line, since they have no tree line
  of their own). The wireframe shows scenario values as content; the tree
  and JSON carry the marking. Static values stay unmarked, and with no
  scenario the output is byte-identical to before.
- Mismatch is a warning, never an error and never silent: CCB-P1022 fires
  when a scenario names an unknown screen, branch entry, list, or field;
  when a list already has static rows; when a button label matches no
  edge; and when a button's edges have no unknown argument to fill. The
  entry is skipped and rendering continues. A scenario never fails the
  build gate.
- Config errors (wrong value type, unknown scenario section, keys directly
  under `[preview.scenario.<name>]`) fail the config load like any other
  config error (CCB-E1007 / CCB-E1001).

The reference sample ships a scenario: `examples/preview-sample` declares
`[preview.scenario.loaded]`, and its scenario renders are golden-tested
under `tests/golden/preview/scenario-loaded/`.
