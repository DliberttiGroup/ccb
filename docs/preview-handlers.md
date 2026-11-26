# Click handlers: derived edges

[Docs index](index.md) · [Preview](preview.md)

*ccb-preview-handlers*

A derived edge renders exactly like a flow-declared edge in the logic
tree, walk mode, storyboard, diff, and JSON (no `derived` marker
anywhere), and the build gate treats it like any other edge — except a
declared edge for the same button always wins (see precedence). A branch
edge adds one annotation: the tree and walk edge lists show
`-> Detail via "Open note" [if ready: then]`, JSON carries it as the
edge's `"branch"` field (string, or `null` for an unconditional edge).

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

## Conditional nav → branch edges

*ccb-preview-branch-edges*

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

## XML `android:onClick`

*ccb-preview-xml-onclick*

A button declaring `android:onClick="methodName"` in a layout XML file
resolves to the named handler function in code: nav calls inside that
function produce derived edges from the button. The layout parser is a
subset parser (no XML library): elements whose tag's local name ends in
`Button` (`Button`, `AppCompatButton`, `ImageButton`, …) carrying
`android:onClick` and `android:id`. `android:id` values `@+id/x` and
`@id/x` both bind under `x`. The view-id → label binding reuses
`[preview.bindings]` (same table, keyed by view id, e.g. `open_button =
"Open note"`), so a layout button and a listener receiver can share one
label. The handler fun must be a top-level fun in a flow file (any flow
file, not necessarily the file declaring the screen); like a `::` handler,
it is not a screen. Its derived edges attach to every screen whose
component tree contains a component with the bound label. Layout files are
located per the `layout` setting/fallbacks ([preview.md](preview.md#config)).
XML comments are skipped; a tag the subset parser doesn't recognize
(including `onClick` on a non-Button element) is simply not an attachment
— no edge, no warning. Distribution compares labels before `@string`
resolution, so a component labeled `@string/name` matches a binding only
under the literal `@string/name` key.

## NavController + nav graph

*ccb-preview-navcontroller*

`navigate(...)` calls inside handler bodies only (a `navigate()` call
elsewhere in flow follows the same no-edge rule as any non-handler
statement) resolve through a navigation-graph file into edges:

- `navigate("Detail")` — string form, a literal destination like
  `nav("Detail")`.
- `navigate(R.id.detail)` — id form: `detail` is looked up in the
  nav-graph table; the destination is the screen name it maps to.
- `navigate(R.id.detail, "note-42")` — extra arguments reuse the nav-args
  classification.

The nav-graph table comes from the `nav_graph` setting/fallbacks
([preview.md](preview.md#config)): any element carrying both
`android:id` and `android:name` is a destination, mapping the id entry
name to the normalized class name. Normalization strips the `[project]
package` prefix and any leading dot, then takes the simple name, so
`.DetailActivity`, `com.example.DetailActivity`, and `DetailActivity`
are the same destination. A destination with no matching flow screen
becomes a node in its own right, marked `[graph]` in the logic tree and
`"graph_only":true` in JSON: a node with unknown layout, not an error,
never triggers the empty-screen warning. Destination matching is
suffix-aware in both directions: `DetailActivity` matches a `Detail`
screen and vice versa, but `DetailActivity` does not match
`DetailFragment`. First entry wins for a duplicate id. `<action>`
elements have no `android:name` and are not destinations, so navigating
to an action id is unknown. A destination id with no graph entry, or a
`navigate()` call with no graph file at all, is an unknown edge — never a
guessed destination. A non-literal `navigate()` destination (variable,
expression) is likewise an unknown edge. A nav target is unknown
(CCB-P1005) only when it matches nothing: no screen, no graph
destination, and no activity declared in the configured manifest.

## Binding handlers to buttons: `[preview.bindings]`

*ccb-preview-bindings*

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

## Precedence: declared beats derived

*ccb-preview-precedence*

After all flow files are parsed, a post-pass reconciles derived edges
against declared edges for the same button (same `from`):

- Same destination → derived edge dropped silently (dedup).
- Different destinations → CCB-P1020 (naming the button and both
  destinations), the declared edge stands, the derived edge is dropped.
- A declared edge of any kind stands over a derived edge that does not
  match it exactly (including a derived unknown edge).

## Unknown cases: exact outcomes, never a guess

*ccb-preview-unknown*

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

## Scope limits (by design)

*ccb-preview-scope*

- Handlers are recognized ONLY inside screen fun bodies (top-level `fun
  Name` bodies that define screens). A handler written elsewhere (e.g. an
  Activity `onCreate` outside any screen fun) is ignored, not warned
  about — in flow files. Real code recognizes `onCreate` handlers in the
  app itself (see [real code](preview-real-code.md)); the flow-file rule
  is unchanged.
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
