// Integration tests for `ccb preview --watch` as a live loop: the real
// ccb binary is spawned on a scratch copy of examples/preview-sample
// (never the real sample, never the checked-in goldens), the initial
// render is awaited over a piped stdout, one file in the copy is
// edited, and a second render is awaited before a bounded timeout.
// Piped stdout is not a TTY, so the framing asserted here is the piped
// framing from docs/preview.md: human renders separated by exactly one
// delimiter line per re-render (none before the first), and --json
// renders as one JSON object per line with no delimiters at all.
// Flushing is proven by observing the second render's bytes while the
// child is still alive, before any kill. Every wait is deadline
// bounded and the child is killed and reaped on all paths, so no
// orphaned watch process survives the test, success or failure.
//
// Binary location: CTest passes $<TARGET_FILE:ccb> as argv[1] (see
// CMakeLists.txt, following the fuzz harnesses' argv-path convention).
// A manual run without argv[1] falls back to a sibling `ccb` next to
// the test executable (both live in the build dir). The sample dir
// derives from __FILE__ like test_preview.c, never from the cwd.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_str.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DELIM "--- ccb preview refresh ---"
#define OLD_LABEL "Text(\"My Notes\")"
#define NEW_LABEL "Text(\"WATCHED NOTES\")"
#define NEW_LABEL_TEXT "WATCHED NOTES"

// ---------- growable byte buffer (always NUL-terminated) ----------

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} outbuf;

static void buf_init(outbuf *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static void buf_free(outbuf *b) {
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static void buf_append(outbuf *b, const char *chunk, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t ncap = b->cap == 0 ? 8192 : b->cap * 2;
    while (ncap < b->len + n + 1) {
      ncap *= 2;
    }
    b->data = ccb_xrealloc(b->data, ncap);
    b->cap = ncap;
  }
  memcpy(b->data + b->len, chunk, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static int buf_contains(const outbuf *b, const char *needle) {
  return b->data != NULL && strstr(b->data, needle) != NULL;
}

// Occurrences of needle in the buffer (0 when the buffer is empty).
static size_t buf_count(const outbuf *b, const char *needle) {
  size_t n = 0;
  const char *p = NULL;
  size_t nlen = 0;
  if (b->data == NULL) {
    return 0;
  }
  nlen = strlen(needle);
  p = b->data;
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += nlen;
  }
  return n;
}

// ---------- monotonic clock + bounded pipe reads ----------

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Read one available chunk into b. Returns 1 on bytes, 0 on EOF,
// -1 on error (EINTR retried by the caller loop via poll).
static int buf_read_chunk(int fd, outbuf *b) {
  char chunk[4096];
  ssize_t n = 0;
  do {
    n = read(fd, chunk, sizeof(chunk));
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    return -1;
  }
  if (n == 0) {
    return 0;
  }
  buf_append(b, chunk, (size_t)n);
  return 1;
}

// Drain fd until needle appears or deadline_ms passes. Returns 1 when
// found, 0 on timeout, -1 on EOF/error (child died or pipe broke).
static int wait_for_substr(int fd, outbuf *b, const char *needle,
    long long deadline_ms) {
  if (buf_contains(b, needle)) {
    return 1;
  }
  for (;;) {
    long long left = deadline_ms - now_ms();
    struct pollfd pfd;
    int pr = 0;
    if (left <= 0) {
      return 0;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
      pr = poll(&pfd, 1, (int)(left > 250 ? 250 : left));
    } while (pr < 0 && errno == EINTR);
    if (pr < 0) {
      return -1;
    }
    if (pr == 0) {
      continue;
    }
    if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
      continue;
    }
    {
      int r = buf_read_chunk(fd, b);
      if (r <= 0) {
        return -1;
      }
    }
    if (buf_contains(b, needle)) {
      return 1;
    }
  }
}

// Drain fd until quiet_ms pass with no new bytes, or deadline_ms
// passes. Returns 1 on quiet, 0 on deadline, -1 on EOF/error. Used to
// capture the complete first render before asserting it has no
// delimiter: the loop only re-renders on a change, so a quiet window
// means the first render is fully flushed.
static int wait_quiet(int fd, outbuf *b, long long quiet_ms,
    long long deadline_ms) {
  long long last_bytes = now_ms();
  for (;;) {
    long long left = deadline_ms - now_ms();
    long long slice = 0;
    struct pollfd pfd;
    int pr = 0;
    if (left <= 0) {
      return 0;
    }
    if (now_ms() - last_bytes >= quiet_ms) {
      return 1;
    }
    slice = quiet_ms - (now_ms() - last_bytes);
    if (slice > left) {
      slice = left;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
      pr = poll(&pfd, 1, (int)slice);
    } while (pr < 0 && errno == EINTR);
    if (pr < 0) {
      return -1;
    }
    if (pr == 0) {
      continue;
    }
    if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
      continue;
    }
    {
      int r = buf_read_chunk(fd, b);
      if (r <= 0) {
        return -1;
      }
      last_bytes = now_ms();
    }
  }
}

// ---------- child management: kill + reap on all paths ----------

// Wait up to timeout_ms for pid to exit. Returns 1 when reaped (status
// filled), 0 on timeout (child still running).
static int wait_child_timeout(pid_t pid, long long timeout_ms, int *status) {
  long long deadline = now_ms() + timeout_ms;
  for (;;) {
    pid_t r = waitpid(pid, status, WNOHANG);
    if (r == pid) {
      return 1;
    }
    if (r < 0) {
      return 1; // already reaped or no such child: nothing left to do
    }
    if (now_ms() >= deadline) {
      return 0;
    }
    {
      struct timespec nap;
      nap.tv_sec = 0;
      nap.tv_nsec = 50 * 1000 * 1000;
      nanosleep(&nap, NULL);
    }
  }
}

// SIGTERM, then SIGKILL when the child ignores it. Always reaps: the
// caller must call this even when the scenario already failed, so a
// failed test never leaves a watch loop behind.
static void stop_child(pid_t pid) {
  int status = 0;
  if (pid <= 0) {
    return;
  }
  kill(pid, SIGTERM);
  if (wait_child_timeout(pid, 5000, &status)) {
    return;
  }
  kill(pid, SIGKILL);
  wait_child_timeout(pid, 5000, &status);
}

// ---------- paths: binary, sample dir, scratch copy ----------

// First of: argv[1], a sibling `ccb` next to this test executable.
// Returns a malloc'd path or NULL when neither is executable.
static char *find_binary(int argc, char **argv) {
  if (argc > 1 && argv[1] != NULL && access(argv[1], X_OK) == 0) {
    return ccb_xstrdup(argv[1]);
  }
  {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      char *dir = NULL;
      char *cand = NULL;
      exe[n] = '\0';
      dir = ccb_dirname(exe);
      cand = ccb_path_join(dir, "ccb");
      free(dir);
      if (access(cand, X_OK) == 0) {
        return cand;
      }
      free(cand);
    }
  }
  return NULL;
}

// Directory containing this test file, without a trailing slash.
static void test_dir(ccb_str *out) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  if (slash != NULL && slash != file) {
    ccb_str_append_n(out, file, (size_t)(slash - file));
  } else {
    ccb_str_append(out, ".");
  }
}

// Scratch copy source: the real examples/preview-sample, located like
// test_preview.c does (test dir + suffix, then cwd candidates).
// Returns a malloc'd directory path or NULL.
static char *find_sample_dir(void) {
  static const char *suffixes[] = {
    "/../examples/preview-sample/",
    "/examples/preview-sample/",
  };
  static const char *cwd_cands[] = {
    "examples/preview-sample/ccb.toml",
    "../examples/preview-sample/ccb.toml",
  };
  size_t i = 0;
  ccb_str dir;
  ccb_str_init(&dir);
  test_dir(&dir);
  for (i = 0; i < 2; i++) {
    ccb_str path;
    FILE *f = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, "ccb.toml");
    f = fopen(path.buf != NULL ? path.buf : "", "rb");
    if (f != NULL) {
      char *out = NULL;
      fclose(f);
      out = ccb_xstrdup(path.buf);
      // Strip the trailing "ccb.toml" to keep the directory.
      out[strlen(out) - strlen("ccb.toml")] = '\0';
      ccb_str_free(&path);
      ccb_str_free(&dir);
      return out;
    }
    ccb_str_free(&path);
  }
  ccb_str_free(&dir);
  for (i = 0; i < 2; i++) {
    FILE *f = fopen(cwd_cands[i], "rb");
    if (f != NULL) {
      char *out = NULL;
      fclose(f);
      out = ccb_xstrdup(cwd_cands[i]);
      out[strlen(out) - strlen("ccb.toml")] = '\0';
      return out;
    }
  }
  return NULL;
}

// Recursive copy of a directory tree (files only by content; the
// sample tree has no symlinks). Returns 1 on success.
static int copy_tree(const char *src, const char *dst) {
  DIR *d = NULL;
  struct dirent *ent = NULL;
  if (ccb_mkdir_p(dst) != CCB_OK) {
    return 0;
  }
  d = opendir(src);
  if (d == NULL) {
    return 0;
  }
  while ((ent = readdir(d)) != NULL) {
    char *spath = NULL;
    char *dpath = NULL;
    struct stat st;
    int ok = 1;
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    spath = ccb_path_join(src, ent->d_name);
    dpath = ccb_path_join(dst, ent->d_name);
    if (stat(spath, &st) != 0) {
      ok = 0;
    } else if (S_ISDIR(st.st_mode)) {
      ok = copy_tree(spath, dpath);
    } else {
      char *data = NULL;
      size_t len = 0;
      ok = ccb_read_file(spath, &data, &len) == CCB_OK &&
        ccb_write_file(dpath, data, len) == CCB_OK;
      free(data);
    }
    free(spath);
    free(dpath);
    if (!ok) {
      closedir(d);
      return 0;
    }
  }
  closedir(d);
  return 1;
}

// Replace the first occurrence of needle with replacement in a file.
// The replacement changes the file size, so the watch snapshot diff
// fires regardless of mtime granularity. Returns 1 when replaced.
static int replace_in_file(const char *path, const char *needle,
    const char *replacement) {
  char *data = NULL;
  size_t len = 0;
  char *hit = NULL;
  ccb_str out;
  int rc = 0;
  if (ccb_read_file(path, &data, &len) != CCB_OK) {
    return 0;
  }
  hit = strstr(data, needle);
  if (hit == NULL) {
    free(data);
    return 0;
  }
  ccb_str_init(&out);
  ccb_str_append_n(&out, data, (size_t)(hit - data));
  ccb_str_append(&out, replacement);
  ccb_str_append(&out, hit + strlen(needle));
  rc = ccb_write_file(path, out.buf != NULL ? out.buf : "",
    strlen(out.buf != NULL ? out.buf : "")) == CCB_OK;
  ccb_str_free(&out);
  free(data);
  return rc;
}

// Print the child's stderr log to our stderr for diagnosis.
static void dump_stderr(const char *errpath) {
  char *data = NULL;
  size_t len = 0;
  if (ccb_read_file(errpath, &data, &len) != CCB_OK) {
    return;
  }
  fprintf(stderr, "--- child stderr (%s) ---\n%.*s\n--- end ---\n",
    errpath, (int)len, data);
  free(data);
}

// Spawn `ccb preview --watch ...` in cwd, stdout to a pipe, stderr to
// errpath. On success returns the child pid and *fd_out is the pipe
// read end. Returns -1 on spawn failure.
static pid_t spawn_watch(const char *binary, const char *cwd,
    const char *const *extra, size_t nextra, const char *errpath,
    int *fd_out) {
  int fds[2];
  pid_t pid = 0;
  if (pipe(fds) != 0) {
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    int efd = 0;
    size_t i = 0;
    const char **args = NULL;
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    efd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (efd >= 0) {
      dup2(efd, STDERR_FILENO);
      if (efd != STDERR_FILENO) {
        close(efd);
      }
    }
    if (chdir(cwd) != 0) {
      _exit(127);
    }
    // argv: ccb preview --watch <extra...> (NULL-terminated).
    args = ccb_xmalloc(sizeof(char *) * (nextra + 4));
    args[0] = "ccb";
    args[1] = "preview";
    args[2] = "--watch";
    for (i = 0; i < nextra; i++) {
      args[3 + i] = extra[i];
    }
    args[3 + nextra] = NULL;
    execv(binary, (char *const *)args);
    _exit(127);
  }
  close(fds[1]);
  *fd_out = fds[0];
  return pid;
}

// Run a command that must exit promptly (the --watch conflict cases,
// which fail before any loop starts) and return its exit code, or -1
// on spawn/timeout (child killed).
static int run_exit_code(const char *binary, const char *cwd,
    const char *const *args, size_t nargs) {
  pid_t pid = fork();
  int status = 0;
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
    }
    if (chdir(cwd) != 0) {
      _exit(127);
    }
    {
      const char **argv = ccb_xmalloc(sizeof(char *) * (nargs + 2));
      size_t i = 0;
      argv[0] = "ccb";
      for (i = 0; i < nargs; i++) {
        argv[1 + i] = args[i];
      }
      argv[1 + nargs] = NULL;
      execv(binary, (char *const *)argv);
    }
    _exit(127);
  }
  if (!wait_child_timeout(pid, 10000, &status)) {
    kill(pid, SIGKILL);
    wait_child_timeout(pid, 5000, &status);
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

// ---------- scenarios ----------

// Human piped mode: initial render, delimiter framing, flush proof.
static void scenario_human(const char *binary, const char *app,
    const char *errpath) {
  static const char *extra[] = { "--ascii" };
  int fd = -1;
  pid_t pid = 0;
  outbuf buf;
  char *home = NULL;
  long long deadline = 0;
  int seen = 0;
  buf_init(&buf);
  pid = spawn_watch(binary, app, extra, 1, errpath, &fd);
  CHECK(pid > 0);
  if (pid <= 0) {
    buf_free(&buf);
    return;
  }
  // Initial render, bounded: never hang forever on a stuck child.
  deadline = now_ms() + 20000;
  seen = wait_for_substr(fd, &buf, "My Notes", deadline);
  CHECK(seen == 1);
  if (seen != 1) {
    fprintf(stderr, "FAIL: initial human render not seen in 20s\n");
    dump_stderr(errpath);
    goto human_done;
  }
  // Drain until quiet so the captured buffer is the whole first
  // render; it must contain no delimiter (none before the first).
  seen = wait_quiet(fd, &buf, 600, now_ms() + 5000);
  CHECK(seen == 1);
  CHECK(buf_count(&buf, DELIM) == 0);
  // The child must still be running: it is a watch loop, not a
  // one-shot render.
  CHECK(kill(pid, 0) == 0);
  // Edit one visible label in a flow file of the COPY.
  home = ccb_path_join(app, "flow");
  {
    char *homekt = ccb_path_join(home, "Home.kt");
    CHECK(replace_in_file(homekt, OLD_LABEL, NEW_LABEL) == 1);
    free(homekt);
  }
  free(home);
  // Second render: delimiter first, then the new label, both bounded.
  deadline = now_ms() + 20000;
  seen = wait_for_substr(fd, &buf, DELIM, deadline);
  CHECK(seen == 1);
  if (seen != 1) {
    fprintf(stderr, "FAIL: refresh delimiter not seen in 20s\n");
    dump_stderr(errpath);
    goto human_done;
  }
  deadline = now_ms() + 20000;
  seen = wait_for_substr(fd, &buf, NEW_LABEL_TEXT, deadline);
  CHECK(seen == 1);
  if (seen != 1) {
    fprintf(stderr, "FAIL: new label not seen in second render in 20s\n");
    dump_stderr(errpath);
    goto human_done;
  }
  // Exactly one re-render so far: one delimiter, not zero, not two.
  CHECK(buf_count(&buf, DELIM) == 1);
  // Flush proof: the second render's bytes were read above while the
  // child was still alive; confirm it is still alive pre-kill (a
  // child that only flushed on exit could not have produced them).
  CHECK(kill(pid, 0) == 0);
human_done:
  close(fd);
  stop_child(pid);
  buf_free(&buf);
}

// JSON piped mode: one JSON object per line, no delimiters.
static void scenario_json(const char *binary, const char *app,
    const char *errpath) {
  static const char *extra[] = { "--json" };
  int fd = -1;
  pid_t pid = 0;
  outbuf buf;
  char *home = NULL;
  char *first_nl = NULL;
  char *second_nl = NULL;
  long long deadline = 0;
  int seen = 0;
  buf_init(&buf);
  pid = spawn_watch(binary, app, extra, 1, errpath, &fd);
  CHECK(pid > 0);
  if (pid <= 0) {
    buf_free(&buf);
    return;
  }
  deadline = now_ms() + 20000;
  seen = wait_for_substr(fd, &buf, "\n", deadline);
  CHECK(seen == 1);
  if (seen != 1) {
    fprintf(stderr, "FAIL: first JSON line not seen in 20s\n");
    dump_stderr(errpath);
    goto json_done;
  }
  first_nl = strchr(buf.data, '\n');
  CHECK(first_nl != NULL);
  CHECK(buf.data[0] == '{');
  CHECK(buf_contains(&buf, "\"event\":\"preview\""));
  // Quiet window before editing, as in the human scenario: the loop
  // installs its watch and takes its baseline snapshot just after the
  // initial render, so an edit landing before that baseline would be
  // (correctly, by any watch design) invisible. The quiet window lets
  // setup complete before the edit.
  seen = wait_quiet(fd, &buf, 600, now_ms() + 5000);
  CHECK(seen == 1);
  CHECK(buf_count(&buf, DELIM) == 0);
  home = ccb_path_join(app, "flow");
  {
    char *homekt = ccb_path_join(home, "Home.kt");
    CHECK(replace_in_file(homekt, OLD_LABEL, NEW_LABEL) == 1);
    free(homekt);
  }
  free(home);
  // Wait for a second complete line (two newlines total), bounded.
  deadline = now_ms() + 20000;
  seen = 0;
  while (now_ms() < deadline) {
    if (buf_count(&buf, "\n") >= 2) {
      seen = 1;
      break;
    }
    {
      long long left = deadline - now_ms();
      struct pollfd pfd;
      int pr = 0;
      pfd.fd = fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      do {
        pr = poll(&pfd, 1, (int)(left > 250 ? 250 : left));
      } while (pr < 0 && errno == EINTR);
      if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        if (buf_read_chunk(fd, &buf) <= 0) {
          break;
        }
      }
    }
  }
  CHECK(seen == 1);
  if (seen != 1) {
    fprintf(stderr, "FAIL: second JSON line not seen in 20s\n");
    dump_stderr(errpath);
    goto json_done;
  }
  first_nl = strchr(buf.data, '\n');
  second_nl = first_nl != NULL ? strchr(first_nl + 1, '\n') : NULL;
  CHECK(first_nl != NULL && second_nl != NULL);
  if (first_nl != NULL && second_nl != NULL) {
    // Second line is one JSON object: starts with '{', the char
    // before its newline is '}', and it carries the new label.
    CHECK(first_nl[1] == '{');
    CHECK(second_nl[-1] == '}');
    CHECK(strstr(first_nl + 1, NEW_LABEL_TEXT) != NULL);
  }
  // No delimiter lines anywhere in JSONL mode.
  CHECK(buf_count(&buf, DELIM) == 0);
  CHECK(kill(pid, 0) == 0);
json_done:
  close(fd);
  stop_child(pid);
  buf_free(&buf);
}

int main(int argc, char **argv) {
  char *binary = NULL;
  char *sample = NULL;
  char root[64];
  char *app = NULL;
  char *app_json = NULL;
  char *errpath = NULL;
  char *errpath_json = NULL;
  binary = find_binary(argc, argv);
  CHECK(binary != NULL);
  if (binary == NULL) {
    fprintf(stderr, "FAIL: ccb binary not found: pass it as argv[1] "
      "(CTest does) or run next to a built ccb\n");
    TEST_MAIN_END();
  }
  sample = find_sample_dir();
  CHECK(sample != NULL);
  if (sample == NULL) {
    fprintf(stderr, "FAIL: examples/preview-sample not found\n");
    free(binary);
    TEST_MAIN_END();
  }
  ccb_test_tmpdir(root, sizeof root, "watch-loop");
  app = ccb_path_join(root, "app");
  app_json = ccb_path_join(root, "app-json");
  errpath = ccb_path_join(root, "human-stderr.log");
  errpath_json = ccb_path_join(root, "json-stderr.log");
  // Scratch copies only: the real sample and goldens are never written.
  CHECK(copy_tree(sample, app) == 1);
  CHECK(copy_tree(sample, app_json) == 1);
  scenario_human(binary, app, errpath);
  scenario_json(binary, app_json, errpath_json);
  // Watch conflicts exit 2 without entering the loop.
  {
    static const char *goldens[] = { "preview", "--watch",
      "--update-goldens" };
    static const char *walk[] = { "preview", "--watch", "--walk" };
    CHECK(run_exit_code(binary, app, goldens, 3) == 2);
    CHECK(run_exit_code(binary, app, walk, 3) == 2);
  }
  free(errpath_json);
  free(errpath);
  free(app_json);
  free(app);
  CHECK(ccb_remove_recursive(root) == CCB_OK);
  free(sample);
  free(binary);
  TEST_MAIN_END();
}
