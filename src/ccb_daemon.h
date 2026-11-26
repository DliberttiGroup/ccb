// ccb_daemon: the opt-in kotlinc compiler daemon (opt-daemon).
//
// By default CCB runs kotlinc as a one-shot subprocess: every compile
// pays a fresh JVM start plus compiler init (~3 s of JVM startup and
// ~5 s of first-compile init on this VM). The Kotlin compiler daemon
// keeps one warm JVM per project and reuses it across builds; a warm
// one-file compile drops from ~8 s to ~2.5 s wall (measured 2026-11-25,
// hello project, this VM).
//
// The daemon is OFF by default ([build] kotlinc_daemon = false): one
// binary, one command, one result, no background process. When opted
// in, the compile step talks to the daemon through the Kotlin daemon
// client (org.jetbrains.kotlin.daemon.client.KotlinCompilerClient,
// shipped in every kotlinc distribution) instead of the kotlinc
// wrapper script. The client starts the daemon on first use, reuses
// it while it lives, and starts a fresh one when the old one is stale
// or crashed.
//
// Two details are load-bearing and must not be "cleaned up":
// - The client and daemon JVMs both get -Djava.net.preferIPv4Stack=true.
//   Without it the daemon binds a dual-stack IPv6 socket and the RMI
//   registry handshake never completes on this VM (verified: plain RMI
//   lookup to a loopback registry hangs/resets; with the flag it works).
// - The client exits 0 even when compilation fails, so the exit code
//   cannot tell a compile error from success: the caller must read the
//   "result code: N" line from the client's output instead. A daemon
//   that cannot be reached exits 1 ("Unable to connect to daemon").
//   That split is what the fallback in the pipeline relies on.
//
// Lifecycle: the daemon's run files live in <build_dir>/kotlin-daemon,
// so each project has its own daemon. It shuts itself down after
// kotlinc_daemon_idle_seconds (default 300) with no compilation, and
// `ccb daemon stop` (or `ccb clean`) shuts it down explicitly via the
// client stop option. The daemon heap is capped at 512 MB; it is not
// configurable, because the daemon compiles one small project at a time.
//
// The idle timeout is enforced with shutdownDelayMilliseconds, not
// autoshutdownIdleSeconds: the latter only applies while a client is
// still connected, and CCB's client exits after each build, so the
// daemon would otherwise shut down 1 s after every build (the
// shutdownDelay default) and no reuse would happen. Both are set to
// the configured idle value. (One benign daemon-log line remains: the
// scripting plugin is not on the daemon classpath, so the daemon logs
// a ClassNotFoundException for it at startup and carries on; CCB
// never compiles scripts.)
#ifndef CCB_DAEMON_H
#define CCB_DAEMON_H

#include "ccb_common.h"

// Daemon heap cap (MB) for both the client and daemon JVMs.
#define CCB_DAEMON_HEAP_MB 512
// Idle shutdown: default and allowed range (seconds) for
// [build] kotlinc_daemon_idle_seconds.
#define CCB_DAEMON_IDLE_DEFAULT_S 300
#define CCB_DAEMON_IDLE_MIN_S 60
#define CCB_DAEMON_IDLE_MAX_S 3600

// The daemon's run files (registry) for a project: <build_dir>/kotlin-daemon.
// Arena-allocated.
char *ccb_daemon_run_dir(ccb_arena *ar, const char *build_dir);
// The client classpath (daemon-client + compiler jars) and the
// compiler classpath the daemon itself runs with (compiler jars only),
// as colon-joined jar lists. NULL when kotlin_home is NULL or any jar
// is missing/unreadable: the caller falls back to plain kotlinc.
// Arena-allocated.
char *ccb_daemon_client_classpath(ccb_arena *ar, const char *kotlin_home);
char *ccb_daemon_compiler_classpath(ccb_arena *ar, const char *kotlin_home);
// Daemon-client argv that compiles exactly what kotlinc_argv
// (NULL-terminated, argv[0] = kotlinc path) would compile. NULL when a
// daemon jar is missing. The argv and its strings are arena-allocated.
char **ccb_daemon_compile_argv(ccb_arena *ar, const char *java,
  const char *kotlin_home, const char *run_dir, long idle_seconds,
  char *const kotlinc_argv[]);
// Daemon-client argv that shuts the daemon for run_dir down. NULL
// when a daemon jar is missing. Exits 0 even when no daemon runs.
char **ccb_daemon_stop_argv(ccb_arena *ar, const char *java,
  const char *kotlin_home, const char *run_dir);
// Classify captured daemon-client output (stdout + stderr searched):
// 1 when it reports "result code: 0" (compiled cleanly).
int ccb_daemon_output_ok(const char *out, const char *err);
// 1 when it reports a daemon-infrastructure failure (unreachable,
// crashed, or unstartable daemon), as opposed to a compile error.
int ccb_daemon_output_broken(const char *out, const char *err);
// 1 when it reports compile errors (ERROR lines from the compiler's
// message collector), as opposed to a daemon-infrastructure failure.
int ccb_daemon_output_has_errors(const char *out, const char *err);

#endif
