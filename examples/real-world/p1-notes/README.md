# notes (P1 real-world test)

A small notes app: Home list, Detail, Form (create/edit), Settings.
Real View/XML app code under `src/`, plus a flow + CSS preview
sketch under `flow/` and `styles/` (preview reads flow files, not
app code).

Build:

```
ccb build
```

Preview battery:

```
ccb preview
ccb preview --json
ccb preview --scenario loaded
ccb preview --walk --record /tmp/notes-walk.json
ccb preview --trace /tmp/notes-walk.json
ccb preview --tree-live
```

The sketch exercises: declared nav with args, a `setOnClickListener`
handler (`editButton`), a NavController `navigate(R.id.detail)`
handler (`quickButton`), an XML `android:onClick` button
(`settings_button` -> `onSettingsClick`), a nav graph, a
loading/loaded branch, `@string`/`@color` resources, and a
`[preview.scenario.loaded]` scenario.

NOTE: `layout` and `nav_graph` are set explicitly here. With only
`resources` set, preview did NOT discover `src/main/res/layout` or
`src/main/res/navigation` via fallback: the `onSettingsClick`
function became a phantom screen, `navigate(R.id.detail)` stayed an
unknown edge, and `Settings` was unreachable. See
`docs/real-world-testing.md` for the full finding.
