# Reproducibility contract

[Docs index](index.md) · [Build](build.md) · [Cache fingerprints](cache-fingerprints.md)

*ccb-reproducibility*

Step 10 of the remediation directive. This defines what "the same
build" means for CCB: which inputs must match for outputs to match,
what is guaranteed, and what is explicitly not guaranteed. The
guarantees are pinned by `tests/test_repro.c` (fast, no real build)
and verified end to end by `benchmarks/check-repro.sh` (two clean
builds, byte comparison).

## The guarantee

Two clean builds of the same project, on the same machine, with the
same declared inputs and the same toolchain identity (below),
produce byte-identical outputs:

- the signed APK (`build/outputs/debug/app.apk`) and, with `--aab`,
  the AAB;
- the cache manifest (`build/ccb-cache.json`);
- the lockfile (`ccb.lock`), when the project has Maven dependencies.

"Clean" means `ccb clean` first (the build directory is removed);
global caches (Maven cache, stdlib dex cache, kotlinc daemon) may be
warm. The guarantee covers the outputs, not the intermediates: a
no-op rebuild reuses cached intermediates, so intermediate file
bytes are not part of the contract.

## Declared inputs that must match

| Input | Where it is recorded | Notes |
| --- | --- | --- |
| Project sources, resources, manifest | Task fingerprints (content hashes) | See [cache-fingerprints](cache-fingerprints.md) |
| `ccb.toml` (project, paths, modules, build options) | Task fingerprints | A config change reruns the affected steps |
| `ccb.lock` (resolved Maven tree) | Enforced at build time (CCB-E3023 on mismatch) | Sorted by `group:artifact`; see below |
| Toolchain identity | `tools` in `ccb-cache.json` | `bt=<build-tools>\|aapt2=<path>\|zipalign=<path>\|kt=<kotlinc>\|java=<major>\|compile=<sdk>` (single definition: `ccb_sdk_toolchain_identity`) |
| NDK version | Native steps key it themselves | The toolchain identity deliberately does not record it |

Environment variables are not recorded as such. What a step keys
on is the *resolved* toolchain identity (tool paths and versions),
so changing `JAVA_HOME` between two JDKs of the same major version
invalidates nothing (Step 2 decision). A toolchain change
invalidates the whole cache: every step reruns.

## Why the outputs are byte-stable

- **ZIP entries.** CCB's own ZIP writer (`src/ccb_zip.c`) stores
  entries uncompressed (method 0) with a fixed DOS timestamp
  (1980-01-01 00:00:00) and no extra fields, so no wall clock ever
  enters a CCB-written archive. Entry order is insertion order.
  For assets, native libraries, and dex files the insertion order
  is the sorted order of `ccb_collect_files`, never filesystem
  order. Entries copied from the link output (`linked.ap_`) keep
  aapt2's order; see "Not guaranteed" below.
- **Lockfile.** `ccb_maven_write_lockfile` sorts entries by
  `group:artifact` before writing, so the file depends only on the
  resolved set, never on declaration order. It is written
  atomically (temp file + rename).
- **Cache manifest.** `ccb_cache_save` writes steps and file
  hashes sorted by key, so the file is byte-identical for
  equivalent caches regardless of the order entries were recorded
  in. That order is not stable in general: module step fingerprints
  merge into the cache in task-completion order, which varies
  between equivalent parallel builds (`ccb_build_module_at`).
  Load is a map lookup, so order never affects behavior.
- **Fingerprints.** A fingerprint is SHA-256 over `key=value`
  lines in insertion order, so it is order-sensitive by
  construction. Builders add keys in fixed code order, which is
  what makes equal inputs hash equal; a reordered builder would
  hash differently. `tests/test_repro.c` pins both halves: same
  order hashes equal, reordered hashes differ.
- **Signing/alignment.** `zipalign` and `apksigner` are external
  tools whose outputs are treated as deterministic for a fixed
  input and tool version; the end-to-end check below is the
  evidence for the current toolchain.

## Not guaranteed

- **Cross-machine byte-identical output.** The toolchain identity
  records absolute tool paths, so moving machines invalidates the
  cache by design, and intermediates may embed absolute paths.
  Outputs are *expected* to match across machines with the same
  tool versions, but that is not pinned or tested; only the
  same-machine case is guaranteed.
- **aapt2 link-output entry order.** The entries CCB copies out of
  `linked.ap_` keep aapt2's order, which CCB does not control. If a
  future aapt2 changed that order, APK bytes would change with it.
  The end-to-end check would catch it; CCB would not fix it by
  reordering the link output (that would change the APK's
  resource table layout assumptions).
- **External-tool determinism in general.** kotlinc, d8, R8,
  aapt2, zipalign, apksigner, cmake, and clang are opaque inputs to
  this contract: it holds only insofar as they are deterministic
  for fixed inputs. A toolchain upgrade is a declared-input change
  and invalidates the cache.
- **No `SOURCE_DATE_EPOCH`.** There is nothing for it to override:
  CCB-written timestamps are already fixed at 1980, and CCB does
  not read the variable. External tools may read it; that is
  outside this contract.
- **Timestamps inside tool outputs.** If an external tool embeds a
  wall clock (for example in a dex file or a generated class),
  CCB cannot strip it. None is known in the current toolchain;
  the end-to-end check is the tripwire.

## How to verify

- Unit level (fast, seconds): `ctest --test-dir build -L fast`
  runs `tests/test_repro.c`: two ZIP writes byte-identical, fixed
  DOS timestamp bytes pinned, entry order is insertion order,
  fingerprint order behavior, manifest save canonical under
  reordered recording, lockfile sorted output independent of
  declaration order.
- End to end (slow, two clean builds):
  `benchmarks/check-repro.sh [project-dir]` compares APK bytes,
  cache-manifest bytes, and APK entry order across two clean
  builds. It exits nonzero on any mismatch and keeps its working
  directory for inspection. Run it after any toolchain upgrade and
  after any change to the packaging, signing, or cache code.
- Benchmarks that re-run the suite: see
  [benchmarks/README.md](../benchmarks/README.md).

## Known gaps (not fixed in this step)

- **Winning-branch metadata never prefetched (Step 9
  follow-up).** Cache-only resolution can still fail with
  CCB-E3021 for a *winning* branch whose metadata was never
  prefetched. Step 9 fixed the losing-branch case (nearest-wins is
  decided before version-spec resolution, so a losing branch never
  fails the build for metadata it does not need). The winning case
  is a resolver behavior question, not a measurement one, and is
  left for a later step; no Maven code was changed here.
- **Truncated-but-non-empty `classes.jar` / `.flat` zips pass the
  output-existence check** and fail loudly downstream instead of
  triggering re-extraction (noted in
  [cache-fingerprints](cache-fingerprints.md)). A wrong-output
  risk, not a reproducibility risk: the build fails, it never
  ships the truncated file silently.
