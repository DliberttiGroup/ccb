# Remote Maven dependencies

CCB resolves remote Maven artifacts for coordinates declared in
`ccb.toml` and feeds them into the existing deps step as if they
were local AARs/JARs. There is no Gradle involved: no daemon, no
build scripts, no plugin resolution — just POM parsing, downloads,
and the local cache.

## Declaring dependencies

```toml
[deps]
maven = [
  "androidx.core:core-ktx:1.13.1",
  "com.google.code.gson:gson:2.11.0",
]
maven_central = true
```

- `maven`: an array of `group:artifact:version` coordinates. Exact
  versions only: ranges (`[1.0,2.0)`), `LATEST`, `RELEASE`, and
  `-SNAPSHOT` versions are rejected at config load with `CCB-E1005`
  naming the coordinate. Local `jars`/`aars` keep working unchanged
  alongside `maven`.
- `maven_central`: when `true`, Maven Central is searched after
  Google Maven. Default `false`: Google Maven only.

## Repositories

- Google Maven (`https://dl.google.com/dl/android/maven2/`) is always
  searched first.
- Maven Central (`https://repo.maven.apache.org/maven2/`) is searched
  second, only when `maven_central = true`.
- No other repositories in v1. There is no per-dependency repo
  selection: every coordinate is tried in every configured repo, in
  order, and the first repo that serves the POM wins.

Test/advanced override: `CCB_MAVEN_REPOS` (comma-separated base
URLs) replaces the configured repos entirely. It exists so tests
can point at a local `file://` fixture repo; it is not a supported
way to add private repositories in v1.

## Packaging: AAR or JAR

The artifact type comes from the artifact's POM `<packaging>`:

- `aar` → downloaded as an AAR and treated exactly like a local
  AAR (extracted, `classes.jar` to the classpath, `res/` merged,
  manifest merged).
- `jar` (or a missing `<packaging>`, which Maven defaults to jar)
  → downloaded as a JAR onto the compile classpath and dex input.
- `pom` → no artifact is downloaded; the POM's own dependencies
  still resolve (this is how BOM-style POMs behave).
- Anything else → `CCB-E3019` naming the coordinate.

## Transitive dependencies

Direct and transitive dependencies resolve from POM
`<dependencies>`:

- Scopes: `compile` (including an absent scope, which Maven treats
  as compile) and `runtime` resolve. `test`, `provided`, `system`,
  and `import` are skipped, as are dependencies marked
  `<optional>true</optional>`.
- Version conflicts resolve nearest-wins: dependencies are walked
  breadth-first from the direct declarations, and the first version
  seen for a `group:artifact` wins. A later, deeper (or
  later-declared, same-depth) version is skipped; with `--verbose`
  the skip is printed as `maven: <g:a>: requested <v>, using
  nearest <w>`.
- Cycles are safe: an already-seen `group:artifact` is never
  re-queued.
- Properties: `${...}` references resolve from the same POM's
  `<properties>` plus `project.groupId` / `project.artifactId` /
  `project.version`. A dependency version may also come from the
  same POM's `<dependencyManagement>` entry.
- Parent POMs: when a POM omits `groupId` or `version`, the values
  from its embedded `<parent>` block are used. The parent POM itself
  is never fetched: a POM whose coordinates depend on a fetched
  parent (or on `dependencyManagement` from a parent) fails with
  `CCB-E3019`.
- Exclusions are ignored in v1: an excluded transitive dependency
  still resolves. If an exclusion matters to a build, declare the
  conflict away by pinning the version directly (nearest-wins makes
  the direct declaration win).
- Profiles are ignored entirely: neither profile properties nor
  profile dependencies resolve. A POM that only declares a dependency
  inside a profile behaves as if it had no such dependency.
- A dependency with no version after the dependencyManagement
  fallback fails with `CCB-E3019` naming the dependency.

## Cache and checksums

Resolved artifacts live in a dedicated Maven cache, not the flat
download cache:

- Location: `$XDG_CACHE_HOME/ccb/maven2`, else
  `~/.cache/ccb/maven2` (`CCB_MAVEN_CACHE` overrides it). Layout
  mirrors the repository: `<cache>/<group-path>/<artifact>/
  <version>/<artifact>-<version>.<ext>`, with the `.pom` alongside.
- Checksums: on download, a published `.sha256` is verified first,
  else a published `.sha1`. A mismatch fails with `CCB-E3020`
  naming the artifact — a bad mirror or tampered file never lands
  in the cache.
- When a repo publishes no checksum, the download is verified
  structurally (curl `--fail`, complete transfer) and its SHA-256 is
  recorded in a `<file>.ccb-sha256` sidecar. Later cache hits verify
  against the sidecar, so a corrupted cache file fails with
  `CCB-E3020` instead of silently poisoning a build. First-download
  trust for checksum-less artifacts is inherent: a repo that
  publishes no checksum gives CCB nothing to check the first fetch
  against. Prefer repositories that publish checksums when integrity
  matters.
- A sidecar-less cache file (placed there by hand, or by an older
  CCB) is trusted on first use and adopted: its SHA-256 is recorded
  for later hits. Explain and dry-run modes never write sidecars.

## Hermeticity: setup prefetches, builds never download

`ccb setup` is still the only command allowed network access
(spec §6.1), and Maven keeps it that way:

- `ccb setup` in a project with `[deps] maven` prefetches every
  direct + transitive artifact into the cache (after the SDK steps,
  before the final verification pass) and reports
  `maven: prefetched N artifacts (A aar, J jar)`. A prefetch
  failure stops setup with the diagnostic — a half-prefetched
  project is never reported as ready.
- `ccb build` and `ccb check` resolve cache-only. A coordinate that
  was never prefetched fails with `CCB-E3021` naming the coordinate
  and telling you to run `ccb setup`. The failure happens in stage
  0, before any build stage runs.
- So the workflow is: add a coordinate, run `ccb setup`, then build
  offline. Deleting the cache (or checking out on a new machine)
  just means running `ccb setup` again.

`ccb check` reports the resolved set on success:
`maven: N artifacts (A aar, J jar)`.

## Limitations (v1, deliberate)

- Exact versions only; no ranges, snapshots, or version mediation
  beyond nearest-wins.
- Google Maven + opt-in Maven Central only; no authenticated or
  private repositories (no credential handling exists).
- No lockfile: the cache is keyed by coordinate, and a
  re-resolve after a cache wipe re-downloads the same pinned
  versions. Reproducibility comes from the pinned coordinates, not
  from a lockfile (a lockfile is the recorded v2 follow-up).
- Parent POMs are not fetched; exclusions are ignored (see above).
- No signature (PGP) verification: checksums only.

## Troubleshooting

- `CCB-E3021 ... run `ccb setup``: the artifact is not in the local
  cache yet. Run `ccb setup` in the project, then build again.
- `CCB-E3020 ... checksum mismatch`: the downloaded file does not
  match the repo's published checksum. Delete the cache entry (or
  the whole `<cache>/<group-path>/<artifact>/<version>/` dir) and
  re-run `ccb setup`; if it persists, the repo or mirror is serving
  bad bytes.
- `CCB-E3019 ... not found in any configured repository`: the
  coordinate does not exist in Google Maven (or Central, if
  enabled). Check the spelling and version, and whether the
  artifact actually lives on Central (then set `maven_central`).
- `CCB-E1005 ... maven coordinate ...`: the declared coordinate is
  malformed or unpinned. Fix the string in `ccb.toml`.
