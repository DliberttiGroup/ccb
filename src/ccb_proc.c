// ccb_proc implementation: fork/exec with pipe capture.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_proc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

void ccb_proc_result_free(ccb_proc_result *r) {
  free(r->out);
  free(r->err);
  r->out = NULL;
  r->err = NULL;
}

static int ccb_env_key_len(const char *entry) {
  const char *eq = strchr(entry, '=');
  return eq == NULL ? (int)strlen(entry) : (int)(eq - entry);
}

// Build a new environment: inherited variables, with env_extra entries
// overriding same-named ones. Returns a NULL-terminated array; the caller
// frees the array (not the strings, which are borrowed).
static char **ccb_build_env(char *const env_extra[], size_t *out_count) {
  size_t base = 0;
  while (environ[base] != NULL) {
    base++;
  }
  size_t extra = 0;
  if (env_extra != NULL) {
    while (env_extra[extra] != NULL) {
      extra++;
    }
  }
  char **env = ccb_xmalloc((base + extra + 1) * sizeof(*env));
  size_t n = 0;
  for (size_t i = 0; i < base; i++) {
    int overridden = 0;
    for (size_t j = 0; j < extra; j++) {
      int kl = ccb_env_key_len(env_extra[j]);
      if (strncmp(environ[i], env_extra[j], (size_t)kl) == 0 && environ[i][kl] == '=') {
        overridden = 1;
        break;
      }
    }
    if (!overridden) {
      env[n++] = environ[i];
    }
  }
  for (size_t j = 0; j < extra; j++) {
    // Bare "K" entries remove the variable; they are not added.
    if (strchr(env_extra[j], '=') != NULL) {
      env[n++] = env_extra[j];
    }
  }
  env[n] = NULL;
  if (out_count != NULL) {
    *out_count = n;
  }
  return env;
}

ccb_status ccb_proc_run(char *const argv[], const char *cwd, char *const env_extra[], ccb_proc_result *result) {  result->exit_code = -1;
  result->signaled = 0;
  result->out = ccb_xstrdup("");
  result->err = ccb_xstrdup("");
  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    return CCB_ERR_IO;
  }
  // Close-on-exec on the parent ends so the child does not inherit them.
  fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return CCB_ERR_IO;
  }
  if (pid == 0) {
    // Child: stdin from /dev/null, stdout/stderr to the pipes.
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, 0);
    }
    dup2(out_pipe[1], 1);
    dup2(err_pipe[1], 2);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    if (cwd != NULL && chdir(cwd) != 0) {
      _exit(127);
    }
    char **env = ccb_build_env(env_extra, NULL);
    execve(argv[0], argv, env);
    _exit(127);
  }
  // Parent: close write ends, drain both pipes without deadlocking.
  close(out_pipe[1]);
  close(err_pipe[1]);
  ccb_str out_buf;
  ccb_str err_buf;
  ccb_str_init(&out_buf);
  ccb_str_init(&err_buf);
  struct pollfd fds[2];
  fds[0].fd = out_pipe[0];
  fds[0].events = POLLIN;
  fds[1].fd = err_pipe[0];
  fds[1].events = POLLIN;
  int open_count = 2;
  char chunk[4096];
  while (open_count > 0) {
    int rc = poll(fds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    for (int i = 0; i < 2; i++) {
      if (fds[i].fd < 0) {
        continue;
      }
      if ((fds[i].revents & POLLIN) != 0) {
        ssize_t n = read(fds[i].fd, chunk, sizeof(chunk));
        if (n > 0) {
          if (i == 0) {
            ccb_str_append_n(&out_buf, chunk, (size_t)n);
          } else {
            ccb_str_append_n(&err_buf, chunk, (size_t)n);
          }
        } else if (n == 0) {
          close(fds[i].fd);
          fds[i].fd = -1;
          open_count--;
        }
      } else if ((fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        ssize_t n = read(fds[i].fd, chunk, sizeof(chunk));
        if (n > 0) {
          if (i == 0) {
            ccb_str_append_n(&out_buf, chunk, (size_t)n);
          } else {
            ccb_str_append_n(&err_buf, chunk, (size_t)n);
          }
        } else {
          close(fds[i].fd);
          fds[i].fd = -1;
          open_count--;
        }
      }
    }
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      break;
    }
  }
  free(result->out);
  free(result->err);
  result->out = ccb_str_steal(&out_buf);
  result->err = ccb_str_steal(&err_buf);
  if (WIFEXITED(status)) {
    result->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result->signaled = 1;
    result->exit_code = 128 + WTERMSIG(status);
  }
  return CCB_OK;
}

static void ccb_render_arg(ccb_str *out, const char *arg) {
  int needs_quote = arg[0] == '\0';
  for (const char *p = arg; *p != '\0'; p++) {
    if (*p == ' ' || *p == '"' || *p == '\'' || *p == '$' || *p == '`' || *p == '\\') {
      needs_quote = 1;
      break;
    }
  }
  if (!needs_quote) {
    ccb_str_append(out, arg);
    return;
  }
  ccb_str_append_c(out, '\'');
  for (const char *p = arg; *p != '\0'; p++) {
    if (*p == '\'') {
      ccb_str_append(out, "'\\''");
    } else {
      ccb_str_append_c(out, *p);
    }
  }
  ccb_str_append_c(out, '\'');
}

void ccb_proc_render(char *const argv[], ccb_str *out) {
  for (int i = 0; argv[i] != NULL; i++) {
    if (i > 0) {
      ccb_str_append_c(out, ' ');
    }
    ccb_render_arg(out, argv[i]);
  }
}

int ccb_proc_run_inherit(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    execve(argv[0], argv, environ);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
}
