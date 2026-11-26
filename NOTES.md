# CCB notes — bugs, changes, optimizations

Found 2026-11-25 while building the CCB Showcase app
(`~/workspace/ccb-showcase/`, package `com.ccb.showcase`) with CCB only.
The same list lives in the showcase app's Notes & roadmap screen, so it can
be checked off there; this file is the copy in the CCB project itself, so the
items can be addressed here later. Check an item off here when it is fixed.

Status: all 6 bugs + `change-dexcheck` fixed 2026-11-25 (see as-built
notes below); only the optimizations / app ideas remain open.

## Bugs

- [x] `bug-aar` — AAR resources are not packaged: library R classes are
  missing at runtime. Evidence: Showcase v1 crashed on launch inflating
  `androidx.recyclerview.widget.RecyclerView` —
  `NoClassDefFoundError: androidx.recyclerview.R$attr`
  (`ClassNotFoundException: androidx.recyclerview.R$attr`), logcat
  2026-11-25. The APK builds and verifies fine, then crashes on the phone.
- [x] `bug-stdlib` — The Kotlin stdlib is not added to d8 inputs, so a
  project with no Maven deps ships an APK with no `kotlin-stdlib` classes.
  Evidence: Showcase v2 (RecyclerView removed) crashed on launch —
  `NoClassDefFoundError: kotlin.jvm.internal.Intrinsics`
  (`ClassNotFoundException: kotlin.jvm.internal.Intrinsics`) at
  `MainActivity.onCreate`. V1 only worked by accident: the RecyclerView
  Maven closure pulled `kotlin-stdlib 1.6.21` in transitively.
  Workaround (removed 2026-11-25, never do workarounds): the showcase
  had `[deps] jars` with the kotlinc `kotlin-stdlib.jar` (2.4.21) in
  `ccb.toml`; removed once the fix landed, and the showcase rebuilt
  clean with the main CCB binary, no `[deps]` section at all.
- [x] `bug-sdk36` — The main CCB binary rejects SDK 36 (`CCB-E1005: must be
  34 or 35`), while only platform android-36 / build-tools 36.0.0 are
  installed on the VM. The showcase and the Emerald build both had to use
  the `ccb-pokeemerald` branch binary (range 34–36) instead.
- [x] `bug-setup` — `ccb setup` always runs sdkmanager and fails behind the
  authenticated egress proxy, even when the platform, build-tools, and
  platform-tools are already installed. It can never succeed on this VM.
- [x] `bug-preview` — `ccb preview` says "not applicable" for real
  View/XML projects (no flow/styles section). Preview only understands the
  CSS + Kotlin-structure convention, not the classic Activity + XML
  layout apps `ccb init` scaffolds.
- [x] `bug-pom` — Maven resolution needed a parent POM hand-fetched into
  the cache (`org.sonatype.oss:oss-parent:9`); `ccb check` failed until it
  was placed by hand. The resolver should fetch it itself or say so.

## As-built notes (2026-11-25)

- `bug-preview`: already fixed by Phase 2 (landed before this round).
  Verified: `ccb preview` on the sketch-less
  `examples/real-world/p1-notes-real/` renders 4 screens with real
  edges instead of "not applicable".
- `bug-sdk36`: `src/ccb_config.c:657-666` — bounds raised to
  min_sdk 21..36, compile_sdk 34–36 (same change as the
  `ccb-pokeemerald` branch). Test: `tests/test_config.c` SDK-36
  accepted / SDK-37 rejected. Verified: `ccb check` passes on the
  showcase (`compile_sdk = 36`). Docs: `docs/setup.md`,
  `requirement.md` §5.1 range 34–36.
- `bug-setup`: `src/ccb_cli.c` — new `ccb_setup_missing_components`
  (shared by the pre-install skip and the post-install
  verification, which previously duplicated the check). When
  platform-tools, the platform, and build-tools are all present,
  setup skips the licenses and sdkmanager steps entirely and
  proceeds (kotlinc/r8/NDK/Maven as applicable). Test:
  `tests/test_setup.c` case 2b — all components present + a
  sdkmanager that exits 1 if invoked; setup still reports ready
  and never calls it. Verified on this VM: `ccb setup` in the
  showcase prints "SDK components already present, skipping
  sdkmanager" and exits 0 (it failed at sdkmanager before).
  Note: if cmdline-tools itself is missing, setup still
  bootstraps it (network) before it can skip.
- `bug-stdlib`: `src/ccb_pipeline.c` `ccb_step_dex` — the kotlinc
  stdlib jar is now a d8/R8 program input for Kotlin projects,
  unless the project already supplies it via `[deps]` jars or
  Maven (`kotlin-stdlib.jar` or `kotlin-stdlib-<ver>.jar` — the
  versioned Maven name initially slipped past the dedup and d8
  failed on duplicate classes; fixed). It is added to the dex
  fingerprint so a kotlinc upgrade re-dexes. For release + minify
  it is an R8 program input, not a `--lib` (same jar as both
  would be a duplicate); when no program input supplies it, R8
  still gets it as `--lib`. Test: `tests/test_dex_contents.c`
  (no-deps Kotlin hello build, dex contains
  `kotlin/jvm/internal/Intrinsics`). Verified: the no-deps hello
  dex now has 1082 kotlin classes (0 before). Workaround removed
  2026-11-25: the showcase's `[deps]` jars stdlib entry was deleted
  and the app rebuilt clean with the main CCB binary in 56 s —
  2.6 MB APK, dex contains `kotlin/jvm/internal/Intrinsics`, no
  duplicate.
- `bug-aar`: root cause was NOT the AAR manifest (the hypothesis
  in the task was wrong — the recyclerview AAR manifest is plain
  text XML with `package="androidx.recyclerview"`, and aapt2 did
  generate `androidx/recyclerview/R.java` with an `attr` class).
  The real hole: kotlinc resolves R symbols from the generated
  R.java but emits no R.class files for it, and javac only ran
  when the project had its own .java sources — explicitly
  excluding R.java. Fix: `src/ccb_pipeline.c`
  `ccb_step_compile_code` — javac now also compiles the
  generated R.java whenever it exists. Test:
  `tests/test_dex_contents.c` (local fabricated AAR with one
  attr; dex contains `com/example/lib/R$attr`). Verified
  end-to-end with a real recyclerview project: the dex now
  contains `androidx/recyclerview/R$attr` (absent before the
  fix). No device launch was available here, so the runtime
  crash itself is verified by dex contents only, not by
  launching the app on a phone.
- `bug-pom`: root cause was `bug-setup`, not the resolver.
  `ccb_maven_resolve` already fetches the full parent chain in
  prefetch mode (`mv_merge_parents`), and the cache-only error
  already names the exact missing parent
  (`CCB-E3021 ... oss-parent:9: parent POM is not in the local
  cache; run 'ccb setup' to prefetch it`). What happened in the
  showcase flow: setup aborted at sdkmanager (bug-setup) before
  ever reaching the Maven prefetch, so the parent was never
  fetched and got hand-placed instead. Verified with a fresh
  `CCB_MAVEN_CACHE`: `ccb setup` on a recyclerview project
  prefetches oss-parent:9 (18 artifacts), and with it deleted
  `ccb check` fails with the exact E3021 above. No code change
  in `ccb_maven.c`; `docs/maven.md` now states that prefetch
  fetches the parent chain. Existing `tests/test_maven.c`
  parent-POM coverage already asserts the fetch.
- `change-dexcheck`: new module `src/ccb_dexcheck.{h,c}` (pure
  dex parsing, libc only: header, string_ids, type_ids,
  class_defs; descriptors normalized — array markers stripped,
  primitives dropped — BEFORE comparing against defined classes)
  plus `ccb_step_dexcheck` in `src/ccb_pipeline.c`, wired
  immediately after `ccb_step_dex` and before `ccb_step_native`,
  so it runs before package for APK and AAB alike. Failure is
  new diagnostic `CCB-E3025`, naming the missing classes (first
  10, then "+ N more", dotted form) and stopping the build
  before packaging. Platform packages (`Ljava/`, `Landroid/`,
  `Ljavax/`, `Ldalvik/`, `Lsun/`, `Lcom/android/`, `Lorg/xml/`,
  `Lorg/w3c/`) are allowlisted; `Lkotlin/` and `Landroidx/` are
  deliberately not. Runs even when dex was cache-skipped, has no
  cache of its own; `--explain` prints its plan line, `--dry-run`
  skips it. Test: `tests/test_dexcheck.c` (51 checks). Verified:
  no-deps hello + hello `--aab` build and ship; d8 on app
  classes without stdlib reports `kotlin.jvm.internal.Intrinsics`
  (and `kotlin.Metadata`, an annotation-only reference, which
  counts by design); a `[deps]` jar referencing a never-supplied
  class fails a real `ccb build` with E3025 naming
  `com.example.Missing` and ships no APK; malformed/truncated dex
  files are build errors, never crashes. Limitation, documented
  in the header and `requirement.md` §5.5: reflection-only
  references (`Class.forName` strings) cannot be caught.

## Should change

- [x] `change-stdlib` — Auto-include `kotlin-stdlib.jar` in d8 program
  inputs for every Kotlin project. (Done with `bug-stdlib`, 2026-11-25.)
- [x] `change-aar` — Merge AAR `res/` and generate library R classes — or
  fail the build loudly instead of shipping an APK that crashes on launch.
  (Done with `bug-aar`, 2026-11-25: R classes are generated and dexed;
  no fail-loud fallback was needed.)
- [x] `change-sdk` — Raise the SDK range to 34–36 in main CCB.
  (Done with `bug-sdk36`, 2026-11-25.)
- [x] `change-setup` — Make `ccb setup` skip sdkmanager when the components
  are already present (and respect proxy env when it does need it).
  (Skip done with `bug-setup`, 2026-11-25; proxy env when sdkmanager
  does need to run is still open — sdkmanager has no proxy flags.)
- [x] `change-preview` — Preview for real layouts (tie into Phase 2
  real-code screens, which parse the app's own Kotlin + XML).
  (Done by Phase 2, 2026-11-25.)
- [x] `change-dexcheck` — Post-build dex check: verify every class
  referenced from code exists in the dex. This would have caught both
  launch crashes above before the APK shipped.
  (Done 2026-11-25: new `ccb_step_dexcheck` after dex, before package;
  see as-built note below.)

## Found building the Material Showcase (2026-11-25)

No new CCB bugs — the M3 showcase (`~/workspace/ccb-showcase-m3/`)
built first try with the main CCB binary, no workarounds, no deps,
no Maven. That itself is the proof the 6 fixes above work. Two
scaffold gaps found while building it:

- [ ] `change-scaffold-insets` — `ccb init` scaffolds no
  `fitsSystemWindows`, so any app targeting SDK 35+ (which enforces
  edge-to-edge) ships with the top bar under the status bar and the
  bottom nav under the system buttons. Found in the Material
  Showcase from the user's screenshot; fixed by hand with
  `android:fitsSystemWindows="true"` on each screen's root layout,
  verified by a screenshot on the phone. The scaffold should
  include it by default.
- [ ] `change-scaffold-sdk` — `ccb init` scaffolds SDK 35
  (`target_sdk`/`compile_sdk = 35`), which fails `ccb check` on a
  machine with only android-36 installed — a fresh project fails
  out of the box and has to be hand-edited to 36. The scaffold
  should default to the newest SDK the range allows, or to an
  installed one.

## Optimizations

- [x] `opt-dex` — d8 on the full kotlin-stdlib takes ~2 minutes; cache the
  stdlib dex or only include used classes (R8 path). DONE 2026-11-25
  (caching half only; R8 unused-class stripping NOT done): the cache
  helpers in `src/ccb_pipeline.c` (~2110–2300) dex the stdlib jar once into
  `$XDG_CACHE_HOME/ccb/dex-stdlib` (else `~/.cache/ccb/dex-stdlib`,
  keyed by stdlib jar hash + min-api + build-tools version) and passes
  the cached `.dex` to d8 as a program input; entry published by atomic
  rename, validated on read (dex magic + header file size), missing /
  corrupt entry falls back to the jar, R8 path untouched. One real bug
  found during verification: `memcmp(head, "dex\n0", 6)` compares 6
  bytes but the literal is 5 chars, so every real dex failed validation
  and the cache never published — fixed to prefix + first version
  digit. Showcase clean build: 57s before → 63s cold cache (one-time
  populate) → 33–34s warm cache. Test: `tests/test_dex_contents.c`
  case 3 (populate / reuse by mtime+hash / jar-change invalidation).
- [x] `opt-apk` — The showcase APK is 2.6 MB, mostly stdlib; strip unused
  stdlib for release builds. DONE 2026-11-25: `minify = true` already ran R8, but
  with no auto-keep rules it shrank the whole program away (the dex
  dir held only resource files, no `classes.dex`, and the build then
  failed dexcheck) — the flag was unusable without hand-written
  keeps. New: CCB generates the keep rules
  (`src/ccb_pipeline.c` `ccb_r8_keep_rules`, manifest entry points
  via new `ccb_manifest_entry_points` in `src/ccb_manifest.{h,c}`):
  the app's whole package plus every manifest component
  (application class, activities, services, receivers, providers,
  activity-aliases + `targetActivity`, from the app, AAR, and module
  manifests) and layout view classes (dotted element names, `class`,
  `android:name`, `layout_behavior`, from app/module res dirs and
  AAR zips). Written to `build/intermediates/r8-keep.pro`, passed to
  R8 ahead of the project's `proguard_rules` (still required, may be
  comment-only), and part of the dex fingerprint. Reflection-only
  references (`Class.forName` strings) still cannot be kept
  automatically — same limitation as dexcheck. The stdlib dex cache
  stays bypassed on the R8 path by design (R8 takes the stdlib as a
  program-input JAR, not dex). Showcase release: APK 2,651,777 ->
  75,393 bytes, dex 2,615,024 -> 37,160 bytes; dexcheck passes,
  `ccb check` passes, no-op rebuild reports up to date. Test:
  `tests/test_r8_minify.c` (debug build keeps full stdlib incl.
  `kotlin/text/Regex`; minify build drops Regex, keeps the app
  classes and two out-of-package XML-only entry points, dex under
  half the size; generated keep file asserted). 48 tests.
- [x] `opt-cache` — Cache the stdlib dex result across clean builds.
  DONE 2026-11-25 — same work as `opt-dex` (same files, same test);
  the cache lives outside `build/` so it survives `ccb clean`.
- [x] `opt-daemon` — Reuse the kotlinc daemon between builds.
  DONE 2026-11-25: opt-in only, default off (requirement.md §1.2,
  §2 goal 4, §6.6 amended: no daemon by default, opt-in daemon with
  auto-shutdown allowed). New `src/ccb_daemon.c/.h` (explicit 7-jar
  client / 6-jar compiler classpath, run dir `build/kotlin-daemon`,
  `result code: 0` / `ERROR\t` output classifiers, 512 MB heap cap);
  config `[build] kotlinc_daemon` + `kotlinc_daemon_idle_seconds`
  (default 300 s, allowed 60–3600, `CCB-E1005` out of range); CLI
  `ccb build --kotlinc-daemon` / `--no-kotlinc-daemon` (mutually
  exclusive), `ccb daemon stop`; `ccb clean` stops it best-effort
  when enabled. Two findings that shaped the design: (1) the
  daemon's RMI/JRMP handshake to the loopback registry hangs in
  this sandboxed VM unless both JVMs get
  `-Djava.net.preferIPv4Stack=true` (verified with a minimal RMI
  repro); (2) `autoshutdownIdleSeconds` alone does not keep the
  daemon alive once the client exits, so CCB also sets
  `--daemon-shutdownDelayMilliseconds=<idle*1000>` — and that delay
  also applies to an explicit stop, so the daemon exits within the
  idle timeout, not instantly. The daemon client exits 0 even on
  compile failure, so CCB parses the output instead of trusting the
  exit code; a dead daemon falls back to plain kotlinc and never
  fails the build. Measured 2026-11-25 (this VM, noisy): hello cold
  23–29 s off vs 25 s on, warm 13.6 s off vs 5.2 s on; Showcase cold
  34–64 s off vs 66–77 s on, warm 21.6 s off vs 17.8 s on — cold is
  a wash, warm rebuilds win. Test: `tests/test_kotlinc_daemon.c`
  (143 checks: classifiers, argv, config, dry-run on/off/override,
  live reuse by same port, stop poll, kill -9 fallback, clean).
  The test fixture uses idle 180, not 60: with idle 60 the daemon
  idle-shut-down mid-build on the sanitizer run (the post-kotlinc
  pipeline is ~95 s under ASan, longer than the 60 s delay — the
  daemon log showed a clean "Delayed shutdown in 60000ms" then
  "Shutdown complete" before `ccb build` returned), so the reuse
  checks failed even though the daemon worked; 180 gives the
  margin, and the stop poll is bounded by idle + 30 s. 50 tests.
- [x] `opt-static` — Static-binary drift resolved 2026-11-25:
  requirement.md:50,564 claimed "one static binary, under 5 MB"
  while the build was actually dynamic PIE with debug info. New
  `CCB_STATIC_RELEASE` option (`cmake --preset release`, dir
  `build-release/`): links `ccb` with `-static` and strips it in a
  post-build step; dev (`build/`) and sanitizer (`build-san/`) stay
  dynamic and unstripped, and the option is a configure-time error
  combined with `CCB_SANITIZE`. Measured: 1,268,272 bytes stripped
  static (1,373,432 unstripped; the ~605 KB figure was the stripped
  dynamic binary — static glibc adds ~700 KB), still well under
  5 MB. Verified with `file`/`ldd` plus `ccb check` and a clean
  Showcase build (30.5 s) using the static binary.
- [x] `opt-d8-heap` — Size d8's heap from its inputs instead of the
  wrapper's 2G default. DONE 2026-11-25: the `d8` wrapper script
  defaults to `-Xmx2G`, which beats CCB's `JAVA_TOOL_OPTIONS=-Xmx1g`
  (verified: command-line `-Xmx` wins, `-XX:+PrintFlagsFinal`
  shows 256m when both are given), so every d8 run reserved 2G and
  four parallel builds committed 8G on a 6G VM. New:
  `ccb_d8_heap_mb` in `src/ccb_pipeline.c` (declared in
  `src/ccb_pipeline.h`): heap = 256MB + 16MB per MiB of dex program
  inputs (app classes + `[deps]` jars + AAR classes.jars + stdlib
  input), capped at 1024MB, passed as a leading `-JXmx<N>m` to d8
  in the dex step and in the stdlib cache populate. Measured: d8
  succeeds at 128m on Showcase warm inputs (dex byte-identical to
  the 2G run) and on the stdlib jar alone, and at 256m on 10,000
  synthetic classes + stdlib dex (8.2MB in) with no time penalty vs
  1024m; Showcase clean build with the new binary: 31s, dex SHA-256
  unchanged. R8 untouched by design (whole-program shrinking, 1G
  cap already). Test: `tests/test_d8_heap.c` (formula unit checks +
  dry-run checks that the flag is on the d8 line and grows with
  input size). 49 tests.

## Bugs found building the Emerald native port (2026-11-25)

Found while building `pokeemerald-multiplatform` (SDL2 native port) with
CCB only, no Gradle. Fixes for these live only in the
`~/workspace/ccb-pokeemerald` branch (`pokeemerald-android`), not in main
CCB — main CCB cannot build this app as-is. Check off here when ported
to main.

- [ ] `bug-cmake` — No CMake / external native build support: main CCB's
  native step only compiles simple NDK sources. It cannot drive an
  app's `CMakeLists.txt` (SDL2 + libmain), run host tools
  (preproc/mapjson/gbagfx/mid2agb), or generate data (518 maps, 420
  songs, 9,300 graphics assets). Branch added `[cmake]` table +
  `ccb_step_cmake()`.
- [ ] `bug-java-extra` — No extra Java roots: SDLActivity's Java lives
  in `android/SDL2/android-project/...`, outside the app's `src/main`.
  Branch added `[paths] java_extra`.
- [ ] `bug-kotlin-required` — kotlinc is required even for Java-only
  projects. Branch: only required when Kotlin sources are present.
- [ ] `bug-javac-encoding` — javac fails on non-ASCII in a comment
  (emoji in SDLActivity.java). Branch: `javac -encoding UTF-8`.
- [ ] `bug-16kb` — Native `.so` files are not 16 KB page-size aligned.
  Evidence: Pixel 7 Pro / Android 17, mGBA fallback —
  "This app isn't 16 KB compatible. LOAD segment alignment check
  failed … lib/arm64-v8a/libmgba-jni.so : LOAD segment not aligned".
  CCB sets no `-Wl,-z,max-page-size=16384`. Same would hit
  libSDL2.so / libmain.so on arm64.
- [ ] `bug-arm-none-eabi` — No arm-none-eabi toolchain support: the
  port's data/song `.s` files need `arm-none-eabi-as/ld/objcopy`,
  which CCB never invokes. Branch works around it inside the cmake step.

## Limitations (Emerald native port, not CCB bugs)

- `limit-arm64` — True native arm64 is blocked, not just unbuilt:
  `SaveBlock1` is 16,280 bytes in 64-bit vs the fixed 15,872-byte GBA
  flash sector (`ObjectEventTemplate.script` pointer 4→8 bytes), and
  `game_data.o` is ELF32 with 48,316 32-bit relocations the aarch64
  linker rejects. Needs a pointer/save/script-format redesign.
  Full analysis: `~/workspace/pokeemerald-multiplatform/ARM64_STATUS.md`.
- `limit-32bit` — The working native build is `armeabi-v7a` only and
  will not install on 64-bit-only phones (Pixel 7 Pro: "You can't
  install the app on your device").
- `limit-generated` — Generated assets live untracked in the
  pokeemerald tree; a `git clean` there requires a full CCB rebuild
  to regenerate them.

## App ideas (showcase app, not CCB)

These live in the showcase app only, listed here so they are not lost:

- [ ] `idea-icon` — Adaptive app icon.
- [ ] `idea-anim` — Screen transition animations.
- [ ] `idea-haptics` — Haptics on the counter buttons.
