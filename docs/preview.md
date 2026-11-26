# CCB Preview — the UI convention

[Docs index](index.md)

`ccb preview` renders a terminal wireframe for projects that follow this
convention. A linter with a picture, not an emulator: it never executes
Kotlin, evaluates conditions, fetches data, or animates. Anything outside
the convention warns and falls back; it never crashes and never silently
guesses.

## Config

```toml
[preview]
styles = "styles"   # directory of CSS files
flow = "flow"       # directory of Kotlin flow files
resources = "res"   # optional: directory of strings.xml / colors.xml
layout = "res/layout"     # optional: layout XML dir or file, for android:onClick
nav_graph = "res/navigation"  # optional: nav-graph XML file or dir
phone_width = 390   # dp, informational: scales px values to columns
phone_height = 844  # dp, informational
strict = false      # true: subset warnings become errors
gate = false        # true: preview errors fail ccb check / ccb build
```

- `layout` / `nav_graph` are overrides, not requirements. Absent a key,
  preview falls back in order: `<paths res>/layout` /
  `<paths res>/navigation` when `[paths] res` is set, then
  `<resources>/layout` / `<resources>/navigation` when `resources` is
  set. Each candidate is existence-checked; a missing candidate falls
  through. Only when no candidate exists is the input set empty (not an
  error). This matters because `resources` normally points at a values
  directory (`src/main/res/values`), not the res root, so
  `<resources>/layout` would not exist and must not shadow the correct
  `<paths res>` fallback. An explicit key stays strict: a configured path
  that doesn't exist is a config error (CCB-E1006). A value may be a single
  file or a directory (a directory contributes every `*.xml` under it, in
  sorted order).
- `resources`, when set, must be an existing directory. It holds
  `strings.xml` / `colors.xml`, parsed with a tiny subset parser (no XML
  library): `<string name="n">v</string>`, `<color
  name="n">#rrggbb</color>`. Comments skipped; `&amp; &lt; &gt; &quot;
  &apos;` decoded in string values. A missing file is an empty table (a
  project may define only strings, only colors, or neither); a malformed
  entry is skipped with CCB-P1016 naming the file, a non-hex color value
  the same way. Resolution happens at load time, before layout: a label
  exactly `@string/name` (whole label, not embedded text) is replaced by
  the resolved string, so clip widths use the resolved text; a `color` or
  `background-color` exactly `@color/name` is replaced by the resolved hex
  at rule load, and counts as resolved for the CCB-P1018 contrast check.
  An unresolved reference warns CCB-P1016 naming the reference, and the
  literal text is kept — rendered as-is, never a crash, never an invented
  value.
- `styles` and `flow` are optional individually, but a project with
  neither is "preview not applicable" (exit 0) — unless it has real Kotlin
  sources (Phase 2): a `[preview]` section with no styles and no flow
  still previews when `[paths] kotlin` (or its fallbacks) exists. With no
  `[preview]` section at all, `ccb preview` prints "preview not
  applicable to this project" and exits 0.
- Frame width is `--width` (default 40 columns, minimum 24 — smaller
  values clamp with a warning).

## Flow files: screens and components

Flow files are Kotlin files in the `flow` directory. Preview parses
structure only — no typecheck, compile, or run.

- A top-level `fun Name(` is a screen named `Name`. Inside a screen,
  component calls nest by braces.
- Known components (matched case-insensitively): `Column`, `Row`, `Text`,
  `Button`, `List`, `TextField`, `Image`, `Box`, `Spacer`. Anything else
  becomes a generic box and warns (CCB-P1012) — still renders, labeled
  with its type.
- A component's label is its first string argument: `Text("My Notes")`,
  `Button("Save", ...)`.
- Navigation is any `nav("Target")` or `navigateTo("Target")` call inside a
  component's arguments or block. The edge is recorded from the enclosing
  component's label. A dynamic destination (variable, template, any
  non-literal) becomes an `unknown` edge — shown, not resolved, not an
  error.
- Arguments after the target are recorded on the edge (logic tree, walk
  edge list, JSON): a single string literal without `$` is its literal
  text, `name = <string literal without $ | number>` is the raw `name =
  value` text as written (never evaluated), anything else (identifiers,
  templates with `$`, expressions, calls) is `unknown`. A dynamic target
  with arguments keeps `target: null, unknown: true` and still records its
  args. Edges deduplicate on target, unknown flag, AND args. Tree format:
  `-> Detail("note-42", unknown) via "Open note"`; an edge with no extra
  args keeps the old format.
- `when (state) { ... }` and `if (cond) { ... }` become branch nodes
  labeled with their conditions. Branches are shown, never evaluated.
  `when` entries (`is Loading -> ...`, `else -> ...`) become entry nodes
  under the branch. An `else` after an `if` becomes an entry node labeled
  `else`.
- Entry screen: `Home` when one exists, else the first declared screen in
  sorted file order.

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

## Click handlers: derived edges

Production Kotlin writes listeners, not `Button { nav() }`. A derived edge
renders exactly like a flow-declared edge in the logic tree, walk mode,
storyboard, diff, and JSON (no `derived` marker anywhere), and the build
gate treats it like any other edge — except a declared edge for the same
button always wins (see precedence). A branch edge adds one annotation:
the tree and walk edge lists show `-> Detail via "Open note" [if ready:
then]`, JSON carries it as the edge's `"branch"` field (string, or `null`
for an unconditional edge).

Recognized shapes (all inside a screen fun body — see scope limits):

| Shape | Example |
| ----- | ------- |
| Trailing lambda | `openButton.setOnClickListener { nav("Detail") }` |
| Parenthesized lambda | `openButton.setOnClickListener({ nav("Detail") })` |
| Property assignment | `openButton.onClick = { nav("Detail") }` |
| Direct method reference | `openButton.setOnClickListener(::onOpenClick)` where `fun onOpenClick(v: View) { nav("Detail") }` is a top-level fun — same file, or any other flow file (whole-project function index resolves cross-file references when the name is unique; a name defined in two flow files warns CCB-P1024 and resolves nothing, never a guess). A reference held in a variable first also resolves, plain shape only: `val h = ::onOpenClick` (or `var`), then `setOnClickListener(h)` or `onClick = h`. The typed form (`val h: Handler = ::onOpenClick`) does not resolve. |
| Helper call | A handler body may call a helper by bare name (`openDetail()`); the helper's body is scanned one level deep with the current branch annotation, so the helper's navs become edges from the button. Calls inside the helper body are NOT followed (depth cap), so every chain terminates, including self-recursion; dotted calls (`x.helper()`) and calls to unknown names are not followed either. |

Navigation calls recognized inside a handler body: `nav("X")`,
`navigateTo("X")`, `startActivity(Intent(this,
DetailActivity::class.java))` (destination is the class literal's simple
name, exactly as written — no suffix stripped, no mapping guessed), plus
NavController `navigate(...)` (below).

### Conditional nav → branch edges

A nav/startActivity/navigate call inside an `if`/`when` branch body within
a handler body becomes an edge annotated with the branch label. The
condition is labeled, never evaluated. Labels mirror the flow branch-node
labels: `if <cond>` with entries `then` / `else`, `when <cond>` with the
entry text (`is Loaded`, `else`, …); a subjectless `when { ... }` labels
entries under just `when`. Full annotation: `"<base>: <entry>"`, e.g. `if
ready: then`, `when state: is Loaded`. Braced bodies and single braceless
statements both count (`if (ready) nav("Detail")`, `is Loaded ->
nav("Detail")`, `else nav("Form")`); a braceless body that is itself a
conditional is skipped whole, so its nav can never leak out as an
unconditional edge. Nested conditionals compound with `", "` (`if x:
then, if y: then`); an `else if` link stands alone (`else if b: then`,
`else if b: else`) — its else branch means "not the outer condition
either", so compounding it with the outer `then` label would be wrong. Nav
inside any other nested block (nested lambda/callback, coroutine, any
inner block) is still no edge at all. A nav call inside a flow-declared
branch (a `when`/`if` node in the component tree, not in a handler body)
keeps its existing behavior: an unannotated edge from the enclosing
component.

Literal arguments on handler edges reuse the flow-declared args machinery
exactly: `nav("Detail", "note-42")` → args `["note-42"]`, `nav("Detail",
id = 7)` → args `["id = 7"]` (named, shown as written). A non-literal
argument is an `unknown` arg entry, not a dropped edge. A non-literal
destination stays an unknown edge (see the outcome table).

### XML `android:onClick`

A button declaring `android:onClick="methodName"` in a layout XML file
resolves to the named handler function in code: nav calls inside that
function produce derived edges from the button. The layout parser is a
tiny subset parser (no XML library): elements whose tag's local name ends
in `Button` (`Button`, `AppCompatButton`, `ImageButton`, …) carrying
`android:onClick` and `android:id`. `android:id` values `@+id/x` and
`@id/x` both bind under `x`. The view-id → label binding reuses
`[preview.bindings]` (same table, keyed by view id, e.g. `open_button =
"Open note"`), so a layout button and a listener receiver can share one
label. The handler fun must be a top-level fun in a flow file (any flow
file, not necessarily the file declaring the screen); like a `::` handler,
it is not a screen. Its derived edges attach to every screen whose
component tree contains a component with the bound label. Layout files are
located per the `layout` setting/fallbacks above. XML comments are skipped;
a tag the subset parser doesn't recognize (including `onClick` on a
non-Button element) is simply not an attachment — no edge, no warning.
Distribution compares labels before `@string` resolution, so a component
labeled `@string/name` matches a binding only under the literal
`@string/name` key.

### NavController + nav graph

`navigate(...)` calls inside handler bodies only (a `navigate()` call
elsewhere in flow follows the same no-edge rule as any non-handler
statement) resolve through a navigation-graph file into edges:

- `navigate("Detail")` — string form, a literal destination like
  `nav("Detail")`.
- `navigate(R.id.detail)` — id form: `detail` is looked up in the
  nav-graph table; the destination is the screen name it maps to.
- `navigate(R.id.detail, "note-42")` — extra arguments reuse the nav-args
  classification.

The nav-graph table comes from the `nav_graph` setting/fallbacks above:
any element carrying both `android:id` and `android:name` is a
destination, mapping the id entry name to the normalized class name.
Normalization strips the `[project] package` prefix and any leading dot,
then takes the simple name, so `.DetailActivity`,
`com.example.DetailActivity`, and `DetailActivity` are the same
destination. A destination with no matching flow screen becomes a node in
its own right, marked `[graph]` in the logic tree and `"graph_only":true`
in JSON: a node with unknown layout, not an error, never triggers the
empty-screen warning. Destination matching is suffix-aware in both
directions: `DetailActivity` matches a `Detail` screen and vice versa, but
`DetailActivity` does not match `DetailFragment`. First entry wins for a
duplicate id. `<action>` elements have no `android:name` and are not
destinations, so navigating to an action id is unknown. A destination id
with no graph entry, or a `navigate()` call with no graph file at all, is
an unknown edge — never a guessed destination. A non-literal `navigate()`
destination (variable, expression) is likewise an unknown edge. A nav
target is unknown (CCB-P1005) only when it matches nothing: no screen, no
graph destination, and no activity declared in the configured manifest.

### Binding handlers to buttons: `[preview.bindings]`

Binding is by an explicit table only — no guessing from declaration order,
no fuzzy name matching:

```toml
[preview.bindings]
openButton = "Open note"
```

The binding key is the RECEIVER identifier at the attachment site (the
last identifier before the method in a dotted chain, so
`binding.openButton.setOnClickListener` binds under `openButton`), not the
handler fun's name. A bound handler's nav calls become derived edges with
`from` = the bound component label. A non-string value in the table is a
config error (CCB-E1007).

### Precedence: declared beats derived

After all flow files are parsed, a post-pass reconciles derived edges
against declared edges for the same button (same `from`):

- Same destination → derived edge dropped silently (dedup).
- Different destinations → CCB-P1020 (naming the button and both
  destinations), the declared edge stands, the derived edge is dropped.
- A declared edge of any kind stands over a derived edge that does not
  match it exactly (including a derived unknown edge).

### Unknown cases: exact outcomes, never a guess

| Handler body | Outcome |
| ---- | ---- |
| Receiver has no `[preview.bindings]` entry | CCB-P1019 ("handler found, button not bound", naming the receiver), no edge — even with a literal nav call inside |
| Nav with a non-literal/computed destination (`nav(target)`, a template) directly in the handler body | Unknown edge (`from` = bound label, `target: null, unknown: true`), exactly like a dynamic flow-declared nav |
| Nav inside an `if`/`when` branch body of the handler body (braced or a single braceless statement) | Branch-annotated edge: `-> Detail via "Open note" [if ready: then]`, condition labeled, never evaluated |
| Nav inside any other nested brace block of the handler body (nested lambda/callback, coroutine, any inner block) | NO edge at all, not even an unknown edge — async territory, never guessed — but CCB-P1025 warns with file:line, so the nav is visible instead of silently dropped |
| Nav with literal arguments in the handler body (`nav("Detail", "note-42")`, `nav("Detail", id = 7)`) | Edge with args, exactly like a flow-declared nav with args |
| Nav with a non-literal argument in the handler body (`nav("Detail", noteId)`) | Edge with an `unknown` arg entry, like a flow-declared nav with a non-literal argument |
| `navigate(R.id.x)` in the handler body, `x` in the nav graph | Edge to the graph's screen name for `x`; name normalized (package prefix and leading dot stripped); a destination with no flow screen becomes a `[graph]` node, not an error |
| Nav-graph destination with no flow screen, in any edge's target position | The destination is a node (`[graph]`, `"graph_only":true` in JSON), reachable like any screen; no CCB-P1005, no CCB-P1007 for the node |
| Nav target matching a manifest activity but no screen or graph destination | No CCB-P1005 (the manifest activity exempts the edge); no node is created for it |
| `navigate(R.id.x)` in the handler body, `x` not in the graph, or no graph file at all | Unknown edge (`from` = bound label, `target: null, unknown: true`), never a guessed destination |
| `navigate("Detail")` in the handler body | Edge to `Detail`, like `nav("Detail")` |
| `navigate(...)` outside a handler body | NO edge — NavController calls follow the same handler-attribution rules |
| XML `android:onClick` button, bound, handler fun found | Derived edges from the bound label, attached to every screen containing that label |
| XML `android:onClick` button with no `[preview.bindings]` entry for its view id | The layout's own `android:text` names the button; only a button with neither a binding nor a text warns CCB-P1019, no edge (same code as an unbound listener receiver) |
| XML `android:onClick` button, bound, but its label in no screen | CCB-P1021, no edge |
| XML `android:onClick` method with no matching fun, or handler with no nav | NO edge, no warning |
| Handler that merely calls a shared helper (`openDetail()`), with the nav inside the helper | Edge attributed to the button: the helper body is scanned one level deep, with the current branch annotation; calls inside the helper body are not followed, so a second-level helper's nav yields no edge — but CCB-P1025 warns for it instead of silence |
| `startActivity` without a class literal (`startActivity(intent)`) | Unknown edge, never a guessed destination |
| Method reference to a fun in another file | Resolved through the whole-project function index when the name is unique across files; same-file definitions shadow cross-file ones |
| Method reference or helper call naming a fun defined in two or more flow files | NO edge, CCB-P1024 (once per file and name) — never a guess |
| Method reference to no fun at all | NO edge, no warning — in flow files. In real code a listener `::handler` with no definition in any scanned Activity warns CCB-P1025 once per method. |

CCB-P1019, CCB-P1020, CCB-P1021, CCB-P1024, and CCB-P1025 are warnings
always — even under `--strict` — so they never fail the build gate.

### Scope limits (by design)

- Handlers are recognized ONLY inside screen fun bodies (top-level `fun
  Name` bodies that define screens). A handler written elsewhere (e.g. an
  Activity `onCreate` outside any screen fun) is ignored, not warned
  about — in flow files. Real code recognizes `onCreate` handlers in the
  app itself (see "Real code" below); the flow-file rule is unchanged.
- Named handlers must be top-level funs. A top-level fun referenced as a
  handler or helper is NOT a screen: reference counting through the index
  disqualifies any fun referenced via `::` anywhere in a fun body, from a
  setOnClickListener/onClick lambda block, or from a handler fun's body
  (iterated to a fixed point, so helper chains count). It does not appear
  in the screen list, and its nav targets become resolvable only through
  the referencing button. A bare call directly in a screen's UI body does
  NOT disqualify: that is screen composition (`Detail()` as a component),
  and the composed screen stays a screen.
- Attachments inside an ignored-call lambda block (e.g. inside a `foo {
  ... }` argument block, which the parser scans for navs only) fall back
  to the existing generic scan instead of handler attribution.
- XML `android:onClick` handler funs, unlike `::` handlers, may live in
  any flow file (the layout file, not a screen body, is the attachment
  site); they are still top-level funs only, never nested funs.
- NavController `navigate()` is recognized only inside handler bodies
  (listener lambdas, `onClick` assignments, named handlers, and
  `android:onClick` handler funs). A `navigate()` call in any other
  position is not an edge.

## Real code: screens and edges from the app itself

The flow sketch is optional. When a project has real Kotlin Activities
under `[paths] kotlin` (else `[paths] java`, else the `kotlin/`/`java/` dir
next to the manifest), preview derives screens and edges from that code,
reusing the handler machinery over a synthesized flow text per Activity —
no second flow parser, no second handler model.

| Shape | Screen / edge |
| ----- | ------------- |
| `class XActivity : Activity()` (any superclass) | Screen name = class name minus a trailing `Activity`/`Fragment` (`MainActivity` -> `Main`). A top-level fun referenced as a handler or helper is NOT a screen. |
| `onCreate` -> `setContentView(R.layout.<name>)` | Binds the Activity to its layout file. No `onCreate` or no `setContentView` yields no screen (Compose `setContent` is out of scope; a nav to such an Activity stays valid through the manifest P1005 exemption). |
| `findViewById<Button>(R.id.<id>).setOnClickListener { ... }` (also parenthesized lambda and `.onClick = { }` forms) inside `onCreate` | Receiver id maps to the layout button's label = its `android:text` (`@string` resolved through the resources table, else the id itself), merged after config `[preview.bindings]` (config wins). Body navs are the same shapes as above: `startActivity(Intent(this, Y::class.java))` (including the `val intent = Intent(...); startActivity(intent)` form), `nav("Y")`, `navigateTo("Y")`, `navigate(R.id.y)` / `navigate("Y")` via the nav-graph table. Helper calls one level deep, with the call-site literal arguments carried onto the helper's edges (`openDetail("note-1")` -> edge args `["note-1"]`; a variable argument becomes an unknown arg entry). |
| Layout `android:onClick="method"` | The method defined INSIDE the Activity class (not just top-level flow funs) -> its navs become edges from that button. The button's label is the config binding, else the layout's own `android:text`, else CCB-P1019 (no binding and no text). |
| Manifest launcher activity (MAIN/LAUNCHER intent-filter) | Entry screen when no screen is named `Home`. A launcher real screen merges into a sketch screen named `Home` (the launcher IS the entry screen; sketches name it Home while real Activities are `MainActivity`); a nav-graph destination named `Home` means the launcher screen when there is no sketch Home. |

**Merge with a sketch** (when `flow/` exists): derived (real-code)
screens/edges merge in; the same button + destination (spelled with or
without the `Activity` suffix — `Form` and `FormActivity` are the same
screen) dedups silently, keeping the sketch edge (it may carry args the
real call does not); a conflicting destination for the same button warns
CCB-P1020 and the real-code destination stands. The sketch root wins over
the real root: the sketch supplies the wireframe, the real code supplies
the edges. Real-only screens keep their layout-derived wireframe (below).

**Wireframe when there is no sketch**: the box tree is derived from the
layout XML — `LinearLayout` vertical -> Column, horizontal -> Row
(horizontal is Android's default when no `android:orientation` is given),
`TextView` -> Text, `Button` -> Button, `EditText` -> TextField,
`ImageView` -> Image, anything else -> a generic box (including
`ConstraintLayout`: no constraint solving, children still mapped).
`android:text` is the label (`@string` resolved with the other component
labels). No CSS is needed; no new layout engine, no font metrics. A
childless generic box (a Switch, a bare view) takes the 1-row leaf height
instead of vanishing. `<include>` is a generic box with no children
(includes are not resolved); `<merge>` as a root stacks its children
vertically.

**Limits, by design**:

- Only `onCreate` attachments are extracted. Listeners attached anywhere
  else, `RecyclerView` adapter bindings, and data-driven lists (a `for`
  loop over view ids, like the p2-catalog product loop) record no edge —
  the loop warns CCB-P1025 with the real source line, so the nav is
  visible instead of silently dropped.
- A listener method reference (`::handler`) with no definition in any
  scanned Activity (a top-level fun, or a typo) warns CCB-P1025 once per
  method. An `android:onClick` method with no matching fun stays silent,
  like Slice 3.
- Top-level funs in real Kotlin files (outside any Activity class) are not
  extracted; only Activity member funs are.
- Diagnostics on the synthesized flow text name the real `.kt` file, but
  the line numbers are the synthesized lines, not the real source lines —
  except the real module's own CCB-P1025s (unresolvable listeners, dropped
  member funs), which carry the true file:line. Treating synthesized line
  numbers as real ones is a known limitation, not a bug to work around
  silently.
- The flow sketch's onClick distribution matches BUTTON boxes only: a
  `TextView` that shares a button's text (a screen title, e.g. `"Settings"`
  on the Settings screen) is a different component and no longer attracts
  the button's edge.

## Scenarios: declared runtime facts

A scenario is runtime facts for a project, declared in ccb.toml — the
answer to "real runtime behavior without an interpreter". A scenario never
evaluates anything: it is a data input merged at render time, on top of the
static tree. It only fills unknowns (unknown arguments, unpicked branches,
placeholder rows, empty fields) and picks branches. It never overrides a
static literal, never resolves a dynamic target, and never hides an
unpicked branch entry. The same rules apply to traces (below) — a trace
fills only the taken edge, positionally; a scenario fills every matching
edge's unknown slots left-to-right.

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
logic tree all accept it; `--walk` takes it in a later chunk, and
`--watch`, `--diff`, `--storyboard`, and `--update-goldens` reject it
rather than silently rendering without it). An unknown scenario name is a
usage error (exit 2) listing the available scenarios.

Merge rules:

- Branches: the named entry of the named screen is marked picked. Every
  entry still renders; the pick is a mark, not a filter. Tree:
  `branches: when state [is Loading, is Loaded [scenario], else]`; JSON:
  the branch's `"picked": {"entry": ..., "provenance": "scenario"}`.
- Lists: rows replace the 4 placeholder rows of a childless list only. A
  list that already has static children keeps them. Rows render as ordinary
  rows in the wireframe and carry `"provenance": "scenario"` in JSON.
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

## Traces: recorded runtime paths

A trace is an ordered list of navigation steps, one per navigation taken —
recorded from a walk, or (later) emitted by a tiny in-app logger. No logger
is built in; the format below is the contract one is written against. Same
merge rules as scenarios (above): marks the taken path, fills unknown
arguments of taken edges, picks branch entries. Never overrides a static
literal, never resolves a dynamic target, never hides an unpicked branch
entry.

Trace format (JSON, version 1):

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

Render rules:

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

## Styles: one CSS file per screen, plus common.css

- Scope: `common.css` (when present in the styles directory) applies to
  every screen. `<Screen>.css` applies to that screen only, and only when
  a screen of that name exists in the flow files. Any other CSS file is
  ignored with a CCB-P1015 warning and is never parsed. Files are parsed
  in sorted order, `common.css` first, then per-screen files. A per-screen
  file redeclaring a selector keeps the earlier rule for other screens and
  adds a scoped copy for its own screen: the copy carries the earlier
  properties plus the per-screen file's declarations, and layout prefers
  the exact-scope rule over the common one. So `common.css` properties
  still apply everywhere, per-screen properties apply only to that screen,
  and two per-screen files declaring the same selector (e.g. `Empty.css`
  and `Settings.css` both declaring `column`) each get their own scoped
  rule.
- Selectors are `screen` (the root box) or a lowercase component type
  (`column`, `row`, `text`, `button`, `list`, `textfield`, `image`, `box`,
  `spacer`). Later declarations for the same selector override earlier
  ones property by property.
- Supported properties: `display` (block, flex), `flex-direction` (row,
  column), `width`/`height` (px, %), `padding`, `margin` (px, 1–4 values
  as in CSS), `background-color`, `color` (hex or named, or an
  `@color/name` reference resolved via `[preview] resources`), `font-size`
  (px, sets the text-leaf line height — round(font-size * 1.2 / 10) rows,
  minimum 1, 1 row when no rule sets it — scales the `text-align` width by
  font-size bucket (small < 14px: 4/5, base 14–23px: 1, large ≥ 24px: 5/4),
  and selects the CCB-P1018 contrast threshold), `position` (static,
  absolute), `top`/`left` (px, absolute only), `gap` (px), `text-align`
  (left, center, right), `border-width` (px), `border-color` (hex or
  named), `flex-wrap` (nowrap, wrap; row containers only).
- Deliberately NOT supported (they warn as unknown properties, CCB-P1001,
  like anything else outside the subset): `justify-content` /
  `align-items` (distribution semantics the sum-based row/column model
  cannot express honestly), `border-radius` (no wireframe-visible rounding
  at 1-cell scale), `line-height` (text is single-row by design; it would
  only pad), and the `border` shorthand (use the two longhands above).
- Anything else warns with the property name and location (CCB-P1001
  unknown property, CCB-P1014 unsupported value) and rendering continues
  with a fallback. Under `--strict` (or `[preview] strict = true`) those
  warnings become errors.

Example:

```css
screen { background-color: #ffffff; padding: 16px; }
text { color: #111111; font-size: 16px; }
button { background-color: #6200ee; color: #ffffff; margin: 8px; }
```

## Layout model (terminal columns and rows)

- The phone frame is `--width` columns wide (default 40); the content
  interior is frame minus the two border columns.
- px values scale to columns/rows by `interior / phone_width` (so 16px of
  padding on a 390dp phone at 40 columns is about 2 rows). `%` widths and
  heights are relative to the parent content size.
- Column containers stack children vertically, each full width minus
  margins. Row containers split the width: fixed sizes honored first, the
  remainder shared equally among auto-sized children.
- Text leaves (`text`, `button`, `textfield`) are as tall as their line
  height: round(font-size * 1.2 / 10) rows, minimum 1 row, 1 row when no
  rule sets `font-size` — so a 16px label is 2 rows tall and a 24px label
  is 3. The painted text itself still sits on one row (top-aligned) inside
  the leaf, clipped to its box (clipping warns, CCB-P1009). Buttons render
  as `[ label ]`, text fields as `[label]`, lists as one `• <label>` row
  per child item when the List box has children (a child with no label
  renders as `• item`); a bare List with no children — its count genuinely
  unknown — renders 4 `• item` placeholder rows and emits no diagnostic
  for the placeholder. A list with children is exactly one row per child
  tall (plus padding); a bare list is 4 rows tall (plus padding); images
  as a `[ image ]` block, 3 rows tall, or the rule's declared `height`
  when one is set (a declared `width` is honored as for any box; with no
  intrinsic size in the convention, one declared side cannot preserve an
  aspect ratio). Unknown components render as `[TypeName]`.
- Leaf content is inset by the leaf's own scaled padding: a leaf paints at
  (box x + left padding, box y + top padding), clipped to the box width
  minus left and right padding. Parent padding insets children the same
  way, through the parent content box.
- `text-align` offsets the painted string within the leaf's content width
  (left = content edge, center = centered, right = flush right, clamped at
  the content edge when the string is wider); it applies to `text`,
  `button`, `textfield`, and list rows. For `text`, `button`, and
  `textfield` the offset uses the font-size-scaled string width (buckets
  above), not the raw painted width — only the offset shifts, painting
  still clips at the box edge.
- Every box's size is also reported in dp in JSON (`w_dp`, `h_dp`): w_dp =
  round(w * phone_width / interior), h_dp = h * 10 (one row is ~10dp).
  Estimates from the cell grid, not measurements.
- A `border-width` that scales to at least 1 cell draws a 1-cell outline
  around the box (Unicode frame chars, or ASCII under `--ascii`) in the
  `border-color`, else the rule's `color`, else the terminal default.
  Content is inset 1 cell on all sides, so a bordered box's content box
  shrinks by 1 cell per side and its height grows by 2 rows (top + bottom
  border); for containers the outline goes around the container box and
  children are laid out inside the inset content box. `border-width: 0`
  (or any value scaling to 0) means no border; a negative width or an
  invalid border color warns (CCB-P1014).
- In a row container with `flex-wrap: wrap`, children are placed
  left-to-right and a child that would exceed the content width starts a
  new line; the new line's y advances by the previous line's height (its
  tallest occupied child height). Total row height is unchanged by
  wrapping: it still sums every child's height, as in the nowrap model.
  `flex-wrap` on a non-row container is ignored with a CCB-P1014 warning
  naming the selector.
- Absolute-positioned boxes do not affect parent height. Boxes that overlap
  (CCB-P1003) or extend past the frame (CCB-P1004) warn.

## Lint diagnostics (after layout)

- CCB-P1017 — tap target too small (warning). Applies to `button` and
  `textfield` boxes only, after layout. Warns when `w < 8` columns OR (`h
  < 2` rows with a known height), in wireframe cells. Known-height rule:
  width is always layout-computed, so `w < 8` always counts, but height
  without a `font-size` rule is the 1-row default, not a measurement, so
  `h < 2` counts only when the effective rule sets `font-size`. A button
  with no sizing rules at all is 38x1 by default and no longer warns. The
  thresholds stay in cells even though JSON reports dp (a 48dp height rule
  would flag nearly every button; a 48dp width rule, ≈ 5 columns, would be
  looser than 8 columns ≈ 82dp and un-flag degenerate row-split cases).
  The message reports the dp size alongside the cells, e.g. `tap target
  too small: button "Go" is 5x1 cells (51dp x 10dp; minimum 8x2 cells)`.
  Documented dp equivalents at the default frame: 8 columns ≈ 82dp wide,
  2 rows = 20dp tall.
- CCB-P1018 — text contrast below threshold (warning). Applies to `text`,
  `button`, and `textfield` leaves, after layout. Foreground = the leaf
  rule's `color`; background = the leaf rule's `background-color`, else
  the `screen` rule's `background-color`. If either side is missing or
  unresolvable to RGB (hex `#rgb`/`#rrggbb` or the renderer's named-color
  table), the check skips silently — the contrast cannot be judged.
  Resolved `@color` values count as resolved. Ratio is WCAG relative
  luminance, contrast = (L1 + 0.05) / (L2 + 0.05). Threshold: **4.5:1**
  normally, **3.0:1** when the leaf rule's `font-size` is ≥ 24px (WCAG
  large-text threshold). The message includes the ratio to one decimal,
  e.g. `text contrast 2.1:1 below 4.5:1 for text "Note detail"`.

Both are warnings always — even under `--strict` — so they never fail the
build gate, which fails only on errors.

## Output and exit codes

- `ccb preview` — every screen's wireframe, then the logic tree, then
  diagnostics. `ccb preview <screen>` (or `--screen NAME`) — one screen's
  wireframe, then the logic tree, then diagnostics.
- `--json` — the same box trees, logic tree, and diagnostics as structured
  data. `--ascii` — ASCII frame characters instead of Unicode. Colors come
  from the CSS colors approximated to ANSI 256-color codes, unless
  `NO_COLOR` is set, the terminal is dumb, or `--ascii` is given.
- `--output FILE` — write the preview render to FILE instead of stdout:
  the human render (frames + logic tree), or the JSON object when `--json`
  is also given. Stdout carries nothing in that case; diagnostics still go
  to stderr, and exit codes are unchanged (a preview error still exits 1
  with the render written; an unwritable output file exits 2).
- `--device phone|tablet` — frame-width presets on top of `--width`: phone
  = 40 columns (the default), tablet = 80 columns. An explicit `--width`
  always wins over `--device`; any other value is a usage error.
- `--storyboard "A,B,C"` — render a journey, not the whole app: the
  argument is a comma-separated list of screen names, in journey order.
  Repeats are allowed (a journey may revisit a screen); each occurrence
  renders again. Human output prints each screen's frame preceded by a
  header line `Journey N/M: <Name>` (N = 1-based position, M = journey
  length), frames in order, then the normal logic tree, then diagnostics —
  exactly like a multi-screen render, with the header lines added.
  `--json` keeps only the journey screens in the `screens` array, in
  journey order (repeats as separate entries). Names are trimmed of
  surrounding whitespace (`"Home, Detail"` works). Errors (exit 2): an
  unknown screen name in the list, an empty list, an empty element
  (`"Home,,Detail"`), or a whitespace-only element. Conflicts (exit 2):
  `--storyboard` with `--screen` (a positional screen counts as
  `--screen`), `--walk`, `--watch`, `--diff`, or `--update-goldens`. It
  composes with `--ascii`, `--width`, `--device`, `--output`, `--strict`,
  and `--json`. Walk/watch are untouched: storyboard is a render mode, not
  a loop.
- `--update-goldens` — regenerate the checked-in golden files under
  `tests/golden/preview/` (one `<Screen>.txt` frame per screen,
  `logic-tree.txt`, `preview.json`) from the current render. Frame goldens
  use the current `--ascii` setting and no color. For testing without
  touching the real goldens, set `CCB_GOLDEN_DIR` to a scratch directory
  (test-only override); otherwise the directory is found by walking up
  from the working directory to `tests/golden/preview/`.

Exit codes: see [diagnostics.md](diagnostics.md) (0 ok, warnings allowed;
1 preview errors; 2 configuration errors). Under `--strict`, subset
warnings are errors, so they exit 1 as well.

### JSON shape (`--json`)

One JSON object, no trailing newline, deterministic (stable order, no
timestamps). `screens` are in flow-file order; `boxes` is the screen's box
tree with the root box as the tree root; `branches` are collected
depth-first from the tree; `diagnostics` are in emission order:

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

`label`, `target`, and `file` are `null` when absent; `line` is -1 for
diagnostics with no source position. In JSON mode stdout carries only this
object; human-mode prose and diagnostics go to stderr or are suppressed.
The object carries `"version":2` alongside `"event":"preview"` (version 2's
shape changes over version 1 are the per-edge `"args"` array,
string-or-null, `[]` when the nav call has no extra arguments, and the
per-edge `"branch"` field, string-or-null, `null` for an unconditional edge
— both additive, so the version stays 2; resolved `@string` labels appear
as plain `label` values, a value change, not a shape change, and only in
projects that use `@string`). The per-box `"w_dp"`/`"h_dp"` fields are
likewise additive, so the version stays 2. With `--storyboard`, the
`screens` array contains only the journey screens, in journey order.

## Build gate: `ccb check` / `ccb build` fail on preview errors

Opt-in, off by default. Enable with `[preview] gate = true` in ccb.toml,
or per-invocation with `ccb check --preview-gate` / `ccb build
--preview-gate`; `--no-preview-gate` forces it off. Precedence:
`--no-preview-gate` beats `--preview-gate` beats the config key; passing
both flags is a usage error (exit 2).

When enabled, the gate runs preview diagnostics for the project:

- In `check`, after toolchain validation; in `build`, before any pipeline
  stage, so a failing gate never starts a build.
- Severity rule: any preview ERROR diagnostic fails the gate (missing nav
  target CCB-P1005, duplicate screen CCB-P1008, and CCB-P1001/CCB-P1014
  under strict); WARNING diagnostics alone never fail it.
- Failure exits 1 with one concise line per failing diagnostic on stderr
  (`CCB-P1005: navigation to unknown screen 'Ghost'`); no screen renders
  are printed. In `--json` mode the failure still surfaces as those stderr
  lines plus the nonzero exit, not as a JSON event.
- Projects with no usable `[preview]` section pass silently — the gate
  never forces preview onto a project that doesn't use it. When config load
  itself fails, the gate is skipped and the normal config error is reported
  instead (check exits 2; build's pipeline reports it).

## Diff mode: `ccb preview --diff <snapshot-dir>`

Compares the current renders against a saved snapshot directory in the
golden-file format (one `<Screen>.txt` frame per screen, `logic-tree.txt`,
`preview.json` — the same files `--update-goldens` writes, rendered with
the same calls, so a snapshot made by `--update-goldens` diffs identical).

- Exit 0 when identical. On differences: exit 1 and one unified diff per
  differing file, headed by the file name, plus a summary line naming all
  differing files (`differing files (2): Home.txt, preview.json`).
- A current screen missing from the snapshot is reported as `missing:
  <file> (not in snapshot)`; a snapshot file with no matching current
  screen as `stale: <file> (no matching current screen)`. An unreadable
  snapshot directory, a project with no usable `[preview]` section, or an
  unknown `--screen` name exits 2 with a clean diagnostic; outside any
  project, `--diff` is a usage error (exit 2) rather than plain preview's
  "not applicable". Preview errors in the project itself do not change the
  diff exit code — the render still compares.
- `--diff` + `--screen NAME` diffs only that screen's `<Screen>.txt` file.
  `--diff` + `--output FILE` writes the diff report to the file instead of
  stdout. `--diff` + `--json` emits one machine-readable object instead of
  human diff text: `{"event":"preview-diff","identical":bool,"files":[{"name":...,"status":"identical|different|missing|stale","diff":string-or-null}]}` (the diff string is present only for "different" files).
- Conflicts (exit 2): `--diff` cannot be combined with `--watch`,
  `--walk`, or `--update-goldens`.

## Walk mode: `ccb preview --walk [screen]`

Walk mode is static exploration of the logic tree, not an emulator: it
moves through screens and edges the project already declares. It never
executes Kotlin, never evaluates a `when`/`if` condition, never fetches
data, and never animates.

`ccb preview --walk` starts at the entry screen (`Home` when one exists,
else the first screen); `ccb preview --walk Detail` (or `--screen Detail`)
starts at the named screen. Each view shows the screen's wireframe, its
outgoing edge list (showing the same argument text as the logic tree, e.g.
`-> Detail("note-42")`) with a `>` cursor, the back stack, and footer
hints.

Keys: up/down arrows or `j`/`k` move the selection (selection clamps at the
ends, it never wraps); Enter follows the selected edge; `b` or Backspace
goes back; `q` quits. `t` enters branch-pick mode for the current screen,
listing its branch entries as `when state: is Loaded`; up/down move among
them and Enter picks one. A pick never evaluates the condition: it follows
the first edge whose source label names a component inside the picked
entry, or, when the entry has no outgoing edge, records the pick and stays
on the screen. Esc (or `b`) leaves branch mode without picking.

Following an unknown edge — a dynamic destination, or a target screen that
does not exist — shows it as unknown and stays on the current screen; walk
mode never invents a destination. Back at the root screen is a no-op.
Ctrl-C quits and restores the terminal, as do SIGINT/SIGTERM. Walk mode
requires a terminal: with non-TTY stdin or stdout it prints an error and
exits 2 instead of hanging.

`ccb preview --walk --record FILE` records the walk as a trace (see
"Traces" above) and writes it when the walk exits: one step per forward
navigation taken, plus pick-only steps for branch picks that did not
navigate. Back navigation and unknown follows record nothing. `--record`
without `--walk` is a usage error.

## Watch mode: `ccb preview --watch [screen]`

Watch mode renders once immediately, then re-renders on every change to
the preview inputs — no restart, no rebuild. It composes with `--screen
NAME` (or a positional screen), `--ascii`, `--width`, `--device`,
`--json`, `--strict`, and `--output`; all of them keep their single-render
meaning on every re-render.

Watched set: every CSS file under the styles directory, every file under
the flow directory, and `ccb.toml` itself. New and deleted files count as
changes, since each rescan is diffed against the previous one. `ccb.toml`
is re-loaded on every change, so editing the `[preview]` paths re-points
the watch at the new directories.

Refresh framing depends on where the render goes:

| Destination | Framing |
| ----------- | ------- |
| TTY stdout | Screen cleared first (`ESC[2J ESC[H`), then the fresh render. The initial render is not cleared. |
| Piped human mode | Renders separated by a delimiter line on its own line, `--- ccb preview refresh ---`, before each re-render after the first. |
| `--json` piped mode | No delimiter — each render is one JSON object plus a newline, i.e. valid JSONL, flushed after each render. |
| `--output FILE` | File rewritten on each change, stdout stays empty, refresh notice plus diagnostics go to stderr. |

Changes are debounced ~200ms: a burst of saves (or one save that touches
several files) produces a single re-render once the inputs have been quiet
for 200ms. A broken save never kills the loop: a half-written TOML, broken
CSS, or broken Kotlin prints diagnostics and the loop keeps watching on the
last good render, re-rendering when the inputs become valid again. If the
selected screen disappears after a change, the loop prints a diagnostic
naming it and keeps watching instead of crashing or exiting.

Quit with Ctrl-C (or SIGTERM): the loop exits 0, flushing stdout first.
Watch mode never enters raw mode, so there is no terminal state to restore.

Conflicts (exit 2, same style as the other preview conflicts): `--watch`
cannot be combined with `--update-goldens`, and it cannot be combined with
`--walk` — an interactive loop and a watch loop cannot share stdout
semantics. `--watch` with `--output` is allowed.

## LSP server: `ccb lsp [--scenario NAME] [--trace FILE]`

A long-running server speaking JSON-RPC over stdio with LSP-style
`Content-Length` framing. It serves the same logic tree as `ccb preview
--json` (screens, edges, branch labels, args, plus the active
scenario/trace overlay when one is selected), live: edit a file and the
tree updates without restarting. Single-threaded `select()` loop over stdin
+ inotify; no threads (v1 rule). The terminal tree client (`ccb preview
--tree-live`, below) and plain LSP clients both talk to this server; no
editor plugin ships with CCB itself.

Standard methods: `initialize`, `shutdown`, `exit`,
`textDocument/didOpen`, `didChange` (full sync: the last
`contentChanges[]` entry's text), `didSave` (no-op — text is already
synced), `didClose`. Custom methods: `ccb/logicTree` (the whole tree
object, spliced verbatim from the preview JSON), `ccb/screenInfo`
(`{"screen": <one screen object>, "incoming": [...]}` — incoming entries
carry `from_screen, from, target, unknown, args, branch`; self-edges
included), `ccb/diagnostics` (`{"diagnostics": [...]}` mirroring the tree's
diagnostics exactly). `ccb/publishTree` is a push notification carrying the
whole tree, sent after `initialize` and after every rebuild that changes
the tree JSON (compared by string equality; the first publish always
sends).

`initialize` result: `{"capabilities":{"textDocumentSync":1},
"serverInfo":{"name":"ccb lsp","version":<version>}}`. Root priority:
`rootUri`, then the first `workspaceFolders[].uri`, then `rootPath`. A
second `initialize` with a different root re-roots: it commits only after a
throwaway config load proves the new root has a usable `[preview]` section;
otherwise the root is ignored and the old tree keeps serving. Exit codes: 0
when `shutdown` preceded `exit`/EOF, 1 otherwise. Error codes: unparseable
JSON `-32700` (null id), unknown method `-32601`, request after shutdown
`-32600`, unknown screen `-32602`, no project `-32603`.

What re-parses when:

- Flow and CSS files parse once per file into staging arenas, re-parsing
  only when that file changes — editor `didChange` rebuilds immediately,
  disk changes debounce 200ms (the watch module's debounce).
- `ccb.toml` is read from disk only: unsaved editor changes take effect
  on save. On reload, a bindings change or a nav/layout/resources path
  change invalidates the whole flow cache; a styles/flow directory change
  rebuilds the watch context and snapshot. A failed reload (unloadable
  TOML or no usable `[preview]`) keeps the old config and the last good
  project and retries on the next change — a broken save never kills the
  server.
- Nav/layout XML is rescanned fresh on every build (small files); a
  nav/layout content change invalidates the flow cache. Resources
  (`strings.xml`/`colors.xml`) are re-read on every build as part of
  assembly, so a resources change only needs to trigger a rebuild.
- Open flow/CSS docs override disk content until closed; disk changes to
  open docs are ignored until close. Open docs for files not yet on disk
  are unioned into the file lists at resync, so unsaved new files still
  parse. A rebuild consumes pending disk changes too and resets the
  debounce.

Incrementality is structural, not a flag: the server deep-copies pristine
per-file parses into a fresh project arena on every build (scenario/trace
apply and resources resolution mutate boxes, so parsed boxes are never
reused across builds), then runs the same assembly the load path runs —
`pv_project_assemble` shared, not forked. The server tree JSON (no
scenario/trace) is byte-identical to `ccb preview --json` for the same
project (modulo the CLI's trailing newline); the test suite pins this
against the sample golden. The CSS merge is likewise shared:
`pv_css_rule_merge` replicates `pv_css_parse`'s cross-file semantics
exactly (same-file/same-scope in place; a per-screen redeclare splits a
copy carrying the new file/scope and the new declaration's line; a common
file merges into the first matching rule).

Other preview flags are rejected for `lsp` (exit 2): it takes only
`--scenario` and `--trace`. Unknown scenario / unreadable trace / no
`ccb.toml` / preview not applicable are exit 2, matching the CLI. The
project is found by upward search from the working directory, like
`preview`.

## Tree-live mode: `ccb preview --tree-live [--scenario NAME] [--trace FILE] [screen]`

The terminal tree client: it spawns `ccb lsp` as a child process,
initializes it at the project directory, and renders the logic tree live in
the terminal. It never parses flow or CSS itself; everything it shows is a
published tree from the server.

The view has three parts. A screen list with a `>` cursor (`[entry]` marks
the entry screen, `(unreachable)` marks screens no edge reaches). A detail
pane for the selected screen: its outgoing edges (`-> Target(args) via
"label" [branch] [trace]`, `-> unknown` for dynamic destinations), its
incoming edges (`<- Screen via "label"` — self-edges included), and its
branches (`when state: is Loading, is Loaded, else (picked: is Loaded
[scenario])`). Diagnostics inline below, in project order (`CODE message
(file:line)`; the file part is omitted when the diagnostic names no file).
A `scenario: NAME` / `trace: N steps` line appears when an overlay is
active. Argument provenance is per-slot: a scenario- or trace-filled
literal renders as `"value" [scenario]` / `"value" [trace]`; static
literals stay unmarked, and the published JSON carries no named-arg flag,
so every literal renders quoted here.

Keys: `j`/`k` or up/down move the selection (clamps at the ends, never
wraps); `q` or Ctrl-C quits. A `ccb/publishTree` notification re-renders
the view: the selection follows the selected screen's name when it still
exists, else clamps to the previous index. Before the first publish the
view says "waiting for logic tree..." and keys still work, so a slow server
never looks hung.

Composition and exit codes: `--scenario` and `--trace` are passed to the
spawned server (an unknown scenario or an unreadable / malformed trace is
exit 2, validated before spawning); `--screen` or a positional screen
selects the initial screen after the first publish (an unknown name is exit
1 with the available screens listed). `--tree-live` rejects `--walk`,
`--watch`, `--diff`, `--storyboard`, `--output`, and `--update-goldens`
(exit 2); `--ascii`/`--width`/`--device` are accepted but have no effect on
the tree view. Non-TTY stdin or stdout is exit 2 with a diagnostic, never
a hang. The server is shut down cleanly on every exit path (shutdown +
exit, bounded wait, SIGKILL fallback), and the terminal is restored. If the
server ends without ever publishing a tree, or ends unexpectedly
mid-session, the client prints a diagnostic and exits 1. The state machine
(selection, publish application, framed-message feed) is unit-tested with
scripted keys and canned payloads in `tests/test_preview_tree.c`; the
terminal loop itself is covered by a scripted pty session on the phone,
like walk mode.

## Worked example

The reference sample app lives in `examples/preview-sample/`. Run it with:

```sh
cd examples/preview-sample
ccb preview
ccb preview Detail
ccb preview --json
```

Its checked-in renders are the golden files under `tests/golden/preview/`
— the test suite diffs actual output against them, so any renderer change
that alters output must update the goldens deliberately, never silently.
