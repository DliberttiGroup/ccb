# CCB — Requirement Document

> Status: DRAFT — 2026-10-24. This document defines what CCB must be able to do.
> It is a requirements spec, not a design doc and not code. Nothing here is built yet.

## 1. What CCB is

**CCB (working expansion: Custom C Builder)** is a small, fast, headless Android
build system written in C. It turns an Android app project into a signed,
installable APK **without Gradle and without Android Studio**.

It is **agent-oriented first**: a human can read its output, but its primary
consumer is an AI agent driving builds from a terminal. Every behavior in this
document is judged against one question: *can an agent run this, understand what
happened, and fix what broke, without guessing?*

### 1.1 The problem it solves

Gradle is the wrong tool for small, agent-driven Android work:

- It is slow (JVM startup, daemon, configuration phase) for projects that
  should build in seconds.
- Its output is long, noisy, and non-deterministic, which makes agent
  debugging expensive and unreliable.
- It downloads plugins and dependencies implicitly, so a build can fail for
  reasons unrelated to the project.
- It is a general-purpose build platform; CCB is a single-purpose APK builder.

### 1.2 What "headless" means (and does not mean)

- Headless means: no Android Studio, no Gradle daemon, no GUI, no background
  processes by default. One binary, one command, one result — unless the
  user opts into the kotlinc daemon (see §2 goal 4 and docs/build.md):
  an opt-in, per-project daemon with auto-shutdown after an idle
  timeout. The daemon is never started unless opted in, and it never
  outlives its project.
- It does **not** mean "no JVM". The Kotlin compiler (`kotlinc`) and the dexer
  (`d8`) are JVM tools and CCB orchestrates them. CCB itself is native C and
  starts instantly; the JVM only runs for the two steps that genuinely need it.

## 2. Goals

1. **Build a real APK from a TOML file.** A complete project (manifest,
   resources, Kotlin/Java sources, assets, native libs) builds to a signed,
   zipaligned, installable APK with one command.
2. **Be fast.** Cold build of a hello-world app in under 10 seconds on a
   normal machine. Incremental rebuild (one source file changed) in under
   2 seconds.
3. **Be agent-readable.** Structured machine output, stable exit codes,
   actionable errors, dry-run planning, and cache explanations (see §6).
4. **Be hermetic by default.** No network access during a build. No implicit
   downloads. No daemon by default; an opt-in kotlinc daemon is allowed
   when the user enables it (`[build] kotlinc_daemon = true` or
   `ccb build --kotlinc-daemon`): it is per-project (run files under the
   project's own `build/kotlin-daemon`), shuts itself down after an idle
   timeout (default 300 s, configurable 60–3600 s), is stopped by
   `ccb daemon stop` and best-effort by `ccb clean`, and a stale or
   crashed daemon falls back to a plain kotlinc invocation instead of
   failing the build. A build either works offline or fails loudly saying
   exactly which tool is missing.
5. **Be small and dependency-free.** One static binary, under 5 MB, linking
   only libc. No runtime dependencies beyond the Android SDK toolchain it
   drives.
6. **Be deterministic.** Same inputs + same tool versions = byte-identical APK
   (sorted zip entries, fixed timestamps via `SOURCE_DATE_EPOCH`).
7. **Support the user's real stack.** Kotlin first, Java second, C/C++ native
   libs via the NDK. This is the stack the user's projects (including melod's
   C core) actually use.
8. **Modular, memory-safe C.** A codebase an agent can navigate and a human
   can trust: small modules with clear boundaries, explicit ownership, and
   safety wrappers that make classic C bugs (leaks, overruns, use-after-free)
   hard to write and easy to catch (see §9).
9. **Preview without a device.** For projects that follow the CCB UI
   convention (CSS styling + Kotlin flow sources), CCB renders an
   approximation of each screen in the terminal plus a logic tree of the
   app flow, so an agent can judge whether layout and logic look right
   without an emulator (see §10). This is an approximation tool, not a
   simulator, and the document is explicit about that boundary.

## 3. Non-goals (v1)

- Not a Gradle replacement for large multi-module apps. One app
  module; library modules are supported (see §4.3). Dynamic feature
  modules remain out of scope.
- No Gradle dependency resolution. Dependencies are local files or Maven
  coordinates declared in TOML (see §5.4); Gradle itself (daemon, build
  scripts, plugins) is out of scope.
- No annotation processing (kapt/KSP). Apps using Dagger, Hilt, or Room
  codegen are out of scope for v1; CCB must fail with a clear message if it
  detects them, not a cryptic compiler error.
- AAB output: `ccb build --aab` produces a base-module-only bundle (see
  §5.10). Split APK generation, asset packs, and dynamic feature
  modules remain out of scope.
- No iOS, no Windows support in v1 (Linux first, macOS second).
- No plugin ecosystem, no build scripting language. TOML is the whole
  configuration surface.

## 4. Project configuration: the `.toml` file

One file per project, named `ccb.toml` by default, overridable with
`--config`. It must be human-readable, commented, and complete: everything
CCB needs to know lives here or in environment variables, never in hidden
state.

### 4.1 Example (complete)

```toml
# Schema version. Mandatory: CCB errors if it is missing or newer than
# the version this CCB binary understands.
ccb_version = 1

[project]
name = "Pixel Wave"
package = "com.pixelwave.demo"
version = "0.1.0"
version_code = 1
min_sdk = 26
target_sdk = 35
compile_sdk = 35

[paths]
manifest = "app/src/main/AndroidManifest.xml"
res = "app/src/main/res"
assets = "app/src/main/assets"
kotlin = "app/src/main/kotlin"
java = "app/src/main/java"
libs = "app/libs"

[sdk]
# Resolution order: this field, then $ANDROID_HOME, then $ANDROID_SDK_ROOT,
# then well-known locations. If none resolve, `ccb doctor` explains how to fix it.
home = "/opt/android-sdk"
build_tools = "35.0.0"
ndk = "27.0.12077973"  # optional; required only if [ndk] is present
# Tool overrides (optional): aapt2 / zipalign may point at a different
# binary than the SDK's build-tools copy, e.g. a distro package build.
# Precedence: this key, then $CCB_AAPT2 / $CCB_ZIPALIGN, then the default
# <sdk-home>/build-tools/<version>/<tool>. A relative value is used
# exactly as given (cwd-relative at invocation time), never rebased
# against the project dir or SDK home. A set-but-broken override is a
# Stage 0 error (CCB-E2009 naming the source and path); CCB never
# silently falls back to the default tool.
# aapt2 = "/usr/bin/aapt2"
# zipalign = "/usr/bin/zipalign"

[signing.debug]
store = "debug.keystore"   # auto-generated with standard debug credentials if missing
alias = "androiddebugkey"

[signing.release]
store = "release.keystore"
alias = "release"
store_password_env = "CCB_STORE_PASSWORD"
key_password_env = "CCB_KEY_PASSWORD"

[build]
variant = "debug"   # or "release"
minify = false      # true = run r8 instead of d8 (release only)
shrink_resources = false
jvm_target = "17"

[deps]
jars = []   # paths to .jar files, relative to the project root
aars = []   # paths to .aar files, relative to the project root

[ndk]
abis = ["arm64-v8a"]
cpp_flags = ["-O2"]
c_sources = ["app/src/main/cpp/native.c"]
```

### 4.2 Configuration rules

- All paths are relative to the directory containing `ccb.toml`, unless
  absolute.
- Config discovery: with no `--config` flag, CCB searches upward from
  the current directory to the filesystem root for `ccb.toml` (like git
  searches for `.git`). `--config` with an explicit path overrides this.
- `ccb_version` is mandatory. A missing version is a configuration
  error (code 2); a version newer than the binary understands is also
  an error, naming the supported range.
- Unknown keys are a **hard error** with the key name and the list of valid
  keys. Silent typos in config are the #1 agent time-waster; CCB must not
  tolerate them.
- Secrets (keystore passwords) are **never** written in TOML. They come from
  environment variables named in TOML, or from an interactive prompt. CCB
  must never print a password, even in `--verbose` or `--dry-run` output.
- `ccb.toml` must be validatable without building: `ccb check` parses,
  resolves paths, and reports every problem at once (not one error per run).
- Tool overrides: `[sdk] aapt2` / `[sdk] zipalign` and the env vars
  `CCB_AAPT2` / `CCB_ZIPALIGN` select a different aapt2 or zipalign
  binary for every invocation (compile, link, badging, zipalign,
  doctor/check reporting, dry-run output). Precedence, highest first:
  `[sdk]` key, env var, default SDK build-tools path. An empty env
  value counts as unset. A relative override path is used exactly as
  written — it is relative to the working directory at invocation
  time, not to the project root or SDK home. A set override that does
  not exist or is not executable fails Stage 0 with CCB-E2009 naming
  the source and the path; there is no silent fallback to the default.
  `ccb doctor` and `ccb check` report the effective path and its
  source (`config`, `env`, or `default`) for both tools, and the
  effective paths participate in the cache fingerprints, so switching
  tools invalidates the affected stages.

### 4.3 Library modules

A project may declare library modules as `[modules.<name>]` tables
in the root `ccb.toml` (full rules in `docs/modules.md`):

```toml
[modules.base]
dir = "libbase"

[modules.core]
dir = "libcore"
depends = ["base"]
```

- `dir` (required): the module root. Standard layout under it:
  `src/main/AndroidManifest.xml` (required), `src/main/res`,
  `src/main/assets`, `src/main/kotlin`, `src/main/java` (each
  optional).
- `depends` (optional): other module names. Modules build in
  topological order, dependencies first; a cycle is a hard error
  naming the cycle (`CCB-E1010`), an unknown dependency is a hard
  error naming it (`CCB-E1011`).
- Each module builds into an intermediate AAR consumed through the
  existing dependency machinery (§5.4, §5.2): manifest merges,
  res merges at link, classes dex, assets package. The app module is
  the only APK/AAB output.
- A module manifest must declare a package different from the app
  package and from every other module's package.
- Library R fields are non-final (the module link generates R.java,
  CCB de-finalizes it, the app link assigns final IDs), so library
  code may reference its own and its dependencies' R classes.
- Module outputs participate in app fingerprints: changing a library
  rebuilds the app (see §5.11).

## 5. Build pipeline

CCB executes a fixed, ordered pipeline. Each stage is independently
skippable by the cache (see §7) and independently reportable in JSON output
(see §6). The stages, in order:

### 5.1 Stage 0 — Load and validate

Parse `ccb.toml`, resolve all paths, validate the schema, check the SDK,
build-tools, platform, `kotlinc`, and `d8`/`r8` exist and meet minimum
versions. Fail here, before any work, if the toolchain is incomplete.
Tool overrides are validated here too: a `[sdk] aapt2` / `[sdk]
zipalign` key or a `CCB_AAPT2` / `CCB_ZIPALIGN` env var that points at
a missing or non-executable binary fails Stage 0 with CCB-E2009 naming
the source and the path, independently of whether the build-tools
directory itself exists — a missing build-tools directory (CCB-E2002)
never masks a broken override, and a broken override never falls back
to the default tool.

Supported v1 ranges (pinned here; `ccb doctor` judges against them):
compileSdk 34–36, build-tools 35.x/36.x, kotlinc 2.x, JDK 17+. A tool
outside its range is a Stage 0 error naming the range and the found
version.

### 5.2 Stage 1 — Compile resources (`aapt2 compile` + `aapt2 link`)

- Compile `res/` into flat files.
- Link with the manifest into a base APK, generating `R.java` (and `R.txt`)
  for the Kotlin/Java compile.
- Manifest merging: when the project declares AARs, each AAR's
  `AndroidManifest.xml` is merged into the app manifest, deterministically
  in the order the AARs are listed in config, and the merged manifest is
  what `aapt2 link` sees (written to
  `build/intermediates/merged-manifest/AndroidManifest.xml` for
  inspection; the pre-merge app manifest stays in the project for
  `--explain`/`--dry-run` inspection). Merged entries: application
  components (`activity`, `service`, `receiver`, `provider`,
  `activity-alias`) and `uses-permission` / `uses-feature`. Rules:
  app-manifest entries always win on name conflict (warning naming the
  AAR and the entry, app's version kept); identical entries from two
  AARs dedup silently; same name with different attributes keeps the
  first AAR's version with a warning naming both; a malformed AAR
  manifest is a hard error (`CCB-E3018`) naming the AAR, failing the
  build before packaging; an AAR with no manifest merges as nothing.
  Relative component names resolve against the owning manifest's
  package and are rewritten to fully-qualified form in the merged
  output. Everything else in a library manifest (application
  attributes, `<uses-sdk>`, `<permission>` declarations, `<meta-data>`,
  `<queries>`) is ignored: the app manifest owns it.

### 5.3 Stage 2 — Compile code (`kotlinc` + `javac`)

- `kotlinc` over Kotlin sources with classpath = `android.jar` +
  `R.java` output + declared jars + extracted AAR `classes.jar`s.
- `javac` over Java sources with the same classpath (if any Java exists),
  plus the generated `R.java` (kotlinc resolves R symbols but emits no
  R.class files, so javac must compile them).
- `d8`/`r8` gets the kotlinc stdlib jar as a program input for Kotlin
  projects, unless the project already supplies it via `[deps]` or Maven.
- Kotlin compiler location: `$KOTLIN_HOME`, then `PATH`, then TOML
  `[sdk].kotlin_home`. Missing kotlinc is a Stage 0 error, not a Stage 2
  surprise.
- Compiler plugins: when `[build] kotlin_plugins` is set, kotlinc gets
  `-Xplugin=<jar>` per resolved plugin jar plus
  `-P plugin:<id>:<key>=<value>` per `[build] kotlin_plugin_options`
  entry; plugin jars and options join the compile fingerprint. The
  Compose compiler plugin path is
  `org.jetbrains.kotlin:kotlin-compose-compiler-plugin:<kotlin-version>`
  (the plain artifact, not the `-embeddable` variant, which is built
  for Gradle's embedded compiler — see §5.4).

### 5.4 Stage 3 — Dependencies (local files + remote Maven)

- JARs go straight onto the compile classpath and into the dex input.
- AARs are unzipped into the build dir: `classes.jar` to the classpath,
  `res/` merged into Stage 1 input, `assets/` merged into Stage 4,
  `jni/<abi>/` copied to the APK's `lib/` dir, and `AndroidManifest.xml`
  merged into the app manifest (see §5.2).
- Duplicate classes across dependencies are a hard error naming both
  sources. No silent shadowing.
- Remote dependencies: `[deps] maven = ["group:artifact:version", ...]`
  resolves through Google Maven (plus Maven Central when
  `[deps] maven_central = true`, plus extra repositories from
  `[deps] maven_repos`, including authenticated ones via
  `env:VAR` credentials) into the local Maven cache
  (`~/.cache/ccb/maven2`), and the resolved AARs/JARs feed this stage
  exactly like local files. Versions may be exact, ranges
  (`[1.0,2.0)`), `LATEST`, `RELEASE`, or `-SNAPSHOT` (resolved from
  `maven-metadata.xml`; the lockfile records the concrete version);
  packaging comes from the POM; transitive resolution is breadth-first
  nearest-wins over compile + runtime scopes. Parent POMs are fetched
  from the same repositories and contribute properties,
  dependencyManagement, `<dependencies>`, and profiles (child entries
  win, nearest ancestor wins among parents; chains capped at 10
  ancestors, cycles fail with `CCB-E3024`). Profiles activate by id
  (`[deps] maven_profiles`) or `<activeByDefault>` when no ids are
  listed. `[deps] maven_exclude` suppresses
  artifacts (`group:artifact` for every version, or
  `group:artifact:version` for one version), checked before
  nearest-wins; excluding a direct dependency is a config error.
  Compiler plugins declared in `[build] kotlin_plugins` resolve via
  Maven the same way (exact versions only, no transitive walk, not in
  the lockfile) and are wired into kotlinc in §5.3. `ccb setup`
  prefetches the cache and writes the resolved tree to
  `ccb.lock` in the project root; builds and `ccb check` resolve
  cache-only, fail with `CCB-E3021` on a miss, and fail with
  `CCB-E3023` on any deviation from a present lockfile. Full rules
  in `docs/maven.md`.

### 5.5 Stage 4 — Dex (`d8`, or `r8` for release)

- `d8` converts compiled classes to `.dex` files.
- On a Kotlin `d8` build the stdlib jar is dexed once into the global
  cache (`$XDG_CACHE_HOME/ccb/dex-stdlib`, else `~/.cache/ccb/dex-stdlib`),
  keyed by the stdlib jar hash + min-api + build-tools version; later
  builds pass the cached `.dex` to `d8` instead of the jar. A missing
  or corrupt entry falls back to the jar and never fails the build.
  Release + minify is excluded: R8 takes the stdlib as a JAR.
- Release + `minify = true` runs `r8` with CCB-generated keep rules
  (`build/intermediates/r8-keep.pro`): the app's whole package plus
  every manifest component and layout view class R8 cannot see from
  code, ahead of the project's ProGuard/R8 rules file (declared in
  TOML as `proguard_rules`; required when `minify = true`, but it may
  hold only a comment when the app needs no extra keeps). R8 failures must
  surface the rule file and line, not just the tool's stderr.
  Reflection-only references (`Class.forName` strings) cannot be
  kept automatically: those keeps belong in the project's rules
  file. The stdlib dex cache is bypassed on this path by design:
  R8 takes the stdlib as a program-input JAR, not dex, so it can
  strip the stdlib classes the kept code never reaches.
- Immediately after dex, a dexcheck step parses the produced dex
  files and verifies every referenced class is defined in the dex.
  A missing program class fails the build with `CCB-E3025`, naming
  the missing classes (first 10, then "+ N more"), before packaging
  — so a missing stdlib or library R class is a build failure, not
  a launch crash. Platform classes (`java.*`, `android.*`, …) are
  `--lib` inputs and are allowlisted; annotation- and
  signature-only references still count. Reflection-only
  references (`Class.forName` strings) cannot be caught: only
  types the dex references are checked. The step runs even when
  dex was cache-skipped, and for AAB builds too.

### 5.6 Stage 5 — Native libs (NDK, optional)

- If `[ndk]` is present, compile C/C++ sources with the NDK Clang
  toolchain once per ABI into `.so` files.
- `.so` files land in the APK at `lib/<abi>/`.
- NDK version is pinned in TOML; a mismatched installed NDK is a Stage 0
  error.

### 5.7 Stage 6 — Package

- CCB assembles the final APK itself: it takes the `aapt2 link` output
  and appends dex files, assets, and native libs using its own ZIP
  reader/writer (no external `zip` binary dependency).
- APK entries are written in sorted order for determinism.

### 5.8 Stage 7 — Align and sign (`zipalign` + `apksigner`)

- `zipalign` (4-byte alignment) then `apksigner` (v2 signature minimum;
  v1+v2+v3 for release when configured).
- Debug builds: the debug keystore defaults to
  `~/.android/debug.keystore` (the Android standard location, so it
  survives `ccb clean`), overridable via `[signing.debug] store`. If it
  is missing, CCB generates it (standard Android debug credentials)
  and records that it did so.
- Release builds: missing keystore, missing alias, or missing password
  env var is a hard error before any compilation starts (fail fast, not
  after a 30-second build).
- After signing, CCB verifies with `apksigner verify` and reports the
  result. An APK is not "built" until it verifies.

### 5.9 Stage 8 — Report

Print the result: output path, size, variant, package, version, signing
status, per-stage timings, and cache hits/misses. In `--json` mode this is
a structured summary event, not prose.

### 5.10 AAB output (`ccb build --aab`)

`ccb build --aab` produces an Android App Bundle alongside the APK
pipeline; APK stays the default. The bundle reuses every stage it can:

- **Link** runs `aapt2 link --proto-format` into a separate
  intermediate (`build/intermediates/linked-aab.ap_`) under its own
  cache key (`link-aab`), so APK and AAB builds never poison each
  other's cached link output. R.java output is identical in both
  formats and stays shared.
- **Package** assembles the base module: the manifest at
  `base/manifest/AndroidManifest.xml`, the protobuf resource table at
  `base/resources.pb`, compiled resources at `base/res/...`, dex at
  `base/dex/`, assets at `base/assets/`, native libs at
  `base/lib/<abi>/`, plus `BundleConfig.pb` at the root. The config
  is hand-encoded on the protobuf wire format (no protobuf
  dependency): `BundleConfig { BundleTool bundletool = 1 }` with
  `BundleTool { string version = 2 }` = `"1.0.0"`. Field numbers are
  ground truth from bundletool 1.18.1's own output — version is
  field 2, and encoding it as field 1 makes `bundletool validate`
  fail with an empty version.
- **Align is skipped**: bundles are not zipaligned, and Play rejects
  aligned bundles.
- **Sign** reuses the APK sign step verbatim (same keystore, alias,
  and password rules, including debug keystore generation), with one
  addition: apksigner gets `--min-sdk-version` because it cannot read
  the manifest from `base/manifest/`. The signature covers the
  bundle as a v1 JAR signature (META-INF entries) plus the v2 block.
- **Verify** is `jarsigner -verify` plus a structural check
  (`base/manifest/AndroidManifest.xml`, `BundleConfig.pb`, and at
  least one META-INF entry present). `apksigner verify` cannot verify
  a bundle: it hard-requires `AndroidManifest.xml` at the ZIP root.
  `bundletool validate` passes on CCB-built bundles (verified against
  bundletool 1.18.1).
- Output is `build/outputs/<variant>/app.aab`; the `--json` build
  event reports it under the `"aab"` key instead of `"apk"`. Cache
  keys for the bundle path (`link-aab`, `package-aab`, `sign-aab`,
  `verify-aab`) are separate from the APK keys, so building one
  format never invalidates or alters the other — the APK build is
  byte-identical before and after an AAB build.
- Out of scope: split APK generation (use `bundletool build-apks`),
  asset packs, dynamic feature modules, Play signing key upload.
  `ccb install` / `ccb run` remain APK-only.

### 5.11 Library modules

Between the deps step and the manifest merge, CCB builds each
declared library module (§4.3) in topological order into an
intermediate AAR (`build/intermediates/modules/<name>/<name>.aar`):
`classes.jar` (when the module has Kotlin/Java sources), the module
manifest, raw `res/` and `assets/` trees. The module AARs join the
declared AARs in the merge list (declared first, then modules in
topological order), so every later stage treats them as local AARs:
manifest merge (§5.2), res compile/link (§5.2), compile classpath
(§5.3), dex inputs (§5.5), assets (§5.7).

- Module resources compile (`aapt2 compile`) and link (`aapt2 link`
  against the module manifest) to generate R.java for the module
  package, but only when the module has code that can reference R;
  a resources-only module skips the link. Generated R fields are
  de-finalized before compiling, so they resolve at runtime against
  the app's R class (see `docs/modules.md`).
- Module `classes.jar` excludes generated R classes: the app link
  generates the real R class for the module package, and shipping
  both would make d8 reject the duplicate.
- Module fingerprints cover tool versions, module inputs, declared
  deps, and upstream module outputs (AAR + generated R.java), so a
  changed library rebuilds its dependents. App fingerprints cover
  module outputs: the compile-code fingerprint hashes classpath jar
  contents (not just paths), the dex fingerprint hashes classes
  jars, and res/manifest inputs flow through their own fingerprints.
  A comment-only library change rebuilds only that module: identical
  class bytes mean an identical AAR hash, so dependents skip.
- Planning modes (`--explain`, `--dry-run`) never execute module
  builds; the merge step reads module manifests from their source
  files (the AAR manifest is a copy), and dex/package fingerprints
  skip intermediates that do not exist yet, so planning works on a
  fresh tree.
- Out of scope: per-module deps/NDK/signing, dynamic feature
  modules, Maven publishing, cross-module resource namespacing
  beyond what aapt2 already does.

## 6. Agent-oriented interface (the distinctive part)

This section is normative. A build system that is merely "scriptable" is
not agent-oriented. CCB must satisfy all of the following:

### 6.1 Commands

| Command | Purpose |
|---|---|
| `ccb init [dir]` | Scaffold a minimal project: `ccb.toml`, manifest, one Kotlin Activity, `res/`, `.gitignore`. Must work offline. |
| `ccb setup` | Provision the toolchain: install a JDK 17+ (via the platform package manager, interactively or `--yes`), SDK command-line tools, platform, and build-tools (checksum-verified downloads), the official kotlinc 2.4.21 compiler into the SDK home when missing, the NDK revision for `[ndk]` projects, r8 9.5.28 into the SDK home for minify projects without an explicit jar, and missing host tools from the platform package manager (Termux: aapt2/android-tools from the official repos, zipalign from the rendiix repo only when the official repos have no candidate; Linux: adb from the distro manager). In a project with `[deps] maven`, also prefetches every direct + transitive artifact into the local Maven cache (see §5.4). Ends with a final verification pass (toolchain resolve + adb) reported as warnings. `--upgrade` reinstalls setup-managed kotlinc/r8 to pinned versions and implies `--yes`; `--print-env` prints shell exports instead of the ready line. Pinned downloads are cached under `~/.cache/ccb/downloads` with resume. This is the only command allowed network access; builds stay hermetic. |
| `ccb check` | Validate config + toolchain only. No build output. Exit non-zero with all problems listed. |
| `ccb build` | Full pipeline (§5). Flags: `--variant`, `--config`, `--json`, `--dry-run`, `--verbose`, `--explain`, `--aab` (build `app.aab` instead of `app.apk`, see §5.10). |
| `ccb clean` | Remove the build directory only. Never touches sources. |
| `ccb doctor` | Check every tool CCB needs (SDK, build-tools, platform, kotlinc, d8, apksigner, zipalign, adb, NDK if configured). For each missing item print what it is, where CCB looked, and the exact install command. |
| `ccb install` | `adb install` the built APK (device selection via `--device`, error if 0 or 2+ devices and no selection). |
| `ccb run` | Install + launch the app's main activity via `adb shell am start`. |
| `ccb logcat` | Stream `adb logcat` filtered to the app's package (convenience for agents reading crash logs). |
| `ccb preview` | Render the app's screens in the terminal from CSS + Kotlin flow sources, and print the logic tree (see §10). No device or emulator needed. |
| `ccb version` | Print CCB version, supported compileSdk range, and embedded tool expectations. |

### 6.2 Structured output (`--json`)

- With `--json`, stdout is JSON Lines: one JSON object per event, nothing
  else on stdout (human prose goes to stderr or is suppressed).
- Event types: `phase` (started/finished, with `duration_ms`),
  `cache` (hit/miss per stage, with reason), `warning`, `error`, `summary`.
- Every `error` event carries: `phase`, `message`, `file`, `line`, `col`
  (when applicable), `command` (the exact command that failed),
  `workdir`, and `suggestion` (one concrete next action, e.g.
  "install platform android-35 with: sdkmanager \"platforms;android-35\"").
- Every `error` event also carries a stable diagnostic `code`
  (e.g. `CCB-E1001`) from the diagnostics catalog in
  `docs/diagnostics.md`. Agents branch on codes, not on prose.
- An agent must be able to reconstruct the full build state from the
  JSON stream alone, without reading any other output.

### 6.3 Dry run and explain

- `ccb build --dry-run` prints the complete ordered command plan
  (tool, arguments, working directory, inputs, outputs) and exits without
  executing anything. Agents inspect before running.
- `ccb build --explain` runs the build and, for every stage, prints WHY
  it ran or was skipped (which input hash changed, which tool version
  changed, or "cache hit").
- Both modes must never print secrets.

### 6.4 Exit codes (stable, documented, never reused for other meanings)

| Code | Meaning |
|---|---|
| 0 | Success (build produced a verified APK, or check/doctor passed). |
| 1 | Build failure (a pipeline stage failed; see the error event). |
| 2 | Configuration error (bad TOML, unknown key, missing path). |
| 3 | Toolchain missing or wrong version (see `ccb doctor`). |
| 4 | Signing error (keystore/alias/password problem). |
| 5 | Device error (`install`/`run`/`logcat` with no device or adb failure). |

### 6.5 Logging rules

- Default output is quiet: one line per stage (started/finished), plus
  warnings and errors. No tool banner spam.
- `--verbose` passes through the underlying tools' output, clearly
  delimited per stage.
- Tool stderr is always captured and attached to the failing stage's
  error event, never dumped raw to the terminal.
- No timestamps in default output (breaks diffing); timestamps exist
  only in `--json` events.

### 6.6 Hermeticity and safety

- Builds never write into source directories. All outputs go to the
  build directory (default `build/`, overridable in TOML).
- Build directory layout (fixed): `build/ccb-cache.json` (cache
  manifest), `build/intermediates/` (per-stage outputs), and
  `build/outputs/<variant>/app.apk` (final artifacts). `ccb clean`
  removes exactly this directory.
- Builds never touch the network. If a tool tries (e.g. a Gradle-style
  dependency fetch), that is a configuration smell CCB must not have.
- Builds are safe to run concurrently for different projects (no global
  daemon — an opt-in kotlinc daemon is per-project, its run files under
  that project's own build dir — no shared mutable state outside the
  project's own build dir).
- `ccb clean` is the only destructive command, and it only removes the
  project's own build directory.

## 7. Incremental builds and caching

- CCB keeps a cache manifest in the build directory recording, per stage:
  input file hashes (SHA-256), tool versions, and relevant config values.
- A stage is skipped on a cache hit (same hashes + same tool versions +
  same config). A cache miss re-runs only that stage and everything
  downstream of it.
- Cache keys must include the kotlinc/d8/aapt2 versions: a toolchain
  upgrade invalidates the cache, loudly, rather than producing a stale APK.
- Cache corruption (unparseable manifest) is not fatal: CCB logs a
  warning, discards the cache, and does a full rebuild.
- `ccb build --explain` is the debugging surface for "why did it rebuild".

## 8. Performance targets (acceptance criteria)

| Metric | Target |
|---|---|
| CCB binary size | < 5 MB static binary |
| CCB startup time | < 10 ms (before any tool invocation) |
| Cold build, hello-world app | < 10 s |
| Incremental build, one Kotlin file changed | < 2 s |
| Incremental build, one resource changed | < 3 s |
| Memory during build (CCB itself, excluding kotlinc/d8) | < 100 MB |

Targets are measured on a mid-range Linux laptop. If a target is missed,
the miss is a bug, not a footnote.

Measured 2026-10-24 on this VM: warm no-op rebuild ~0.2 s (probe cache;
was ~3.4 s before caching), cold hello-world ~19 s (still above the
<10 s target — JVM startup in kotlinc/d8/apksigner dominates).

## 9. Implementation requirements (C)

- Language: C17, single codebase, no C++.
- Style (matches the user's established C conventions): 2-space indent,
  K&R braces, `//` comments, strict warnings (`-Wall -Wextra -Werror`
  in CI).
- Dependencies: libc only. The TOML parser, SHA-256, ZIP reader/writer,
  and JSON writer are implemented in-tree (small, single-file style
  modules are acceptable; vendored third-party code must be licensed
  compatibly and credited in the source).
- Process management: `posix_spawn` (or `fork`/`exec` with careful fd
  hygiene). No `system()` calls. Every spawned tool's argv, cwd, and
  environment must be constructible in `--dry-run` output.
- No threads in v1. The pipeline is sequential; parallelism is a v2
  optimization, not a v1 requirement.
- Test strategy:
  - Unit tests for the TOML parser, path resolution, hashing, ZIP
    read/write, and command construction (a small C test harness, no
    external framework).
  - Integration tests that build a sample app (Kotlin Activity + res +
    assets + one native lib) and verify the APK with `aapt2 dump
    badging` and `apksigner verify`.
  - A `ccb check`-style self-test in CI: `ccb doctor` against a
    container with a known SDK.
- Repository layout (proposed):
  - `src/` — CCB sources (`main.c`, `toml.c`, `build.c`, `zip.c`,
    `hash.c`, `json.c`, `doctor.c`, `cmd_*.c`).
  - `tests/` — unit + integration tests.
  - `examples/hello/` — minimal sample project built by CI.
  - `docs/` — user guide, TOML reference, JSON protocol reference.
  - `CMakeLists.txt` — CMake-only build, matching the user's convention.

### 9.1 Module map (highly modular)

The codebase is split into small modules with one responsibility each.
A module is one `.c` file plus one `.h` file, and its header documents
every pointer it returns: who owns it, who frees it, and when.

| Module | Responsibility |
|---|---|
| `ccb_toml` | Parse and validate `ccb.toml`; schema errors with key names. |
| `ccb_fs` | Path resolution, directory walking, file read/write. |
| `ccb_hash` | SHA-256 / SHA-1 and file hashing for the cache. |
| `ccb_net` | curl-based downloads and the shared cache root (used by setup and Maven). |
| `ccb_maven` | Maven coordinate parsing, POM subset parser, breadth-first transitive resolution, checksum-verified cache (§5.4). |
| `ccb_zip` | APK ZIP reader/writer (sorted entries, alignment-aware). |
| `ccb_proc` | Spawn tools, capture output, render commands for `--dry-run`. |
| `ccb_sdk` | SDK/NDK/kotlinc discovery and version checks. |
| `ccb_diag` | The error type: phase, file/line/col, command, suggestion. |
| `ccb_json` | JSONL event writer for `--json` output. |
| `ccb_cache` | Cache manifest and invalidation rules. |
| `ccb_pipeline` | Stage orchestration. Each stage is a function with explicit inputs and outputs. |
| `ccb_manifest` | Android manifest subset parser + AAR manifest merger (§5.2). |
| `ccb_preview` | CSS parser, box layout, Kotlin structure parser, terminal renderer, logic tree (see §10). |
| `ccb_cli` | Argument parsing and command dispatch. |

Dependency rules (enforced by code review and CI include checks):

- Dependencies flow downward only: `cli` → `pipeline` → `sdk`/`proc`/
  `zip`/etc. No module imports `pipeline` or `cli`.
- No circular imports, ever. A cycle is a design error, not a build
  problem to work around.
- Every module has its own tests. A module without tests is unfinished.

### 9.2 Code style (best practices, enforced)

- C17, 2-space indent, K&R braces, `//` comments, strict warnings
  (`-Wall -Wextra -Werror -Wpedantic` in CI).
- One module = one `.c` + one `.h`. Headers document purpose,
  ownership, and error contracts; implementations do not leak internals
  through headers.
- No global mutable state. The logger is the single exception, and it is
  append-only.
- Every fallible function returns a `ccb_status`. Callers check it.
  Constructors carry `warn_unused_result`. Ignored errors are bugs.
- Public symbols are named `ccb_<module>_<verb>`. Static helpers are
  unprefixed.
- Functions do one thing. Guideline: under ~100 lines, and no function
  with more than ~10 branches without a comment justifying it.
- No dead code, no commented-out code in the tree (CI grep fails on it).

### 9.3 Safety wrappers (no leaks, no classic C bugs)

Raw `malloc`/`free`, unchecked string functions, and bare pointer
arithmetic are banned outside the wrappers below. CI enforces the ban
list (`strcpy`, `strcat`, `sprintf`, `gets`, and friends) with grep.

- **Checked allocation**: `xmalloc`/`xcalloc`/`xrealloc`/`xstrdup` abort
  with a file:line diagnostic on out-of-memory. Fail-fast beats a
  half-built APK: recovery mid-build is meaningless, so a loud death
  is the correct behavior.
- **Arena allocator** (`ccb_arena`): all per-build transient allocations
  come from arenas that are freed in a single call at stage/build end.
  This removes the largest leak class by construction. Long-lived data
  uses explicit ownership documented in headers.
- **Growable strings** (`ccb_str`): always NUL-terminated, checked
  appends, no fixed-size buffers for paths or tool output.
- **Slices** (`ccb_slice`: pointer + length): all parsers (TOML, ZIP,
  CSS, Kotlin structure) work on slices, never rely on NUL termination,
  and never read past the slice end.
- **Checked arithmetic**: `__builtin_add_overflow` /
  `__builtin_mul_overflow` for all size and offset math. Mandatory in
  ZIP parsing, the classic overflow vector.
- **Scope cleanup**: `__attribute__((cleanup))` for files, directories,
  arenas, and child processes, so early returns cannot leak.
- **Sanitizers and fuzzing**: ASan + UBSan + LSan on every test run in
  CI; a leaking test fails. Fuzz harnesses exist for the TOML, ZIP,
  CSS, and Kotlin-structure parsers.

## 10. CCB Preview — terminal UI preview

### 10.1 The idea

For projects that follow the **CCB UI convention** — UI styling written
in CSS, app flow logic written in Kotlin in a designated folder — CCB
can show what the app will roughly look like, in the terminal, without
a device, emulator, or Android runtime:

- It reads the CSS and the Kotlin flow sources.
- It builds a **box tree** (where each element goes on a phone screen)
  and a **logic tree** (screens, states, and transitions between them).
- It draws each screen as terminal wireframes (Unicode borders, ANSI
  colors approximating the CSS colors, text clipped to its box) inside
  a phone frame, and prints the logic tree beside it.
- It reports structural problems: overlapping boxes, off-screen
  elements, navigation targets that do not exist, unreachable screens,
  empty screens.

The point is to give an agent a cheap "does this look right?" check
before device testing. It is a linter with a picture, not a simulator.

### 10.2 Input convention

Declared in TOML:

```toml
[preview]
styles = "app/src/main/styles"   # CSS files
flow = "app/src/main/flow"       # Kotlin flow sources
phone_width = 390
phone_height = 844
strict = false
```

The full convention — screen and component declarations, the one-CSS-
file-per-screen rule plus `common.css`, the selector set, the layout
model in terminal columns, and the exit codes — is frozen in
`docs/preview.md`. The terminal frame is `--width` columns (default
40, minimum 24; smaller values clamp with a warning). `strict = true`
(or `--strict`) turns subset warnings into errors. Phase 2 needs no
new `[preview]` keys: when `flow` is absent, preview derives
screens, edges, and the wireframe from the app's own Kotlin
Activities and layout XML instead (see `docs/preview.md`, "Real code
(Phase 2)").

Projects that do not follow the convention get a clear
"preview not applicable to this project" message, not a crash and not
a hallucinated render. Android XML layouts are understood by preview
(Phase 2: layout buttons and `android:onClick`, and the
layout-derived wireframe when there is no flow sketch); Compose
code is NOT understood (see open question 8).

### 10.3 CSS: a subset, parsed properly

Preview parses real CSS, but only a documented subset:

- Supported: `display` (block, flex), `flex-direction` (row, column),
  `width`/`height` (px, %), `padding`, `margin` (px), `background-color`,
  `color` (hex, named, or an `@color/name` reference), `font-size` (px),
  `position` (static, absolute subset), `gap`, `text-align` (left,
  center, right), `border-width` (px), `border-color` (hex, named),
  `flex-wrap` (nowrap, wrap; row containers only).
- Deliberately NOT supported: `justify-content` / `align-items`
  (distribution semantics the sum-based model cannot express),
  `border-radius` (no visible rounding at 1-cell scale),
  `line-height` (text is single-row by design), the `border`
  shorthand.
- Anything outside the subset produces a **warning** naming the
  property and its location, and rendering continues with a fallback.
  Unknown properties never fail a preview; silently ignoring them
  would be worse than warning.
- `--strict` turns subset warnings into errors.

### 10.4 Kotlin: structure analysis, not interpretation

This is the key scoping decision: preview does **not** interpret or
execute Kotlin. It statically parses structure:

- Screen and component declarations, component trees per screen.
- Navigation calls (`nav("Home")`, `navigateTo(...)`) → edges in the
  logic tree. Arguments after the target are recorded on the edge
  (string literals literally, `name = value` pairs as written,
  anything else as `unknown`) and shown in the tree, walk mode's
  edge list, and JSON (`"args"` array; JSON `"version":2`).
- Slice 1 click handlers (implemented 2026-10-24): `setOnClickListener`
  lambdas (trailing and parenthesized), `onClick = { ... }`
  assignments, and direct `::handler` references inside screen fun
  bodies also yield edges — derived, not declared. Binding is an
  explicit `[preview.bindings]` table (code identifier → component
  label); an unbound handler warns (CCB-P1019) and yields no edge.
  `startActivity(Intent(this, X::class.java))` yields the class
  simple name as the target. A nav nested in any non-branch brace
  block inside a handler yields no edge (branch bodies are Slice 2,
  below); a computed destination
  yields an unknown edge; a declared edge for the same button always
  wins, with a CCB-P1020 warning on disagreement. See
  `docs/preview.md` (click handlers) for the exact outcome table
  and scope limits. Slice 2 (conditional nav → branch edges,
  implemented 2026-10-24: `if`/`when` branch bodies in handlers,
  braced or braceless, yield edges annotated with the branch label,
  e.g. `if ready: then`; literal arguments on handler nav calls
  yield edges with args) and Slice 3 (XML onClick, NavController,
  implemented 2026-10-24: layout `android:onClick` buttons resolve
  via `[preview.bindings]` view-id keys to handler funs, whose
  edges attach to screens containing the bound label — unbound is
  CCB-P1019, label in no screen is CCB-P1021; `navigate(R.id.x)`
  resolves through a nav-graph file — unlisted id or no graph file
  is an unknown edge; layout/graph locations: explicit `[preview]`
  `layout`/`nav_graph` keys, else `<resources>/layout` /
  `<resources>/navigation`, else `<paths res>/layout` /
  `<paths res>/navigation`) are implemented; see `docs/preview.md`
  for the exact shapes, locations, and outcome tables.
- `when`/`if` branches over state variables → branches in the logic
  tree, labeled with their conditions, **not evaluated**. A branch is
  shown, not resolved.
- Dynamic data (network, database, sensors) becomes a placeholder node.

Diagnostics from this analysis: navigation to a screen that does not
exist is an error; a screen nothing navigates to is a warning; a
screen with no components is a warning.

### 10.5 Output

- Terminal render per screen (default): phone frame, boxes, colors,
  clipped text, then the logic tree, then diagnostics.
- `--json`: the box tree, logic tree, and diagnostics as structured
  data for agents (JSON `"version":2`; edge `"args"` arrays; every
  box carries `"w_dp"`/`"h_dp"` alongside its cell size).
- `--screen NAME`: render one screen only.
- `--storyboard "A,B,C"`: render a journey of screens in order
  (repeats allowed), each frame preceded by a `Journey N/M: <Name>`
  header; `--json` keeps only the journey screens, in order.
  Conflicts with `--screen`, `--walk`, `--watch`, `--diff`,
  `--update-goldens` (exit 2).
- `[preview] resources = "res"`: optional directory of `strings.xml`
  / `colors.xml`; whole-label `@string/name` and `color` /
  `background-color` `@color/name` values resolve at load time.
  Unresolved references warn (CCB-P1016) and keep the literal.
- Lint warnings (always warnings, even under `--strict`, never
  build-gate failures): CCB-P1017 tap target too small (`button` /
  `textfield` under 8 columns or 2 rows, wireframe cells),
  CCB-P1018 text contrast below 4.5:1 (3.0:1 at `font-size` ≥ 24px)
  by WCAG relative luminance.
- Derived handler edges (Slices 1–3, implemented 2026-10-24) render
  exactly like declared edges in every output mode (tree, walk,
  storyboard, diff, JSON — no `derived` marker in output), and the
  post-load reconciliation (declared beats derived, CCB-P1020 on
  disagreement, CCB-P1019 for unbound handlers, CCB-P1021 for an
  onClick label in no screen) runs before layout
  and diagnosis, so every consumer sees the reconciled edges.
  CCB-P1019/CCB-P1020/CCB-P1021 are warnings always, never gate failures.
  Branch edges (Slice 2) add the tree/walk suffix ` [branch]` and
  the JSON per-edge `"branch"` field (string, or `null` when
  unconditional); JSON stays `"version":2` (both edge fields are
  additive).
- `--walk [screen]`: interactive static exploration of the logic
  tree (arrow keys/`j`/`k`, Enter, `b`, `t` branch mode, `q`); see
  `docs/preview.md` Walk mode. Still static only — never execution
  or evaluation.
- `--watch [screen]`: render once, then re-render on every change to
  the styles/flow files or `ccb.toml` (~200ms debounce; TTY clear,
  piped delimiter, `--json` JSONL, `--output` rewrite); Ctrl-C /
  SIGTERM exits 0; a broken save prints diagnostics and keeps
  watching. See `docs/preview.md` Watch mode. Conflicts with
  `--update-goldens` and `--walk` (exit 2).
- `--output FILE`, `--device phone|tablet`, `--update-goldens`:
  write the render to a file, frame-width presets, regenerate the
  checked-in goldens. `--json` carries an additive `"version":2`
  field.
- `--diff DIR`: compare the current renders against a snapshot
  directory in the golden-file format; exit 0 identical, 1 with a
  unified diff per differing file, 2 for usage/config errors
  (unreadable snapshot dir, no usable [preview] section, unknown
  --screen). `--screen` scopes to one screen; `--json` emits a
  machine-readable diff object. Conflicts with --watch, --walk,
  --update-goldens (exit 2).
- Build gate (opt-in, off by default): `[preview] gate = true` or
  `check`/`build --preview-gate` (`--no-preview-gate` forces off;
  explicit flag beats config) runs preview diagnostics as part of
  `check` (after toolchain validation) and `build` (before any
  pipeline stage). Any preview ERROR fails the gate (exit 1, one
  concise line per failing diagnostic on stderr); warnings alone
  pass; preview-not-applicable projects pass silently. See
  `docs/preview.md` Build gate.
- Exit codes reuse §6.4 (2 for preview config errors, 1 for preview
  failures such as an unresolvable navigation target).

### 10.6 Explicit non-capabilities

Preview is NOT: an emulator, a pixel-perfect render, an animation
player, an interaction simulator, a Kotlin interpreter, or a source
of real data. Fonts are the terminal's fonts. System UI, themes, and
Android framework behavior are not modeled. An interactive walk mode
(arrow keys through the logic tree) was a v2 candidate; it is
implemented (see `docs/preview.md` Walk mode) — still static
exploration only, never execution or evaluation.

### 10.7 Feasibility: yes, scoped this way

A full-fidelity version — a complete CSS engine plus a Kotlin
interpreter plus the Android framework — would be reimplementing a
browser and a runtime, and is NOT feasible or in scope. The scoped
version above is feasible because it is three tractable pieces: a CSS
subset parser, a box layout pass, and a Kotlin structure parser, plus
a terminal renderer. That is the same size class as a linter, and
existing tools prove each piece separately (CSS parsers, static
analyzers, terminal UI libraries all exist).

Effort is real, not two minutes: roughly CSS + layout 40%, Kotlin
structure parsing 30%, renderer + tree + diagnostics 30%. Phase it:
P0 CSS subset + static boxes; P1 Kotlin structure → logic tree;
P2 diagnostics; P3 (v2) interactive walk mode. Edge cases are handled
by the subset + warnings + placeholder/unknown nodes: preview never
crashes on input it does not understand, and never silently guesses.

## 11. Security considerations

- Keystore passwords come only from environment variables or prompts,
  never from TOML, never from the command line (which leaks to process
  listings), never in logs.
- The auto-generated debug keystore uses the standard Android debug
  credentials and is gitignored by `ccb init`.
- CCB never executes anything from the project directory (no hooks, no
  scripts, no "prebuild" commands) in v1. A project that needs custom
  steps is out of scope rather than a code-execution vector.
- Downloaded-tool integrity: CCB does not download tools. If a future
  version adds `ccb doctor --install`, it must verify checksums against
  a pinned list.

## 12. Milestones and acceptance criteria

### M0 — Skeleton (the walking frame)

- `ccb init`, `ccb setup`, `ccb check`, `ccb doctor`, `ccb version` work.
- TOML parser complete with unknown-key errors and full error listing.
- Acceptance: `ccb doctor` on a machine with a real SDK prints a clean
  bill of health; on a machine without one, every missing item has an
  install command.

### M1 — First real APK

- Full pipeline for a Kotlin hello-world app (manifest + res + one
  Activity), debug-signed, zipaligned, `apksigner verify` clean.
- `ccb build`, `ccb clean`, `--json`, `--dry-run` work.
- Acceptance: the APK installs and launches on a device/emulator via
  `ccb install` + `ccb run`.

### M2 — Incremental builds

- Cache manifest, per-stage skip, `--explain`, cache invalidation on
  tool upgrade.
- Acceptance: the performance targets in §8 hold for the sample app.

### M3 — Native libs and AARs

- NDK compilation per ABI, AAR unpacking (classes/res/assets/jni),
  duplicate-class errors.
- Acceptance: a sample app with one native method and one AAR builds,
  installs, and calls the native method correctly.

### M4 — Release readiness and agent polish

- Release signing (fail-fast on missing credentials), r8 minification
  path, `ccb logcat`, full JSON protocol documentation, reproducible
  build verification (two builds, identical bytes).
- Acceptance: a release APK built twice from the same inputs is
  byte-identical; the JSON protocol doc is complete enough for an agent
  to drive CCB without reading its source.

### M5 — Preview

- CSS subset parser + box layout + terminal render for a sample
  project; logic tree from Kotlin flow sources; diagnostics for missing
  navigation targets and off-screen boxes.
- Acceptance: the sample project renders recognizably in a terminal;
  the logic tree matches the hand-written expectation; an unresolvable
  navigation target is reported as an error; an unsupported CSS
  property warns and renders with a fallback instead of crashing.

## 13. Open questions (for the user to decide)

1. **AAB support?** Implemented: `ccb build --aab` produces a
   base-module-only `.aab` (see §5.10); bundletool-based split APK
   generation stays out of scope.
2. **Remote dependencies?** Implemented: a minimal Maven resolver
   (Google Maven + opt-in Central, exact versions, POM packaging,
   transitive nearest-wins, checksum-verified cache, setup-time
   prefetch, cache-only builds) — see §5.4 and `docs/maven.md`.
   Follow-ups implemented: dependency exclusions (`maven_exclude`)
   and a `ccb.lock` lockfile (setup writes, check/build enforce);
   kotlinc compiler plugins (`[build] kotlin_plugins`, exact versions,
   no transitive walk); version ranges, `LATEST`/`RELEASE`, and
   `-SNAPSHOT` handling; parent-POM `<dependencies>` and profile
   inheritance; profile support (by id or `<activeByDefault>`);
   authenticated/private repositories (`[deps] maven_repos`).
3. **Compose?** Implemented: the Compose compiler plugin path —
   `[build] kotlin_plugins` with
   `org.jetbrains.kotlin:kotlin-compose-compiler-plugin:<kotlin-version>`;
   a minimal Compose sample builds with `ccb build` — see §5.3 and
   `docs/maven.md`.
4. **Manifest merging?** Implemented: the AAR manifest merge subset
   (components, permissions, features; app-wins conflicts) — see §5.2.
5. **Windows?** v2 platform, or Linux/macOS only by design?
6. **Multi-module?** Implemented: library modules via
   `[modules.<name>]` tables (standard `src/main` layout,
   topological build into intermediate AARs consumed through the
   existing deps + manifest-merge machinery) — see §4.3, §5.11, and
   `docs/modules.md`. Dynamic feature modules stay out of scope.
7. **Name expansion.** "Custom C Builder" is a placeholder. Keep CCB as
   a bare name, or pick a real expansion?
8. **Preview scope.** Implemented (Phase 2): preview derives
   screens, edges, and the wireframe from Android XML layouts and
   the app's own Kotlin Activities, so a project needs no flow
   sketch. Compose code in v2, or keep it CSS-convention-only?
   Interactive walk mode through the logic tree in v2?
9. **CCB's own license.** Pick before publishing: permissive, copyleft,
   or source-available (as with melod)?

## 14. Glossary

- **aapt2** — Android Asset Packaging Tool: compiles and links resources
  and the manifest.
- **d8 / r8** — Dex compilers: convert JVM bytecode to Android dex
  format (r8 also shrinks and optimizes).
- **zipalign** — Aligns APK entries for efficient runtime loading.
- **apksigner** — Signs and verifies APKs (v1/v2/v3 signature schemes).
- **AAR** — Android library archive: classes, resources, manifest,
  assets, and native libs in one file.
- **ABI** — Application Binary Interface: `arm64-v8a`, `armeabi-v7a`,
  `x86_64`, etc. One `.so` per ABI in the APK.
- **Arena allocator** — An allocator that hands out memory from a pool
  and frees the entire pool in one call, removing per-allocation leaks
  for transient data.
- **Slice** — A pointer + length pair used for parsing, so parsers never
  rely on NUL termination and never read past the input end.
- **Box tree** — The computed layout of a screen: each element's box
  (position, size) on a phone-sized canvas, derived from the component
  tree plus CSS rules.
- **Logic tree** — The app flow as static structure: screens, their
  states, and transitions between them, with branch conditions labeled
  but not evaluated.
- **CSS subset** — The documented portion of CSS that preview parses.
  Properties outside it warn and fall back; they never crash a preview.

## 15. Pre-start decisions (approved by the user 2026-10-24)

Items 1–6 below were approved and folded into the normative sections
above (config schema, commands, JSON protocol, build layout, Stage 0,
signing). Item 7 remains open: the user has not yet picked a license.

1. Schema version + config discovery → §4.
2. Stable diagnostic codes → §6.2.
3. Debug keystore location → §5.8.
4. Build directory layout → §6.6.
5. Compatibility matrix → §5.1.
6. `ccb setup` command → §6.1, M0.
7. CCB's own license → OPEN (see open question 9). No license file is
   added and no license is chosen on the user's behalf until they pick
   one.
