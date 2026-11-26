# CCB real-world testing round — 2026-10-24

[Docs index](index.md)

*ccb-real-world*

Testing only: no `src/` changes, no fixes. Tool: `ccb 0.1.0` from phone
clone `~/ccb` at `9b6a079`. Phone: Termux, JDK 17, SDK build-tools 35.0.0,
platform android-35, kotlinc 2.4.21, native aapt2/zipalign/adb. All builds
ran with only `JAVA_HOME` exported (no `ANDROID_HOME`), relying on the
`~/Android/Sdk` and `<sdk-home>/kotlinc` fallbacks.

Method: projects were written following ONLY the repo docs (`README.md`,
`docs/*.md`, `requirement.md`), the way an average user would. Every place
the docs were insufficient, confusing, or wrong is logged below.
Workarounds used during the round are logged as findings, not hidden.

Projects live under `examples/real-world/`. Each has real View/XML app
code under `src/` and a flow + CSS preview sketch under `flow/` and
`styles/` (preview reads flow files, not app code).

`adb devices` on the phone shows no device attached, so `ccb install` was
not attempted. APK verification (`jarsigner -verify` + `zipalign -c`) is
the install substitute.

## Results

*ccb-real-world-results*

| Project | Build | Preview | Notes |
| ------- | ----- | ------- | ----- |
| P0 init-smoke | PASS, 24 s cold, APK 12,765 B; extended build 20 s, 16,937 B | 2 screens, 2 resolved edges, 0 unknown, 0 missing; 2× P1017 | `ccb init` → `build` → verified APK works first try. `ccb init p0-init-smoke` without flags scaffolds name "hello" and package `com.example.hello` even though the directory is named otherwise; `--package`/`--name` must be passed explicitly. `ccb init --help` prints only the global usage; the flags appear in the usage line but their semantics are undocumented (finding 8). |
| P1 notes | PASS, 26 s, APK 29,451 B | PASS with explicit keys (finding 1); 4 screens, 12 edges, all resolved, DIAGS [] | 4 activities, 3 note buttons, New-note and Settings navigation, a loading/loaded branch on Detail, one `setOnClickListener` handler, one XML `android:onClick`, nav-graph entries. Scenario `loaded`: branch Detail picked "is Loaded" [scenario]; arg "Open related" filled "note-7" [scenario]; field Title "Milk" [scenario]; list rows carry scenario provenance. Only 3× P1009 clip warnings on the scenario rows (finding 7). Walk smoke: `ccb preview --walk --record` rendered Home + edge list; keys Enter, t, j, Enter, b, q produced exactly `{"version":1,"steps":[{"screen":"Home","edge":"Open note","branch":null,"args":["note-42"]},{"screen":"Detail","edge":null,"branch":"is Loaded","args":[]}]}` — back navigation correctly NOT recorded. Trace replay: 2 steps, taken edge marked, branch picked [trace], DIAGS []. Tree-live smoke: 4 screens rendered, j/j moved selection, detail pane showed all outgoing edges + correct incoming edges, clean quit, no hang. |
| P2 catalog | PASS, 26 s, APK 29,450 B; with dep 49 s, APK 3,706,281 B | PASS, 4 screens, 13 edges, all resolved, DIAGS [] | Shop app: 4 activities, 8 product buttons, product detail with image + price, cart, checkout form (3 textfields), shape drawable, two themes. Scenario `full`: branch Detail picked "is Loaded" [scenario]; all 8 product rows + 3 cart rows with scenario provenance; fields Name "Ana", Address "1 Main St", Card "4242" all applied. Diagnostics: 11× P1009 — exactly one per scenario row (finding 7). Maven stretch: `[deps] maven = ["androidx.recyclerview:recyclerview:1.3.2"]`. First attempt (Google Maven only, `maven_central` unset): `ccb setup --yes` FAILED after 50 s, exit 3: `error CCB-E3019 [maven]: maven org.jetbrains.kotlin:kotlin-stdlib:1.6.21: POM not found in any configured repository` (finding 6). Second attempt with `maven_central = true`: setup exit 0, 45 s, "prefetched 18 artifacts (9 aar, 9 jar)", `ccb.lock` written (committed). Recyclerview classes ARE present in `classes.dex` (verified via dex strings). |
| P3 messy | PASS, 25 s, APK 21,185 B | MEASUREMENT COMPLETE — 62% of buttons edged | Deliberately messy flow sketch (4 real screens) exercising seven average-user patterns. 7 screens: 4 real + 3 phantom screens (`openDetail`, `onVarClick`, `onCrossClick`). Real buttons: 13 (Home 8, Detail 3, Form 1, Settings 1). Resolved edges from real screens: 7 (Home Go→Detail, Share→Settings; Detail Go→Home, Share→Settings, Back→Home; Form Back→Home; Settings Back→Home). Unknown edges: 1 (Home "Dynamic" → template route, per docs). Missing edges (button, no edge at all): 5, ALL on Home — Helper nav, Cross file, Var ref, Nested, Mystery. Only Mystery warns (P1019); the other 4 are completely silent. Phantom-screen edges (not button edges): openDetail→Detail, onVarClick→Form, onCrossClick→Form. Diagnostics (8 total): 1× P1019 (Mystery), 4× P1006 (Form + 3 phantoms), 3× P1007 (3 phantoms). No P1017 (P1-style CSS with explicit button/textfield font-size avoids it). The `val h = ::onVarClick` statement produced NO diagnostic at all. Duplicated labels behave as documented: both "Go" edges survive, and the "Share" onClick edge attaches to BOTH screens carrying the label (finding 9). |

P1 with docs-following config (`resources = "src/main/res/values"`, NO
explicit `layout`/`nav_graph` keys): exit 0 but DEGRADED — 5 screens
including a phantom screen `onSettingsClick`, Home's
`navigate(R.id.detail)` stayed an UNKNOWN edge, Settings unreachable
(finding 1). The committed config keeps the explicit keys.

P3 scratch variant (NOT committed): a nav graph whose destination uses a
fully-qualified name (`android:name="com.example.messy.DetailActivity"`)
makes preview exit 1 with `CCB-P1005: navigation to unknown screen
'DetailActivity'` — a real Android nav graph breaks preview with an ERROR
(finding 2).

## Ranked gaps that most hurt the average user

*ccb-real-world-gaps*

| # | Severity | Finding |
| - | -------- | ------- |
| 1 | MAJOR | `[preview] resources` silently breaks layout/nav_graph fallback discovery. Docs say `resources` holds Android values files (standard value `src/main/res/values`), but the fallback formula (`<resources>/layout`, `<resources>/navigation`) only yields a real directory if `resources` is the res ROOT. Both cannot hold for the standard layout. Result: XML-onClick handlers become phantom screens, graph navigation stays unknown, screens go unreachable — with exit 0. Repro: take committed P1, delete the explicit `layout` + `nav_graph` keys, run `ccb preview --json`. |
| 2 | MAJOR | Real nav graphs (fully-qualified `android:name`) break preview with P1005 + exit 1. The graph table keeps the class simple name, which never equals a flow screen name unless the user writes non-standard simple names in the graph. Repro: P3 scratch variant above (graph + `navigate(R.id.fqcn)`). |
| 3 | HIGH | Every button without an explicit `font-size` triggers P1017, including with the documented `common.css` example (which sets `font-size` only on `text`). Buttons/textfields default to 1 row tall, below the 2-row minimum. The reference `examples/preview-sample/` emits P1017 ten times. Workaround (validated in P1/P2/P3): add `font-size: 16px` to the `button` and `textfield` rules. |
| 4 | MEDIUM | Human-mode diagnostics are all labeled "error" regardless of severity. P1017/P1006/P1007 are warnings always per `docs/diagnostics.md` and exit code correctly stays 0, but they print as `error CCB-P1017 ...` on stderr. A user seeing "error" lines with exit 0 cannot tell whether preview failed. |
| 5 | MEDIUM | Handler funs (helper, cross-file, variable-held) become phantom screens carrying their own edges. Docs say "no edge" for the button but do not clearly say the fun becomes a screen; the user gets mystery screens plus P1006/P1007 warnings for each. P3 shows 3 phantoms from 3 ordinary patterns. |
| 6 | MEDIUM | Maven: CCB-E3019 ("POM not found in any configured repository") never suggests `maven_central`, although that is the fix when the missing artifact lives on Central (P2: kotlin-stdlib). Also, a setup prefetch failure exits 3, the code the spec reserves for toolchain problems — confusing for scripts. |
| 7 | MEDIUM | Scenario P1009 clip noise scales with data: every scenario list row warns (P2: 11 warnings for 11 rows; P1: 3 for 3). On a full scenario it buries real diagnostics. |
| 8 | MINOR | `ccb init --help` prints only the global usage, and init defaults to name "hello" / package `com.example.hello` regardless of the directory name. `ccb version` prints only `ccb 0.1.0` while spec §6.1 promises the supported compileSdk range and embedded tool expectations. Repo `README.md` Status still says "M5 preview: stretch, not started" although preview (slices, scenarios, traces, LSP, tree-live) is shipped. |
| 9 | OBSERVATION (documented, surprising) | XML-onClick derived edges attach to EVERY screen containing a component with the bound label, matched by label regardless of component type. In P1 the Settings screen's title Text("Settings") attracts the button's edge, producing a self-edge. Per docs, but an average user will not expect a title text to attract a button's edge. |

Confirmed working: `init` → `build` → signed, verified APK first try on
the phone with only `JAVA_HOME` exported; init `--package`/`--name`;
4-activity apps with drawables, themes, and nav graphs build; `@string`/
`@color` resolution works; scenario merge (branch picks, arg fills, list
rows, field values, provenance) works; walk record/replay works exactly
per docs (back nav not recorded); tree-live renders, moves, shows correct
incoming edges, quits cleanly; same-target different-button edges are
preserved; quoted TOML keys with spaces work in scenario args; Maven
prefetch → lockfile → compile → dex works end to end.

## Overall judgment

*ccb-real-world-verdict*

For BUILDING: yes, clearly. A docs-following user gets from `ccb init` to
a signed, verified APK in ~25 seconds on a phone, first try, with no
insider knowledge.

For PREVIEW: not yet, on ordinary Android code. On clean, docs-shaped
sketches preview is excellent. But the moment the input looks like a real
Android project — a `resources` dir plus layouts, a nav graph with
fully-qualified names, a handler factored into a helper or another file —
preview either silently produces a wrong tree (finding 1), fails outright
(finding 2), or fills the tree with phantom screens (finding 5).

Fix finding 1 first (the resources/layout/nav_graph fallback
contradiction: the first thing a docs-following user hits when they add
`@string`/`@color` resolution, and it fails silently), then finding 2
(FQCN graph names). Until then, preview is a tool for sketches that
already look like its examples, not yet a tool for the average user's
existing project.
