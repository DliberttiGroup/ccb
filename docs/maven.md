# Remote Maven dependencies

[Docs index](index.md)

*ccb-maven*

CCB resolves remote Maven artifacts declared in `ccb.toml` and feeds them
into the deps step as if they were local AARs/JARs. No Gradle: POM
parsing, downloads, and the local cache.

Sub-pages: [resolution](maven-resolve.md) · [cache, hermeticity,
lockfile](maven-cache.md) · [troubleshooting](maven-troubleshooting.md)

## Declaring dependencies

*ccb-maven-declare*

```toml
[deps]
maven = [
  "androidx.core:core-ktx:1.13.1",
  "com.google.code.gson:gson:2.11.0",
]
maven_central = true
```

- `maven`: array of `group:artifact:version` coordinates. Version specs:
  - exact: `1.13.1`
  - range: `[1.0,2.0)`, `[1.0]`, `(,1.5]` — resolved from the artifact's
    `maven-metadata.xml`; the highest published version in the range
    wins
  - `LATEST` / `RELEASE`: from the metadata's `<latest>` / `<release>`
    tags
  - `-SNAPSHOT`: `1.0-SNAPSHOT` resolves to the newest timestamped
    build from the snapshot metadata (`1.0-20261126.123456-3`); the
    lockfile records the concrete timestamped version
  Version comparison follows Maven's ComparableVersion rules (numeric
  tokens numerically; qualifier order alpha < beta < milestone < rc <
  snapshot < release < sp; `1.0` == `1.0.0`). A range no published
  version satisfies, or a snapshot with no snapshot metadata, fails
  with `CCB-E3019`. Metadata is re-downloaded fresh at setup time, so
  ranges and snapshots float; cache-only builds use the cached copy, so
  a build never downloads. Local `jars`/`aars` keep working alongside
  `maven`.
- `maven_central`: when `true`, Maven Central is searched after Google
  Maven. Default `false`: Google Maven only.
- `maven_profiles`: array of profile ids to activate (see
  [resolution](maven-resolve.md#profiles)). Default empty: only
  `<activeByDefault>` profiles apply.
- `maven_repos`: array of extra repositories (see below).
- `maven_exclude`: array of exclusions, each `group:artifact` (every
  version) or `group:artifact:version` (that version only). An excluded
  artifact never resolves, even transitively. Excluding a direct
  dependency is a config error at load with `CCB-E1005` naming both the
  exclusion and the dependency; excluding some other version of a direct
  dependency is allowed. Per-dependency `<exclusions>` in POMs are
  honored too (see [resolution](maven-resolve.md#exclusions)).

## Compiler plugins

*ccb-maven-plugins*

`[build] kotlin_plugins` declares kotlinc compiler plugins as Maven
coordinates:

```toml
[build]
kotlin_plugins = ["org.jetbrains.kotlin:kotlin-compose-compiler-plugin:2.4.21"]
kotlin_plugin_options = ["androidx.compose.compiler.plugins.kotlin:generateFunctionKeyMetaClasses=true"]
```

- Plugin coordinates must be exact versions (ranges, `LATEST`,
  `RELEASE`, `-SNAPSHOT` rejected at config load with `CCB-E1005`): a
  plugin must match the kotlinc version.
- Plugin jars resolve via Maven like any artifact (same repositories,
  cache, and checksums) but without a transitive walk: a plugin jar must
  be self-contained. `pom` or `aar` packaging fails with `CCB-E3019`.
  Plugin jars are not in `ccb.lock`: the exact coordinate in `ccb.toml`
  pins them.
- `ccb setup` prefetches plugin jars alongside Maven artifacts;
  `ccb check` reports `kotlin plugins: N jars`; builds fail with
  `CCB-E3021` on a cache miss, in stage 0.
- kotlinc gets `-Xplugin=<jar>` per plugin jar and
  `-P plugin:<id>:<key>=<value>` per `kotlin_plugin_options` entry
  (each option is `plugin-id:key=value`). The compile fingerprint
  includes the plugin jars and options, so changing either recompiles.
- Compose: use
  `org.jetbrains.kotlin:kotlin-compose-compiler-plugin:<kotlin-version>`
  with `maven_central = true` and the Compose libraries from Google
  Maven (runtime, ui, foundation-layout, material3, activity-compose).
  Use the plain artifact, not
  `kotlin-compose-compiler-plugin-embeddable`: the `-embeddable`
  variant is built for Gradle's embedded compiler (shaded
  `org.jetbrains.kotlin.com.intellij.*` classes), while CCB runs the
  standalone `kotlinc`, whose `kotlin-compiler.jar` carries the
  unshaded `com.intellij.*` classes — the `-embeddable` plugin crashes
  kotlinc with a `ClassNotFoundException`.
- Pin these versions directly (nearest-wins keeps older ones otherwise):
  `androidx.core:core-ktx` 1.18.0 (for activity 1.13.0; the older 1.16.0
  lacks the `PictureInPictureProvider` supertype `ComponentActivity`
  1.13.0 needs), `androidx.activity:activity-ktx` 1.13.0,
  `androidx.lifecycle:lifecycle-runtime-ktx` 2.10.0,
  `androidx.lifecycle:lifecycle-viewmodel-ktx` 2.10.0, and the savedstate
  trio (`androidx.savedstate:savedstate` 1.5.0,
  `androidx.savedstate:savedstate-compose-android` 1.5.0,
  `androidx.savedstate:savedstate-ktx` 1.5.0 — the first brings
  `kotlinx-serialization-core` 1.7.3, which `runtime-saveable` needs).
  Older versions bundle classes that moved in the newer ones, so the
  stale versions duplicate classes at dex time.
- Exclude `androidx.compose.animation:animation-core`: the animation
  library ships under two binary-incompatible names — the new KMP
  `animation-core` (1.2.1, rewritten `Transition` API) and the old
  `animation-core-android` (1.12.0). Material3 1.5.0 and the old
  animation line call old-core-only methods, and nothing in the graph
  calls the new core's unique methods, so the old core is the one to
  keep. A minimal Compose sample builds end to end with `ccb build`.

## Repositories

*ccb-maven-repos*

- Google Maven (`https://dl.google.com/dl/android/maven2/`) always first.
- Maven Central (`https://repo.maven.apache.org/maven2/`) second, only
  when `maven_central = true`.
- Extra repositories, in declared order, after Google Maven and
  Central: `[deps] maven_repos = ["https://repo.example.com/maven2",
  "https://priv.example.com/m2 alice env:PRIV_PASS"]`. Each entry is a
  base URL, or `url username password-source` where the password source
  is `env:VAR` (read from the environment at resolve time) or a literal
  password. Credentials travel only in the curl `-u` argument, never in
  a URL, log, or diagnostic: userinfo (`user:pass@host`) in the URL is
  rejected at config load with `CCB-E1005`, and a missing credential
  variable fails with `CCB-E3019` naming the repo and the variable.
- No per-dependency repo selection: every coordinate is tried in every
  configured repo, in order, first repo that serves the POM wins.

Test override: `CCB_MAVEN_REPOS` (comma-separated base URLs) replaces the
configured repos entirely. Exists so tests can point at a local `file://`
fixture repo; not a supported way to add private repositories.

## Packaging: AAR or JAR

*ccb-maven-packaging*

Artifact type comes from the artifact's POM `<packaging>`:

| Packaging | Result |
| --------- | ------ |
| `aar` | Downloaded as an AAR, treated exactly like a local AAR (extracted, `classes.jar` to classpath, `res/` merged, manifest merged). |
| `jar` (or missing `<packaging>`, which Maven defaults to jar) | Downloaded as a JAR onto the compile classpath and dex input. |
| `pom` | No artifact downloaded; the POM's own dependencies still resolve (how BOM-style POMs behave). |
| Anything else | `CCB-E3019` naming the coordinate. |

## Limitations

*ccb-maven-limits*

- No signature (PGP) verification: checksums only.
- Profile activation is by id only: `<jdk>`, `<os>`, `<property>`,
  `<file>` activation is not evaluated. A profile using one of those
  forms stays inactive unless its id is listed in `maven_profiles`;
  with `--verbose`, resolution notes each such profile by id and
  form. A profile id in `maven_profiles` that matches no resolved
  POM is likewise noted verbosely (usually a typo).
- `<scope>import</scope>` dependencyManagement entries (BOM imports)
  are not applied: the imported BOM is never fetched and its managed
  versions never apply. A version-less dependency whose version lives
  only in the imported BOM fails with `CCB-E3019` naming the cause;
  give the dependency an explicit version or a
  dependencyManagement entry instead.
- `<classifier>` on a dependency is ignored: the base artifact is
  fetched (noted with `--verbose`). A `<type>` outside `jar` / `aar` /
  `pom` is likewise ignored; packaging comes from the artifact POM
  (noted with `--verbose`).
