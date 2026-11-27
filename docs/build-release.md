# Release binary (static + stripped)

[Docs index](index.md) · [Build](build.md)

*ccb-build-release*

The shippable binary is the release build: `ccb` linked with `-static`,
stripped. Dev and sanitizer builds stay dynamic and unstripped; the two
options are mutually exclusive in CMake (configuring both fails).

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
static binary is ~1.27 MB. Both are under the 5 MB limit in
requirement.md §8. Verify with:

```
file build-release/ccb   # statically linked, stripped
ldd build-release/ccb    # not a dynamic executable
```
