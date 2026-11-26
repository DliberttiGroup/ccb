<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/build-tests.md. -->

# 运行测试

[文档索引](index.md) · [构建](build.md)

先构建 CCB（[README](../../README.md#install-from-source)）。然后：

```
ctest --test-dir build -L fast     # quick gate: 43 tests, ~16s quiet
                                   # (~45s when the VM is busy)
ctest --test-dir build -L <area>   # one area: config, setup, pipeline,
                                   # preview, misc, dexcheck, daemon,
                                   # maven, modules
ctest --test-dir build -LE slow    # same as -L fast
ctest --test-dir build             # full suite: 50 tests; runs in CI
```

每个测试有两个标签：速度（`fast` / `slow`）和领域。Fast 表示没有真实的 Android 构建，每个只需数秒。Slow 表示一次真实的 `ccb build` 或一次超时等待。每次改动后，运行 fast 加受影响的领域。推送之前，完整套件在 GitHub Actions（`.github/workflows/ci.yml`，plain + sanitizer 两个 job）中运行——不要在虚拟机上运行它。

Slow 测试，上次完整运行（2026-11-25）：`test_modules` 450s、`test_kotlinc_daemon` 330s、`test_dex_contents` 199s、`test_r8_minify` 133s、`test_aab` 88s、`test_maven` 82s、`test_dexcheck` 49s。领域合计：pipeline 约 7 分钟，modules 约 7.5 分钟，daemon 约 5.5 分钟，maven 约 1.5 分钟，dexcheck 约 1 分钟。

绝不要同时运行两次 `ctest` 调用（plain + sanitizer，或两次 plain）：请依次运行。每个测试二进制使用各自进程的 `mkdtemp` 目录（`/tmp/ccb_test_<name>_XXXXXX`），因此一次 `ctest --jN` 运行内的并行测试不会冲突——但两次并发的 `ctest` 运行仍会共享 CPU 和内存。在这台虚拟机上 `-j4` plain 实测为 1037s，而顺序运行为 739s，`-j2` 为 1082s（2026-11-25，47 个测试）：瓶颈是 CPU 密集的 JVM 工作（kotlinc + d8），而非串行化。共 50 个测试。
