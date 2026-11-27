// ccb_tui_input: byte stream -> key/paste events.
// The parser is a small state machine over a byte buffer with
// tri-state semantics (crossterm's shape): a complete event, a
// need-more-bytes, or a parse error that clears the buffer.
// A lone ESC is disambiguated by a short timeout (50 ms, the
// same value walk mode uses): if nothing follows within the
// timeout, it is an Esc key; otherwise it starts a sequence.
// Mouse reports (SGR and X10) and focus events are consumed
// and ignored in v1 — the parser never emits them as keys.
// Bracketed paste (ESC[200~ ... ESC[201~) is one paste event;
// the kitty keyboard protocol (CSI ... u) is parsed for the
// common codes (enter, tab, backspace, escape, space, letters,
// digits, Ctrl+letters) with modifiers and event types;
// functional-key codepoints above the ASCII range are dropped
// in v1 (arrows and F1-F4 arrive as legacy CSI/SS3 forms).
//
// What this module does NOT do: it never touches terminal
// modes (raw mode, alt screen, kitty push) — the term module
// owns those. It never blocks except inside poll().
#ifndef CCB_TUI_INPUT_H
#define CCB_TUI_INPUT_H

#include "ccb_common.h"
#include "ccb_tui_cell.h"

// Modifier bitmask for key events.
#define CCB_TUI_MOD_SHIFT 0x01u
#define CCB_TUI_MOD_ALT 0x02u
#define CCB_TUI_MOD_CTRL 0x04u
#define CCB_TUI_MOD_SUPER 0x08u

typedef enum {
  CCB_TUI_KEY_UNKNOWN = 0,
  CCB_TUI_KEY_CHAR, // ch holds the codepoint
  CCB_TUI_KEY_ENTER,
  CCB_TUI_KEY_TAB,
  CCB_TUI_KEY_BACKSPACE,
  CCB_TUI_KEY_ESC,
  CCB_TUI_KEY_UP,
  CCB_TUI_KEY_DOWN,
  CCB_TUI_KEY_LEFT,
  CCB_TUI_KEY_RIGHT,
  CCB_TUI_KEY_HOME,
  CCB_TUI_KEY_END,
  CCB_TUI_KEY_PAGE_UP,
  CCB_TUI_KEY_PAGE_DOWN,
  CCB_TUI_KEY_INSERT,
  CCB_TUI_KEY_DELETE,
  CCB_TUI_KEY_F1,
  CCB_TUI_KEY_F2,
  CCB_TUI_KEY_F3,
  CCB_TUI_KEY_F4,
  CCB_TUI_KEY_F5,
  CCB_TUI_KEY_F6,
  CCB_TUI_KEY_F7,
  CCB_TUI_KEY_F8,
  CCB_TUI_KEY_F9,
  CCB_TUI_KEY_F10,
  CCB_TUI_KEY_F11,
  CCB_TUI_KEY_F12
} ccb_tui_key;

typedef enum {
  CCB_TUI_EV_NONE = 0, // no event (timeout with nothing pending)
  CCB_TUI_EV_KEY,
  CCB_TUI_EV_PASTE,
  CCB_TUI_EV_RESIZE, // terminal size changed (term module only)
  CCB_TUI_EV_INTERRUPT, // SIGINT/SIGTERM/SIGHUP/SIGQUIT (term only)
  CCB_TUI_EV_EOF // input fd reached end of file
} ccb_tui_event_kind;

typedef struct {
  ccb_tui_event_kind kind;
  ccb_tui_key key;  // KEY only
  uint32_t ch;      // codepoint for CHAR keys, 0 otherwise
  uint16_t mods;    // CCB_TUI_MOD_* bitmask
  int evtype;       // 1 press, 2 repeat, 3 release (kitty); 1 otherwise
  char *paste;      // PASTE only: xmalloc'd, caller frees; NULL otherwise
} ccb_tui_event;

typedef struct ccb_tui_input ccb_tui_input;

// Create a parser. fd < 0 means feed-only (no reads; poll()
// only drains already-fed bytes). fd >= 0 is read by poll().
ccb_tui_input *ccb_tui_input_new(int fd);
void ccb_tui_input_free(ccb_tui_input *in); // NULL-safe

// Feed bytes into the parser. Returns 1 when an event was
// produced (ev filled), 0 when more bytes are needed, -1 on a
// parse error (the buffer is cleared, no event). A NULL bytes
// with n == 0 only drains already-buffered bytes. A paste
// event's text is owned by the caller (free it).
int ccb_tui_input_feed(ccb_tui_input *in, const unsigned char *bytes,
  size_t n, ccb_tui_event *ev);

// True when a lone ESC is buffered, waiting for its timeout.
int ccb_tui_input_esc_pending(ccb_tui_input *in);
// Milliseconds until a pending lone ESC becomes an Esc key:
// 0 when the grace already expired, -1 when no ESC is pending.
long ccb_tui_input_esc_wait_ms(ccb_tui_input *in);
// Turn a pending lone ESC into an Esc key event, once its
// 50 ms grace has passed. Returns 1 when an event was
// produced, 0 otherwise.
int ccb_tui_input_esc_timeout(ccb_tui_input *in, ccb_tui_event *ev);

// Wait for one event. timeout_ms < 0 blocks forever, 0 returns
// at once. Returns CCB_OK and fills ev (kind NONE when the
// timeout expired with no event, EOF on end of file), or
// CCB_ERR_IO on a read error. A feed-only parser (fd < 0)
// never blocks: it only drains buffered bytes.
ccb_status ccb_tui_input_poll(ccb_tui_input *in, int timeout_ms,
  ccb_tui_event *ev);

#endif
