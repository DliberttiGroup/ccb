// ccb_tui_term: the terminal connection.
#include "ccb_tui_term.h"

#include "ccb_tui_diff.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// The one active terminal (for the atexit handler and the
// signal handlers, which cannot take a context argument).
// At most one term is open at a time; open() refuses a second.
static ccb_tui_term *tui_active_term = NULL;
static int tui_atexit_installed = 0;

struct ccb_tui_term {
  int in_fd;
  int out_fd;
  int alt_screen;
  int bracketed_paste;
  int kitty_keyboard;
  int sync_output;
  ccb_tui_color_level level;
  int cols;
  int rows;
  struct termios saved;
  int termios_saved;
  int entered; // modes + raw mode currently active
  int pipe_rd;
  int pipe_wr;
  struct sigaction old_winch;
  struct sigaction old_int;
  struct sigaction old_term;
  struct sigaction old_hup;
  struct sigaction old_quit;
  struct sigaction old_tstp;
  struct sigaction old_cont;
  ccb_tui_input *input;
  ccb_tui_screen *prev; // previous frame, NULL until first present
};

void ccb_tui_term_opts_default(ccb_tui_term_opts *opts) {
  if (opts == NULL) {
    return;
  }
  opts->in_fd = -1;
  opts->out_fd = -1;
  opts->alt_screen = 1;
  opts->bracketed_paste = 1;
  opts->kitty_keyboard = 0;
  opts->sync_output = 1;
}

// Write all bytes, retrying on EINTR and short writes.
// Returns 0 on success, -1 on error.
static int tui_write_all(int fd, const char *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t m = write(fd, buf + off, n - off);
    if (m < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (m == 0) {
      return -1;
    }
    off += (size_t)m;
  }
  return 0;
}

// Write a NUL-terminated escape sequence (lengths are easy to
// miscount by hand, so they are never written literally).
static int tui_write_str(int fd, const char *s) {
  return tui_write_all(fd, s, strlen(s));
}

// Restore modes + termios. Normal context only (atexit, close,
// the Ctrl-Z dance) — never called from a signal handler.
static void tui_restore(ccb_tui_term *t) {
  if (t == NULL || !t->entered) {
    return;
  }
  t->entered = 0;
  if (t->kitty_keyboard) {
    tui_write_str(t->out_fd, "\x1b[<u");
  }
  if (t->bracketed_paste) {
    tui_write_str(t->out_fd, "\x1b[?2004l");
  }
  if (t->sync_output) {
    tui_write_str(t->out_fd, "\x1b[?2026l");
  }
  tui_write_str(t->out_fd, "\x1b[?25h");
  if (t->alt_screen) {
    tui_write_str(t->out_fd, "\x1b[?1049l");
  }
  if (t->termios_saved) {
    tcsetattr(t->in_fd, TCSANOW, &t->saved);
    t->termios_saved = 0;
  }
}

// Enter raw mode + DEC modes. Reverse of tui_restore.
static int tui_enter(ccb_tui_term *t) {
  struct termios raw;
  if (tcgetattr(t->in_fd, &t->saved) != 0) {
    return -1;
  }
  t->termios_saved = 1;
  raw = t->saved;
  cfmakeraw(&raw);
  if (tcsetattr(t->in_fd, TCSANOW, &raw) != 0) {
    return -1;
  }
  if (t->alt_screen &&
      tui_write_str(t->out_fd, "\x1b[?1049h") != 0) {
    return -1;
  }
  if (t->bracketed_paste &&
      tui_write_str(t->out_fd, "\x1b[?2004h") != 0) {
    return -1;
  }
  if (t->kitty_keyboard &&
      tui_write_str(t->out_fd, "\x1b[>3u") != 0) {
    return -1;
  }
  t->entered = 1;
  return 0;
}

static void tui_atexit_restore(void) {
  if (tui_active_term != NULL) {
    tui_restore(tui_active_term);
  }
}

// Signal handler: write one byte to the self-pipe and nothing
// else. write() is async-signal-safe; nothing else here is.
static void tui_on_signal(int sig) {
  char b = '?';
  if (tui_active_term == NULL) {
    return;
  }
  switch (sig) {
    case SIGWINCH: b = 'W'; break;
    case SIGINT:
    case SIGTERM:
    case SIGHUP:
    case SIGQUIT: b = 'S'; break;
    case SIGTSTP: b = 'T'; break;
    case SIGCONT: b = 'C'; break;
    default: break;
  }
  if (b != '?') {
    ssize_t r = write(tui_active_term->pipe_wr, &b, 1);
    (void)r; // a full pipe already signals the main loop
  }
}

static int tui_install_handlers(ccb_tui_term *t) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = tui_on_signal;
  // No SA_RESTART: select() must return EINTR so the poll loop
  // notices the pipe promptly.
  if (sigaction(SIGWINCH, &sa, &t->old_winch) != 0) {
    return -1;
  }
  if (sigaction(SIGINT, &sa, &t->old_int) != 0) {
    return -1;
  }
  if (sigaction(SIGTERM, &sa, &t->old_term) != 0) {
    return -1;
  }
  if (sigaction(SIGHUP, &sa, &t->old_hup) != 0) {
    return -1;
  }
  if (sigaction(SIGQUIT, &sa, &t->old_quit) != 0) {
    return -1;
  }
  if (sigaction(SIGTSTP, &sa, &t->old_tstp) != 0) {
    return -1;
  }
  if (sigaction(SIGCONT, &sa, &t->old_cont) != 0) {
    return -1;
  }
  return 0;
}

static void tui_remove_handlers(ccb_tui_term *t) {
  sigaction(SIGWINCH, &t->old_winch, NULL);
  sigaction(SIGINT, &t->old_int, NULL);
  sigaction(SIGTERM, &t->old_term, NULL);
  sigaction(SIGHUP, &t->old_hup, NULL);
  sigaction(SIGQUIT, &t->old_quit, NULL);
  sigaction(SIGTSTP, &t->old_tstp, NULL);
  sigaction(SIGCONT, &t->old_cont, NULL);
}

static long long tui_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

ccb_status ccb_tui_term_open(const ccb_tui_term_opts *opts,
    ccb_tui_term **out) {
  ccb_tui_term *t = NULL;
  ccb_tui_term_opts o;
  int fds[2] = { -1, -1 };
  if (opts == NULL || out == NULL) {
    return CCB_ERR_USAGE;
  }
  *out = NULL;
  if (tui_active_term != NULL) {
    return CCB_ERR_IO; // one terminal at a time
  }
  ccb_tui_term_opts_default(&o);
  o = *opts;
  t = ccb_xmalloc(sizeof(*t));
  memset(t, 0, sizeof(*t));
  t->in_fd = o.in_fd < 0 ? STDIN_FILENO : o.in_fd;
  t->out_fd = o.out_fd < 0 ? STDOUT_FILENO : o.out_fd;
  t->alt_screen = o.alt_screen;
  t->bracketed_paste = o.bracketed_paste;
  t->kitty_keyboard = o.kitty_keyboard;
  t->sync_output = o.sync_output;
  t->pipe_rd = -1;
  t->pipe_wr = -1;
  if (!isatty(t->in_fd) || !isatty(t->out_fd)) {
    free(t);
    return CCB_ERR_IO;
  }
  if (pipe(fds) != 0) {
    free(t);
    return CCB_ERR_IO;
  }
  t->pipe_rd = fds[0];
  t->pipe_wr = fds[1];
  fcntl(t->pipe_rd, F_SETFL, O_NONBLOCK);
  fcntl(t->pipe_wr, F_SETFL, O_NONBLOCK);
  tui_active_term = t;
  if (tui_install_handlers(t) != 0) {
    tui_active_term = NULL;
    close(t->pipe_rd);
    close(t->pipe_wr);
    free(t);
    return CCB_ERR_IO;
  }
  if (tui_enter(t) != 0) {
    tui_remove_handlers(t);
    tui_active_term = NULL;
    close(t->pipe_rd);
    close(t->pipe_wr);
    free(t);
    return CCB_ERR_IO;
  }
  if (!tui_atexit_installed) {
    atexit(tui_atexit_restore);
    tui_atexit_installed = 1;
  }
  t->level = ccb_tui_detect_color_level();
  t->cols = 80;
  t->rows = 24;
  ccb_tui_term_query_size(t);
  t->input = ccb_tui_input_new(t->in_fd);
  *out = t;
  return CCB_OK;
}

ccb_status ccb_tui_term_close(ccb_tui_term *t) {
  if (t == NULL) {
    return CCB_OK;
  }
  tui_restore(t);
  tui_remove_handlers(t);
  if (tui_active_term == t) {
    tui_active_term = NULL;
  }
  ccb_tui_input_free(t->input);
  ccb_tui_screen_free(t->prev);
  if (t->pipe_rd >= 0) {
    close(t->pipe_rd);
  }
  if (t->pipe_wr >= 0) {
    close(t->pipe_wr);
  }
  free(t);
  return CCB_OK;
}

int ccb_tui_term_cols(const ccb_tui_term *t) {
  return t == NULL ? 0 : t->cols;
}

int ccb_tui_term_rows(const ccb_tui_term *t) {
  return t == NULL ? 0 : t->rows;
}

ccb_tui_color_level ccb_tui_term_color_level(const ccb_tui_term *t) {
  return t == NULL ? CCB_TUI_LEVEL_NONE : t->level;
}

int ccb_tui_term_query_size(ccb_tui_term *t) {
  struct winsize ws;
  int cols = 0;
  int rows = 0;
  if (t == NULL) {
    return -1;
  }
  if (ioctl(t->out_fd, TIOCGWINSZ, &ws) != 0) {
    return -1;
  }
  // A 0x0 size (pty created before the frontend sets a size)
  // falls back to 80x24; never divide by zero on it.
  cols = ws.ws_col == 0 ? 80 : ws.ws_col;
  rows = ws.ws_row == 0 ? 24 : ws.ws_row;
  if (cols == t->cols && rows == t->rows) {
    return 0;
  }
  t->cols = cols;
  t->rows = rows;
  return 1;
}

void ccb_tui_term_invalidate(ccb_tui_term *t) {
  if (t == NULL) {
    return;
  }
  ccb_tui_screen_free(t->prev);
  t->prev = NULL;
}

ccb_status ccb_tui_term_present(ccb_tui_term *t,
    const ccb_tui_screen *next) {
  ccb_tui_screen *blank = NULL;
  ccb_str diff;
  ccb_str frame;
  ccb_status st = CCB_OK;
  if (t == NULL || next == NULL) {
    return CCB_ERR_USAGE;
  }
  ccb_tui_term_query_size(t);
  if (t->prev != NULL &&
      (ccb_tui_screen_cols(t->prev) != ccb_tui_screen_cols(next) ||
       ccb_tui_screen_rows(t->prev) != ccb_tui_screen_rows(next))) {
    ccb_tui_term_invalidate(t); // dimensions changed: full repaint
  }
  ccb_str_init(&diff);
  if (t->prev == NULL) {
    blank = ccb_tui_screen_new(ccb_tui_screen_cols(next),
      ccb_tui_screen_rows(next));
    ccb_tui_diff(blank, next, t->level, &diff);
    ccb_tui_screen_free(blank);
  } else {
    ccb_tui_diff(t->prev, next, t->level, &diff);
  }
  if (diff.len == 0) {
    ccb_str_free(&diff);
    return CCB_OK; // unchanged frame: write nothing
  }
  ccb_str_init(&frame);
  if (t->sync_output) {
    ccb_str_append(&frame, "\x1b[?2026h");
  }
  ccb_str_append(&frame, "\x1b[?25l"); // hide cursor for the frame
  ccb_str_append_n(&frame, diff.buf, diff.len);
  ccb_str_free(&diff);
  ccb_str_append(&frame, "\x1b[?25h"); // show it again at the end
  if (t->sync_output) {
    ccb_str_append(&frame, "\x1b[?2026l");
  }
  if (tui_write_all(t->out_fd, frame.buf, frame.len) != 0) {
    st = CCB_ERR_IO;
  }
  ccb_str_free(&frame);
  if (st != CCB_OK) {
    return st;
  }
  if (t->prev == NULL) {
    t->prev = ccb_tui_screen_new(ccb_tui_screen_cols(next),
      ccb_tui_screen_rows(next));
  }
  return ccb_tui_screen_copy(t->prev, next);
}

// Ctrl-Z dance (normal context only): restore the terminal,
// stop the process with the default disposition, and on
// continue re-enter raw mode + DEC modes transparently.
static void tui_stop_resume(ccb_tui_term *t) {
  struct sigaction sa;
  tui_restore(t);
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigaction(SIGTSTP, &sa, NULL);
  raise(SIGTSTP); // process stops here until continued
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = tui_on_signal;
  sigaction(SIGTSTP, &sa, NULL);
  tui_enter(t);
}

ccb_status ccb_tui_term_poll_event(ccb_tui_term *t, int timeout_ms,
    ccb_tui_event *ev) {
  long long deadline = -1;
  if (t == NULL || ev == NULL) {
    return CCB_ERR_USAGE;
  }
  memset(ev, 0, sizeof(*ev));
  ev->kind = CCB_TUI_EV_NONE;
  if (timeout_ms >= 0) {
    deadline = tui_now_ms() + timeout_ms;
  }
  for (;;) {
    ccb_status st = CCB_OK;
    long long now = 0;
    long long wait_ms = -1;
    fd_set rfds;
    struct timeval tv;
    int sel = 0;
    int maxfd = 0;
    st = ccb_tui_input_poll(t->input, 0, ev);
    if (st != CCB_OK) {
      return st;
    }
    if (ev->kind != CCB_TUI_EV_NONE) {
      return CCB_OK;
    }
    now = tui_now_ms();
    if (deadline >= 0 && now >= deadline) {
      return CCB_OK;
    }
    if (deadline >= 0) {
      wait_ms = deadline - now;
    }
    // A pending lone ESC caps the wait at its grace, so the
    // Esc key fires promptly instead of at the app's deadline.
    if (ccb_tui_input_esc_wait_ms(t->input) >= 0) {
      long esc_left = ccb_tui_input_esc_wait_ms(t->input);
      if (wait_ms < 0 || esc_left < wait_ms) {
        wait_ms = esc_left;
      }
    }
    FD_ZERO(&rfds);
    FD_SET(t->in_fd, &rfds);
    FD_SET(t->pipe_rd, &rfds);
    maxfd = t->in_fd > t->pipe_rd ? t->in_fd : t->pipe_rd;
    tv.tv_sec = (time_t)(wait_ms < 0 ? 0 : wait_ms / 1000);
    tv.tv_usec = (suseconds_t)(wait_ms < 0 ? 0 :
      (wait_ms % 1000) * 1000);
    if (wait_ms < 0) {
      sel = select(maxfd + 1, &rfds, NULL, NULL, NULL);
    } else {
      sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    }
    if (sel == 0) {
      // Timeout: a lone ESC whose grace expired becomes an
      // Esc key; otherwise loop and keep waiting.
      if (ccb_tui_input_esc_timeout(t->input, ev)) {
        return CCB_OK;
      }
      continue;
    }
    if (sel < 0) {
      if (errno == EINTR) {
        continue;
      }
      return CCB_ERR_IO;
    }
    if (FD_ISSET(t->pipe_rd, &rfds)) {
      char bytes[64];
      ssize_t m = read(t->pipe_rd, bytes, sizeof(bytes));
      ssize_t i = 0;
      if (m < 0 && errno != EAGAIN && errno != EINTR) {
        return CCB_ERR_IO;
      }
      if (m > 0) {
        for (i = 0; i < m; i++) {
          if (bytes[i] == 'W') {
            if (ccb_tui_term_query_size(t) == 1) {
              ccb_tui_term_invalidate(t);
              ev->kind = CCB_TUI_EV_RESIZE;
              return CCB_OK;
            }
          } else if (bytes[i] == 'S') {
            ev->kind = CCB_TUI_EV_INTERRUPT;
            return CCB_OK;
          } else if (bytes[i] == 'T') {
            tui_stop_resume(t);
          }
          // 'C' (continued): nothing to do, keep waiting.
        }
      }
    }
    // Input readable (or the pipe dance consumed the slot):
    // loop back and let input_poll drain it.
  }
}
