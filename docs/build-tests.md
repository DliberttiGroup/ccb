# Running the tests

[Docs index](index.md) · [Build](build.md)

*ccb-build-tests*

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
totals: pipeline ~7 min, modules ~7.5 min, daemon ~5.5 min, maven
~1.5 min, dexcheck ~1 min.

Never run two `ctest` invocations at once (plain + sanitizer, or two plain
runs): run them sequentially. Each test binary uses a per-process `mkdtemp`
dir (`/tmp/ccb_test_<name>_XXXXXX`), so parallel tests within one `ctest
--jN` run can't collide — but two concurrent `ctest` runs still share CPU
and memory. On this VM `-j4` plain measured 1037s vs 739s sequential, `-j2`
1082s (2026-11-25, 47 tests): CPU-bound JVM work (kotlinc + d8), not
serialization. 50 tests total.
