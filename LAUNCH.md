# CCB — Launch Checklist

Get the repo from "works on my machine" to "a stranger can clone it,
build it, and contribute". Check items off as they land.

## 1. Ship the code that's already done

- [ ] Push everything since `10934ae` (HEAD on `main`, 2026-10-25).
  Pushed to the `test` branch (`DliberttiGroup/ccb`); CI green on run #3.
  Merge to `main` still pending. Work on the test branch, all verified
  green: preview Phase 2, NOTES.md bug fixes,
  dexcheck (CCB-E3025), test-speed work, config-loader split, dead tgfx
  header removal, R8 minify, static release binary, d8 heap sizing,
  kotlinc daemon (opt-in), audit refactors.
- [ ] Confirm GitHub HEAD builds clean from a fresh clone (see
  [README](README.md#install-from-source)).
- [ ] Tag the first prototype release (e.g. `v0.1.0-prototype`).

## 2. Repo hygiene

- [x] `LICENSE` — Apache 2.0, added 2026-11-25.
- [ ] Audit refactor round finished and green (plain + sanitizer 50/50):
  Maven nesting, `ccb_cmd_setup` / `ccb_cmd_preview` splits,
  `ccb_step_package` AAB split, dexcheck sorted merge, `ccb_sdk_resolve`
  split, preview nesting, preview-test mkdtemp migration.
- [ ] Docs split finished: compact human docs in `docs/`, Chinese LLM docs
  in `docs/llm/` (English header comment in each), `llms.txt` index.
- [ ] `README.md` answers in 30 seconds: what it is, one build command,
  one example app. Link to `docs/` and `llms.txt`.
- [x] Set up CI (build + `ctest` on plain and sanitizer configs) so a PR
  can't land red. Live since 2026-11-26 (`.github/workflows/ci.yml`,
  plain + sanitizer jobs, green).
- [ ] Decide public vs private: repo is currently private
  (`destroK503/ccb`). Going public is a separate, explicit step.

## 3. Known gaps (file as issues, fix after launch)

- [ ] `ccb init` scaffold gaps: no `fitsSystemWindows`, default SDK 35
  fails `ccb check` on android-36-only machines.
- [ ] AAR dex cache (extend the stdlib dex-cache pattern to AAR classes).
- [ ] Cold build <10s / incremental <2s goal in `requirement.md` vs
  measured ~19s / ~17s (daemon helps warm rebuilds; cold is still over
  target). Numbers: [docs/build.md](docs/build.md#known-limits-and-costs).
- [ ] Preview follow-ups: synthesized-text diagnostics carry fake line
  numbers, top-level functions in real Kotlin files not extracted,
  `<include>` layouts not resolved, LSP server still styles/flow only.
- [ ] Walk mode still broken in Termux.
- [ ] No library support: frameworks (e.g. Orbit) must be vendored into
  each app instead of depending on CCB. Biggest structural gap.

## 4. For a new contributor

### Setup (5 minutes)

Toolchain: [docs/setup.md](docs/setup.md). Then:

```sh
cmake -S . -B build && cmake --build build
./build/ccb version
```

### How to work

- Read `requirement.md` first — it is the spec, and the consumer is an AI
  agent driving builds from a terminal.
- Human docs: `docs/` (compact). Agent docs: `docs/llm/` (Chinese) and
  `llms.txt` at the root.
- Build a real app with it before changing it: `examples/` and
  `~/workspace/ccb-showcase` (not in this repo) are the proof.
- One change, then verify. Never claim a fix without a passing run.

### Code rules

- C17, 2-space indent, `//` comments, K&R braces,
  `-Wall -Wextra -Werror -Wpedantic`. CMake only, libc only, arena
  allocation. No `system()`, no threads, no nested ifs, max 3 indentation
  levels. Root-cause fixes only — no workarounds.
- Tests: [docs/build-tests.md](docs/build-tests.md#running-the-tests). Run sequentially,
  never concurrently.
- New behavior needs a test. A refactor needs the suite green before and
  after, with no test weakened.
- No commits from the VM are the maintainer's rule for now — check before
  pushing.

### Where to start (good first issues)

1. The scaffold gaps above — small, well-scoped, high value for new users.
2. The dexcheck sorted merge — one function, measurable.
3. Any item in `NOTES.md` under "should-change" — each is a real bug
   someone hit building a real app.
