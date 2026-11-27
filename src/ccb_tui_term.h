// ccb_tui_term: the terminal connection — the ONLY module that
// touches the terminal. It owns the raw-mode state, the alternate
// screen, the DEC modes, the resize self-pipe, the signal
// handlers, and the previous frame. Everything else in the
// library is pure data or pure bytes.
//
// Lifecycle: open() requires both fds to be TTYs, saves the
// termios state, enters raw mode and the alternate screen, and
// enables bracketed paste and (opt-in) the kitty keyboard
// protocol. close() restores everything in reverse order, even
// on signals: SIGWINCH and stop signals are delivered through a
// self-pipe (the handlers only write one byte), and an atexit
// handler restores the terminal if the process exits without
// calling close(). At most one terminal is open at a time —
// the atexit handler needs a single place to look.
//
// Presenting: present() diffs the given frame against the
// previous one (kept inside the term), wraps the bytes in DEC
// 2026 synchronised output with the cursor hidden, and issues
// one write(). An unchanged frame writes nothing. A resize (or
// a frame of different dimensions) forces a full repaint.
//
// What this module does NOT do: it does not draw anything
// (callers build screens) and does not parse input bytes (the
// input module does) — poll_event() only multiplexes the input
// fd with the self-pipe.
#ifndef CCB_TUI_TERM_H
#define CCB_TUI_TERM_H

#include "ccb_common.h"
#include "ccb_tui_input.h"
#include "ccb_tui_screen.h"
#include "ccb_tui_style.h"

typedef struct ccb_tui_term ccb_tui_term;

typedef struct {
  int in_fd;           // default STDIN_FILENO when -1
  int out_fd;          // default STDOUT_FILENO when -1
  int alt_screen;      // default 1: enter ESC[?1049h
  int bracketed_paste; // default 1: enable ESC[?2004h
  int kitty_keyboard;  // default 0: push kitty flags 1|2
  int sync_output;     // default 1: wrap frames in DEC 2026
} ccb_tui_term_opts;

// Fill opts with the defaults above.
void ccb_tui_term_opts_default(ccb_tui_term_opts *opts);

// Open the terminal. Returns CCB_OK, CCB_ERR_IO when the fds
// are not TTYs, raw mode cannot be set, or a terminal is
// already open, CCB_ERR_USAGE for NULL arguments.
ccb_status ccb_tui_term_open(const ccb_tui_term_opts *opts,
  ccb_tui_term **out);
// Restore modes and termios, free everything. NULL-safe.
ccb_status ccb_tui_term_close(ccb_tui_term *t);

int ccb_tui_term_cols(const ccb_tui_term *t);
int ccb_tui_term_rows(const ccb_tui_term *t);
ccb_tui_color_level ccb_tui_term_color_level(const ccb_tui_term *t);

// Re-read the size with TIOCGWINSZ. Returns 1 when it changed
// since the last query, 0 when unchanged, -1 on error. A 0x0
// size (pty created before the frontend sets a size) falls back
// to 80x24 and counts as a change only when it differs.
int ccb_tui_term_query_size(ccb_tui_term *t);
// Force a full repaint on the next present (front buffer
// untrusted, e.g. after Ctrl-L).
void ccb_tui_term_invalidate(ccb_tui_term *t);

// Present `next` as the current frame. Returns CCB_OK, or
// CCB_ERR_IO when the frame cannot be written, CCB_ERR_USAGE
// for NULL arguments.
ccb_status ccb_tui_term_present(ccb_tui_term *t,
  const ccb_tui_screen *next);

// Wait for one event. timeout_ms < 0 blocks forever, 0 returns
// at once. Event kinds: KEY, PASTE (from the input parser),
// EOF, plus RESIZE (a SIGWINCH arrived and the size changed)
// and INTERRUPT (SIGINT/SIGTERM/SIGHUP/SIGQUIT: the app should
// close the term and exit). Ctrl-Z is handled inside: the
// terminal is restored, the process stops, and on continue the
// terminal is re-entered transparently. Returns CCB_OK, or
// CCB_ERR_IO on a select/read error.
ccb_status ccb_tui_term_poll_event(ccb_tui_term *t, int timeout_ms,
  ccb_tui_event *ev);

#endif
