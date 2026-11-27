# Contributing to CCB

## Setup (5 minutes)

Toolchain: [docs/setup.md](docs/setup.md). Then:

```sh
cmake -S . -B build && cmake --build build
./build/ccb version
```

## How to work

- Read `requirement.md` first — it is the spec, and the consumer is an AI
  agent driving builds from a terminal.
- Human docs: `docs/` (compact). Agent docs: `docs/llm/` (Chinese) and
  `llms.txt` at the root.
- Build a real app with it before changing it: `examples/` are the proof.
- One change, then verify. Never claim a fix without a passing run.
- Fork, branch, open a PR. CI (plain + sanitizer jobs) must be green
  before merge.

## Code rules

- C17, 2-space indent, `//` comments, K&R braces,
  `-Wall -Wextra -Werror -Wpedantic`. CMake only, libc only, arena
  allocation. No `system()`, no threads, no nested ifs, max 3 indentation
  levels. Root-cause fixes only — no workarounds.
- Tests: [docs/build-tests.md](docs/build-tests.md#running-the-tests). Run sequentially,
  never concurrently.
- New behavior needs a test. A refactor needs the suite green before and
  after, with no test weakened.

## Where to start (good first issues)

1. The `ccb init` scaffold gaps: no `fitsSystemWindows`, default SDK 35
   fails `ccb check` on android-36-only machines.
2. The AAR dex cache — extend the stdlib dex-cache pattern to AAR classes.
3. Cold build <10s / incremental <2s goal in `requirement.md` vs measured
   ~19s / ~17s.
