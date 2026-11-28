# Maven cache, hermeticity, and lockfile

[Docs index](index.md) · [Maven](maven.md)

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
  When every repository fails to serve fresh metadata (no network, repo
  down), prefetch reuses the cached metadata copy when one exists and
  its recorded checksum still verifies, so setup still works offline
  for ranges and snapshots it already resolved; a cached copy that
  fails its checksum fails with `CCB-E3020` instead.
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
