# CCB — Requirement Document

> Status: SPEC — 2026-11-27. Rewritten from scratch. This document defines
> what CCB must do. It is a requirements spec, not a design doc and not
> code. Normative rules are stated as rules. `docs/` holds the user guide;
> where this spec and a doc disagree, this spec wins.

## 1. What CCB is

**CCB** is a build system for Android apps. Headless-first: one static
binary, no daemon by default, no GUI, no background processes unless the
user opts in. It turns an Android app project into a signed, installable
APK (or an AAB with `--aab`) from a single TOML file, without Gradle and
without Android Studio.

Three design commitments, in order:

1. **Speed.** Cold builds, caching, no wasted work. A build that can be
   skipped is skipped. A tool that can be probed once is probed once.
2. **LLM-optimized.** Structured docs, JSON protocol, deterministic
   explainable output. Every diagnostic names a file, a line, and a
   rule. An agent must be able to run a build, understand what happened,
   and fix what broke, without guessing.
3. **Human-optimized.** TOML config, plain-language errors, a README a
   stranger can follow. No hidden state: everything CCB needs lives in
   `ccb.toml` or in environment variables.

### 1.1 Scope line

CCB **is**:

- A build system for Android apps: Kotlin first, Java second, C/C++
  native libs via the NDK.
- A fixed, ordered pipeline (stages 0–14, §5) with per-stage caching.
- A Maven resolver for remote AAR/JAR dependencies (§5.4).
- A library builder: in-tree library modules and standalone library
  projects (§4.3, §5.12).
- A terminal UI preview for projects that follow the preview convention
  (§10). A linter with a picture, not a simulator.

CCB **is not**:

- A Gradle replacement for every workflow. One app module; library
  modules are supported; dynamic feature modules are out of scope.
- A plugin ecosystem. There is no plugin API and no plugin registry.
  The one exception is kotlinc compiler plugins declared in TOML
  (`[build] kotlin_plugins`, §5.3) — resolved via Maven, exact versions
  only, no transitive walk.
- A build scripting language. TOML is the whole configuration surface.
  No hooks, no scripts, no prebuild commands. CCB never executes
  anything from the project directory.
- A dependency manager beyond Maven coordinates in TOML. No Gradle
  daemon, no build scripts, no implicit downloads during a build.
- An annotation processor host. kapt/KSP apps (Dagger, Hilt, Room
  codegen) are out of scope; CCB fails with a clear message if it
  detects them, not a cryptic compiler error.
- iOS or Windows. Linux first, macOS second.

Rule: if a feature cannot be expressed in a few TOML lines and cannot
build fast, it does not go in.

### 1.2 "Headless" means

- No Android Studio, no Gradle daemon, no GUI, no background processes
  by default. One binary, one command, one result.
- It does **not** mean "no JVM". The Kotlin compiler (`kotlinc`) and the
  dexer (`d8`) are JVM tools and CCB orchestrates them. CCB itself is
  native C and starts instantly; the JVM only runs for the steps that
  genuinely need it.
- The opt-in kotlinc daemon (§5.13) is the only background process CCB
  ever starts. It is per-project, auto-shuts down after an idle timeout,
  and never outlives its project.

## 2. Goals

1. **Build a real APK from a TOML file.** Manifest, resources,
   Kotlin/Java sources, assets, native libs → a signed, zipaligned,
   installable APK with one command.
2. **Be fast.** Cold build of a hello-world app in under 10 seconds.
   Incremental rebuild (one source file changed) in under 2 seconds.
   See §8 for the targets and the measured status.
3. **Be agent-readable.** Structured machine output, stable exit codes,
   actionable errors, dry-run planning, cache explanations (§6).
4. **Be hermetic by default.** No network access during a build. No
   implicit downloads. A build either works offline or fails loudly
   saying exactly which tool is missing. `ccb setup` is the only
   command allowed network access.
5. **Be small and dependency-free.** One static binary, under 5 MB,
   linking only libc. No runtime dependencies beyond the Android SDK
   toolchain it drives.
6. **Be deterministic.** Same inputs + same tool versions = byte-identical
   APK (sorted zip entries, fixed timestamps).
7. **Support the user's real stack.** Kotlin first, Java second, C/C++
   native libs via the NDK.
8. **Modular, memory-safe C.** Small modules with clear boundaries,
   explicit ownership, safety wrappers that make classic C bugs hard
   to write and easy to catch (§9).
9. **Preview without a device.** For projects that follow the CCB UI
   convention (CSS styling + Kotlin flow sources), CCB renders an
   approximation of each screen in the terminal plus a logic tree of
   the app flow (§10). An approximation tool, not a simulator.

## 3. Non-goals

- Large multi-module Gradle-style builds. One app module; library
  modules supported (§4.3). Dynamic feature modules out of scope.
- AAB split APKs, asset packs, Play signing key upload. `ccb build
  --aab` produces a base-module-only bundle (§5.11).
- kapt/KSP annotation processing (see §1.1).
- iOS, Windows.
- A plugin ecosystem or a build scripting language (see §1.1).
- Maven publishing of library projects. `ccb` builds an `.aar` file;
  publishing it is out of scope.
- Per-module deps/NDK/signing. Modules and library projects use the
  project's jars/AARs on the classpath and have no native libs.
- Compose code in preview. Compose apps build (§5.3); preview does not
  understand Compose code.

## 4. Project configuration: `ccb.toml`

One file per project, named `ccb.toml` by default, overridable with
`--config`. Everything CCB needs lives here or in environment variables,
never in hidden state.

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
# kotlinc_daemon = false
# kotlinc_daemon_idle_seconds = 300
# kotlin_plugins = ["org.jetbrains.kotlin:kotlin-compose-compiler-plugin:2.4.21"]
# kotlin_plugin_options = ["androidx.compose.compiler.plugins.kotlin:generateFunctionKeyMetaClasses=true"]
# proguard_rules = "app/proguard-rules.pro"   # required when minify = true

[deps]
jars = []   # paths to .jar files, relative to the project root
aars = []   # paths to .aar files, relative to the project root
# maven = ["androidx.core:core-ktx:1.13.1"]
# maven_central = true
# maven_repos = ["https://repo.example.com/maven2"]
# maven_profiles = []
# maven_exclude = []

[ndk]
abis = ["arm64-v8a"]
cpp_flags = ["-O2"]
c_sources = ["app/src/main/cpp/native.c"]

[preview]
styles = "app/src/main/styles"
flow = "app/src/main/flow"
strict = false
gate = false
```

### 4.2 Configuration rules

- All paths are relative to the directory containing `ccb.toml`, unless
  absolute.
- Config discovery: with no `--config` flag, CCB searches upward from
  the current directory to the filesystem root for `ccb.toml` (like git
  searches for `.git`). `--config` with an explicit path overrides this.
- `ccb_version` is mandatory. A missing version is a configuration
  error (CCB-E1002); a version newer than the binary understands is
  also an error (CCB-E1003), naming the supported range. Only version
  1 is supported.
- Unknown keys are a **hard error** (CCB-E1001) with the key name and
  the list of valid keys. Silent typos in config are the #1 agent
  time-waster; CCB must not tolerate them.
- Secrets (keystore passwords) are **never** written in TOML. They come
  from environment variables named in TOML, or from an interactive
  prompt. CCB must never print a password, even in `--verbose` or
  `--dry-run` output. Passwords never appear on a command line (which
  leaks to process listings); release signing passes them to apksigner
  as `env:VAR` references only.
- `ccb.toml` must be validatable without building: `ccb check` parses,
  resolves paths, and reports every problem at once (not one error per
  run).
- Tool overrides: `[sdk] aapt2` / `[sdk] zipalign` and the env vars
  `CCB_AAPT2` / `CCB_ZIPALIGN` select a different aapt2 or zipalign
  binary for every invocation (compile, link, badging, zipalign,
  doctor/check reporting, dry-run output). Precedence, highest first:
  `[sdk]` key, env var, default SDK build-tools path. An empty env
  value counts as unset. A relative override path is used exactly as
  written — relative to the working directory at invocation time, not
  to the project root or SDK home. A set override that does not exist
  or is not executable fails Stage 0 with CCB-E2009 naming the source
  and the path; there is no silent fallback to the default.
  `ccb doctor` and `ccb check` report the effective path and its
  source (`config`, `env`, or `default`) for both tools, and the
  effective paths participate in the cache fingerprints, so switching
  tools invalidates the affected stages.

### 4.3 Library modules and standalone libraries

A project may declare library modules as `[modules.<name>]` tables
(full rules in `docs/modules.md`):

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
- `depends` (optional, default `[]`): other module names. Modules
  build in topological order, dependencies first; a cycle is a hard
  error naming the cycle (`CCB-E1010`), an unknown dependency is a
  hard error naming it (`CCB-E1011`).
- Each module builds into an intermediate AAR consumed through the
  existing dependency machinery (§5.2, §5.4): manifest merges, res
  merges at link, classes dex, assets package. The app module is the
  only APK/AAB output.
- A module manifest must declare a package different from the app
  package and from every other module's package (CCB-E1005).
- A module name must not collide with a declared AAR's base name
  (`CCB-E3022`, checked at build time after Maven resolution).
- Library R fields are non-final (the module link generates R.java,
  CCB de-finalizes it, the app link assigns final IDs), so library
  code may reference its own and its dependencies' R classes.
- Module outputs participate in app fingerprints: changing a library
  rebuilds the app (see §7).
- A project can itself be a library: an empty `[library]` table makes
  `ccb build` produce `build/<name>-<version>.aar` (`classes.jar`,
  `AndroidManifest.xml`, `res/`, `assets/`, `R.txt`) instead of an
  APK. Any key under `[library]` is a hard error (CCB-E1001). A
  library project declares no modules, NDK, deps, or path libraries
  of its own (CCB-E1005), and cannot build an AAB (CCB-E1005).
- An app consumes a standalone library from a local path as
  `[deps.library.<name>]` with `path = "..."` (a library project
  dir, built first, or a built `.aar` file), or from Maven
  coordinates: an AAR dependency resolved via Maven works like a
  local AAR. Library R symbols are namespaced by package
  (`--extra-packages`); resource conflicts are won by the app, with
  a warning naming the winner (see `docs/library.md`).

### 4.4 Config key reference

Every key CCB accepts. Unknown keys are CCB-E1001.

| Table | Keys |
|---|---|
| (top level) | `ccb_version` |
| `[project]` | `name`, `package`, `version`, `version_code`, `min_sdk`, `target_sdk`, `compile_sdk` |
| `[paths]` | `manifest`, `res`, `assets`, `kotlin`, `java`, `libs` |
| `[sdk]` | `home`, `build_tools`, `ndk`, `kotlin_home`, `aapt2`, `zipalign`, `r8_jar` |
| `[signing.debug]`, `[signing.release]` | `store`, `alias`, `store_password_env`, `key_password_env` |
| `[build]` | `variant`, `minify`, `shrink_resources`, `jvm_target`, `kotlinc_daemon`, `kotlinc_daemon_idle_seconds`, `kotlin_plugins`, `kotlin_plugin_options`, `proguard_rules` |
| `[deps]` | `jars`, `aars`, `maven`, `maven_central`, `maven_repos`, `maven_profiles`, `maven_exclude` |
| `[deps.library.<name>]` | `path` |
| `[ndk]` | `abis`, `cpp_flags`, `c_sources` |
| `[modules.<name>]` | `dir`, `depends` |
| `[library]` | (empty table; any key is CCB-E1001) |
| `[preview]` | `styles`, `flow`, `resources`, `layout`, `nav_graph`, `phone_width`, `phone_height`, `strict`, `gate` |
| `[preview.bindings]` | `<identifier> = "<component label>"` (code identifier → component label) |

Environment variables CCB reads: `ANDROID_HOME`, `ANDROID_SDK_ROOT`,
`KOTLIN_HOME`, `CCB_AAPT2`, `CCB_ZIPALIGN`, `CCB_MAVEN_CACHE`,
`XDG_CACHE_HOME`, `SOURCE_DATE_EPOCH`, `NO_COLOR`. Test/support
overrides: `CCB_MAVEN_REPOS` (replaces configured repos entirely;
tests only), `CCB_GOLDEN_DIR` (preview golden output dir), `CCB_R8_SHA256`
(pinned r8 checksum override, see `docs/setup-tools.md`).

## 5. Build pipeline

CCB executes a fixed, ordered pipeline. Each stage is independently
skippable by the cache (§7) and independently reportable in JSON output
(§6). Stages, in order:

| # | Stage | What it does |
|---|---|---|
| 0 | Load | Load + validate `ccb.toml`, report every problem at once. Resolve the toolchain. Release builds verify signing config before compiling anything. |
| 1 | deps | Extract AARs (classes, resources, assets, JNI). Resolve Maven coordinates cache-only (§5.4). |
| 2 | libraries | Build each `[deps.library.<name>]` path library into `build/intermediates/libraries/<name>/<name>.aar`. A built `.aar` path joins the declared AARs in stage 0 instead. Skipped with no path libraries. |
| 3 | modules | Build each `[modules.<name>]` library module into `build/intermediates/modules/<name>/<name>.aar`, in topological order. Skipped with no modules. |
| 4 | merge-manifest | Merge AAR manifests into the app manifest, in config order. Output: `build/intermediates/merged-manifest/`. Skipped with no AARs. |
| 5 | compile-res | `aapt2 compile` each resource directory into a `.flat` zip. |
| 6 | link | `aapt2 link` into `linked.ap_`, generating `R.java`. Uses the merged manifest when AARs are present, else the app manifest. The manifest's `package` must match `[project] package`. Library zips link as `-R` overlays with the app zip last, so the app wins resource conflicts; a resource defined by more than one source warns, naming the winner. |
| 7 | compile-code | `kotlinc` (Kotlin + `R.java` + Java sources), then `javac` for Java sources and generated `R.java`. Classpath: `android.jar` + `deps.jars` + AAR `classes.jar` files. |
| 8 | dex | `d8` (or `r8` for release + minify). See §5.6. |
| 9 | dexcheck | Parse the dex files, verify every referenced class is defined in the dex. See §5.7. |
| 10 | native | NDK clang per ABI (only with an `[ndk]` section). |
| 11 | package | CCB's own ZIP writer: `linked.ap_` entries, dex files, assets (app assets win over AAR assets with the same name), `lib/<abi>/*.so` (duplicate `.so` names are an error, CCB-E3007). |
| 12 | align | `zipalign -f 4`. |
| 13 | sign | `apksigner`. Debug uses `~/.android/debug.keystore` (created with keytool when missing). Release uses `env:VAR` password references; passwords never appear in commands or logs. |
| 14 | verify | `apksigner verify` and `aapt2 dump badging`. An APK counts as built only when both pass. |

A library project (`[library]` in `ccb.toml`) runs only stages 0–2
in the list above — deps, then its single module build into
`build/<name>-<version>.aar` — and stops: no merge, link, dex, or
package. See `docs/library.md`.

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

Supported ranges (pinned here; `ccb doctor` judges against them):
compileSdk 34–36, build-tools 35.x/36.x, kotlinc 2.x, JDK 17+. A tool
outside its range is a Stage 0 error naming the range and the found
version.

Kotlin compiler location: `$KOTLIN_HOME`, then `PATH`, then TOML
`[sdk].kotlin_home`. Missing kotlinc is a Stage 0 error (CCB-E2004),
not a Stage 7 surprise.

### 5.2 Stage 6 — Link (resources + manifest)

- Compile `res/` into flat files (stage 5), then link with the manifest
  into a base APK, generating `R.java` for the Kotlin/Java compile.
  The app link emits `R.java` only — no `R.txt`. `R.txt` is emitted by
  library/module links (§5.12), never by the app link.
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

### 5.3 Stage 7 — Compile code (`kotlinc` + `javac`)

- `kotlinc` over Kotlin sources with classpath = `android.jar` +
  `R.java` output + declared jars + extracted AAR `classes.jar`s.
- `javac` over Java sources with the same classpath (if any Java exists),
  plus the generated `R.java` (kotlinc resolves R symbols but emits no
  R.class files, so javac must compile them).
- `d8`/`r8` gets the kotlinc stdlib jar as a program input for Kotlin
  projects, unless the project already supplies it via `[deps]` or Maven.
- Compiler plugins: when `[build] kotlin_plugins` is set, kotlinc gets
  `-Xplugin=<jar>` per resolved plugin jar plus
  `-P plugin:<id>:<key>=<value>` per `[build] kotlin_plugin_options`
  entry (each option is `plugin-id:key=value`); plugin jars and options
  join the compile fingerprint. Plugin coordinates must be exact
  versions (ranges, `LATEST`, `RELEASE`, `-SNAPSHOT` rejected at config
  load with `CCB-E1005`). Plugin jars resolve via Maven the same way
  (§5.4) but without a transitive walk: a plugin jar must be
  self-contained. `pom` or `aar` packaging fails with `CCB-E3019`.
  Plugin jars are not in `ccb.lock`: the exact coordinate in `ccb.toml`
  pins them.
- The Compose compiler plugin path is
  `org.jetbrains.kotlin:kotlin-compose-compiler-plugin:<kotlin-version>`
  (the plain artifact, not the `-embeddable` variant, which is built
  for Gradle's embedded compiler — see `docs/maven.md`). A minimal
  Compose sample builds with `ccb build`.

### 5.4 Stage 1 — Dependencies (local files + remote Maven)

- JARs go straight onto the compile classpath and into the dex input.
- AARs are unzipped into the build dir: `classes.jar` to the classpath,
  `res/` merged into stage 5 input, `assets/` merged into stage 11,
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
  nearest-wins; excluding a direct dependency is a config error
  (CCB-E1005). `ccb setup` prefetches the cache and writes the
  resolved tree to `ccb.lock` in the project root; builds and
  `ccb check` resolve cache-only, fail with `CCB-E3021` on a miss,
  and fail with `CCB-E3023` on any deviation from a present lockfile.
  Full rules in `docs/maven.md`.

### 5.5 Stage 10 — Native libs (NDK, optional)

- If `[ndk]` is present, compile C/C++ sources with the NDK Clang
  toolchain once per ABI into `.so` files.
- `.so` files land in the APK at `lib/<abi>/`.
- NDK version is pinned in TOML; a mismatched installed NDK is a Stage 0
  error (CCB-E2008).

### 5.6 Stage 8 — Dex (`d8`, or `r8` for release)

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
- `d8` heap: CCB passes `-JXmx<N>m` to d8, N = 256 + 16 per MiB of dex
  program inputs, capped at 1024 MB. Heap never changes dex output, so
  it is not in the dex fingerprint. R8 is untouched: its 1G cap already
  equals the cap here.
- `.class` files pass to d8 individually, not as a directory: kotlinc
  also writes `META-INF/*.kotlin_module` files, and d8 rejects
  non-class files in directory inputs.

### 5.7 Stage 9 — Dexcheck

Immediately after dex, a dexcheck step parses the produced dex
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

### 5.8 Stages 11–14 — Package, align, sign, verify

- Package (stage 11): CCB assembles the final APK itself: it takes the
  `aapt2 link` output and appends dex files, assets, and native libs
  using its own ZIP reader/writer (no external `zip` binary
  dependency). APK entries are written in sorted order for
  determinism. ZIP writer uses fixed 1980-01-01 timestamps and
  deterministic entry order, so two clean builds of the same inputs
  produce byte-identical APKs.
- Align (stage 12): `zipalign -f 4`.
- Sign (stage 13): `zipalign` then `apksigner` (v2 signature minimum;
  v1+v2+v3 for release when configured). Debug builds: the debug
  keystore defaults to `~/.android/debug.keystore` (the Android
  standard location, so it survives `ccb clean`), overridable via
  `[signing.debug] store`. If it is missing, CCB generates it
  (standard Android debug credentials) and records that it did so.
  Release builds: missing keystore, missing alias, or missing password
  env var is a hard error before any compilation starts (fail fast, not
  after a 30-second build; CCB-E3001, exit 4).
- Verify (stage 14): after signing, CCB verifies with `apksigner
  verify` and `aapt2 dump badging` and reports the result. An APK is
  not "built" until both pass.

### 5.9 Report

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

### 5.11 Library modules (stages 2–3)

Between the deps step and the manifest merge, CCB builds each
declared library module (§4.3) in topological order into an
intermediate AAR (`build/intermediates/modules/<name>/<name>.aar`):
`classes.jar` (when the module has Kotlin/Java sources), the module
manifest, raw `res/` and `assets/` trees, and `R.txt`. Path libraries
(`[deps.library.<name>]`, §4.3) build first, into
`build/intermediates/libraries/<name>/<name>.aar`, through the same
machinery. The module AARs join the
declared AARs in the merge list (declared first, then modules in
topological order), so every later stage treats them as local AARs:
manifest merge (§5.2), res compile/link (§5.2), compile classpath
(§5.3), dex inputs (§5.6), assets (§5.8).

- Module resources compile (`aapt2 compile`) and link (`aapt2 link`
  against the module manifest) to generate R.java for the module
  package and R.txt, whenever the module has resources — even
  without code, so a resources-only module still ships a real R.txt.
  A module with no resources ships an empty R.txt. Generated R
  fields are de-finalized before compiling, so they resolve at
  runtime against the app's R class (see `docs/modules.md`).
- Library resources link as aapt2 `-R` overlays with the app zip
  last, so the app wins same-named resource conflicts (previously a
  hard link error); a resource defined by more than one source warns
  at link time, naming the winner and the shadowed sources. R
  symbols are namespaced by package (`--extra-packages`), so a
  library R class cannot collide with the app's.
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
  modules, Maven publishing, transitive path libraries (a library
  project cannot consume path libraries).

### 5.12 Library projects

A library project (`[library]`, §4.3) runs stages 0–2 only: deps,
then its single module build (resources, code, AAR), and stops. The
AAR (`build/<name>-<version>.aar`) contains `classes.jar` (compiled
Kotlin/Java except generated R classes), `AndroidManifest.xml` (copy
of the library manifest), raw `res/` and `assets/` trees, and `R.txt`
(the library link's symbol table; empty when the library has no
resources). Library R fields are non-final: the library link
generates R.java, CCB de-finalizes it, and the app link assigns final
IDs. See `docs/library.md`.

### 5.13 Kotlinc daemon (opt-in)

Default: plain `kotlinc` subprocess. Opt in with `[build]
kotlinc_daemon = true` or `ccb build --kotlinc-daemon`;
`ccb build --no-kotlinc-daemon` opts out for one build. Combining
both flags is an error. `ccb daemon stop` asks the project's daemon
to shut down; `ccb clean` stops it best-effort (only when daemon
mode is enabled in config) before removing `build/`.

- Per-project: run files under `build/kotlin-daemon`. Two projects
  never share one.
- Idle shutdown: default 300 s, configurable 60–3600 s via
  `kotlinc_daemon_idle_seconds`.
- A stale or crashed daemon falls back to a plain kotlinc invocation
  instead of failing the build.
- Toggling the daemon does not invalidate outputs: same compiler,
  same arguments, compile-code fingerprint unchanged.
- Full rules and measured times: `docs/build-daemon.md`.

## 6. Agent-oriented interface

This section is normative. A build system that is merely "scriptable" is
not agent-oriented. CCB must satisfy all of the following.

### 6.1 Commands

| Command | Purpose |
|---|---|
| `ccb init [dir]` | Scaffold a minimal project: `ccb.toml`, manifest, one Kotlin Activity, `res/`, `.gitignore`. Must work offline. Flags: `--package`, `--name`. Without them, name `hello` and package `com.example.hello` regardless of the directory name. |
| `ccb setup` | Provision the toolchain: install a JDK 17+ (via the platform package manager, interactively or `--yes`), SDK command-line tools, platform, and build-tools (checksum-verified downloads), the official kotlinc 2.4.21 compiler into the SDK home when missing, the NDK revision for `[ndk]` projects, r8 9.5.28 into the SDK home for minify projects without an explicit jar, and missing host tools from the platform package manager (Termux: aapt2/android-tools from the official repos, zipalign from the rendiix repo only when the official repos have no candidate; Linux: adb from the distro manager). In a project with `[deps] maven`, also prefetches every direct + transitive artifact into the local Maven cache (see §5.4) and writes `ccb.lock`; in a project with `[build] kotlin_plugins`, also prefetches the plugin jars. Ends with a final verification pass (toolchain resolve + adb) reported as warnings. `--upgrade` reinstalls setup-managed kotlinc/r8 to pinned versions and implies `--yes`; `--print-env` prints shell exports instead of the ready line. Pinned downloads are cached under `~/.cache/ccb/downloads` with resume. This is the only command allowed network access; builds stay hermetic. |
| `ccb check` | Validate config + toolchain only. No build output. Exit non-zero with all problems listed. Reports consumed libraries (name, version, path/coordinates, AAR contents summary); for a library project, the library itself and its AAR. Flags: `--json`, `--preview-gate`, `--no-preview-gate`. |
| `ccb build` | Full pipeline (§5). Flags: `--variant`, `--config`, `--json`, `--dry-run`, `--verbose`, `--explain`, `--aab`, `--kotlinc-daemon`, `--no-kotlinc-daemon`, `--preview-gate`, `--no-preview-gate`. |
| `ccb clean` | Remove the build directory only. Never touches sources. |
| `ccb doctor` | Check every tool CCB needs (SDK, build-tools, platform, kotlinc, d8, apksigner, zipalign, adb, NDK if configured). For each missing item print what it is, where CCB looked, and the exact install command. Flag: `--json`. |
| `ccb install` | `adb install` the built APK (device selection via `--device`, error if 0 or 2+ devices and no selection). |
| `ccb run` | Install + launch the app's main activity via `adb shell am start`. |
| `ccb logcat` | Stream `adb logcat` filtered to the app's package (convenience for agents reading crash logs). |
| `ccb preview` | Render the app's screens in the terminal from CSS + Kotlin flow sources, and print the logic tree (see §10). No device or emulator needed. Flags: §10.5. |
| `ccb daemon stop` | Ask the project's kotlinc daemon to shut down (§5.13). |
| `ccb version` | Print CCB version, supported compileSdk range, and embedded tool expectations. Flag: `--json`. |

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
  (e.g. `CCB-E1001`) from the diagnostics catalog in §12. Agents branch
  on codes, not on prose.
- An agent must be able to reconstruct the full build state from the
  JSON stream alone, without reading any other output.
- Full event shapes: `docs/json-protocol.md`.

### 6.3 Dry run and explain

- `ccb build --dry-run` prints the complete ordered command plan
  (tool, arguments, working directory, inputs, outputs) and exits without
  executing anything. Agents inspect before running.
- `ccb build --explain` runs the build and, for every stage, prints WHY
  it ran or was skipped (which input hash changed, which tool version
  changed, or "cache hit").
- `--explain` is conservative downstream of link: when link will run,
  later steps report "will change" even if their inputs turn out
  identical. Explain never over-promises a skip; it may over-predict runs.
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

Preview reuses these codes: 2 for preview config errors, 1 for preview
failures such as an unresolvable navigation target (§10.5).

### 6.5 Logging rules

- Default output is quiet: one line per stage (started/finished), plus
  warnings and errors. No tool banner spam.
- `--verbose` passes through the underlying tools' output, clearly
  delimited per stage.
- Tool stderr is always captured and attached to the failing stage's
  error event, never dumped raw to the terminal.
- No timestamps in default output (breaks diffing); timestamps exist
  only in `--json` events.
- Human-mode diagnostics all print with an `error[...]` prefix,
  regardless of severity: a warning such as CCB-P1017 prints as
  `error[CCB-P1017] ...` on stderr. Judge pass/fail by the diagnostic's
  severity in §12 and by the exit code, not by the prefix.

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
- Fingerprint rules that matter:
  - Compile-code fingerprint hashes classpath jar contents, not just
    paths; dex fingerprint hashes classes jars.
  - Tool probes (`kotlinc -version` / `java -version`) are cached in
    `build/intermediates/tool-probes.json`, keyed by tool path + mtime +
    size. Mismatch, missing entry, or corrupt file re-probes; `ccb clean`
    removes it with `build/`. Probes never change build outputs: they
    only feed the tool fingerprint.
  - Comment-only Kotlin change re-runs kotlinc (source hash changed) but
    skips dex and downstream: `.class` files — and every downstream
    fingerprint — are identical.
  - A comment-only library change rebuilds only that module: identical
    class bytes mean an identical AAR hash, so dependents skip.

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

Measured status: warm no-op rebuild ~0.2 s. Cold hello-world ~19–23 s
and one-file Kotlin rebuild ~14 s — both still above target; JVM
startup in kotlinc/d8/apksigner dominates. The opt-in kotlinc daemon
(§5.13) removes kotlinc JVM startup from warm rebuilds only (1-file
rebuild 13.6 s → 5.2 s on hello, 2026-11-25). The <10 s cold target is
not met and stays open work.

## 9. Implementation requirements (C)

- Language: C17, single codebase, no C++.
- Style: 2-space indent, K&R braces, `//` comments, strict warnings
  (`-Wall -Wextra -Werror -Wpedantic` in CI).
- Dependencies: libc only. The TOML parser, SHA-256, ZIP reader/writer,
  and JSON writer are implemented in-tree (small, single-file style
  modules are acceptable; vendored third-party code must be licensed
  compatibly and credited in the source).
- Process management: `posix_spawn` (or `fork`/`exec` with careful fd
  hygiene). No `system()` calls. Every spawned tool's argv, cwd, and
  environment must be constructible in `--dry-run` output.
- No threads. The pipeline is sequential; parallelism is a v2
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
  - Test suite: 52 tests total (44 fast + 8 slow). Every test has two
    labels: speed (`fast` / `slow`) and area. Fast means no real Android
    build, seconds each. Slow means a real `ccb build` or a timeout
    wait. Run suites sequentially, never concurrently
    (`ctest --test-dir build -L fast` for the quick gate,
    `ctest --test-dir build` for the full suite; see
    `docs/build-tests.md`).
- Repository layout:
  - `src/` — CCB sources (`main.c`, `toml.c`, `build.c`, `zip.c`,
    `hash.c`, `json.c`, `doctor.c`, `cmd_*.c`).
  - `tests/` — unit + integration tests.
  - `examples/hello/` — minimal sample project built by CI.
  - `docs/` — user guide, TOML reference, JSON protocol reference.
  - `CMakeLists.txt` — CMake-only build.

### 9.1 Module map

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

Full convention — screen and component declarations, the one-CSS-
file-per-screen rule plus `common.css`, the selector set, the layout
model in terminal columns, and the exit codes — is frozen in
`docs/preview.md`. The terminal frame is `--width` columns (default
40, minimum 24; smaller values clamp with a warning, CCB-P1013).
`strict = true` (or `--strict`) turns subset warnings into errors.
When `flow` is absent, preview derives screens, edges, and the
wireframe from the app's own Kotlin Activities and layout XML instead
(see `docs/preview.md`, "Real code").

Projects that do not follow the convention get a clear
"preview not applicable to this project" message, not a crash and not
a hallucinated render. Android XML layouts are understood by preview;
Compose code is NOT understood.

`[preview]` keys: `styles`, `flow`, `resources`, `layout`, `nav_graph`,
`phone_width`, `phone_height`, `strict`, `gate`. `resources` is an
optional directory of `strings.xml` / `colors.xml`; whole-label
`@string/name` and `color` / `background-color` `@color/name` values
resolve at load time. Unresolved references warn (CCB-P1016) and keep
the literal. `layout` / `nav_graph` are overrides; without the key,
preview tries `<paths res>/layout` and `<paths res>/navigation`, then
`<resources>/layout` and `<resources>/navigation`. A configured path
that does not exist is a config error (CCB-E1006).

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
- `--strict` turns subset warnings into errors (CCB-P1001, CCB-P1014).

### 10.4 Kotlin: structure analysis, not interpretation

Preview does **not** interpret or execute Kotlin. It statically parses
structure:

- Screen and component declarations, component trees per screen.
- Navigation calls (`nav("Home")`, `navigateTo(...)`) → edges in the
  logic tree. Arguments after the target are recorded on the edge
  (string literals literally, `name = value` pairs as written,
  anything else as `unknown`) and shown in the tree, walk mode's
  edge list, and JSON (`"args"` array; JSON `"version":2`).
- Click handlers (Slice 1): `setOnClickListener` lambdas (trailing and
  parenthesized), `onClick = { ... }` assignments, and direct
  `::handler` references inside screen fun bodies yield edges —
  derived, not declared. Binding is an explicit `[preview.bindings]`
  table (code identifier → component label); an unbound handler warns
  (CCB-P1019) and yields no edge. `startActivity(Intent(this,
  X::class.java))` yields the class simple name as the target. A
  declared edge for the same button always wins, with a CCB-P1020
  warning on disagreement. Slice 2 (conditional nav → branch edges:
  `if`/`when` branch bodies in handlers, braced or braceless, yield
  edges annotated with the branch label, e.g. `if ready: then`) and
  Slice 3 (XML onClick, NavController: layout `android:onClick`
  buttons resolve via `[preview.bindings]` view-id keys to handler
  funs, whose edges attach to screens containing the bound label —
  unbound is CCB-P1019, label in no screen is CCB-P1021;
  `navigate(R.id.x)` resolves through a nav-graph file — unlisted id
  or no graph file is an unknown edge) are implemented; see
  `docs/preview.md` for the exact shapes, locations, and outcome
  tables.
- `when`/`if` branches over state variables → branches in the logic
  tree, labeled with their conditions, **not evaluated**. A branch is
  shown, not resolved.
- Dynamic data (network, database, sensors) becomes a placeholder node.

Diagnostics from this analysis: navigation to a screen that does not
exist is an error (CCB-P1005); a screen nothing navigates to is a
warning (CCB-P1006); a screen with no components is a warning
(CCB-P1007).

Derived handler edges render exactly like declared edges in every
output mode (tree, walk, storyboard, diff, JSON — no `derived` marker
in output), and the post-load reconciliation (declared beats derived,
CCB-P1020 on disagreement, CCB-P1019 for unbound handlers, CCB-P1021
for an onClick label in no screen) runs before layout and diagnosis,
so every consumer sees the reconciled edges. CCB-P1019/CCB-P1020/
CCB-P1021 are warnings always, never gate failures. Branch edges add
the tree/walk suffix ` [branch]` and the JSON per-edge `"branch"` field
(string, or `null` when unconditional); JSON stays `"version":2`.

### 10.5 Output

- Terminal render per screen (default): phone frame, boxes, colors,
  clipped text, then the logic tree, then diagnostics.
- `--json`: the box tree, logic tree, and diagnostics as structured
  data for agents (JSON `"version":2`; edge `"args"` arrays and
  `"branch"` fields; every box carries `"w_dp"`/`"h_dp"` alongside
  its cell size). One JSON object, no trailing newline, deterministic.
- `--screen NAME`: render one screen only. `ccb preview <screen>` is
  the same.
- `--storyboard "A,B,C"`: render a journey of screens in order
  (repeats allowed), each frame preceded by a `Journey N/M: <Name>`
  header; `--json` keeps only the journey screens, in order.
  Conflicts with `--screen`, `--walk`, `--watch`, `--diff`,
  `--update-goldens` (exit 2).
- `[preview] resources = "res"`: optional directory of `strings.xml`
  / `colors.xml` (see §10.2).
- Lint warnings (always warnings, even under `--strict`, never
  build-gate failures): CCB-P1017 tap target too small (`button` /
  `textfield` under 8 columns or 2 rows, wireframe cells),
  CCB-P1018 text contrast below 4.5:1 (3.0:1 at `font-size` ≥ 24px)
  by WCAG relative luminance.
- `--walk [screen]`: interactive static exploration of the logic
  tree (arrow keys/`j`/`k`, Enter, `b`, `t` branch mode, `q`). Static
  only — never execution or evaluation.
- `--watch [screen]`: render once, then re-render on every change to
  the styles/flow files or `ccb.toml` (~200ms debounce; TTY clear,
  piped delimiter, `--json` JSONL, `--output` rewrite); Ctrl-C /
  SIGTERM exits 0; a broken save prints diagnostics and keeps
  watching. Conflicts with `--update-goldens` and `--walk` (exit 2).
- `--output FILE`, `--device phone|tablet`, `--update-goldens`:
  write the render to a file, frame-width presets (phone = 40
  columns, tablet = 80; an explicit `--width` always wins), regenerate
  the checked-in goldens.
- `--ascii`: ASCII frame characters instead of Unicode.
- `--diff DIR`: compare the current renders against a snapshot
  directory in the golden-file format; exit 0 identical, 1 with a
  unified diff per differing file, 2 for usage/config errors
  (unreadable snapshot dir, no usable [preview] section, unknown
  --screen). `--screen` scopes to one screen; `--json` emits a
  machine-readable diff object. Conflicts with --watch, --walk,
  --update-goldens (exit 2).
- `--scenario FILE`, `--trace FILE`, `--record`, `--tree-live`: scenario
  and trace modes and the LSP tree-live mode (see
  `docs/preview-scenarios.md`, `docs/preview-traces.md`,
  `docs/preview-lsp.md`). Scenario entries and trace steps that do not
  match the project warn (CCB-P1022, CCB-P1023) and never fail the
  build gate.
- Build gate (opt-in, off by default): `[preview] gate = true` or
  `check`/`build --preview-gate` (`--no-preview-gate` forces off;
  explicit flag beats config; passing both flags is a usage error,
  exit 2) runs preview diagnostics as part of `check` (after toolchain
  validation) and `build` (before any pipeline stage). Any preview
  ERROR fails the gate (exit 1, one concise line per failing diagnostic
  on stderr); warnings alone pass; preview-not-applicable projects pass
  silently. See `docs/preview.md` Build gate.
- Exit codes reuse §6.4 (2 for preview config errors, 1 for preview
  failures such as an unresolvable navigation target).

### 10.6 Explicit non-capabilities

Preview is NOT: an emulator, a pixel-perfect render, an animation
player, an interaction simulator, a Kotlin interpreter, or a source
of real data. Fonts are the terminal's fonts. System UI, themes, and
Android framework behavior are not modeled.

### 10.7 Feasibility: yes, scoped this way

A full-fidelity version — a complete CSS engine plus a Kotlin
interpreter plus the Android framework — would be reimplementing a
browser and a runtime, and is NOT feasible or in scope. The scoped
version above is feasible because it is three tractable pieces: a CSS
subset parser, a box layout pass, and a Kotlin structure parser, plus
a terminal renderer. That is the same size class as a linter. Edge
cases are handled by the subset + warnings + placeholder/unknown
nodes: preview never crashes on input it does not understand, and
never silently guesses.

## 11. Security considerations

- Keystore passwords come only from environment variables or prompts,
  never from TOML, never from the command line (which leaks to process
  listings), never in logs.
- The auto-generated debug keystore uses the standard Android debug
  credentials and is gitignored by `ccb init`.
- CCB never executes anything from the project directory (no hooks, no
  scripts, no "prebuild" commands). A project that needs custom
  steps is out of scope rather than a code-execution vector.
- Downloaded-tool integrity: `ccb setup` verifies checksums of pinned
  downloads against a pinned list. Maven artifacts verify against
  published `.sha256` / `.sha1`, else a recorded sidecar hash (§5.4).
  No PGP signature verification: checksums only.

## 12. Diagnostic codes

Every failure carries a stable code. Agents branch on codes, not on
prose. Exit codes: 0 success, 1 build failure, 2 config error,
3 toolchain missing/wrong version, 4 signing error, 5 device error.
Tool failures include the exact command that failed and the tail of its
stderr. Keystore passwords never appear.

### 12.1 Config (exit 2)

`ccb check` reports every config problem at once, not just the first.

| Code | Meaning |
| ---- | ------- |
| CCB-E1001 | Unknown key in `ccb.toml` (hard error; key and table named) |
| CCB-E1002 | Missing required `ccb_version` |
| CCB-E1003 | Unsupported `ccb_version` (only 1 is supported) |
| CCB-E1004 | Missing required key |
| CCB-E1005 | Invalid value (bad variant, SDK range, min > target, manifest package mismatch, minify on debug, …) |
| CCB-E1006 | Declared path does not exist |
| CCB-E1007 | Wrong value type for a key |
| CCB-E1008 | TOML parse error (with line number) |
| CCB-E1009 | Config file not found |
| CCB-E1010 | Module dependency cycle (names the cycle path, e.g. `a -> b -> a`) |
| CCB-E1011 | Module depends on an unknown module (names the module and the dependency) |

### 12.2 Toolchain (exit 3)

| Code | Meaning |
| ---- | ------- |
| CCB-E2001 | Android SDK not found (set `[sdk] home` or `ANDROID_HOME`) |
| CCB-E2002 | Requested build-tools version not installed |
| CCB-E2003 | Requested platform `android.jar` not found |
| CCB-E2004 | kotlinc not found (set `[sdk] kotlin_home` or `KOTLIN_HOME`) |
| CCB-E2005 | java missing or older than 17 |
| CCB-E2006 | kotlinc is not 2.x |
| CCB-E2007 | NDK not installed for the requested version |
| CCB-E2008 | NDK revision mismatch |
| CCB-E2009 | Required tool binary missing or not executable. Also a broken explicit tool override: `[sdk] aapt2` / `[sdk] zipalign` key or `CCB_AAPT2` / `CCB_ZIPALIGN` env var pointing at a missing or non-executable path fails with this code, message names the override source and the path. No silent fallback to the default tool. |

### 12.3 Build (exit 1 unless noted)

| Code | Meaning |
| ---- | ------- |
| CCB-E3001 | Release signing pre-check failed (exit 4) |
| CCB-E3002 | aapt2 compile failed |
| CCB-E3003 | aapt2 link failed |
| CCB-E3004 | kotlinc failed |
| CCB-E3005 | javac failed |
| CCB-E3006 | d8 failed |
| CCB-E3007 | APK packaging failed (includes duplicate native libraries) |
| CCB-E3008 | zipalign failed |
| CCB-E3009 | Signing failed (exit 4) |
| CCB-E3010 | apksigner verify failed |
| CCB-E3011 | aapt2 dump badging failed |
| CCB-E3012 | NDK compile failed |
| CCB-E3013 | NDK link failed |
| CCB-E3014 | r8 requested but no `[sdk] r8_jar` set (release + minify) |
| CCB-E3015 | AAR extraction failed |
| CCB-E3016 | Cannot create or write a build directory or file |
| CCB-E3017 | r8 failed |
| CCB-E3018 | Malformed manifest during merge (names the AAR or the app manifest) |
| CCB-E3019 | Maven resolution failed: unresolvable coordinate, malformed or unsupported POM, or artifact not found in any configured repository |
| CCB-E3020 | Maven checksum mismatch: downloaded or cached file does not match its published or recorded checksum |
| CCB-E3021 | Maven artifact not in the local cache; run `ccb setup` to prefetch it (builds never download) |
| CCB-E3022 | Library module or path-library name collides with a declared AAR's base name (their extraction paths would overwrite each other) |
| CCB-E3023 | Maven lockfile mismatch: resolved tree deviates from `ccb.lock` (version, packaging, hash, missing, or stale entry), or the lockfile is malformed; run `ccb setup` to regenerate it |
| CCB-E3024 | Maven parent POM chain error: a cycle in the chain, or more than 10 ancestors, naming the chain |
| CCB-E3025 | Dex check failed: the dex references one or more classes it does not define (first 10 named, then "+ N more"), or a dex file is malformed/truncated; the build stops before packaging |

### 12.4 Preview (exit 1 unless noted)

Preview warnings exit 0; only CCB-P1005, CCB-P1008, and (under `--strict`)
CCB-P1001/CCB-P1014 exit 1. CCB-P1016–P1025 are warnings even under
`--strict`, so they never fail the build gate. A project with no usable
`[preview]` section is not a diagnostic at all: `ccb preview` prints
"preview not applicable to this project" and exits 0.

| Code | Meaning |
| ---- | ------- |
| CCB-P1001 | Unknown CSS property (warning; error under `--strict` / `[preview] strict`) |
| CCB-P1002 | CSS parse error, partial parse kept (warning) |
| CCB-P1003 | Overlapping boxes (warning) |
| CCB-P1004 | Box extends past the interior bounds (warning) |
| CCB-P1005 | Navigation to a target matching nothing: no screen, no nav-graph destination, and no manifest activity (error) |
| CCB-P1006 | Unreachable screen, nothing navigates to it (warning) |
| CCB-P1007 | Empty screen, no components (warning) |
| CCB-P1008 | Duplicate screen name, 2nd and later occurrences (error) |
| CCB-P1009 | Text clipped to its box (warning) |
| CCB-P1010 | Flow parse error, partial parse kept (warning) |
| CCB-P1011 | No screens found (warning) |
| CCB-P1012 | Unknown Kotlin construct, kept as a generic box (warning) |
| CCB-P1013 | Frame width clamped to the minimum of 24 columns (warning) |
| CCB-P1014 | CSS value outside the subset, e.g. unsupported unit or display value (warning; error under `--strict` / `[preview] strict`) |
| CCB-P1015 | CSS file ignored: neither `common.css` nor a `<Screen>.css` file for a screen in the project (warning) |
| CCB-P1016 | Unresolved `@string/` or `@color/` reference, or a malformed values entry / non-hex color value skipped (warning; literal kept) |
| CCB-P1017 | Tap target too small: `button` / `textfield` under 8 columns or 2 rows (warning, always) |
| CCB-P1018 | Text contrast below threshold: WCAG ratio under 4.5:1, or 3.0:1 at `font-size` ≥ 24px (warning, always) |
| CCB-P1019 | Handler found, button not bound: a click handler whose receiver has no `[preview.bindings]` entry (warning, always; no edge recorded) |
| CCB-P1020 | Declared navigation disagrees with a derived edge for the same button; the declared edge stands, the derived edge is dropped (warning, always) |
| CCB-P1021 | XML `android:onClick` button not found in any screen: the bound component label appears in no screen's component tree, so its derived edges have nowhere to attach (warning, always; no edge recorded) |
| CCB-P1022 | Scenario entry does not match the project (warning, always; a scenario never fails the build gate) |
| CCB-P1023 | Trace step does not match the project (warning, always; a trace never fails the build gate) |
| CCB-P1024 | Ambiguous function reference: a click-handler method reference or a helper call names a top-level fun defined in more than one flow file, so no edge is recorded for it — the reference is never guessed. Warns once per (file, name) (warning, always; never fails the build gate) |
| CCB-P1025 | Unattributed navigation: a nav call in a flow file that no parse recorded as an edge — inside a nested block, a skipped statement, or a helper never scanned from a button. Reported with file:line so it is never silently dropped; never invents an edge. Navs already reported by CCB-P1019 (unbound handler) or CCB-P1024 (ambiguous reference) are not repeated here (warning, always; never fails the build gate) |

### 12.5 Device (exit 5)

| Code | Meaning |
| ---- | ------- |
| CCB-E5001 | adb command failed or no device connected |
| CCB-E5002 | No APK to install (run `ccb build` first) |

## 13. Status and acceptance criteria

Shipped and verified: `ccb init`, `ccb setup`, `ccb check`,
`ccb doctor`, `ccb version`, full pipeline for a Kotlin hello-world app
(debug-signed, zipaligned, `apksigner verify` clean), incremental builds
with per-stage cache and `--explain`, NDK per-ABI compilation, AAR
unpacking with duplicate-class errors, release signing with fail-fast
credential checks, r8 minification path, `ccb logcat`, JSON protocol,
reproducible builds (two clean builds byte-identical), preview
(CSS subset + box layout + terminal render + logic tree + diagnostics +
walk/watch/diff/scenario/trace modes + build gate), AAB output
(base-module-only, `bundletool validate` passes), Maven remote
dependencies (ranges, snapshots, `LATEST`/`RELEASE`, parent-POM
deps/profiles, private repos, exclusions, `ccb.lock`), kotlinc compiler
plugins including the Compose compiler plugin path, library modules,
standalone library projects (`.aar` build + path/Maven consumption),
dexcheck gate, opt-in kotlinc daemon.

Acceptance: the APK installs and launches on a device via `ccb install` +
`ccb run`; a release APK built twice from the same inputs is
byte-identical; the JSON protocol doc is complete enough for an agent to
drive CCB without reading its source; the sample project renders
recognizably in a terminal; an unresolvable navigation target is
reported as an error; an unsupported CSS property warns and renders
with a fallback instead of crashing.

Open performance work: cold hello-world <10 s and one-file Kotlin
rebuild <2 s are not met (§8).

## 14. Open questions (for the user to decide)

1. **Windows?** v2 platform, or Linux/macOS only by design?
2. **Name expansion.** "Custom C Builder" is a placeholder. Keep CCB as
   a bare name, or pick a real expansion?
3. **Preview scope.** Compose code is NOT understood. Compose in
   preview, or keep it CSS-convention-only?
4. **CCB's own license.** Pick before publishing: permissive, copyleft,
   or source-available? No license is chosen on the user's behalf
   until they pick one.
5. **Cold-build speed.** The <10 s cold target is missed (§8). Daemon
   helps warm rebuilds only. What closes the cold gap: d8/apksigner
   JVM startup, or a different approach?

## 15. Glossary

- **aapt2** — Android Asset Packaging Tool: compiles and links resources
  and the manifest.
- **d8 / r8** — Dex compilers: convert JVM bytecode to Android dex
  format (r8 also shrinks and optimizes).
- **zipalign** — Aligns APK entries for efficient runtime loading.
- **apksigner** — Signs and verifies APKs (v1/v2/v3 signature schemes).
- **AAR** — Android library archive: classes, resources, manifest,
  assets, `R.txt`, and native libs in one file.
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
