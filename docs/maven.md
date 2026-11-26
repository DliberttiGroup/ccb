# Remote Maven dependencies

[Docs index](index.md)

*ccb-maven*

CCB resolves remote Maven artifacts declared in `ccb.toml` and feeds them
into the deps step as if they were local AARs/JARs. No Gradle: no daemon,
no build scripts, no plugin resolution — just POM parsing, downloads, and
the local cache.

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
- `maven_profiles`: array of profile ids to activate (see Profiles
  below). Default empty: only `<activeByDefault>` profiles apply.
- `maven_repos`: array of extra repositories (see Repositories below).
- `maven_exclude`: array of exclusions, each `group:artifact` (every
  version) or `group:artifact:version` (that version only). An excluded
  artifact never resolves, even transitively. Excluding a direct
  dependency is a config error at load with `CCB-E1005` naming both the
  exclusion and the dependency; excluding some other version of a direct
  dependency is allowed.

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
- Compose: the Compose compiler plugin path is
  `org.jetbrains.kotlin:kotlin-compose-compiler-plugin:<kotlin-version>`
  with `maven_central = true` and the Compose libraries from Google
  Maven (runtime, ui, foundation-layout, material3, activity-compose).
  Use the plain artifact, not
  `kotlin-compose-compiler-plugin-embeddable`: the `-embeddable`
  variant is built for Gradle's embedded compiler (shaded
  `org.jetbrains.kotlin.com.intellij.*` classes), while CCB runs the
  standalone `kotlinc`, whose `kotlin-compiler.jar` carries the
  unshaded `com.intellij.*` classes — the `-embeddable` plugin crashes
  kotlinc with a `ClassNotFoundException`. Pin
  `androidx.core:core-ktx` directly (1.18.0 for activity 1.13.0):
  nearest-wins otherwise keeps the older core some Compose libraries
  declare (1.16.0), which lacks the `PictureInPictureProvider`
  supertype `ComponentActivity` 1.13.0 needs — Gradle's newest-wins
  masks this, Maven's nearest-wins does not. The same applies to
  `androidx.activity:activity-ktx` (1.13.0),
  `androidx.lifecycle:lifecycle-runtime-ktx` (2.10.0),
  `androidx.lifecycle:lifecycle-viewmodel-ktx` (2.10.0), and the
  savedstate trio (`androidx.savedstate:savedstate` 1.5.0,
  `androidx.savedstate:savedstate-compose-android` 1.5.0,
  `androidx.savedstate:savedstate-ktx` 1.5.0 — the first brings
  `kotlinx-serialization-core` 1.7.3, which `runtime-saveable`
  needs): older versions bundle classes that moved in the newer
  ones, so the stale versions duplicate classes at dex time. And exclude `androidx.compose.animation:animation-core`:
  the animation library ships under two binary-incompatible names —
  the new KMP `animation-core` (1.2.1, rewritten `Transition` API)
  and the old `animation-core-android` (1.12.0). Material3 1.5.0 and
  the old animation line call old-core-only methods, and nothing in
  the graph calls the new core's unique methods, so the old core is
  the one to keep. A minimal Compose sample builds end to end with
  `ccb build`.

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

## Transitive dependencies

*ccb-maven-transitive*

Direct and transitive dependencies resolve from POM `<dependencies>`:

- Scopes: `compile` (including absent scope, which Maven treats as
  compile) and `runtime` resolve. `test`, `provided`, `system`, `import`
  skipped, as are dependencies marked `<optional>true</optional>`.
- Version conflicts resolve nearest-wins: dependencies walked
  breadth-first from the direct declarations, first version seen for a
  `group:artifact` wins. A later, deeper (or later-declared, same-depth)
  version is skipped; with `--verbose` the skip prints as `maven:
  <g:a>: requested <v>, using nearest <w>`.
- Cycles safe: an already-seen `group:artifact` is never re-queued.
- Properties: `${...}` resolve from the same POM's `<properties>` plus
  `project.groupId` / `project.artifactId` / `project.version`. A
  dependency version may also come from the same POM's
  `<dependencyManagement>` entry.
- Parent POMs: when a POM has a `<parent>` block, the parent POM is fetched
  from the same repositories (same cache and checksum rules as any other
  POM) and applied to the child, recursively up the chain. Parent's
  `<properties>`, `<dependencyManagement>`, `<dependencies>`, and
  `<profiles>` merge into the child's: child entries win, nearest
  ancestor wins among parents; `${...}` expansion uses the merged set.
  A POM omitting `groupId` or `version` inherits those from its
  `<parent>` block. Chains capped at 10 ancestors; longer chain, or a
  cycle (parent coordinate repeating an earlier coordinate in the
  chain), fails with `CCB-E3024` naming the chain. A parent version may
  itself be a range or `-SNAPSHOT` spec. Not inherited: parent-declared
  exclusions (ignored).
- Exclusions (`[deps] maven_exclude`) are checked before nearest-wins: an
  excluded artifact never enters the seen set, so an excluded version can
  never win, and another version of the same artifact from a later path
  can win instead. The excluded artifact's own transitive dependencies are
  pruned with it: never queued. With `--verbose`, each skipped exclusion
  prints as `maven: <g:a:v>: excluded, skipping`.
- KMP variants: a `-jvm` artifact is dropped when its `-android`
  sibling (same group, same version) is also resolved — a Kotlin
  multiplatform library publishes both, and dex rejects the classes
  both define. A `-jvm` artifact with no `-android` sibling is kept
  (pure-JVM libraries have none). The sibling must be the same
  version; a version mismatch keeps both.
- Profiles: see the Profiles section below.
- A dependency with no version after the dependencyManagement fallback
  fails with `CCB-E3019` naming the dependency.

## Profiles

*ccb-maven-profiles*

- A profile is active when its id is listed in `[deps] maven_profiles`,
  or — when that list is empty — when its
  `<activation><activeByDefault>` is true. Any other activation
  (`<jdk>`, `<os>`, `<property>`, `<file>`) is not evaluated: the
  profile stays inactive unless listed.
- An active profile's properties and dependencyManagement override the
  POM's own (in place); its dependencies append after the POM's own.
  Profiles inherited from parent POMs apply the same way.
- An unknown profile id in `maven_profiles` activates nothing and fails
  nothing.

## Cache and checksums

*ccb-maven-cache*

Resolved artifacts live in a dedicated Maven cache, not the flat download
cache:

- Location: `$XDG_CACHE_HOME/ccb/maven2`, else `~/.cache/ccb/maven2`
  (`CCB_MAVEN_CACHE` overrides it). Layout mirrors the repository:
  `<cache>/<group-path>/<artifact>/<version>/<artifact>-<version>.<ext>`,
  with the `.pom` alongside.
- Checksums: on download, a published `.sha256` is verified first, else a
  published `.sha1`. Mismatch fails with `CCB-E3020` naming the artifact.
- No published checksum: download verified structurally (curl `--fail`,
  complete transfer) and its SHA-256 recorded in a `<file>.ccb-sha256`
  sidecar. Later cache hits verify against the sidecar, so a corrupted
  cache file fails with `CCB-E3020` instead of silently poisoning a build.
- Sidecar-less cache file (placed there by hand, or by an older CCB) is
  trusted on first use and adopted: its SHA-256 is recorded for later
  hits. Explain and dry-run modes never write sidecars.

## Hermeticity: setup prefetches, builds never download

*ccb-maven-hermetic*

`ccb setup` is the only command allowed network access (spec §6.1):

- `ccb setup` in a project with `[deps] maven` prefetches every direct +
  transitive artifact into the cache (after the SDK steps, before the
  final verification pass) and reports `maven: prefetched N artifacts (A
  aar, J jar)`. A prefetch failure stops setup with the diagnostic — a
  half-prefetched project is never reported as ready. Prefetch fetches the
  full parent-POM chain for every resolved POM, and re-downloads version
  metadata fresh, so ranges and snapshots float to the newest match.
  In a project with `[build] kotlin_plugins`, setup also prefetches the
  plugin jars and reports `maven: prefetched N compiler plugin jars`.
- `ccb build` and `ccb check` resolve cache-only. A coordinate never
  prefetched fails with `CCB-E3021` naming the coordinate and telling you
  to run `ccb setup`. Failure happens in stage 0, before any build stage
  runs.
- Workflow: add a coordinate, run `ccb setup`, then build offline.
  Deleting the cache (or checking out on a new machine) just means running
  `ccb setup` again.

`ccb check` reports the resolved set on success: `maven: N artifacts (A
aar, J jar)`. When exclusions are configured, check also reports them:
`maven: excluded <g:a>, ...` (and a `maven_excluded` array in `--json`
output). When compiler plugins are configured, check also reports
`kotlin plugins: N jars`.

## Lockfile (`ccb.lock`)

*ccb-maven-lockfile*

`ccb setup` writes the fully resolved tree to `ccb.lock` in the project
root after a successful prefetch, and `ccb check` / `ccb build` enforce it
whenever present:

- Format: one coordinate per line, sorted by `group:artifact`, with
  packaging and the artifact's SHA-256:
  `com.example:lib:1.0 aar <sha256>`. Comment lines (`#`) ignored. For
  `pom` packaging (no artifact file) the recorded hash is the POM's hash,
  so a changed BOM still fails enforcement.
- Enforcement is exact: resolved version, packaging, or hash differing
  from the lockfile fails with `CCB-E3023` naming the coordinate, as does a
  resolved coordinate missing from the lockfile or a locked coordinate
  that no longer resolves. A stale lockfile never silently resolves
  something else: run `ccb setup` to regenerate it (setup re-resolves
  from the declarations and overwrites the file, even when a stale
  lockfile is present).
- No lockfile means no enforcement: pre-lockfile projects behave exactly
  as before. A project with no `[deps] maven` never writes or consults a
  lockfile, so removing all Maven coordinates leaves a stale `ccb.lock`
  harmlessly in place.
- The lockfile is a project file: commit it. It makes a checkout
  reproducible without trusting the cache: the cache is keyed by
  coordinate, but the lockfile pins the exact tree (including transitive
  versions) and every artifact hash.

## Limitations

*ccb-maven-limits*

- No signature (PGP) verification: checksums only.
- No per-dependency `<exclusions>` in POMs: exclusions are only the
  project-level `[deps] maven_exclude`.
- Profile activation is by id only: `<jdk>`, `<os>`, `<property>`,
  `<file>` activation is not evaluated.
- Parent-declared `<exclusions>` are ignored.

## Troubleshooting

*ccb-maven-troubleshooting*

Code meanings: [diagnostics.md](diagnostics.md).

| Code | Fix |
| ---- | --- |
| `CCB-E3021` | Artifact not in the local cache yet. Run `ccb setup`, then build again. Same for a parent POM evicted from the cache: setup re-prefetches the whole chain. |
| `CCB-E3024` | Parent chain loops, or is deeper than 10 ancestors. Fix the chain in the POMs. |
| `CCB-E3020` | Downloaded file doesn't match the repo's published checksum. Delete the cache entry (or the whole `<cache>/<group-path>/<artifact>/<version>/` dir) and re-run `ccb setup`; if it persists, the repo or mirror is serving bad bytes. |
| `CCB-E3019` | Coordinate doesn't exist in Google Maven (or Central, if enabled). Check spelling and version, and whether the artifact actually lives on Central (then set `maven_central`). Also: a range no published version satisfies, a snapshot with no snapshot metadata, or a missing credential variable for a private repo (the diagnostic names the variable). |
| `CCB-E1005` (coordinate) | Declared coordinate malformed or unpinned. Fix the string in `ccb.toml`. Plugin coordinates must be exact versions. |
| `CCB-E1005` (repo entry) | `[deps] maven_repos` entry malformed: use `url` or `url username env:VAR` (or a literal password). Userinfo in the URL is not allowed. |
| `CCB-E1005` (exclusion) | Exclusion removes something the project declares directly. Remove the exclusion or the direct coordinate. |
| `CCB-E3023` | Resolved tree deviates from `ccb.lock` (version, packaging, hash, missing, or stale entry), or the lockfile is malformed. Run `ccb setup` to regenerate it from the current declarations. |
