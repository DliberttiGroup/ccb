# Standalone library projects

[Docs index](index.md)

*ccb-library*

A project can be a library instead of an app: it builds an `.aar`,
not an APK. An app consumes a library from a local path (a library
project dir or a built `.aar` file) or from Maven coordinates. In-tree
`[modules.<name>]` libraries are a different thing — see
[modules.md](modules.md): they live inside the app project, while a
standalone library is its own project with its own `ccb.toml`.

## Declaring a library project

*ccb-library-config*

```toml
[project]
name = "orbit"
package = "orbit"
version = "0.15.9"
# min_sdk / target_sdk / compile_sdk as usual

[paths]
manifest = "src/main/AndroidManifest.xml"
kotlin = "src/main/kotlin"
# res = "src/main/res" (optional)

[library]
```

- `[library]` (empty table) marks the project as a library. Any key
  under it is a hard error (`CCB-E1001`).
- `[project]` name, package, and version are the AAR identity: the
  build writes `build/<name>-<version>.aar`.
- `[paths]` manifest is required; `res`, `assets`, `kotlin`, `java`
  are optional, standard layout under `src/main/`.
- A library project cannot declare `[modules.<name>]`, `[ndk]`,
  `[deps]` jars/aars/maven, or path libraries of its own
  (`CCB-E1005`): it is built with the consuming app's toolchain and
  classpath. It cannot build an AAB (`CCB-E1005`).
- `ccb build` in a library project runs deps, then the single module
  build (resources, code, AAR), and stops: no merge, link, dex, or
  package. `ccb build --explain` / `--dry-run` plan the same way.

## AAR contents

*ccb-library-aar*

- `classes.jar`: compiled Kotlin/Java, except generated R classes
  (`R.class`, `R$*.class`). The consuming app's link generates the
  real R class for the library package; shipping both would make d8
  reject the duplicate.
- `AndroidManifest.xml`: copy of the library manifest.
- `res/` and `assets/`: raw trees, compiled by the consuming app at
  link time.
- `R.txt`: the library link's symbol table (`int string name 0x...`
  per symbol). A library with no resources ships an empty `R.txt`.
  Every AAR carries the file, including intermediate module AARs.

Library R fields are non-final (see [modules.md](modules.md)): the
library link generates R.java, CCB de-finalizes it, and the app link
assigns final IDs. Library code references R fields symbolically.

## Consuming a library from a path

*ccb-library-consume*

```toml
[deps.library.orbit]
path = "../orbit"
```

- `path` (required): a library project dir, or a built `.aar` file.
- A project dir must contain a `ccb.toml` declaring `[library]`
  (read minimally at load: no recursive config load, so a library
  cycle cannot recurse), a `[project]` name/version/package, and a
  manifest whose package matches `[project] package`. A dir that is
  not a library project is a hard error (`CCB-E1005`); a missing path
  is `CCB-E1006`; a missing `path` key is `CCB-E1004`.
- A `.aar` file joins the declared AARs at build time (deps step);
  a project dir is built first, into
  `build/intermediates/libraries/<name>/<name>.aar`, then consumed
  through the same AAR machinery as declared AARs and modules.
- Path libraries build in declaration order, between deps and
  modules. A later library sees earlier libraries' `classes.jar` on
  its kotlinc classpath, but not their R.java: path libraries are
  independent (no `depends` between them).
- A library name must not collide with a module name (`CCB-E1005`)
  or a declared AAR's base name (`CCB-E3022`, checked at build time,
  since the extraction directories would overwrite each other).
- A library package must differ from the app package, every module
  package, and every other library package (`CCB-E1005`).

## Consuming a library from Maven

An AAR dependency resolved via Maven works like a local AAR: see
[maven.md](maven.md) (packaging `aar`). The resolver's AARs join the
declared AARs in stage 0, so manifest merge, res merge, classpath,
and dex all treat them as local files. `ccb setup` prefetches them;
builds never download.

## Resource merging and namespacing

*ccb-library-r*

- Library `res/` merges with app `res/` at link. The app wins
  conflicts: the link passes library zips as aapt2 `-R` overlays in
  reverse merge order (so the first library wins among libraries)
  with the app zip last. Without this, a same-named app/library
  resource was a hard link error.
- R symbols are namespaced by package: the app link passes every
  library package as `--extra-packages`, so each library gets its
  own R class (`com.example.lib.R`) and cannot collide with the
  app's R class.
- A resource defined by more than one source is a warning at link
  time, naming the winner and the shadowed sources:
  `resource 'string/shared' defined by app and library orbit; app
  wins`. The scan is (type, name) only: a qualified variant
  (`values-night`) counts as the same entry, so the warning means
  the winner's definition applies where both apply. The warning
  prints only when the link runs, never on a cache-skipped link.
- Manifest merging is the same as for declared AARs: see
  [build.md](build.md) (stage 4). Library components are kept;
  app-manifest entries win on name conflict.

## `ccb check` reports libraries

*ccb-library-check*

For a library project, `check` prints the library and its AAR:

```
  library: mylib 1.2 (com.example.mylib)
    aar: build/mylib-1.2.aar (classes.jar, 1 res files, 2 R symbols)
```

For an app, one block per consumed library (path, Maven, declared
AAR), with the AAR contents summary (classes.jar present, res file
count, R.txt symbol count). A path library that has not been built
yet reports `(not built yet)`. `--json` carries the same data as a
`"library"` object (library projects) or a `"libraries"` array (apps),
each entry with `name`, `version`, `package`, `source`
(`path`/`file`/`maven`), `ref`, `aar`, `built`, `classes`,
`res_files`, `r_symbols`.

## Limitations (v1)

- No per-library `[deps]`, `[ndk]`, or signing: libraries use the
  app's jars/AARs on the classpath and have no native libs.
- No publishing library projects to a Maven repo (`ccb` builds an
  `.aar` file; publishing it is out of scope).
- No `depends` between path libraries, no transitive path libraries
  (a library project cannot consume path libraries).
- No dynamic feature modules.
