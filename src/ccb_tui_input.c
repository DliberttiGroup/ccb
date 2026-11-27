// ccb_tui_input: byte stream -> key/paste events.
#include "ccb_tui_input.h"

#include "ccb_alloc.h"
#include "ccb_str.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// Lone-ESC grace: if nothing follows within this long, a buffered
// ESC is an Esc key. Same value walk mode uses.
#define TUI_ESC_TIMEOUT_MS 50
// Parser input buffer: one read chunk (256) plus leftovers.
#define TUI_IN_BUF 1024
#define TUI_PASTE_END "\x1b[201~"

struct ccb_tui_input {
  int fd;
  unsigned char buf[TUI_IN_BUF];
  size_t len;
  int in_paste;
  ccb_str paste;
  long long esc_since_ms; // -1 when no lone ESC is pending
};

// Forward declarations: the CSI dispatcher calls these two.
static int tui_tilde_dispatch(ccb_tui_input *in, int p0, int p1,
  ccb_tui_event *ev);
static int tui_kitty_dispatch(ccb_tui_input *in,
  int nf, const unsigned char **fields, size_t *flens,
  ccb_tui_event *ev);
// Forward declarations: the parse step calls these three.
static int tui_parse_esc(ccb_tui_input *in, ccb_tui_event *ev);
static int tui_parse_csi(ccb_tui_input *in, ccb_tui_event *ev);
static int tui_parse_control(ccb_tui_input *in, ccb_tui_event *ev,
  unsigned char b0);
static int tui_parse_char(ccb_tui_input *in, ccb_tui_event *ev);

// Parse-step outcomes.
#define TUI_STEP_EVENT 1
#define TUI_STEP_MORE 0
#define TUI_STEP_ERROR -1
#define TUI_STEP_CONSUMED 2

static long long tui_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

ccb_tui_input *ccb_tui_input_new(int fd) {
  ccb_tui_input *in = ccb_xmalloc(sizeof(*in));
  memset(in, 0, sizeof(*in));
  in->fd = fd;
  in->esc_since_ms = -1;
  ccb_str_init(&in->paste);
  return in;
}

void ccb_tui_input_free(ccb_tui_input *in) {
  if (in == NULL) {
    return;
  }
  ccb_str_free(&in->paste);
  free(in);
}

static void tui_buf_consume(ccb_tui_input *in, size_t n) {
  if (n == 0) {
    return;
  }
  if (n >= in->len) {
    in->len = 0;
    return;
  }
  memmove(in->buf, in->buf + n, in->len - n);
  in->len -= n;
}

static void tui_make_key(ccb_tui_event *ev, ccb_tui_key key,
    uint32_t ch, uint16_t mods, int evtype) {
  memset(ev, 0, sizeof(*ev));
  ev->kind = CCB_TUI_EV_KEY;
  ev->key = key;
  ev->ch = ch;
  ev->mods = mods;
  ev->evtype = evtype;
}

// Modifier bits from a CSI parameter (1 + bitmask: Shift 1,
// Alt 2, Ctrl 4, Super 8). p <= 1 means no modifiers.
static uint16_t tui_mods_from_param(int p) {
  uint16_t mods = 0;
  int bits = p - 1;
  if (bits <= 0) {
    return 0;
  }
  if ((bits & 1) != 0) {
    mods |= CCB_TUI_MOD_SHIFT;
  }
  if ((bits & 2) != 0) {
    mods |= CCB_TUI_MOD_ALT;
  }
  if ((bits & 4) != 0) {
    mods |= CCB_TUI_MOD_CTRL;
  }
  if ((bits & 8) != 0) {
    mods |= CCB_TUI_MOD_SUPER;
  }
  return mods;
}

// Parse one ';'-separated integer field; stops at ':' (kitty
// sub-parameters) or ';'. Returns the value; 0 when no digits.
static int tui_field_int(const unsigned char *p, size_t n) {
  int v = 0;
  size_t i = 0;
  while (i < n && p[i] >= '0' && p[i] <= '9') {
    v = v * 10 + (p[i] - '0');
    i++;
  }
  return v;
}

// Split params on ';' into at most max fields. fields[i] is the
// field start, flens[i] its length. Returns the field count.
static int tui_split_fields(const unsigned char *p, size_t n,
    const unsigned char **fields, size_t *flens, int max) {
  int count = 0;
  size_t i = 0;
  if (n == 0) {
    return 0;
  }
  while (count < max) {
    size_t start = i;
    while (i < n && p[i] != ';') {
      i++;
    }
    fields[count] = p + start;
    flens[count] = i - start;
    count++;
    if (i >= n) {
      break;
    }
    i++; // skip ';'
  }
  return count;
}

static ccb_tui_key tui_fkey_from_param(int p) {
  static const struct {
    int param;
    ccb_tui_key key;
  } map[] = {
    { 11, CCB_TUI_KEY_F1 }, { 12, CCB_TUI_KEY_F2 },
    { 13, CCB_TUI_KEY_F3 }, { 14, CCB_TUI_KEY_F4 },
    { 15, CCB_TUI_KEY_F5 }, { 17, CCB_TUI_KEY_F6 },
    { 18, CCB_TUI_KEY_F7 }, { 19, CCB_TUI_KEY_F8 },
    { 20, CCB_TUI_KEY_F9 }, { 21, CCB_TUI_KEY_F10 },
    { 23, CCB_TUI_KEY_F11 }, { 24, CCB_TUI_KEY_F12 },
  };
  size_t i = 0;
  for (i = 0; i < CCB_ARRAY_LEN(map); i++) {
    if (map[i].param == p) {
      return map[i].key;
    }
  }
  return CCB_TUI_KEY_UNKNOWN;
}

// Dispatch one CSI sequence (params p[0..n), final byte fin).
// Fills ev on an event. Returns a TUI_STEP_* outcome. A paste
// start sets in->in_paste; the caller then routes the rest of
// the buffered bytes to the paste accumulator.
static int tui_csi_dispatch(ccb_tui_input *in,
    const unsigned char *p, size_t n, unsigned char fin,
    ccb_tui_event *ev) {
  const unsigned char *fields[4];
  size_t flens[4];
  int nf = 0;
  int p0 = 0;
  int p1 = 1;
  if (fin == 'M' && n == 0) {
    // X10 mouse: ESC[M plus 3 coordinate bytes, consumed and
    // ignored in v1. The caller guarantees 6 bytes buffered.
    return TUI_STEP_CONSUMED;
  }
  if (n > 0 && p[0] == '<') {
    return TUI_STEP_CONSUMED; // SGR mouse: consumed, ignored
  }
  if (fin == 'I' || fin == 'O') {
    return TUI_STEP_CONSUMED; // focus in/out: ignored
  }
  nf = tui_split_fields(p, n, fields, flens, 4);
  if (nf > 0) {
    p0 = tui_field_int(fields[0], flens[0]);
  }
  if (nf > 1) {
    p1 = tui_field_int(fields[1], flens[1]);
  }
  switch (fin) {
    case 'A':
      tui_make_key(ev, CCB_TUI_KEY_UP, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case 'B':
      tui_make_key(ev, CCB_TUI_KEY_DOWN, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case 'C':
      tui_make_key(ev, CCB_TUI_KEY_RIGHT, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case 'D':
      tui_make_key(ev, CCB_TUI_KEY_LEFT, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case 'H':
      tui_make_key(ev, CCB_TUI_KEY_HOME, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case 'F':
      tui_make_key(ev, CCB_TUI_KEY_END, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case 'Z':
      tui_make_key(ev, CCB_TUI_KEY_TAB, 0, CCB_TUI_MOD_SHIFT, 1);
      return TUI_STEP_EVENT;
    case 'P':
      tui_make_key(ev, CCB_TUI_KEY_F1, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case 'Q':
      tui_make_key(ev, CCB_TUI_KEY_F2, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case 'R':
      // Kitty legacy F3 is "1" or "1;mods"; anything else is a
      // cursor-position report: dropped.
      if (nf == 0 || p0 == 1) {
        tui_make_key(ev, CCB_TUI_KEY_F3, 0,
          tui_mods_from_param(p1), 1);
        return TUI_STEP_EVENT;
      }
      return TUI_STEP_CONSUMED;
    case 'S':
      tui_make_key(ev, CCB_TUI_KEY_F4, 0,
        tui_mods_from_param(p1), 1);
      return TUI_STEP_EVENT;
    case '~':
      return tui_tilde_dispatch(in, p0, p1, ev);
    case 'u':
      return tui_kitty_dispatch(in, nf, fields, flens, ev);
    default:
      return TUI_STEP_CONSUMED; // unknown CSI: dropped
  }
}

// CSI ... ~ dispatch: p0 selects the key, p1 the modifiers.
static int tui_tilde_dispatch(ccb_tui_input *in, int p0, int p1,
    ccb_tui_event *ev) {
  uint16_t mods = tui_mods_from_param(p1);
  ccb_tui_key key = CCB_TUI_KEY_UNKNOWN;
  if (p0 == 200) {
    in->in_paste = 1;
    ccb_str_free(&in->paste);
    ccb_str_init(&in->paste);
    return TUI_STEP_CONSUMED;
  }
  if (p0 == 201) {
    return TUI_STEP_CONSUMED; // stray paste end: ignored
  }
  switch (p0) {
    case 1: key = CCB_TUI_KEY_HOME; break;
    case 2: key = CCB_TUI_KEY_INSERT; break;
    case 3: key = CCB_TUI_KEY_DELETE; break;
    case 4: key = CCB_TUI_KEY_END; break;
    case 5: key = CCB_TUI_KEY_PAGE_UP; break;
    case 6: key = CCB_TUI_KEY_PAGE_DOWN; break;
    default: key = tui_fkey_from_param(p0); break;
  }
  if (key == CCB_TUI_KEY_UNKNOWN) {
    return TUI_STEP_CONSUMED;
  }
  tui_make_key(ev, key, 0, mods, 1);
  return TUI_STEP_EVENT;
}

// Kitty keyboard CSI ... u dispatch. fields[0] is the key code
// (with ':' sub-parameters for shifted/base keys), fields[1]
// is "modifiers[:event-type]", fields[2] is associated text
// (ignored). Only the common codes are mapped; functional
// codepoints above ASCII are dropped in v1.
static int tui_kitty_dispatch(ccb_tui_input *in,
    int nf, const unsigned char **fields, size_t *flens,
    ccb_tui_event *ev) {
  int code = 0;
  int mods = 1;
  int evtype = 1;
  size_t i = 0;
  CCB_UNUSED(in);
  if (nf == 0) {
    return TUI_STEP_CONSUMED;
  }
  code = tui_field_int(fields[0], flens[0]);
  if (nf > 1) {
    // "modifiers" or "modifiers:event-type".
    mods = tui_field_int(fields[1], flens[1]);
    for (i = 0; i < flens[1]; i++) {
      if (fields[1][i] == ':') {
        evtype = tui_field_int(fields[1] + i + 1,
          flens[1] - i - 1);
        break;
      }
    }
  }
  if (evtype < 1 || evtype > 3) {
    evtype = 1;
  }
  switch (code) {
    case 13:
      tui_make_key(ev, CCB_TUI_KEY_ENTER, 0,
        tui_mods_from_param(mods), evtype);
      return TUI_STEP_EVENT;
    case 9:
      tui_make_key(ev, CCB_TUI_KEY_TAB, 0,
        tui_mods_from_param(mods), evtype);
      return TUI_STEP_EVENT;
    case 127:
      tui_make_key(ev, CCB_TUI_KEY_BACKSPACE, 0,
        tui_mods_from_param(mods), evtype);
      return TUI_STEP_EVENT;
    case 27:
      tui_make_key(ev, CCB_TUI_KEY_ESC, 0,
        tui_mods_from_param(mods), evtype);
      return TUI_STEP_EVENT;
    default:
      break;
  }
  if (code >= 32 && code <= 126) {
    tui_make_key(ev, CCB_TUI_KEY_CHAR, (uint32_t)code,
      tui_mods_from_param(mods), evtype);
    return TUI_STEP_EVENT;
  }
  if (code >= 1 && code <= 26) {
    tui_make_key(ev, CCB_TUI_KEY_CHAR, (uint32_t)('a' + code - 1),
      (uint16_t)(tui_mods_from_param(mods) | CCB_TUI_MOD_CTRL),
      evtype);
    return TUI_STEP_EVENT;
  }
  return TUI_STEP_CONSUMED;
}

// SS3 dispatch (ESC O x): arrows, Home/End, F1-F4.
static int tui_ss3_dispatch(unsigned char c, ccb_tui_event *ev) {
  ccb_tui_key key = CCB_TUI_KEY_UNKNOWN;
  switch (c) {
    case 'A': key = CCB_TUI_KEY_UP; break;
    case 'B': key = CCB_TUI_KEY_DOWN; break;
    case 'C': key = CCB_TUI_KEY_RIGHT; break;
    case 'D': key = CCB_TUI_KEY_LEFT; break;
    case 'H': key = CCB_TUI_KEY_HOME; break;
    case 'F': key = CCB_TUI_KEY_END; break;
    case 'P': key = CCB_TUI_KEY_F1; break;
    case 'Q': key = CCB_TUI_KEY_F2; break;
    case 'R': key = CCB_TUI_KEY_F3; break;
    case 'S': key = CCB_TUI_KEY_F4; break;
    default: break;
  }
  if (key == CCB_TUI_KEY_UNKNOWN) {
    return TUI_STEP_CONSUMED;
  }
  tui_make_key(ev, key, 0, 0, 1);
  return TUI_STEP_EVENT;
}

// Parse one step from the buffer. Returns a TUI_STEP_* outcome.
static int tui_parse_step(ccb_tui_input *in, ccb_tui_event *ev) {
  unsigned char b0 = 0;
  if (in->len == 0) {
    return TUI_STEP_MORE;
  }
  b0 = in->buf[0];
  if (b0 == 0x1B) {
    return tui_parse_esc(in, ev);
  }
  in->esc_since_ms = -1;
  if (b0 < 0x20 || b0 == 0x7F) {
    return tui_parse_control(in, ev, b0);
  }
  return tui_parse_char(in, ev);
}

// ESC-prefixed input: sequences, Alt-chords, or a lone ESC.
static int tui_parse_esc(ccb_tui_input *in, ccb_tui_event *ev) {
  unsigned char b1 = 0;
  if (in->len == 1) {
    if (in->esc_since_ms < 0) {
      in->esc_since_ms = tui_now_ms();
    }
    return TUI_STEP_MORE;
  }
  in->esc_since_ms = -1;
  b1 = in->buf[1];
  if (b1 == 0x1B) {
    tui_buf_consume(in, 2);
    tui_make_key(ev, CCB_TUI_KEY_ESC, 0, 0, 1);
    return TUI_STEP_EVENT;
  }
  if (b1 == '[') {
    return tui_parse_csi(in, ev);
  }
  if (b1 == 'O') {
    int r = 0;
    if (in->len < 3) {
      return TUI_STEP_MORE;
    }
    r = tui_ss3_dispatch(in->buf[2], ev);
    tui_buf_consume(in, 3);
    return r;
  }
  if (b1 >= 0x80) {
    // ESC + multibyte char: an Alt-chord with a UTF-8 char.
    uint32_t cp = 0;
    size_t used = ccb_tui_utf8_decode((const char *)in->buf + 1,
      in->len - 1, &cp);
    if (used == 0) {
      return TUI_STEP_MORE;
    }
    if (used == (size_t)-1) {
      tui_buf_consume(in, 1); // drop the ESC, retry the byte
      tui_make_key(ev, CCB_TUI_KEY_ESC, 0, 0, 1);
      return TUI_STEP_EVENT;
    }
    tui_buf_consume(in, 1 + used);
    tui_make_key(ev, CCB_TUI_KEY_CHAR, cp, CCB_TUI_MOD_ALT, 1);
    return TUI_STEP_EVENT;
  }
  if (b1 < 0x20) {
    // ESC + control byte: Alt+Ctrl+letter.
    uint32_t ch = b1 == 0 ? ' ' : (uint32_t)(b1 + 0x60);
    tui_buf_consume(in, 2);
    tui_make_key(ev, CCB_TUI_KEY_CHAR, ch,
      (uint16_t)(CCB_TUI_MOD_ALT | CCB_TUI_MOD_CTRL), 1);
    return TUI_STEP_EVENT;
  }
  tui_buf_consume(in, 2);
  tui_make_key(ev, CCB_TUI_KEY_CHAR, b1, CCB_TUI_MOD_ALT, 1);
  return TUI_STEP_EVENT;
}

// CSI: ESC [ params... intermediates... final. X10 mouse
// (ESC[M + 3 bytes) needs 6 bytes buffered before dispatch.
static int tui_parse_csi(ccb_tui_input *in, ccb_tui_event *ev) {
  size_t i = 2;
  int r = 0;
  while (i < in->len) {
    unsigned char c = in->buf[i];
    if ((c >= 0x30 && c <= 0x3F) || (c >= 0x20 && c <= 0x2F)) {
      i++;
      continue;
    }
    if (c >= 0x40 && c <= 0x7E) {
      break; // final byte at i
    }
    return TUI_STEP_ERROR;
  }
  if (i >= in->len) {
    if (in->len >= sizeof(in->buf)) {
      return TUI_STEP_ERROR;
    }
    return TUI_STEP_MORE;
  }
  if (in->buf[i] == 'M' && i == 2 && in->len < 6) {
    return TUI_STEP_MORE; // X10 mouse: wait for the 3 coord bytes
  }
  r = tui_csi_dispatch(in, in->buf + 2, i - 2, in->buf[i], ev);
  if (in->buf[i] == 'M' && i == 2) {
    // X10 mouse: the final byte plus its 3 coordinate bytes.
    tui_buf_consume(in, 6);
  } else {
    tui_buf_consume(in, i + 1);
  }
  return r;
}

// Control bytes and DEL in ground state.
static int tui_parse_control(ccb_tui_input *in, ccb_tui_event *ev,
    unsigned char b0) {
  tui_buf_consume(in, 1);
  switch (b0) {
    case '\r':
      tui_make_key(ev, CCB_TUI_KEY_ENTER, 0, 0, 1);
      return TUI_STEP_EVENT;
    case '\t':
      tui_make_key(ev, CCB_TUI_KEY_TAB, 0, 0, 1);
      return TUI_STEP_EVENT;
    case 0x7F:
    case 0x08:
      tui_make_key(ev, CCB_TUI_KEY_BACKSPACE, 0, 0, 1);
      return TUI_STEP_EVENT;
    case 0x00:
      tui_make_key(ev, CCB_TUI_KEY_CHAR, ' ', CCB_TUI_MOD_CTRL, 1);
      return TUI_STEP_EVENT;
    default:
      break;
  }
  if (b0 >= 0x01 && b0 <= 0x1A) {
    tui_make_key(ev, CCB_TUI_KEY_CHAR, (uint32_t)('a' + b0 - 1),
      CCB_TUI_MOD_CTRL, 1);
    return TUI_STEP_EVENT;
  }
  if (b0 >= 0x1C && b0 <= 0x1F) {
    tui_make_key(ev, CCB_TUI_KEY_CHAR, (uint32_t)(b0 + 0x40),
      CCB_TUI_MOD_CTRL, 1);
    return TUI_STEP_EVENT;
  }
  return TUI_STEP_CONSUMED; // other C0 controls: dropped
}

// A printable (possibly multibyte) character in ground state.
static int tui_parse_char(ccb_tui_input *in, ccb_tui_event *ev) {
  uint32_t cp = 0;
  size_t used = ccb_tui_utf8_decode((const char *)in->buf, in->len,
    &cp);
  if (used == 0) {
    return TUI_STEP_MORE;
  }
  if (used == (size_t)-1) {
    tui_buf_consume(in, 1);
    tui_make_key(ev, CCB_TUI_KEY_CHAR, 0xFFFD, 0, 1);
    return TUI_STEP_EVENT;
  }
  tui_buf_consume(in, used);
  tui_make_key(ev, CCB_TUI_KEY_CHAR, cp, 0, 1);
  return TUI_STEP_EVENT;
}

// Scan bytes into the paste accumulator. Returns 1 with a PASTE
// event when the ESC[201~ terminator arrives (*consumed is the
// bytes used, including the terminator); 0 when all n bytes were
// consumed into the accumulator. The terminator itself is never
// part of the pasted text.
static int tui_paste_scan(ccb_tui_input *in,
    const unsigned char *bytes, size_t n, ccb_tui_event *ev,
    size_t *consumed) {
  size_t i = 0;
  size_t term_len = sizeof(TUI_PASTE_END) - 1;
  *consumed = n;
  for (i = 0; i < n; i++) {
    ccb_str_append_c(&in->paste, (char)bytes[i]);
    if (in->paste.len >= term_len &&
        memcmp(in->paste.buf + in->paste.len - term_len,
          TUI_PASTE_END, term_len) == 0) {
      in->paste.len -= term_len;
      in->paste.buf[in->paste.len] = '\0';
      memset(ev, 0, sizeof(*ev));
      ev->kind = CCB_TUI_EV_PASTE;
      ev->paste = ccb_str_steal(&in->paste);
      ccb_str_init(&in->paste);
      in->in_paste = 0;
      *consumed = i + 1;
      return 1;
    }
  }
  return 0;
}

int ccb_tui_input_feed(ccb_tui_input *in, const unsigned char *bytes,
    size_t n, ccb_tui_event *ev) {
  int r = 0;
  if (in == NULL || ev == NULL) {
    return -1;
  }
  if (bytes != NULL && n > 0) {
    if (in->len + n > sizeof(in->buf)) {
      in->len = 0;
      return -1;
    }
    memcpy(in->buf + in->len, bytes, n);
    in->len += n;
  }
  for (;;) {
    if (in->in_paste) {
      size_t consumed = 0;
      if (in->len == 0) {
        return 0;
      }
      r = tui_paste_scan(in, in->buf, in->len, ev, &consumed);
      tui_buf_consume(in, consumed);
      return r;
    }
    r = tui_parse_step(in, ev);
    if (r == TUI_STEP_EVENT || r == TUI_STEP_MORE) {
      return r;
    }
    if (r == TUI_STEP_ERROR) {
      in->len = 0;
      return -1;
    }
    // TUI_STEP_CONSUMED: keep parsing the rest of the buffer.
  }
}

int ccb_tui_input_esc_pending(ccb_tui_input *in) {
  return in != NULL && in->len == 1 && in->buf[0] == 0x1B &&
    in->in_paste == 0;
}

long ccb_tui_input_esc_wait_ms(ccb_tui_input *in) {
  long left = 0;
  if (!ccb_tui_input_esc_pending(in)) {
    return -1;
  }
  if (in->esc_since_ms < 0) {
    return 0;
  }
  left = (long)(in->esc_since_ms + TUI_ESC_TIMEOUT_MS - tui_now_ms());
  return left < 0 ? 0 : left;
}

int ccb_tui_input_esc_timeout(ccb_tui_input *in, ccb_tui_event *ev) {
  if (in == NULL || ev == NULL) {
    return 0;
  }
  if (!ccb_tui_input_esc_pending(in)) {
    return 0;
  }
  if (in->esc_since_ms < 0 ||
      tui_now_ms() < in->esc_since_ms + TUI_ESC_TIMEOUT_MS) {
    return 0;
  }
  tui_buf_consume(in, 1);
  in->esc_since_ms = -1;
  tui_make_key(ev, CCB_TUI_KEY_ESC, 0, 0, 1);
  return 1;
}

ccb_status ccb_tui_input_poll(ccb_tui_input *in, int timeout_ms,
    ccb_tui_event *ev) {
  long long deadline = -1;
  int attempted = 0; // a select attempt has happened
  if (in == NULL || ev == NULL) {
    return CCB_ERR_USAGE;
  }
  memset(ev, 0, sizeof(*ev));
  ev->kind = CCB_TUI_EV_NONE;
  if (timeout_ms >= 0) {
    deadline = tui_now_ms() + timeout_ms;
  }
  for (;;) {
    int r = ccb_tui_input_feed(in, NULL, 0, ev);
    long long now = 0;
    long long wait_ms = -1;
    fd_set rfds;
    struct timeval tv;
    int sel = 0;
    unsigned char chunk[256];
    ssize_t m = 0;
    if (r == 1) {
      return CCB_OK;
    }
    if (r == -1) {
      continue; // parse error: buffer cleared, keep waiting
    }
    now = tui_now_ms();
    // Only give up on an expired deadline after at least one
    // select attempt: with timeout 0 the first attempt is the
    // non-blocking read itself, not an instant NONE.
    if (deadline >= 0 && now >= deadline && attempted) {
      // Expired: a lone ESC whose grace has passed still
      // becomes an Esc key instead of a bare timeout.
      if (ccb_tui_input_esc_timeout(in, ev)) {
        return CCB_OK;
      }
      return CCB_OK;
    }
    if (deadline >= 0) {
      wait_ms = deadline - now;
    }
    if (ccb_tui_input_esc_pending(in)) {
      long long left = in->esc_since_ms + TUI_ESC_TIMEOUT_MS - now;
      if (left <= 0) {
        if (ccb_tui_input_esc_timeout(in, ev)) {
          return CCB_OK;
        }
        continue;
      }
      if (wait_ms < 0 || left < wait_ms) {
        wait_ms = left;
      }
    }
    if (in->fd < 0) {
      return CCB_OK; // feed-only parser: never blocks
    }
    FD_ZERO(&rfds);
    FD_SET(in->fd, &rfds);
    tv.tv_sec = (time_t)(wait_ms < 0 ? 0 : wait_ms / 1000);
    tv.tv_usec = (suseconds_t)(wait_ms < 0 ? 0 :
      (wait_ms % 1000) * 1000);
    if (wait_ms < 0) {
      sel = select(in->fd + 1, &rfds, NULL, NULL, NULL);
    } else {
      sel = select(in->fd + 1, &rfds, NULL, NULL, &tv);
    }
    attempted = 1;
    if (sel == 0) {
      continue; // timeout: loop re-checks feed + ESC grace
    }
    if (sel < 0) {
      if (errno == EINTR) {
        continue;
      }
      return CCB_ERR_IO;
    }
    m = read(in->fd, chunk, sizeof(chunk));
    if (m == 0) {
      ev->kind = CCB_TUI_EV_EOF;
      return CCB_OK;
    }
    if (m < 0) {
      if (errno == EINTR) {
        continue;
      }
      return CCB_ERR_IO;
    }
    r = ccb_tui_input_feed(in, chunk, (size_t)m, ev);
    if (r == 1) {
      return CCB_OK;
    }
  }
}
