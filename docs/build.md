# Build pipeline and caching

[Docs index](index.md)

*ccb-build*

Sub-pages: [kotlinc daemon](build-daemon.md) · [release binary](build-release.md) ·
[running the tests](build-tests.md)

## Stages (in order)

*ccb-build-stages*

| # | Stage | What it does |
| - | ----- | ------------ |
| 0 | Load | Load + validate `ccb.toml`, report every problem at once. Resolve the toolchain. Release builds verify signing config before compiling anything. |
| 1 | deps | Extract AARs (classes, resources, assets, JNI). |
| 2 | libraries | Build each `[deps.library.<name>]` path library into `build/intermediates/libraries/<name>/<name>.aar` (a built `.aar` path joins the declared AARs in stage 0 instead). Skipped with no path libraries. See [library.md](library.md). |
| 3 | modules | Build each `[modules.<name>]` library module into `build/intermediates/modules/<name>/<name>.aar`, in topological order. Skipped with no modules. See [modules.md](modules.md). |
| 4 | merge-manifest | Merge AAR manifests into the app manifest, in config order. Output: `build/intermediates/merged-manifest/`. Skipped with no AARs. |
| 5 | compile-res | `aapt2 compile` each resource directory into a `.flat` zip. |
| 6 | link | `aapt2 link` into `linked.ap_`, generating `R.java`. Uses the merged manifest when AARs are present, else the app manifest. The manifest's `package` must match `[project] package`. Library zips link as `-R` overlays with the app zip last, so the app wins resource conflicts (see [library.md](library.md)); a resource defined by more than one source warns, naming the winner. |
| 7 | compile-code | `kotlinc` (Kotlin + `R.java` + Java sources), then `javac` for Java sources and generated `R.java`. kotlinc resolves R symbols but emits no R.class files, so javac must compile them — otherwise library R classes are missing from the dex at runtime. Classpath: `android.jar` + `deps.jars` + AAR `classes.jar` files. When `[build] kotlin_plugins` is set, kotlinc also gets `-Xplugin=<jar>` per resolved plugin jar and `-P plugin:<id>:<key>=<value>` per `kotlin_plugin_options` entry; plugin jars and options join the compile fingerprint, so changing either recompiles (see [maven](maven.md)). |
| 8 | dex | `d8` (or `r8` for release + minify). See below. |
| 9 | dexcheck | Parse the dex files, verify every referenced class is defined in the dex. Referenced-but-undefined program class fails the build with `CCB-E3025` naming the missing classes, before packaging. Platform classes (`java.*`, `android.*`, …) come from `--lib` inputs, never in the dex: allowlisted, never reported. Annotation- and signature-only references still count. Runs even when dex was cache-skipped, and for AAB builds. Milliseconds, no cache of its own, doesn't count toward `steps_run`, so a no-op rebuild still reports "up to date". `--explain` prints its plan line; `--dry-run` doesn't run it. |
| 10 | native | NDK clang per ABI (only with an `[ndk]` section). |
| 11 | package | CCB's own ZIP writer: `linked.ap_` entries, dex files, assets (app assets win over AAR assets with the same name), `lib/<abi>/*.so` (duplicate `.so` names are an error). |
| 12 | align | `zipalign -f 4`. |
| 13 | sign | `apksigner`. Debug uses `~/.android/debug.keystore` (created with keytool when missing). Release uses `env:VAR` password references; passwords never appear in commands or logs. |
| 14 | verify | `apksigner verify` and `aapt2 dump badging`. An APK counts as built only when both pass. |

A library project (`[library]` in `ccb.toml`) runs only stages 0–2
in the list above — deps, then its single module build into
`build/<name>-<version>.aar` — and stops: no merge, link, dex, or
package. See [library.md](library.md).

Merge rules (stage 4): app entries win on name conflict (warning names
the AAR and entry); identical entries from two AARs dedup silently; same
name with different attributes keeps the first AAR's version (warning
names both AARs). Merged: application components (`activity`, `service`,
`receiver`, `provider`, `activity-alias`), `uses-permission`,
`uses-feature`. Everything else in a library manifest is ignored.
Relative component names resolve against the owning manifest's package
and are rewritten fully-qualified. Malformed AAR manifest fails the build
with `CCB-E3018` before packaging; an AAR with no manifest merges as
nothing. Fingerprinted from inputs, so `link` re-runs when any manifest
changes.

### Dex (stage 8)

*ccb-build-dex*

`.class` files pass individually, not as a directory: kotlinc also writes
`META-INF/*.kotlin_module` files, and d8 rejects non-class files in
directory inputs. For Kotlin projects the kotlinc stdlib jar is a program
input automatically (skipped when the project already supplies it via
`[deps]` or Maven); for release + minify it is a program input to R8,
not a `--lib`.

**Stdlib dex cache.** The stdlib jar is dexed once into the global cache
`$XDG_CACHE_HOME/ccb/dex-stdlib` (else `~/.cache/ccb/dex-stdlib`, same
root as the Maven cache, survives `ccb clean`), keyed by stdlib jar hash +
min-api + build-tools version. Later builds pass the cached `.dex` to d8
instead of the jar. Single `<key>.dex` file, atomic rename on publish,
validated on read (dex magic + header file size). Missing or corrupt entry
falls back to the jar, never fails the build. Release + minify excluded:
R8 takes the stdlib as a program-input JAR, not dex.

**R8 keep rules (release + minify).** CCB generates keep rules
(`build/intermediates/r8-keep.pro`, passed to R8 ahead of the project's
own `proguard_rules` file): the app's whole package (`-keep class
<package>.** { *; }`), the `<application android:name>` and every manifest
component (activities, services, receivers, providers, activity-aliases
and their `targetActivity`), and view classes from layout XML (dotted
element names, `class`, `android:name`, `layout_behavior`). Manifest
entries come from the app, AAR, and library-module manifests; layouts from
app and module res dirs and AAR zips. Generated text is part of the dex
fingerprint, so a manifest or layout change re-runs dex. R8 tolerates a
keep for a nonexistent class (warning only), so a stale layout reference
can't break a build. Reflection-only references (`Class.forName` strings)
can't be kept automatically: put those keeps in the project's own
`proguard_rules` file, still required when `minify = true` (may hold only
a comment otherwise).

**d8 heap.** CCB passes `-JXmx<N>m` to d8, N = 256 + 16 per MiB of dex
program inputs (app `.class` files + `[deps]` jars + AAR `classes.jar`
files + stdlib input, cached `.dex` or jar), capped at 1024MB. Same flag
populates the stdlib dex cache (input: stdlib jar alone). Heap never
changes dex output, so it's not in the dex fingerprint. R8 untouched: its
1G cap already equals the cap here.

**Measured, Showcase:** clean build 57s before, 63s cold cache (one-time
populate), 33–34s warm cache. Release + minify: APK 2,651,777 bytes off,
75,393 on; dex 2,615,024 -> 37,160.

## AAB output (`--aab`)

*ccb-build-aab*

Same stages, bundle differences (spec §5.10): link runs with
`--proto-format` into `linked-aab.ap_` (own cache key); package lays out
the base module (`base/manifest/`, `base/resources.pb`, `base/res/`,
`base/dex/`, `base/assets/`, `base/lib/<abi>/`) plus a hand-encoded
`BundleConfig.pb`; align skipped; sign adds `--min-sdk-version`; verify is
`jarsigner -verify` plus a structural entry check (apksigner verify can't
verify a bundle — it requires a root manifest). Output:
`build/outputs/<variant>/app.aab`. `bundletool validate` passes on
CCB-built bundles.

## Caching

*ccb-build-cache*

Per step: each fingerprint hashes inputs (file contents + relevant config
values + tool versions). Unchanged step is skipped, previous outputs reused.

- Comment-only Kotlin change re-runs kotlinc (source hash changed) but
  skips dex and downstream: `.class` files — and every downstream
  fingerprint — are identical.
- String-*value* change re-runs compile, link, package, align, sign,
  verify, but skips kotlinc and dex (`R.java` unchanged).
- Tool upgrade changes the tool fingerprint, invalidates all steps.
- `--explain` prints the run/skip plan without executing. Conservative
  downstream of `link` — see [json-protocol.md](json-protocol.md).
- `--dry-run` prints the exact commands, executes nothing.

Cache lives at `build/ccb-cache.json`. Missing or corrupt cache means
"rebuild everything", never an error. `ccb clean` removes `build/`,
cache included.

## Reproducible builds

*ccb-build-reproducible*

ZIP writer uses fixed 1980-01-01 timestamps and deterministic entry order
(linked resources in aapt2 order, then sorted dex, assets, native libs).
Two clean builds of the same inputs produce byte-identical APKs:

```
ccb clean && ccb build && sha256sum build/outputs/debug/app.apk
ccb clean && ccb build && sha256sum build/outputs/debug/app.apk
```

apksigner also writes a sidecar `app.apk.idsig` next to the APK; the APK
bytes are what's compared.

## Known limits and costs

*ccb-build-limits*

- **JVM startup dominates wall time.** kotlinc, d8, apksigner each boot a
  JVM (seconds on a slow VM). Spec §8 targets (<10 s cold, <2 s
  incremental Kotlin) are not met: cold hello-world ~23 s, one-file Kotlin
  rebuild ~14 s. The opt-in kotlinc daemon ([build-daemon.md](build-daemon.md))
  removes kotlinc JVM startup from warm rebuilds only.
- **Tool probe cache.** `kotlinc -version` / `java -version` probes are
  cached in `build/intermediates/tool-probes.json`, keyed by tool path +
  mtime + size. Repeat build with unchanged tools skips both probes (warm
  no-op ~0.2 s). Mismatch, missing entry, or corrupt file re-probes;
  `ccb clean` removes it with `build/`. Never changes build outputs: only
  feeds the tool fingerprint.
- **d8 input size.** `.class` files pass as individual arguments;
  extremely large projects could approach the OS argument limit. d8
  argfiles are the fix if that ever bites.
- **Build-tools wrapper scripts** (`d8`, `apksigner`) exec plain `java`
  from PATH and ignore `JAVA_HOME`, so CCB prefixes the resolved JDK
  `bin` directory onto PATH for JVM tool runs.
- **No device or emulator** here, so install/launch are verified only up
  to the device boundary (see [device-setup.md](device-setup.md)).
