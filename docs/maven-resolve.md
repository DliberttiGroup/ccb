# Maven resolution: transitive dependencies and profiles

[Docs index](index.md) · [Maven](maven.md)

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
- Profiles: see below.
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
