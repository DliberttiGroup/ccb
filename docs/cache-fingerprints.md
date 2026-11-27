# Cache fingerprints: the audited table

[Docs index](index.md) · [Build](build.md)

*ccb-cache-fingerprints*

One audited table of task → fingerprint inputs: what each pipeline
step's fingerprint covers, and what it deliberately does not. Written
in Step 3 of the remediation directive, from reading the fingerprint
code at `test` head `de37a20` — not from what the steps were assumed
to cover. Two defects found by this audit are fixed in the same step
(the sign alias and the cmake configure inputs); both are noted below.

`tests/test_cache_meta.c` checks this document mechanically: every
`fp` block below must list exactly the literal keys the code adds for
that task, every config field in `ccb_config.h` / `ccb_module` must
have a row in the config-coverage table, and every cache step the code
uses must have a section here. If you add a fingerprint key, change a
step's inputs, or add a config key, update this table in the same
commit or the test fails. `tests/test_cache_regression.c` proves the
important rows behaviorally (change exactly one input, assert exactly
the expected tasks rerun).

How to read an `fp` block: a bare word is a literal key added with
`ccb_fp_add` / `ccb_fp_addl` (the key's value follows it in prose).
`file:` is an input hashed by content with `ccb_fp_add_file` (the key
is the file path, the value its SHA-256). `adhoc:` is a component of a
fingerprint built by string concatenation rather than the builder
(`align`, `sign`, `verify`).

A fingerprint match alone never skips a step: the skip also requires
the step's outputs to still be present and whole. The per-output
validation tiers live in [build.md](build.md) ("Output validation");
this table lists the validation each skip performs alongside the
fingerprint, because for two inputs the output check — not the
fingerprint — is what catches the change (project `name` for native
libs, library `version` for the AAR filename).

## Tool fingerprint (global)

*ccb-cache-fingerprints-tool*

One string, built in `ccb_pipeline.c` and stored as `tools` in
`build/ccb-cache.json`:

```
bt=<build-tools version>|aapt2=<aapt2 path>|zipalign=<zipalign path>
  |kt=<kotlinc version>|java=<java major>|compile=<compile_sdk>
```

A mismatch empties the whole cache: every step reruns. That is the
only way a toolchain change invalidates steps that do not key on the
tool themselves. What it deliberately does not record (Step 2
decision, directive §1): environment variables as such. What a step
keys on is the *resolved* toolchain identity — tool paths (aapt2,
zipalign) and versions (kotlinc, java major, compile SDK). Changing
`JAVA_HOME` between two JDKs of the same major version therefore
invalidates nothing, because same-major JDKs produce the same output.
It also does not record the NDK version or the cmake/make paths:
native steps key the NDK version themselves, and the clang step
records the NDK version rather than the clang path.

## Tasks

### deps

Cache key `deps`. Extracts declared AARs.

```fp
file: each declared AAR ([deps] aars), by content hash
```

Skip also requires the extracted outputs to still be there: each
AAR's extraction dir, each extracted `classes.jar` non-empty, each
res/assets/jni dir (`ccb_deps_outputs_valid`). A truncated but
non-empty `classes.jar` passes that check and fails loudly downstream
at javac/d8 (`CCB-E3005`) — the build never ships it silently; see
`test_cache_regression.c`. Re-extraction on corrupt-but-non-empty
jars is a follow-up, not a silent-ship risk.

### merge-manifest

Cache key `merge-manifest`. Merges AAR manifests into the app
manifest. Manifests are read from the AAR zips directly, not from
the extracted copies, so the merge does not depend on the deps cache.

```fp
app-manifest
file: each AAR manifest, by text (keyed by AAR path)
```

`app-manifest` is the app manifest text; each AAR contributes its
manifest text under its AAR path as the key, so a manifest content
change invalidates even when the AAR path is unchanged. Skip also
requires the merged manifest file to exist.

### compile-res

Cache key `compile-res`. `aapt2 compile` each resource directory
into a `.flat` zip.

```fp
build_tools
aapt2
file: every file under each resource dir (app res + each AAR res dir), by content hash
```

Skip also requires every compiled zip to exist. A truncated zip
passes the existence check and fails loudly downstream at link
(`CCB-E3003`, aapt2 reports "Invalid file") — never shipped silently.

### link

Cache keys `link` and `link-aab` (separate, so APK and AAB builds
never poison each other's cached link output). `aapt2 link` into
`linked.ap_`, generating `R.java`.

```fp
build_tools
aapt2
package
overlay
version
version_code
min_sdk
target_sdk
compile_sdk
merge-manifest
compile-res
extra-package
file: compiled AAR resource zips
file: app manifest
file: android.jar
```

`overlay` is always `1`; `merge-manifest` and `compile-res` are the
upstream steps' fingerprints, so link invalidates whenever an
upstream step's inputs changed — even if the upstream output bytes
happen to come out identical (e.g. a resource edit that compiles to
the same `.flat` bytes). That is conservative, and conservative is
correct here: a stale link is worse than a redundant one. `extra-package`
is one key per AAR package. Skip also requires the link output to
exist and the generated `R.java` dir to hold at least one non-empty
generated file.

### compile-code

Cache key `compile-code`. `kotlinc` (Kotlin + `R.java` + Java), then
`javac` for Java sources and generated `R.java`.

```fp
kotlinc
jvm_target
classpath
kotlin_plugin_option
file: Kotlin sources (.kt)
file: Java sources (java_dir + java_extra)
file: generated R.java
file: [deps] jars (when present)
file: AAR classes.jars (when present)
file: Kotlin plugin jars (when present)
```

`kotlinc` is the kotlinc version; `classpath` is the classpath
*string* (android.jar + jars + AAR classes.jars paths) — content
changes are caught by the jar file hashes, and android.jar content
is hashed by `link` (its path here plus `compile_sdk` in the tool
fingerprint cover the code step). `kotlin_plugin_option` is one key
per `[build] kotlin_plugin_options` entry. Skip also requires the
classes dir to hold at least one whole `.class` file (`CAFEBABE`).

### dex

Cache key `dex`. `d8`, or `r8` for release + minify.

```fp
build_tools
min_sdk
classes
tool
r8-keep
file: r8 jar (release + minify only)
file: proguard rules (release + minify only)
file: [deps] jars
file: AAR classes.jars
file: stdlib jar
```

`classes` is the dependency output, not a source file: the hash of
the compiled `.class` files. A comment-only Kotlin change reruns
kotlinc but leaves the classes (and every downstream fingerprint)
identical, so dex and downstream skip. `tool` is `r8` when release +
minify, absent otherwise; `r8-keep` is the generated keep-rules text
(from the manifest and layouts). Skip also requires the dex dir to
hold at least one whole `.dex` file (dex magic + a header size that
matches the file).

### dexcheck

No fingerprint: it always runs, and does not count toward
`steps_run`. It parses the dex files and fails the build on a
referenced-but-undefined class (`CCB-E3025`).

```fp
none: always runs, no fingerprint
```

### native (clang)

Cache key `native-<abi>`, one per ABI. NDK clang per source, linked
into `lib<sanitized project name>.so`.

```fp
ndk
abi
min_sdk
page-size
cflag
dep-missing
file: [ndk] sources, by content hash
file: headers from the previous build's depfiles, by content hash
```

`page-size` (`16384`) is present for `arm64-v8a` only.
`dep-missing` is a marker when a depfile lists a header that no
longer exists (forces a rebuild; the rebuild's compile then reports
the real error, or succeeds when the include is gone). The project
`name` is deliberately *not* a fingerprint key: the skip checks the
`.so` at the name-derived path (`lib<name>.so`, ELF magic), so a
rename finds no `.so` at the new path and rebuilds. The clang path
records the NDK version, not the clang path.

### native (cmake)

Cache key `native-<abi>`, one per ABI. CMake drives the project's
own `CMakeLists.txt` instead of the clang-per-source step.

```fp
ndk
min_sdk
variant
cmake_arg
file: CMakeLists.txt, by content hash
```

Step 3 fix: before this step the fingerprint was the lists file
alone, so changing `[cmake] args`, `min_sdk` (`ANDROID_PLATFORM`),
`variant` (`CMAKE_BUILD_TYPE`), or the NDK version silently reused
the old `.so` — the skip only checks that *some* whole `.so` exists
in the ABI dir. All four are fingerprint keys now
(`test_cache_regression.c` proves the `cmake_arg` case). The
cmake/make binary paths are not fingerprinted, following the clang
precedent (the NDK version covers NDK upgrades).

### dex-stdlib

Not a pipeline task: the global stdlib dex cache
(`$XDG_CACHE_HOME/ccb/dex-stdlib`, one `<key>.dex` file, published
by atomic rename). Its key is a fingerprint of the stdlib jar hash,
min-api, and build-tools version, so a kotlinc upgrade, a min-api
change, or a build-tools upgrade all miss and re-dex. A missing or
corrupt entry falls back to the jar — the build never fails because
of this cache, it just goes slow again.

```fp
stdlib
min_sdk
build_tools
```

### module

Cache keys `module-<name>` and `library-<name>`, one per module or
path library. The module build (resource compile + kotlinc/javac +
classes jar + AAR assembly) with its own fingerprint.

```fp
build_tools
aapt2
kotlinc
jvm_target
kotlin_plugin_option
file: android.jar
file: module manifest
file: module res files
file: module assets
file: module Kotlin sources (.kt)
file: module Java sources (.java)
file: [deps] jars
file: AAR classes.jars (when present)
file: Kotlin plugin jars (when present)
file: upstream module AARs + their generated R.java
```

The module `name` is in the cache key, not the fingerprint. Skip
also requires the module AAR to exist, and the classes jar to exist
when the module has sources.

### package

Cache keys `package` and `package-aab`. CCB's own ZIP writer:
linked entries, dex, assets, native libs (plus `BundleConfig.pb`
for AAB). No literal keys — everything is file contents.

```fp
file: link output
file: dex files (archive name is a key)
file: assets, app + AAR (archive names are keys)
file: native .so files (archive names are keys)
```

`BundleConfig.pb` is constant (`1.0.0`) and needs no input. Skip
also requires the unsigned package to exist.

### align

Cache key `align`. `zipalign -f 4`. The fingerprint is an ad-hoc
string, not a builder fingerprint.

```fp
adhoc: zipalign path
adhoc: unsigned APK hash
```

Skip also requires the aligned APK to exist. A truncated aligned
APK changes its hash, so sign reruns and apksigner fails loudly
(`CCB-E3009`) instead of shipping it.

### sign

Cache keys `sign` and `sign-aab`. `apksigner`. The fingerprint is an
ad-hoc string, not a builder fingerprint.

```fp
adhoc: variant
adhoc: keystore path
adhoc: input APK hash
adhoc: keystore hash (when the keystore exists)
adhoc: keystore alias
```

Step 3 fix: before this step the alias was missing, so switching
`[signing.debug] alias` / `[signing.release] alias` silently reused
the APK signed with the previous key — the keystore path and hash
cannot see the switch. The alias is a fingerprint component now
(`test_cache_regression.c` proves it with a two-alias debug
keystore). Password env var names and values are never fingerprinted
or stored: a password unlocks the keystore, it does not change the
signature bytes. Skip also requires the signed APK/AAB to exist. A
truncated signed APK changes its hash, so verify reruns and
apksigner verify fails loudly (`CCB-E3010`).

### verify

Cache keys `verify` and `verify-aab`. `apksigner verify` (plus
`aapt2 dump badging`) for APKs; `jarsigner -verify` plus a
structural check for AABs. The "fingerprint" is the signed
artifact's own hash: the step skips only when the hash matches the
recorded one.

```fp
adhoc: signed artifact hash
```

Skip also requires the artifact to exist (missing is `CCB-E3010`).
Verification itself runs whenever the hash differs, so a corrupt
signed artifact is verified — and rejected — rather than shipped.

## Config coverage

*ccb-cache-fingerprints-config*

Every field of `ccb_config` and `ccb_module` (`src/ccb_config.h`),
and what covers it. `tool` means the global tool fingerprint;
`indirect` means the value selects resolved artifacts whose
*contents* are hashed; `none` means deliberately not fingerprinted,
with the reason; `output-check` means a skip's output-existence
check catches the change, not a fingerprint key. (`*_count` length
fields are not config keys and are not listed.)

| Field | TOML key | Coverage |
| --- | --- | --- |
| config_path | — (meta) | none: invocation, not a build input |
| project_root | — (meta) | none: invocation, not a build input |
| ccb_version | ccb_version | none: config format version, validated at load |
| name | [project] name | output-check: the clang native .so path embeds the sanitized name, so a rename fails the skip's output check and rebuilds; the standalone library AAR filename likewise |
| package | [project] package | link |
| version | [project] version | link; output-check: the standalone library AAR filename embeds it, so a version change fails the skip's output-existence check and rebuilds |
| version_code | [project] version_code | link |
| min_sdk | [project] min_sdk | link, dex, native |
| target_sdk | [project] target_sdk | link |
| compile_sdk | [project] compile_sdk | tool, link |
| manifest | [paths] manifest | link, merge-manifest, module |
| res | [paths] res | compile-res |
| assets | [paths] assets | package |
| kotlin_dir | [paths] kotlin | compile-code |
| java_dir | [paths] java | compile-code |
| java_extra_dirs | [paths] java_extra | compile-code |
| libs_dir | [paths] libs | none: parsed and validated as a directory, but no task reads it (dead config; Step 7 owns removing it or wiring it in) |
| sdk_home | [sdk] home | indirect: resolved tool paths and versions land in the tool fingerprint and per-step keys; an identical SDK at a new path changes nothing, a different-version one changes the recorded versions |
| build_tools | [sdk] build_tools | tool, compile-res, link, dex, module |
| ndk_version | [sdk] ndk_version | native |
| kotlin_home | [sdk] kotlin_home | indirect: the resolved kotlinc version is in the tool fingerprint and the compile-code kotlinc key; the path itself is not recorded |
| r8_jar | [sdk] r8_jar | dex |
| aapt2_override | [sdk] aapt2 | tool, compile-res, link, module |
| zipalign_override | [sdk] zipalign | tool, align |
| adb_override | [sdk] adb | none: device operations only, never a build input |
| debug_store | [signing.debug] store | sign |
| debug_alias | [signing.debug] alias | sign |
| has_release_signing | [signing.release] (presence) | none: gate; release signing config is validated before compiling, it selects the sign inputs rather than being one |
| release_store | [signing.release] store | sign |
| release_alias | [signing.release] alias | sign |
| release_store_pw_env | [signing.release] store_password_env | none: password values are never fingerprinted or stored; the password unlocks the keystore and does not change signature bytes |
| release_key_pw_env | [signing.release] key_password_env | none: password values are never fingerprinted or stored; the password unlocks the keystore and does not change signature bytes |
| variant | [build] variant | sign, dex, native |
| minify | [build] minify | dex |
| shrink_resources | [build] shrink_resources | none: parsed but no task reads it (dead config; Step 7 owns it) |
| jvm_target | [build] jvm_target | compile-code, module |
| proguard_rules | [build] proguard_rules | dex |
| build_dir | [build] build_dir | none: output location, not an input |
| kotlinc_daemon | [build] kotlinc_daemon | none: execution mode (daemon vs direct kotlinc); same version, same args, same output by design, with fallback to direct |
| kotlinc_daemon_idle_seconds | [build] kotlinc_daemon_idle_seconds | none: daemon lifetime, not a build input |
| kotlin_plugins | [build] kotlin_plugins | indirect: resolved to kotlin_plugin_jars, hashed by compile-code and module |
| kotlin_plugin_options | [build] kotlin_plugin_options | compile-code, module |
| kotlin_plugin_jars | (resolved) | compile-code, module |
| jars | [deps] jars | compile-code, dex, module |
| aars | [deps] aars | deps |
| maven | [deps] maven | indirect: resolved artifacts are hashed as jars/AARs by compile-code, dex, deps |
| maven_central | [deps] maven_central | indirect: resolution input; resolved artifact contents are hashed |
| maven_repos | [deps] maven_repos | indirect: resolution input; resolved artifact contents are hashed (credentials never fingerprinted) |
| maven_profiles | [deps] maven_profiles | indirect: resolution input; resolved artifact contents are hashed |
| maven_exclude | [deps] maven_exclude | indirect: resolution input; resolved artifact contents are hashed |
| has_ndk | [ndk] (presence) | none: gate; selects the native task, not a step input |
| ndk_abis | [ndk] abis | native |
| ndk_sources | [ndk] sources | native |
| ndk_cflags | [ndk] cflags | native |
| has_cmake | [cmake] (presence) | none: gate; selects cmake instead of clang for the native task |
| cmake_lists | [cmake] lists | native |
| cmake_args | [cmake] args | native |
| has_preview | [preview] (presence) | none: preview-only, never a build input |
| preview_styles | [preview] styles | none: preview-only |
| preview_flow | [preview] flow | none: preview-only |
| preview_resources | [preview] resources | none: preview-only |
| preview_layout | [preview] layout | none: preview-only |
| preview_nav_graph | [preview] nav_graph | none: preview-only |
| preview_phone_width | [preview] phone_width | none: preview-only |
| preview_phone_height | [preview] phone_height | none: preview-only |
| preview_strict | [preview] strict | none: preview-only |
| preview_gate | [preview] gate | none: the opt-in preview gate can fail a build, but it does not change build outputs |
| preview_binding_keys | [preview.bindings] | none: preview-only |
| preview_binding_values | [preview.bindings] | none: preview-only |
| preview_scenarios | [preview.scenario.<name>] | none: preview-only |
| modules | [modules.<name>] | indirect: each module's inputs are fingerprinted under module-<name>; the list selects which module tasks run |
| is_library | [library] (presence) | none: gate; library projects run stages 0–2 only |
| libraries | [deps.library.<name>] | indirect: each library's inputs are fingerprinted under library-<name>; the list selects which library tasks run |
| name | [modules.<name>] / [deps.library.<name>] | module |
| dir | (module root) | indirect: all module inputs are hashed by content from paths under dir |
| depends | [modules.<name>] depends | module |
| manifest | <module>/src/main/AndroidManifest.xml | module |
| res | <module>/src/main/res | module |
| assets | <module>/src/main/assets | module |
| kotlin_dir | <module>/src/main/kotlin | module |
| java_dir | <module>/src/main/java | module |
| package | (from module manifest) | indirect: manifest file contents are hashed by module |
| is_file | (path library is a built .aar) | indirect: the AAR file it selects is hashed by deps/module; the flag selects how the AAR is obtained |
| version | (library version) | output-check: the standalone library AAR filename embeds it, so a version change fails the skip's output-existence check and rebuilds |

The last eleven rows are `ccb_module` fields; the rest are
`ccb_config` fields. Where a `ccb_config` field and a `ccb_module`
field share a name (`name`, `manifest`, `res`, `assets`,
`kotlin_dir`, `java_dir`, `package`, `version`), both rows are
present and both must hold.

## What this table does not cover

- **Concurrent access.** No parallelism exists yet; cache behavior
  under concurrent execution belongs to Step 8 (scheduler), which
  owns single-writer atomic publication and concurrent-access tests.
  Nothing here claims anything about two writers.
- **`tool-probes.json`.** `build/intermediates/tool-probes.json`
  (tool version probes, keyed by path + mtime + size) still writes
  non-atomically — a Step 2 follow-up (D-5 convergence). It feeds
  only the tool fingerprint, and a corrupt probe file is re-probed
  rather than trusted; no test in this step showed it corrupting a
  build.
- **Fingerprint collisions in principle.** The fingerprint is a
  SHA-256 over `key=value\n` lines in builder order; key order is
  part of the fingerprint (builders always add keys in a fixed
  order, so this is stable, not a source of flakiness). Values are
  hex hashes, config values, or version strings in practice.
- **Config-table attribution.** `tests/test_cache_meta.c` verifies
  the Config coverage table for completeness (every struct field has
  a row) and vocabulary (every coverage word is a known task or
  keyword), and verifies every task's fingerprint contents against
  the code in both directions. It does not verify that a row's listed
  tasks are the *correct* tasks for that field — that attribution is
  a human audit claim, and a wrong task name in a row would pass.
  The fingerprint contents themselves (the load-bearing part) are
  machine-verified; the row attributions are not.
- **Environment beyond the tool fingerprint.** `PATH`,
  `JAVA_HOME`, `KOTLIN_HOME`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`
  select the toolchain; only the resolved versions/paths are
  recorded. A same-version toolchain swap is a deliberate no-op
  (Step 2 decision, recorded in [build.md](build.md)).
