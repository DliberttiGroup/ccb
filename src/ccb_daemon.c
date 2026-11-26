// ccb_daemon implementation: argv + output classification for the
// opt-in kotlinc daemon. No process is spawned here; the caller runs
// the returned argv with ccb_proc_run and classifies the output.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_daemon.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include <unistd.h>

char *ccb_daemon_run_dir(ccb_arena *ar, const char *build_dir) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, build_dir);
  ccb_str_append(&s, "/kotlin-daemon");
  char *out = ccb_arena_strdup(ar, s.buf);
  ccb_str_free(&s);
  return out;
}

// Jars the daemon JVM runs with: the compiler, the daemon itself, and
// the runtime/reflect jars the compiler links against. The client needs
// one more jar (the client main class). The lists are explicit, not a
// lib/* wildcard, so dry-run output shows exactly what runs.
static const char *ccb_daemon_compiler_jars[] = {
  "kotlin-compiler.jar",
  "kotlin-daemon.jar",
  "kotlin-stdlib.jar",
  "kotlin-reflect.jar",
  "annotations-13.0.jar",
  "kotlin-script-runtime.jar",
};
static const size_t ccb_daemon_compiler_jars_n =
  sizeof(ccb_daemon_compiler_jars) / sizeof(*ccb_daemon_compiler_jars);

static char *ccb_daemon_join_jars(ccb_arena *ar, const char *kotlin_home,
    const char *const *jars, size_t njars) {
  if (kotlin_home == NULL) {
    return NULL;
  }
  ccb_str s;
  ccb_str_init(&s);
  for (size_t i = 0; i < njars; i++) {
    char *path = ccb_path_join3(kotlin_home, "lib", jars[i]);
    if (path == NULL || access(path, R_OK) != 0) {
      free(path);
      ccb_str_free(&s);
      return NULL;
    }
    if (i > 0) {
      ccb_str_append_c(&s, ':');
    }
    ccb_str_append(&s, path);
    free(path);
  }
  char *out = ccb_arena_strdup(ar, s.buf);
  ccb_str_free(&s);
  return out;
}

char *ccb_daemon_compiler_classpath(ccb_arena *ar, const char *kotlin_home) {
  return ccb_daemon_join_jars(ar, kotlin_home, ccb_daemon_compiler_jars,
    ccb_daemon_compiler_jars_n);
}

char *ccb_daemon_client_classpath(ccb_arena *ar, const char *kotlin_home) {
  static const char *jars[] = {
    "kotlin-compiler.jar",
    "kotlin-daemon.jar",
    "kotlin-daemon-client.jar",
    "kotlin-stdlib.jar",
    "kotlin-reflect.jar",
    "annotations-13.0.jar",
    "kotlin-script-runtime.jar",
  };
  return ccb_daemon_join_jars(ar, kotlin_home, jars,
    sizeof(jars) / sizeof(*jars));
}

// One argv builder for compile and stop: the shared JVM prefix
// (java, heap, IPv4, daemon JVM options, client classpath, client
// main), the client stop option when stopping, the daemon options
// (run dir, compiler classpath, idle timeout when compiling), then
// the kotlinc arguments verbatim for compile (argv[0] is the kotlinc
// path, which the client does not take). Exact-sized: counted first,
// allocated once, no shifting.
static char **ccb_daemon_argv(ccb_arena *ar, const char *java,
    const char *client_cp, const char *compiler_cp, const char *run_dir,
    long idle_seconds, int stop, char *const kotlinc_argv[]) {
  size_t nkot = 0;
  while (kotlinc_argv != NULL && kotlinc_argv[nkot] != NULL) {
    nkot++;
  }
  char idle[48];
  char delay[48];
  char heap[16];
  char jvm_opts[96];
  snprintf(heap, sizeof(heap), "-Xmx%dm", CCB_DAEMON_HEAP_MB);
  snprintf(jvm_opts, sizeof(jvm_opts),
    "-Dkotlin.daemon.jvm.options=-Xmx%dm,-Djava.net.preferIPv4Stack=true",
    CCB_DAEMON_HEAP_MB);
  if (!stop) {
    snprintf(idle, sizeof(idle), "--daemon-autoshutdownIdleSeconds=%ld",
      idle_seconds);
    snprintf(delay, sizeof(delay),
      "--daemon-shutdownDelayMilliseconds=%ld", idle_seconds * 1000);
  }
  // Prefix: java, -Xmx, preferIPv4Stack, daemon JVM options, -cp,
  // client classpath, client main = 7. Stop flag = 1 when stopping.
  // Daemon options: runFilesPath pair + compilerClasspath pair = 4,
  // plus idle + shutdown delay = 2 when compiling. Kotlinc args =
  // nkot - 1 when compiling. Plus NULL.
  size_t n = 7 + (stop ? 1 : 0) + 4 + (stop ? 0 : 2) +
    (stop ? 0 : (nkot > 0 ? nkot - 1 : 0)) + 1;
  char **argv = ccb_arena_alloc(ar, n * sizeof(*argv));
  size_t at = 0;
  argv[at++] = ccb_arena_strdup(ar, java);
  argv[at++] = ccb_arena_strdup(ar, heap);
  argv[at++] = ccb_arena_strdup(ar, "-Djava.net.preferIPv4Stack=true");
  argv[at++] = ccb_arena_strdup(ar, jvm_opts);
  if (stop) {
    argv[at++] = ccb_arena_strdup(ar,
      "-Dkotlin.daemon.client.options=stop");
  }
  argv[at++] = ccb_arena_strdup(ar, "-cp");
  argv[at++] = ccb_arena_strdup(ar, client_cp);
  argv[at++] = ccb_arena_strdup(ar,
    "org.jetbrains.kotlin.daemon.client.KotlinCompilerClient");
  argv[at++] = ccb_arena_strdup(ar, "--daemon-runFilesPath");
  argv[at++] = ccb_arena_strdup(ar, run_dir);
  argv[at++] = ccb_arena_strdup(ar, "--daemon-compilerClasspath");
  argv[at++] = ccb_arena_strdup(ar, compiler_cp);
  if (!stop) {
    argv[at++] = ccb_arena_strdup(ar, idle);
    argv[at++] = ccb_arena_strdup(ar, delay);
    for (size_t i = 1; i < nkot; i++) {
      argv[at++] = ccb_arena_strdup(ar, kotlinc_argv[i]);
    }
  }
  argv[at] = NULL;
  return argv;
}

char **ccb_daemon_compile_argv(ccb_arena *ar, const char *java,
    const char *kotlin_home, const char *run_dir, long idle_seconds,
    char *const kotlinc_argv[]) {
  if (java == NULL || kotlinc_argv == NULL || kotlinc_argv[0] == NULL) {
    return NULL;
  }
  char *client_cp = ccb_daemon_client_classpath(ar, kotlin_home);
  char *compiler_cp = ccb_daemon_compiler_classpath(ar, kotlin_home);
  if (client_cp == NULL || compiler_cp == NULL) {
    return NULL;
  }
  return ccb_daemon_argv(ar, java, client_cp, compiler_cp, run_dir,
    idle_seconds, 0, kotlinc_argv);
}

char **ccb_daemon_stop_argv(ccb_arena *ar, const char *java,
    const char *kotlin_home, const char *run_dir) {
  if (java == NULL) {
    return NULL;
  }
  char *client_cp = ccb_daemon_client_classpath(ar, kotlin_home);
  char *compiler_cp = ccb_daemon_compiler_classpath(ar, kotlin_home);
  if (client_cp == NULL || compiler_cp == NULL) {
    return NULL;
  }
  // The stop flag is a client option, read from the system property
  // kotlin.daemon.client.options (comma-separated): there is no
  // --daemon-stop argument.
  return ccb_daemon_argv(ar, java, client_cp, compiler_cp, run_dir,
    0, 1, NULL);
}

static int ccb_daemon_contains(const char *text, const char *needle) {
  return text != NULL && strstr(text, needle) != NULL;
}

int ccb_daemon_output_ok(const char *out, const char *err) {
  return ccb_daemon_contains(out, "result code: 0") ||
    ccb_daemon_contains(err, "result code: 0");
}

int ccb_daemon_output_broken(const char *out, const char *err) {
  static const char *markers[] = {
    "Unable to connect to daemon",
    "Failed connecting to the daemon",
    "The daemon has terminated unexpectedly",
    "Could not find or load main class",
  };
  for (size_t i = 0; i < sizeof(markers) / sizeof(*markers); i++) {
    if (ccb_daemon_contains(out, markers[i]) ||
        ccb_daemon_contains(err, markers[i])) {
      return 1;
    }
  }
  return 0;
}

int ccb_daemon_output_has_errors(const char *out, const char *err) {
  // The daemon's message collector renders each compile error as a
  // line starting with "ERROR\t". Match at a line start only, so the
  // text of a source file cannot fake it.
  const char *texts[2] = { out, err };
  for (size_t t = 0; t < 2; t++) {
    const char *s = texts[t];
    if (s == NULL) {
      continue;
    }
    if (strncmp(s, "ERROR\t", 6) == 0) {
      return 1;
    }
    const char *nl = strchr(s, '\n');
    while (nl != NULL) {
      if (strncmp(nl + 1, "ERROR\t", 6) == 0) {
        return 1;
      }
      nl = strchr(nl + 1, '\n');
    }
  }
  return 0;
}
