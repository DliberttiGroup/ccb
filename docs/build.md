# Build pipeline and caching

## Stages (in order)

1. **Stage 0** — Load + validate `ccb.toml`, report every problem at once.
   Resolve the toolchain. Release builds verify signing config before
   compiling anything.
2. **deps** — Extract AARs (classes, resources, assets, JNI).
3. **merge-manifest** — Merge AAR `AndroidManifest.xml` files into the app
   manifest, in config order. Output:
   `build/intermediates/merged-manifest/`. Skipped with no AARs.
   App entries win on name conflict (warning names the AAR and entry);
   identical entries from two AARs dedup silently; same name with
   different attributes keeps the first AAR's version (warning names both
   AARs). Merged: application components (`activity`, `service`,
   `receiver`, `provider`, `activity-alias`), `uses-permission`,
   `uses-feature`. Everything else in a library manifest is ignored.
   Relative component names resolve against the owning manifest's package
   and are rewritten fully-qualified. Malformed AAR manifest fails the
   build with `CCB-E3018` before packaging; an AAR with no manifest merges
   as nothing. Fingerprinted from inputs, so `link` re-runs when any
   manifest changes.
4. **compile-res** — `aapt2 compile` each resource directory into a `.flat`
   zip.
5. **link** — `aapt2 link` into `linked.ap_`, generating `R.java`. Uses the
   merged manifest when AARs are present, else the app manifest. The
   manifest's `package` must match `[project] package`.
6. **compile-code** — `kotlinc` (Kotlin + `R.java` + Java sources), then
   `javac` for Java sources and generated `R.java`. kotlinc resolves R
   symbols but emits no R.class files, so javac must compile them —
   otherwise library R classes are missing from the dex at runtime.
   Classpath: `android.jar` + `deps.jars` + AAR `classes.jar` files.
7. **dex** — `d8` (or `r8` for release + minify). `.class` files pass
   individually, not as a directory: kotlinc also writes
   `META-INF/*.kotlin_module` files, and d8 rejects non-class files in
   directory inputs. For Kotlin projects the kotlinc stdlib jar is a
   program input automatically (skipped when the project already supplies
   it via `[deps]` or Maven); for release + minify it is a program input
   to R8, not a `--lib`.
   - **Stdlib dex cache.** The stdlib jar is dexed once into the global
     cache `$XDG_CACHE_HOME/ccb/dex-stdlib` (else `~/.cache/ccb/dex-stdlib`,
     same root as the Maven cache, survives `ccb clean`), keyed by stdlib
     jar hash + min-api + build-tools version. Later builds pass the
     cached `.dex` to d8 instead of the jar. Single `<key>.dex` file,
     atomic rename on publish, validated on read (dex magic + header file
     size). Missing or corrupt entry falls back to the jar, never fails
     the build. Release + minify excluded: R8 takes the stdlib as a
     program-input JAR, not dex.
   - **R8 keep rules (release + minify).** CCB generates keep rules
     (`build/intermediates/r8-keep.pro`, passed to R8 ahead of the
     project's own `proguard_rules` file): the app's whole package
     (`-keep class <package>.** { *; }`), the `<application android:name>`
     and every manifest component (activities, services, receivers,
     providers, activity-aliases and their `targetActivity`), and view
     classes from layout XML (dotted element names, `class`,
     `android:name`, `layout_behavior`). Manifest entries come from the app,
     AAR, and library-module manifests; layouts from app and module res
     dirs and AAR zips. Generated text is part of the dex fingerprint, so
     a manifest or layout change re-runs dex. R8 tolerates a keep for a
     nonexistent class (warning only), so a stale layout reference can't
     break a build. Reflection-only references (`Class.forName` strings)
     can't be kept automatically: put those keeps in the project's own
     `proguard_rules` file, still required when `minify = true` (may hold
     only a comment otherwise).
   - **Dexcheck still runs** after dex and passes — cached stdlib classes
     are in the merged dex.
   - **Measured, Showcase:** clean build 57s before, 63s cold cache
     (one-time populate), 33–34s warm cache. Release + minify: APK
     2,651,777 bytes off, 75,393 on; dex 2,615,024 -> 37,160.
   - **d8 heap.** The build-tools `d8` wrapper defaults to `-Xmx2G`,
     which beats CCB's `JAVA_TOOL_OPTIONS=-Xmx1g` (command-line `-Xmx`
     wins over the env var). CCB passes `-JXmx<N>m` instead, N = 256 +
     16 per MiB of dex program inputs (app `.class` files + `[deps]`
     jars + AAR `classes.jar` files + stdlib input, cached `.dex` or
     jar), capped at 1024MB. Same flag populates the stdlib dex cache
     (input: stdlib jar alone). Floor evidence (2026-11-25, build-tools
     36.0.0): d8 succeeds at 128m on Showcase warm inputs and on the
     stdlib jar alone, and at 256m on 10,000 synthetic classes + stdlib
     dex (8.2MB in) with no time penalty vs 1024m; Showcase dex is
     byte-identical at 256m and at the 2G default. Heap never changes dex
     output, so it's not in the dex fingerprint. R8 untouched: whole-program
     shrinking doesn't follow this rule, and its 1G cap already equals
     the cap here.
8. **dexcheck** — Parse the dex files and verify every referenced class is
   defined in the dex. Referenced-but-undefined program class fails the
   build with `CCB-E3025` naming the missing classes, before packaging.
   Platform classes (`java.*`, `android.*`, …) come from `--lib` inputs,
   never in the dex: allowlisted, never reported. Annotation- and
   signature-only references still count. Runs even when dex was
   cache-skipped, and for AAB builds. Milliseconds, no cache of its own,
   doesn't count toward `steps_run`, so a no-op rebuild still reports
   "up to date". `--explain` prints its plan line; `--dry-run` doesn't
   run it.
9. **native** — NDK clang per ABI (only with an `[ndk]` section).
10. **package** — CCB's own ZIP writer: `linked.ap_` entries, dex files,
    assets (app assets win over AAR assets with the same name),
    `lib/<abi>/*.so` (duplicate `.so` names are an error).
11. **align** — `zipalign -f 4`.
12. **sign** — `apksigner`. Debug uses `~/.android/debug.keystore`
    (created with keytool when missing). Release uses `env:VAR` password
    references; passwords never appear in commands or logs.
13. **verify** — `apksigner verify` and `aapt2 dump badging`. An APK counts
    as built only when both pass.

## AAB output (`--aab`)

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

Per step: each fingerprint hashes inputs (file contents + relevant config
values + tool versions). Unchanged step is skipped, previous outputs reused.

- Comment-only Kotlin change re-runs kotlinc (source hash changed) but
  skips dex and downstream: `.class` files — and every downstream
  fingerprint — are identical.
- String-*value* change re-runs compile, link, package, align, sign,
  verify, but skips kotlinc and dex (`R.java` unchanged).
- Tool upgrade changes the tool fingerprint, invalidates all steps.
- `--explain` prints the run/skip plan without executing. Conservative
  downstream of `link` — see [docs/json-protocol.md](json-protocol.md).
- `--dry-run` prints the exact commands, executes nothing.

Cache lives at `build/ccb-cache.json`. Missing or corrupt cache means
"rebuild everything", never an error. `ccb clean` removes `build/`,
cache included.

## Reproducible builds

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

- **JVM startup dominates wall time.** kotlinc, d8, apksigner each boot a
  JVM (seconds on a slow VM). CCB's own work — hashing, decisions,
  packaging — is well under a second. Spec §8 targets (<10 s cold, <2 s
  incremental Kotlin) are not met on this VM: cold hello-world ~23 s,
  one-file Kotlin rebuild ~14 s. The opt-in kotlinc daemon (below) removes
  kotlinc JVM startup from warm rebuilds, not from cold builds — starting
  the daemon costs about what starting kotlinc costs.
- **Tool probe cache.** `kotlinc -version` / `java -version` probes used to
  dominate no-op rebuilds (~3 s of a ~3.4 s no-op). Now cached in
  `build/intermediates/tool-probes.json`, keyed by tool path + mtime +
  size. Repeat build with unchanged tools skips both probes (warm no-op
  ~0.2 s). Mismatch, missing entry, or corrupt file re-probes; `ccb
  clean` removes it with `build/`. Never changes build outputs: only
  feeds the tool fingerprint, and a cached version for an unchanged binary
  is identical to a fresh probe.
- **d8 input size.** `.class` files pass as individual arguments;
  extremely large projects could approach the OS argument limit. d8
  argfiles are the fix if that ever bites.
- **Build-tools wrapper scripts** (`d8`, `apksigner`) exec plain `java`
  from PATH and ignore `JAVA_HOME`, so CCB prefixes the resolved JDK
  `bin` directory onto PATH for JVM tool runs.
- **No device or emulator** here, so install/launch are verified only up
  to the device boundary (see [docs/device-setup.md](device-setup.md)).

## Kotlinc daemon (opt-in)

Default: plain `kotlinc` subprocess. One binary, one command, one result,
no background process. Opt in to reuse a kotlinc compile daemon across
builds of the same project — warm rebuilds skip the kotlinc JVM startup.

```toml
[build]
kotlinc_daemon = true            # default false
kotlinc_daemon_idle_seconds = 300 # default 300, allowed 60-3600
```

```sh
ccb build --kotlinc-daemon      # opt in for this build (overrides config)
ccb build --no-kotlinc-daemon   # opt out for this build (overrides config)
ccb daemon stop                 # ask the project's daemon to shut down
```

Combining both flags is an error. `ccb clean` stops the daemon
best-effort (only when daemon mode is enabled in config) before removing
`build/`.

- Per-project: run files under `build/kotlin-daemon`. Two projects never
  share one; `ccb clean` removes it with `build/`.
- CCB invokes the daemon client
  (`org.jetbrains.kotlin.daemon.client.KotlinCompilerClient`) directly
  with an explicit classpath — 7 jars for the client, 6 for the compiler
  (including `kotlin-daemon.jar`) — never a `lib/*` wildcard. Missing jar
  falls back to plain kotlinc, silently.
- Client and daemon JVMs get `-Djava.net.preferIPv4Stack=true`: the
  daemon's RMI/JRMP handshake to the loopback registry hangs in this
  sandboxed VM without it. Load-bearing; don't remove it.
- Both JVMs capped at 512 MB heap (`-Xmx512m`, not configurable).
- Idle shutdown needs both options:
  `--daemon-autoshutdownIdleSeconds=<idle>` and
  `--daemon-shutdownDelayMilliseconds=<idle * 1000>`. Idle timeout alone
  only applies while a client is connected; CCB's client exits after each
  build, so without the shutdown delay the daemon would exit ~1 s after
  the first build and never be reused.
- `ccb daemon stop` asks the daemon to shut down, but the daemon still
  honors the shutdown delay: the process exits within the idle timeout,
  not instantly. Poll if you need to wait.
- The daemon client exits 0 even when compilation fails, so CCB doesn't
  trust its exit code: it parses captured output for `result code: 0`
  (success) and line-start `ERROR\t` (compile errors, reported as
  `CCB-E3004`).
- Stale or crashed daemon never fails the build: client can't connect (or
  exits without a result code and without error lines) → CCB logs a
  fallback note, runs plain kotlinc. Build fails only if plain kotlinc
  also fails.
- The daemon log may contain a `ClassNotFoundException` for the scripting
  plugin (`ScriptingK2CompilerPluginRegistrar`): benign, the scripting
  plugin isn't on the daemon classpath and CCB never compiles scripts.
- Toggling the daemon doesn't invalidate outputs: same compiler, same
  arguments, compile-code fingerprint unchanged.

Measured on this VM (2026-11-25, kotlinc 2.4.21, single runs, shared VM,
~1.5 CPUs — noisy, treat cold numbers as a range, not a promise):

| Project | Cold, daemon off | Cold, daemon on | 1-file rebuild, off | 1-file rebuild, on |
|---|---|---|---|---|
| hello | 23–29 s | 25 s | 13.6 s | 5.2 s |
| Showcase (copy) | 34–64 s | 66–77 s | 21.6 s | 17.8 s |

Cold builds are a wash (daemon startup costs about what plain kotlinc
startup costs, Showcase cold range overlaps either way). Warm rebuilds
win — the kotlinc step itself drops from ~8 s to ~2.5 s wall. The
Showcase warm win is smaller in absolute terms because its rebuild is
dominated by non-kotlinc stages (aapt2, dex, packaging).

## Release binary (static + stripped)

The shippable binary is the release build: `ccb` linked with `-static`,
stripped. Dev and sanitizer builds stay dynamic and unstripped — ASan
doesn't work with a static binary, so the two options are mutually
exclusive in CMake (configuring both fails).

```
cmake --preset release && cmake --build --preset release
# manual form:
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCCB_STATIC_RELEASE=ON
cmake --build build-release -j4
```

`CCB_STATIC_RELEASE` applies only to the `ccb` executable: `-static` plus
a post-build strip. Test executables in the same directory stay dynamic
and unstripped (dev artifacts, not shipped). The `dev` and `sanitizer`
presets configure `build/` and `build-san/` with the option off, so plain
and sanitizer workflows are unchanged. `ccb check` and full app builds
verified with the static binary (Showcase clean build, 2026-11-25).

Measured sizes (2026-11-25):

| Binary | Size |
|---|---|
| Dev `build/ccb` (Debug, dynamic, unstripped) | 1,151,424 |
| Dynamic Release, stripped (for comparison) | 531,208 |
| Static Release, unstripped | 1,373,432 |
| Static Release, stripped (`build-release/ccb`) | 1,268,272 |

The ~605 KB figure sometimes quoted is the stripped *dynamic* binary; the
static binary is ~1.27 MB because glibc is linked in. Either way well
under the 5 MB limit in requirement.md §8, and the static build makes the
"one static binary" wording in requirement.md §2.5 true for the release
artifact. Verify with:

```
file build-release/ccb   # statically linked, stripped
ldd build-release/ccb    # not a dynamic executable
```

## Running the tests

Build CCB first ([README](../README.md#install-from-source)). Then:

```
ctest --test-dir build -L fast     # quick gate: 43 tests, ~16s quiet
                                   # (~45s when the VM is busy)
ctest --test-dir build -L <area>   # one area: config, setup, pipeline,
                                   # preview, misc, dexcheck, daemon,
                                   # maven, modules
ctest --test-dir build -LE slow    # same as -L fast
ctest --test-dir build             # full suite: 50 tests; runs in CI
```

Every test has two labels: speed (`fast` / `slow`) and area. Fast means
no real Android build, seconds each. Slow means a real `ccb build` or a
timeout wait. Per-change, run fast + the affected area. Before a push,
the full suite runs in GitHub Actions (`.github/workflows/ci.yml`,
plain + sanitizer jobs) — do not run it on the VM.

Slow tests, last full run (2026-11-25): `test_modules` 450s,
`test_kotlinc_daemon` 330s, `test_dex_contents` 199s, `test_r8_minify`
133s, `test_aab` 88s, `test_maven` 82s, `test_dexcheck` 49s. Area
totals follow from those: pipeline ~7 min, modules ~7.5 min, daemon
~5.5 min, maven ~1.5 min, dexcheck ~1 min.

Never run two `ctest` invocations at once (plain + sanitizer, or two plain
runs): run them sequentially. Each test binary uses a per-process `mkdtemp`
dir (`/tmp/ccb_test_<name>_XXXXXX`), so parallel tests within one `ctest
--jN` run can't collide — but two concurrent `ctest` runs still share CPU
and memory. On this VM `-j4` plain measured 1037s vs 739s sequential, `-j2`
1082s (2026-11-25, 47 tests). The bottleneck is CPU-bound JVM work
(kotlinc + d8), not test serialization. d8's heap is now sized (256MB
floor, 1024MB cap — see dex above), so four parallel d8 runs commit at most
4G instead of 8G; the -j4 slowness itself is CPU-bound, not memory. The
stdlib dex cache above is what actually cuts per-build time (Showcase 57s
→ 33–34s warm; `test_aab` 107s → 90s). 50 tests total.
