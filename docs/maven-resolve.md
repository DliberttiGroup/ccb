# Maven resolution: transitive dependencies and profiles

[Docs index](index.md) · [Maven](maven.md)

*ccb-maven-transitive*

Direct and transitive dependencies resolve from POM `<dependencies>`:

- Scopes: `compile` (including absent scope, which Maven treats as
  compile) and `runtime` resolve. `test`, `provided`, `system`, `import`
  skipped, as are dependencies marked `<optional>true</optional>`.
  When a dependency declares no scope or no `<optional>`, the value
  from its `<dependencyManagement>` entry applies instead.
- Version conflicts resolve nearest-wins: dependencies walked
  breadth-first from the direct declarations, first version seen for a
  `group:artifact` wins. A later, deeper (or later-declared, same-depth)
  version is skipped; with `--verbose` the skip prints as `maven:
  <g:a>: requested <v>, using nearest <w>`. Nearest-wins is decided on
  `group:artifact` alone, before a losing branch's version spec is
  resolved: a transitive range, `LATEST`, `RELEASE`, or `-SNAPSHOT`
  that loses never fetches metadata, so a losing branch cannot fail
  the build for metadata it does not need. A dependency with no
  version at all still fails (see below) even on a losing branch: a
  missing version is a malformed POM, not a resolution detail.
- Cycles safe: an already-seen `group:artifact` is never re-queued.
- Properties: `${...}` resolve from the same POM's `<properties>` plus
  `project.groupId` / `project.artifactId` / `project.version`, plus
  environment variables as `${env:VAR}`. A dependency version may also
  come from the same POM's `<dependencyManagement>` entry. A parent
  reference (`<parent><version>${v}</version></parent>`) is
  substituted against the POM that declares it, not the merged child
  set: a child's same-named property cannot redirect a grandparent
  lookup.
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
  itself be a range or `-SNAPSHOT` spec.
- Exclusions: a dependency's `<exclusions>` prune that edge's subtree:
  an excluded `group:artifact` never resolves below the declaring
  dependency, and the exclusion is inherited down the path (a
  transitive dependency of an excluded-from artifact is still pruned
  even when reached deeper), but never leaks to sibling branches —
  the same artifact reached through a clean edge still resolves, and
  an excluded artifact never enters the seen set, so it can never win
  nearest-wins. Exclusions declared by a parent POM (on its
  `<dependencies>` entries) and in `<dependencyManagement>` apply the
  same way when the edge is inherited. Project-level exclusions
  (`[deps] maven_exclude`) are checked before nearest-wins too: an
  excluded artifact never enters the seen set, so an excluded version
  can never win, and another version of the same artifact from a later
  path can win instead. The excluded artifact's own transitive
  dependencies are pruned with it: never queued. With `--verbose`,
  each skipped exclusion prints as `maven: <g:a:v>: excluded, skipping`
  (project-level, versioned) or `maven: <coord>: dependency <g:a> is
  excluded, skipping` (unversioned / per-dependency).
- KMP variants: a `-jvm` artifact is dropped when its `-android`
  sibling (same group, same version) is also resolved — a Kotlin
  multiplatform library publishes both, and dex rejects the classes
  both define. A `-jvm` artifact with no `-android` sibling is kept
  (pure-JVM libraries have none). The sibling must be the same
  version; a version mismatch keeps both.
- Profiles: see below.
- A dependency with no version after the dependencyManagement fallback
  fails with `CCB-E3019` naming the dependency. When the POM imports
  a BOM with `<scope>import</scope>` (unsupported, see below), the
  diagnostic says so: the version may live only in the imported BOM.
- A `<scope>import</scope>` entry in `<dependencyManagement>` (a BOM
  import) is not applied: the imported BOM is never fetched and its
  managed versions never apply. With `--verbose`, each import is
  noted as `maven: <coord>: dependencyManagement imports BOM <g:a:v>
  with <scope>import</scope>, which CCB does not support`.
- A dependency's `<classifier>` is ignored (the base artifact is
  fetched) and a `<type>` outside `jar` / `aar` / `pom` is ignored
  (packaging comes from the artifact POM); both are noted with
  `--verbose` as `maven: <coord>: dependency <g:a> requests classifier
  '<c>', which CCB ignores` / `requests type '<t>', which CCB ignores`.

## Profiles

*ccb-maven-profiles*

- A profile is active when its id is listed in `[deps] maven_profiles`,
  or — when that list is empty — when its
  `<activation><activeByDefault>` is true. Any other activation
  (`<jdk>`, `<os>`, `<property>`, `<file>`) is not evaluated: the
  profile stays inactive unless listed. With `--verbose`, each such
  inactive profile is noted as `maven: <coord>: profile '<id>' uses
  <activation><<form>>, which CCB does not evaluate; list its id in
  [deps] maven_profiles to activate it`.
- An active profile's properties and dependencyManagement override the
  POM's own (in place); its dependencies append after the POM's own.
  Profiles inherited from parent POMs apply the same way.
- An unknown profile id in `maven_profiles` activates nothing and fails
  nothing. With `--verbose`, it is noted after the walk as `maven:
  profile '<id>' in [deps] maven_profiles matched no resolved POM`
  (usually a typo).
