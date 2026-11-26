# Kotlinc daemon (opt-in)

[Docs index](index.md) · [Build](build.md)

*ccb-build-daemon*

Default: plain `kotlinc` subprocess. One binary, one command, one result,
no background process. Opt in to reuse a kotlinc compile daemon across
builds of the same project — warm rebuilds skip the kotlinc JVM startup.

```toml
[build]
kotlinc_daemon = true            # default false
kotlinc_daemon_idle_seconds = 300 # default 300, allowed 60-3600
```

```sh
ccb build --kotlinc-daemon      # opt in for this build (overrides config)
ccb build --no-kotlinc-daemon   # opt out for this build (overrides config)
ccb daemon stop                 # ask the project's daemon to shut down
```

Combining both flags is an error. `ccb clean` stops the daemon
best-effort (only when daemon mode is enabled in config) before removing
`build/`.

- Per-project: run files under `build/kotlin-daemon`. Two projects never
  share one; `ccb clean` removes it with `build/`.
- CCB invokes the daemon client
  (`org.jetbrains.kotlin.daemon.client.KotlinCompilerClient`) directly
  with an explicit classpath — 7 jars for the client, 6 for the compiler
  (including `kotlin-daemon.jar`) — never a `lib/*` wildcard. Missing jar
  falls back to plain kotlinc, silently.
- Client and daemon JVMs get `-Djava.net.preferIPv4Stack=true`. Don't
  remove it: the daemon handshake hangs without it.
- Both JVMs capped at 512 MB heap (`-Xmx512m`, not configurable).
- Idle shutdown needs both options:
  `--daemon-autoshutdownIdleSeconds=<idle>` and
  `--daemon-shutdownDelayMilliseconds=<idle * 1000>`. Without the
  shutdown delay the daemon exits ~1 s after the first build (the client
  exits after each build) and is never reused.
- `ccb daemon stop` asks the daemon to shut down, but the daemon still
  honors the shutdown delay: the process exits within the idle timeout,
  not instantly. Poll if you need to wait.
- The daemon client exits 0 even when compilation fails, so CCB doesn't
  trust its exit code: it parses captured output for `result code: 0`
  (success) and line-start `ERROR\t` (compile errors, reported as
  `CCB-E3004`).
- Stale or crashed daemon never fails the build: client can't connect (or
  exits without a result code and without error lines) → CCB logs a
  fallback note, runs plain kotlinc. Build fails only if plain kotlinc
  also fails.
- The daemon log may contain a `ClassNotFoundException` for the scripting
  plugin (`ScriptingK2CompilerPluginRegistrar`): benign, the scripting
  plugin isn't on the daemon classpath and CCB never compiles scripts.
- Toggling the daemon doesn't invalidate outputs: same compiler, same
  arguments, compile-code fingerprint unchanged.

Measured (2026-11-25, kotlinc 2.4.21, single runs, shared VM, ~1.5 CPUs
— noisy, treat cold numbers as a range, not a promise):

| Project | Cold, daemon off | Cold, daemon on | 1-file rebuild, off | 1-file rebuild, on |
|---|---|---|---|---|
| hello | 23–29 s | 25 s | 13.6 s | 5.2 s |
| Showcase (copy) | 34–64 s | 66–77 s | 21.6 s | 17.8 s |

Cold builds are a wash. Warm rebuilds win — the kotlinc step itself drops
from ~8 s to ~2.5 s wall.
