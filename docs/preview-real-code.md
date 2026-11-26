# Real code: screens and edges from the app itself

[Docs index](index.md) · [Preview](preview.md)

*ccb-preview-real-code*

The flow sketch is optional. When a project has real Kotlin Activities
under `[paths] kotlin` (else `[paths] java`, else the `kotlin/`/`java/` dir
next to the manifest), preview derives screens and edges from that code,
reusing the handler machinery over a synthesized flow text per Activity.

| Shape | Screen / edge |
| ----- | ------------- |
| `class XActivity : Activity()` (any superclass) | Screen name = class name minus a trailing `Activity`/`Fragment` (`MainActivity` -> `Main`). A top-level fun referenced as a handler or helper is NOT a screen. |
| `onCreate` -> `setContentView(R.layout.<name>)` | Binds the Activity to its layout file. No `onCreate` or no `setContentView` yields no screen (Compose `setContent` is out of scope; a nav to such an Activity stays valid through the manifest P1005 exemption). |
| `findViewById<Button>(R.id.<id>).setOnClickListener { ... }` (also parenthesized lambda and `.onClick = { }` forms) inside `onCreate` | Receiver id maps to the layout button's label = its `android:text` (`@string` resolved through the resources table, else the id itself), merged after config `[preview.bindings]` (config wins). Body navs are the same shapes as [handlers](preview-handlers.md): `startActivity(Intent(this, Y::class.java))` (including the `val intent = Intent(...); startActivity(intent)` form), `nav("Y")`, `navigateTo("Y")`, `navigate(R.id.y)` / `navigate("Y")` via the nav-graph table. Helper calls one level deep, with the call-site literal arguments carried onto the helper's edges (`openDetail("note-1")` -> edge args `["note-1"]`; a variable argument becomes an unknown arg entry). |
| Layout `android:onClick="method"` | The method defined INSIDE the Activity class (not just top-level flow funs) -> its navs become edges from that button. The button's label is the config binding, else the layout's own `android:text`, else CCB-P1019 (no binding and no text). |
| Manifest launcher activity (MAIN/LAUNCHER intent-filter) | Entry screen when no screen is named `Home`. A launcher real screen merges into a sketch screen named `Home` (the launcher IS the entry screen; sketches name it Home while real Activities are `MainActivity`); a nav-graph destination named `Home` means the launcher screen when there is no sketch Home. |

## Merge with a sketch

*ccb-preview-real-merge*

When `flow/` exists, derived (real-code) screens/edges merge in: the same
button + destination (spelled with or without the `Activity` suffix —
`Form` and `FormActivity` are the same screen) dedups silently, keeping
the sketch edge (it may carry args the real call does not); a conflicting
destination for the same button warns CCB-P1020 and the real-code
destination stands. The sketch root wins over the real root: the sketch
supplies the wireframe, the real code supplies the edges. Real-only
screens keep their layout-derived wireframe (below).

## Wireframe when there is no sketch

*ccb-preview-real-wireframe*

The box tree is derived from the layout XML — `LinearLayout` vertical ->
Column, horizontal -> Row (horizontal is Android's default when no
`android:orientation` is given), `TextView` -> Text, `Button` -> Button,
`EditText` -> TextField, `ImageView` -> Image, anything else -> a generic
box (including `ConstraintLayout`: no constraint solving, children still
mapped). `android:text` is the label (`@string` resolved with the other
component labels). No CSS is needed; no new layout engine, no font
metrics. A childless generic box (a Switch, a bare view) takes the 1-row
leaf height instead of vanishing. `<include>` is a generic box with no
children (includes are not resolved); `<merge>` as a root stacks its
children vertically.

## Limits, by design

*ccb-preview-real-limits*

- Only `onCreate` attachments are extracted. Listeners attached anywhere
  else, `RecyclerView` adapter bindings, and data-driven lists (a `for`
  loop over view ids, like the p2-catalog product loop) record no edge —
  the loop warns CCB-P1025 with the real source line, so the nav is
  visible instead of silently dropped.
- A listener method reference (`::handler`) with no definition in any
  scanned Activity (a top-level fun, or a typo) warns CCB-P1025 once per
  method. An `android:onClick` method with no matching fun stays silent.
- Top-level funs in real Kotlin files (outside any Activity class) are not
  extracted; only Activity member funs are.
- Diagnostics on the synthesized flow text name the real `.kt` file, but
  the line numbers are the synthesized lines, not the real source lines —
  except the real module's own CCB-P1025s (unresolvable listeners,
  dropped member funs), which carry the true file:line.
- The flow sketch's onClick distribution matches BUTTON boxes only: a
  `TextView` that shares a button's text (a screen title, e.g. `"Settings"`
  on the Settings screen) is a different component and no longer attracts
  the button's edge.
