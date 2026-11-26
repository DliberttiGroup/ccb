# Library modules

[Docs index](index.md)

*ccb-modules*

A project can declare library modules in its root `ccb.toml`. Each module
builds into an intermediate AAR and is consumed through the existing AAR
machinery: manifest merges, res merges at link, classes dex, assets
package. The app module is the only APK/AAB output; library modules
produce no APK/AAB.

## Config

*ccb-modules-config*

```toml
[modules.base]
dir = "libbase"

[modules.core]
dir = "libcore"
depends = ["base"]
```

- `dir` (required): module root, relative to project root unless absolute.
  Standard layout under it, mirroring `[paths]`:
  `src/main/AndroidManifest.xml` (required), `src/main/res`,
  `src/main/assets`, `src/main/kotlin`, `src/main/java` (each optional;
  absent means the module has no such input).
- `depends` (optional, default `[]`): other module names this module
  depends on. Every declared module is built and consumed by the app;
  there is no separate "app depends on" list.
- Unknown keys in a module table are a hard error (`CCB-E1001`).

## Graph rules (validated at load, `ccb check` reports them)

*ccb-modules-graph*

- Modules build in topological order, dependencies first.
- Dependency cycle is a hard error naming the cycle path (`CCB-E1010`,
  e.g. `module dependency cycle: a -> b -> a`).
- Depending on an undeclared module is a hard error naming it
  (`CCB-E1011`).
- A module manifest must exist and declare a package; the package must
  differ from the app package and from every other module's package
  (`CCB-E1005`).
- A module name must not collide with a declared AAR's base name
  (`CCB-E3022`, checked at build time after Maven resolution, since the
  extraction directories would overwrite each other).

## Build flow

*ccb-modules-flow*

Pipeline order: deps (declared AARs) → modules → merge-manifest →
compile-res → link → code → dex → … The manifest merge sees declared AARs
first, then module AARs in topological order. Merge rules: see
[build.md](build.md) (stage 2).

Per module, in topological order:

1. Fingerprint: tool versions, module manifest, res, assets, Kotlin and
   Java sources, declared jars, declared AAR classes, upstream module
   outputs (AAR + generated R.java). Unchanged inputs skip the module.
2. Resources (only when the module has code *and* resources): `aapt2
   compile` the module res, then `aapt2 link` against the module manifest
   to generate R.java for the module package. Link output (throwaway
   `.ap_`) is discarded; only R.java is kept.
3. Code: `kotlinc` over module Kotlin sources, Java sources, generated
   R.java, and upstream modules' R.java (a module may reference a
   dependency's R class), classpath = `android.jar` + declared jars +
   declared and already-built module classes. `javac` then compiles the
   Java sources canonically, as for the app. A module with no Kotlin/Java
   sources skips this step.
4. `classes.jar`: every compiled file except generated R classes
   (`R.class`, `R$*.class`). The app link generates the real R class for
   the module package; shipping both would make d8 reject the duplicate.
   A resources-only module contributes no classes.
5. Intermediate AAR
   (`build/intermediates/modules/<name>/<name>.aar`): `classes.jar` (when
   present), `AndroidManifest.xml` (copy of the module manifest), raw
   `res/` and `assets/` trees. aapt2 compiles the res at app link time,
   exactly as for declared AARs.

## R semantics

*ccb-modules-r*

Library R fields are **non-final**. The module link generates R.java, CCB
rewrites its fields from `public static final int` to `public static
int`, and the app link assigns the final resource IDs. Library code
references R fields symbolically, so it resolves against the app's R class
at runtime.

- Library code may reference its own R class and its dependencies' R
  classes.
- Don't name your own class `R` in a module with resources: it would
  collide with the generated R class.

## Incremental builds

*ccb-modules-incremental*

A library module's outputs participate in the app module's fingerprints,
so changing a library rebuilds the app:

- Module res flows through compile-res (file hashes) and link (compiled
  zips + merge fingerprint).
- Module manifests flow through the merge fingerprint.
- Module classes flow through the app code fingerprint (hashes classpath
  jar contents, not just paths) and the dex fingerprint (hashes classes
  jars).

A comment-only change to a library source rebuilds only that module: its
class bytes are unchanged, so the AAR hash is unchanged and dependent
modules, app code, and dex correctly skip.

## Limitations (v1)

*ccb-modules-limits*

- No per-module `[deps]`, `[ndk]`, or signing sections: modules use the
  project's jars/AARs on the classpath and have no native libs.
- No dynamic feature modules.
- No publishing library modules to a Maven repo.
- No cross-module resource namespacing beyond what aapt2 already does
  (`--extra-packages` + `--auto-add-overlay`).
- The app module is not configurable per-module: all declared modules are
  built and consumed by every build.
