# Build pipeline and caching

## Stages (in order)

1. **Stage 0** — load + validate `ccb.toml` (every problem reported at
   once), resolve the toolchain, and — for release builds — verify the
   signing configuration before any compilation starts.
2. **deps** — extract AARs (classes, resources, assets, JNI).
3. **merge-manifest** — merge AAR `AndroidManifest.xml` files into the
   app manifest, in config order (see "Manifest merging" below), and
   write the result to `build/intermediates/merged-manifest/`.
   Skipped when the project declares no AARs.
4. **compile-res** — `aapt2 compile` each resource directory into a
   `.flat` zip.
5. **link** — `aapt2 link` into `linked.ap_` and generate `R.java`,
   using the merged manifest when AARs are present (else the app
   manifest directly). The manifest's `package` must match
   `[project] package`.
6. **compile-code** — `kotlinc` (Kotlin + `R.java` + Java sources for
   symbol resolution), then `javac` for the Java sources (excluding
   `R.java`). The classpath is `android.jar` + `deps.jars` + AAR
   `classes.jar` files.
7. **dex** — `d8` (or `r8` for release + minify, once provisioned).
   `.class` files are passed individually, not as a directory, because
   kotlinc also writes `META-INF/*.kotlin_module` files and d8 rejects
   non-class files in directory inputs.
8. **native** — NDK clang per ABI (only with an `[ndk]` section).
9. **package** — CCB's own ZIP writer: `linked.ap_` entries, dex files,
   assets (app assets win over AAR assets with the same name),
   `lib/<abi>/*.so` (duplicate `.so` names are an error).
10. **align** — `zipalign -f 4`.
11. **sign** — `apksigner`. Debug uses `~/.android/debug.keystore`
    (created with keytool when missing). Release uses `env:VAR`
    password references; passwords never appear in commands or logs.
12. **verify** — `apksigner verify` and `aapt2 dump badging`. An APK
    counts as built only when both pass.

## Manifest merging

At `merge-manifest` time, each AAR's `AndroidManifest.xml` (read from
the AAR zip, not the extracted copy) merges into the app manifest in
the order the AARs are listed in config. Merged entries: application
components (`activity`, `service`, `receiver`, `provider`,
`activity-alias`) and `uses-permission` / `uses-feature`. Conflict
rules: app entries always win on name conflict (warning naming the
AAR and the entry); identical entries from two AARs dedup silently;
same name with different attributes keeps the first AAR's version
with a warning naming both AARs. A malformed AAR manifest fails the
build with `CCB-E3018` before packaging; an AAR with no manifest
merges as nothing. Relative component names resolve against the
owning manifest's package and are rewritten fully-qualified in the
merged output. Everything else in a library manifest (application
attributes, `<uses-sdk>`, `<permission>` declarations, `<meta-data>`,
`<queries>`) is ignored. The merged manifest is fingerprinted from
its inputs (app manifest + AAR manifest texts), so `link` re-runs
when any of them change; `--explain` reports the merge step as
run/skip like any other step.

## Caching (M2)

Granularity is per step: each step's fingerprint is a hash of its
inputs (file contents + relevant config values + tool versions). An
unchanged step is skipped and its previous outputs reused. Consequences:

- A comment-only Kotlin change re-runs kotlinc (source hash changed)
  but skips dex and everything downstream, because the `.class` files
  — and therefore every downstream fingerprint — are identical.
- A string-*value* change re-runs compile, link, package, align, sign,
  verify, but skips kotlinc and dex (`R.java` is unchanged).
- A tool upgrade changes the tool fingerprint and invalidates all steps.
- `--explain` prints the run/skip plan without executing. It is
  conservative downstream of `link` (see docs/json-protocol.md).
- `--dry-run` prints the exact commands and executes nothing.

The cache lives at `build/ccb-cache.json`. A missing or corrupt cache
means "rebuild everything", never an error. `ccb clean` removes the
whole `build/` directory, including the cache.

## Reproducible builds (M4)

The ZIP writer uses fixed 1980-01-01 timestamps and deterministic
entry order (linked resources in aapt2 order, then sorted dex, assets,
and native libs), so two clean builds of the same inputs produce
byte-identical APKs. Verify with:

```
ccb clean && ccb build && sha256sum build/outputs/debug/app.apk
ccb clean && ccb build && sha256sum build/outputs/debug/app.apk
```

Note: apksigner also writes a sidecar `app.apk.idsig` file next to the
APK; the APK bytes themselves are what is compared.

## Known limits and costs

- **JVM startup dominates wall time.** kotlinc, d8, and apksigner each
  boot a JVM (several seconds on a slow VM). CCB's own work — hashing,
  decisions, packaging — is well under a second. The §8 targets
  (<10 s cold, <2 s incremental Kotlin) are therefore not met on this
  VM: a cold hello-world build takes ~19 s and a one-file Kotlin
  rebuild ~17 s. A kotlinc/d8 daemon would fix this; it is out of
  v1 scope.
- **Tool probe cache.** The `kotlinc -version` / `java -version`
  probes in toolchain resolution used to dominate no-op rebuilds
  (~3 s of a ~3.4 s no-op on this VM). They are now cached in
  `build/intermediates/tool-probes.json`, keyed by tool path + mtime
  + size: a repeat build with unchanged tools skips both probes
  (warm no-op ~0.2 s on this VM). Any mismatch, missing entry, or
  corrupt file re-probes; `ccb clean` removes the cache with the
  rest of `build/`. The cache never changes build outputs: it only
  feeds the tool fingerprint, and a cached version for an unchanged
  binary is identical to a fresh probe.
- **d8 input size.** `.class` files are passed as individual arguments;
  extremely large projects could approach the OS argument limit. d8
  argfiles are the future fix if that ever bites.
- **Build-tools wrapper scripts** (`d8`, `apksigner`) exec plain `java`
  from PATH and ignore `JAVA_HOME`, so CCB prefixes the resolved JDK
  `bin` directory onto PATH for JVM tool runs.
- **No device or emulator** is available in this environment, so
  install/launch are verified only up to the device boundary (see
  docs/device-setup.md).
