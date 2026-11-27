// ccb_tui tests: cell, style, screen, diff, input, term.
// Library only — no preview code is touched here.
// _XOPEN_SOURCE for posix_openpt/grantpt/unlockpt/ptsname.
#define _XOPEN_SOURCE 700
#include "test.h"

#include "ccb_tui_cell.h"
#include "ccb_tui_diff.h"
#include "ccb_tui_input.h"
#include "ccb_tui_screen.h"
#include "ccb_tui_style.h"
#include "ccb_tui_term.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>

static void test_cell(void) {
  uint32_t cp = 0;
  char encoded[4];
  ccb_tui_color a = ccb_tui_color_rgb(1, 2, 3);
  ccb_tui_color b = ccb_tui_color_rgb(1, 2, 3);
  ccb_tui_color c = ccb_tui_color_rgb(1, 2, 4);
  ccb_tui_cell blank = ccb_tui_cell_blank();
  ccb_tui_cell other = blank;

  // UTF-8 decode: ASCII, 2-byte, 3-byte, invalid, truncated.
  CHECK(ccb_tui_utf8_decode("A", 1, &cp) == 1 && cp == 'A');
  CHECK(ccb_tui_utf8_decode("\xC3\xA9", 2, &cp) == 2 && cp == 0xE9);
  CHECK(ccb_tui_utf8_decode("\xE4\xB8\xAD", 3, &cp) == 3 &&
    cp == 0x4E2D);
  CHECK(ccb_tui_utf8_decode("\xFF", 1, &cp) == (size_t)-1);
  CHECK(ccb_tui_utf8_decode("\xE4\xB8", 2, &cp) == 0);
  CHECK(ccb_tui_utf8_decode("", 0, &cp) == 0);
  // UTF-8 encode: 3-byte, surrogate rejected, out of range.
  CHECK(ccb_tui_utf8_encode(0x4E2D, encoded) == 3 &&
    memcmp(encoded, "\xE4\xB8\xAD", 3) == 0);
  CHECK(ccb_tui_utf8_encode(0xD800, encoded) == 0);
  CHECK(ccb_tui_utf8_encode(0x110000, encoded) == 0);
  // Widths: ASCII 1, CJK 2, combining 0, control 0, emoji 2.
  CHECK(ccb_tui_char_width('A') == 1);
  CHECK(ccb_tui_char_width(0x4E2D) == 2);
  CHECK(ccb_tui_char_width(0x0301) == 0);
  CHECK(ccb_tui_char_width('\n') == 0);
  CHECK(ccb_tui_char_width(0x1F600) == 2);
  CHECK(ccb_tui_char_width(0x00E9) == 1);
  // Colours: equality is by kind + value, clamping on build.
  CHECK(ccb_tui_color_equal(a, b));
  CHECK(!ccb_tui_color_equal(a, c));
  CHECK(!ccb_tui_color_equal(ccb_tui_color_ansi(1),
    ccb_tui_color_rgb(255, 0, 0)));
  CHECK(ccb_tui_color_ansi(99).idx == 15);
  CHECK(ccb_tui_color_256(300).idx == 255);
  CHECK(ccb_tui_color_rgb(300, 0, 0).r == 255);
  // Cells: blank is a width-1 space; equality is exact.
  CHECK(ccb_tui_cell_is_blank(&blank));
  CHECK(ccb_tui_cell_equal(&blank, &other));
  other.mods = CCB_TUI_BOLD;
  CHECK(!ccb_tui_cell_equal(&blank, &other));
  CHECK(!ccb_tui_cell_is_blank(&other));
}

static void test_style(void) {
  ccb_tui_style def = ccb_tui_style_default();
  ccb_tui_style bold = def;
  ccb_tui_style dim = def;
  ccb_tui_style red = def;
  ccb_str out;
  const char *saved_no = getenv("NO_COLOR");
  const char *saved_ct = getenv("COLORTERM");
  const char *saved_term = getenv("TERM");
  char *keep_no = saved_no != NULL ? ccb_xstrdup(saved_no) : NULL;
  char *keep_ct = saved_ct != NULL ? ccb_xstrdup(saved_ct) : NULL;
  char *keep_term = saved_term != NULL ? ccb_xstrdup(saved_term) : NULL;
  bold.mods = CCB_TUI_BOLD;
  dim.mods = CCB_TUI_DIM;
  red.fg = ccb_tui_color_ansi(1);

  // Semantic ids resolve to the documented styles.
  CHECK(ccb_tui_style_resolve(CCB_TUI_STYLE_TITLE).mods ==
    CCB_TUI_BOLD);
  CHECK(ccb_tui_style_resolve(CCB_TUI_STYLE_SELECTED).mods ==
    CCB_TUI_REVERSE);
  CHECK(ccb_tui_style_resolve(CCB_TUI_STYLE_ERROR).fg.idx == 1);
  CHECK(ccb_tui_style_resolve(CCB_TUI_STYLE_DIMMED).mods ==
    CCB_TUI_DIM);
  CHECK(ccb_tui_style_equal(
    ccb_tui_style_resolve(CCB_TUI_STYLE_BORDER), def));
  CHECK(ccb_tui_style_equal(
    ccb_tui_style_resolve((ccb_tui_style_id)99), def));

  // SGR diff: one combined sequence, modifier diff, no spam.
  ccb_str_init(&out);
  ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_RGB, &def, &bold);
  CHECK_STR_EQ(out.buf, "\x1b[1m");
  ccb_str_free(&out);
  ccb_str_init(&out);
  ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_RGB, &bold, &def);
  CHECK_STR_EQ(out.buf, "\x1b[22m");
  ccb_str_free(&out);
  ccb_str_init(&out);
  ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_RGB, &bold, &dim);
  CHECK_STR_EQ(out.buf, "\x1b[22;2m"); // 22 drops bold+dim, re-add 2
  ccb_str_free(&out);
  ccb_str_init(&out);
  ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_RGB, &def, &red);
  CHECK_STR_EQ(out.buf, "\x1b[31m");
  ccb_str_free(&out);
  ccb_str_init(&out);
  ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_RGB, &red, &def);
  CHECK_STR_EQ(out.buf, "\x1b[39m");
  ccb_str_free(&out);
  ccb_str_init(&out);
  ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_RGB, &def, &def);
  CHECK(out.len == 0); // equal styles: nothing emitted
  ccb_str_free(&out);
  ccb_str_init(&out);
  ccb_tui_sgr_reset(&out);
  CHECK_STR_EQ(out.buf, "\x1b[0m");
  ccb_str_free(&out);
  // Truecolour, 256 and 16 emission of the same RGB colour.
  ccb_str_init(&out);
  {
    ccb_tui_style rgb = def;
    rgb.fg = ccb_tui_color_rgb(255, 0, 0);
    ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_RGB, &def, &rgb);
    CHECK_STR_EQ(out.buf, "\x1b[38;2;255;0;0m");
  }
  ccb_str_free(&out);
  ccb_str_init(&out);
  {
    ccb_tui_style rgb = def;
    rgb.fg = ccb_tui_color_rgb(255, 0, 0);
    ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_256, &def, &rgb);
    CHECK_STR_EQ(out.buf, "\x1b[38;5;196m");
  }
  ccb_str_free(&out);
  ccb_str_init(&out);
  {
    ccb_tui_style rgb = def;
    rgb.fg = ccb_tui_color_rgb(255, 0, 0);
    ccb_tui_sgr_diff(&out, CCB_TUI_LEVEL_16, &def, &rgb);
    CHECK_STR_EQ(out.buf, "\x1b[91m");
  }
  ccb_str_free(&out);
  // Quantisers.
  CHECK(ccb_tui_rgb_to_256(255, 0, 0) == 196);
  CHECK(ccb_tui_rgb_to_256(0, 0, 0) == 16);
  CHECK(ccb_tui_rgb_to_256(255, 255, 255) == 231);
  CHECK(ccb_tui_rgb_to_256(128, 128, 128) == 244);
  CHECK(ccb_tui_rgb_to_16(255, 0, 0) == 9);
  CHECK(ccb_tui_rgb_to_16(0, 0, 0) == 0);
  CHECK(ccb_tui_rgb_to_16(255, 255, 255) == 15);
  // Colour-level detection: NO_COLOR wins; COLORTERM beats TERM.
  unsetenv("NO_COLOR");
  unsetenv("COLORTERM");
  setenv("TERM", "xterm-256color", 1);
  CHECK(ccb_tui_detect_color_level() == CCB_TUI_LEVEL_256);
  setenv("COLORTERM", "truecolor", 1);
  CHECK(ccb_tui_detect_color_level() == CCB_TUI_LEVEL_RGB);
  setenv("NO_COLOR", "1", 1);
  CHECK(ccb_tui_detect_color_level() == CCB_TUI_LEVEL_NONE);
  unsetenv("NO_COLOR");
  unsetenv("COLORTERM");
  setenv("TERM", "xterm", 1);
  CHECK(ccb_tui_detect_color_level() == CCB_TUI_LEVEL_16);
  setenv("TERM", "dumb", 1);
  CHECK(ccb_tui_detect_color_level() == CCB_TUI_LEVEL_NONE);
  unsetenv("TERM");
  CHECK(ccb_tui_detect_color_level() == CCB_TUI_LEVEL_NONE);
  if (keep_no != NULL) {
    setenv("NO_COLOR", keep_no, 1);
  } else {
    unsetenv("NO_COLOR");
  }
  if (keep_ct != NULL) {
    setenv("COLORTERM", keep_ct, 1);
  } else {
    unsetenv("COLORTERM");
  }
  if (keep_term != NULL) {
    setenv("TERM", keep_term, 1);
  } else {
    unsetenv("TERM");
  }
  free(keep_no);
  free(keep_ct);
  free(keep_term);
}

static void test_screen(void) {
  ccb_tui_screen *s = NULL;
  ccb_tui_style def = ccb_tui_style_default();
  ccb_str out;
  const ccb_tui_cell *c = NULL;

  CHECK(ccb_tui_screen_new(0, 5) == NULL);
  CHECK(ccb_tui_screen_new(5, 0) == NULL);
  s = ccb_tui_screen_new(5, 3);
  CHECK(s != NULL);
  CHECK(ccb_tui_screen_cols(s) == 5);
  CHECK(ccb_tui_screen_rows(s) == 3);
  CHECK(ccb_tui_screen_get(s, 5, 0) == NULL); // out of bounds
  CHECK(ccb_tui_screen_get(s, -1, 0) == NULL);
  // put_text + dump (trailing empty rows stay empty).
  CHECK(ccb_tui_screen_put_text(s, 0, 0, "hello", def) == 5);
  ccb_str_init(&out);
  ccb_tui_screen_dump(s, &out);
  CHECK_STR_EQ(out.buf, "hello\n\n");
  ccb_str_free(&out);
  // '\n' moves to the next line at the start column.
  ccb_tui_screen_clear(s);
  CHECK(ccb_tui_screen_put_text(s, 0, 0, "ab\ncd", def) == 4);
  ccb_str_init(&out);
  ccb_tui_screen_dump(s, &out);
  CHECK_STR_EQ(out.buf, "ab\ncd\n");
  ccb_str_free(&out);
  // Clipping: text stops at the right edge.
  ccb_tui_screen_clear(s);
  ccb_tui_screen_put_text(s, 3, 0, "hello", def);
  ccb_str_init(&out);
  ccb_tui_screen_dump(s, &out);
  CHECK_STR_EQ(out.buf, "   he\n\n");
  ccb_str_free(&out);
  // Invalid bytes become '?'.
  ccb_tui_screen_clear(s);
  ccb_tui_screen_put_text(s, 0, 0, "a\xFF" "b", def);
  ccb_str_init(&out);
  ccb_tui_screen_dump(s, &out);
  CHECK_STR_EQ(out.buf, "a?b\n\n");
  ccb_str_free(&out);
  // Box corners and edges.
  ccb_tui_screen_clear(s);
  ccb_tui_screen_box(s, 0, 0, 5, 3, def);
  ccb_str_init(&out);
  ccb_tui_screen_dump(s, &out);
  CHECK_STR_EQ(out.buf,
    "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90\n"
    "\xE2\x94\x82   \xE2\x94\x82\n"
    "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x98");
  ccb_str_free(&out);
  // Wide glyph: continuation cell, last-column rule, overwrite.
  ccb_tui_screen_clear(s);
  CHECK(ccb_tui_screen_put(s, 0, 0, 0x4E2D, def) == 2);
  c = ccb_tui_screen_get(s, 1, 0);
  CHECK(c != NULL && c->width == 0 && c->text[0] == '\0');
  CHECK(ccb_tui_screen_put(s, 4, 0, 0x4E2D, def) == 1);
  c = ccb_tui_screen_get(s, 4, 0);
  CHECK(c != NULL && c->width == 1 && c->text[0] == ' ');
  CHECK(ccb_tui_screen_put(s, 1, 0, 'A', def) == 1); // into a
  c = ccb_tui_screen_get(s, 0, 0); // continuation: glyph cleared
  CHECK(c != NULL && c->width == 1 && c->text[0] == ' ');
  c = ccb_tui_screen_get(s, 1, 0);
  CHECK(c != NULL && c->text[0] == 'A');
  // Combining marks are dropped in v1.
  CHECK(ccb_tui_screen_put(s, 0, 0, 0x0301, def) == 0);
  // Fill + half-block primitive.
  ccb_tui_screen_clear(s);
  ccb_tui_screen_fill(s, 0, 0, 3, 2, 'x', def);
  ccb_str_init(&out);
  ccb_tui_screen_dump(s, &out);
  CHECK_STR_EQ(out.buf, "xxx\nxxx\n");
  ccb_str_free(&out);
  ccb_tui_screen_clear(s);
  ccb_tui_screen_half(s, 0, 0, ccb_tui_color_rgb(255, 0, 0),
    ccb_tui_color_rgb(0, 0, 255));
  c = ccb_tui_screen_get(s, 0, 0);
  CHECK(c != NULL && memcmp(c->text, "\xE2\x96\x80", 3) == 0);
  CHECK(c != NULL && c->width == 1);
  CHECK(c != NULL && c->fg.r == 255 && c->fg.g == 0);
  CHECK(c != NULL && c->bg.b == 255);
  // Resize preserves the overlap, clears the rest.
  ccb_tui_screen_clear(s);
  ccb_tui_screen_put_text(s, 0, 0, "ab", def);
  CHECK(ccb_tui_screen_resize(s, 6, 4) == CCB_OK);
  ccb_str_init(&out);
  ccb_tui_screen_dump(s, &out);
  CHECK_STR_EQ(out.buf, "ab\n\n\n");
  ccb_str_free(&out);
  CHECK(ccb_tui_screen_resize(s, 2, 1) == CCB_OK);
  ccb_str_init(&out);
  ccb_tui_screen_dump(s, &out);
  CHECK_STR_EQ(out.buf, "ab");
  ccb_str_free(&out);
  CHECK(ccb_tui_screen_resize(s, 0, 1) == CCB_ERR_USAGE);
  // Equal / copy.
  {
    ccb_tui_screen *t = ccb_tui_screen_new(2, 1);
    CHECK(ccb_tui_screen_equal(s, t) == 0);
    CHECK(ccb_tui_screen_copy(t, s) == CCB_OK);
    CHECK(ccb_tui_screen_equal(s, t));
    ccb_tui_screen_free(t);
  }
  ccb_tui_screen_free(s);
  ccb_tui_screen_free(NULL); // NULL-safe
}

static void diff_case(ccb_tui_screen *old_s, ccb_tui_screen *new_s,
    ccb_tui_color_level level, const char *want) {
  ccb_str out;
  ccb_str_init(&out);
  ccb_tui_diff(old_s, new_s, level, &out);
  // An empty ccb_str has buf == NULL: compare by length first.
  if (out.len == 0) {
    CHECK(want[0] == '\0');
  } else {
    CHECK_STR_EQ(out.buf, want);
  }
  ccb_str_free(&out);
}

static void test_diff(void) {
  ccb_tui_screen *old_s = ccb_tui_screen_new(5, 2);
  ccb_tui_screen *new_s = ccb_tui_screen_new(5, 2);
  ccb_tui_style def = ccb_tui_style_default();
  ccb_tui_style bold = def;
  ccb_tui_style rgb = def;
  bold.mods = CCB_TUI_BOLD;
  rgb.fg = ccb_tui_color_rgb(255, 0, 0);

  // Identical frames: zero bytes.
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB, "");
  // One cell at (0,0): Home + the char, no SGR, no reset.
  ccb_tui_screen_put_text(new_s, 0, 0, "A", def);
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB, "\x1b[HA");
  // Bold cell at (2,0): CUP + SGR + char + reset.
  ccb_tui_screen_clear(new_s);
  ccb_tui_screen_put(new_s, 2, 0, 'A', bold);
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB,
    "\x1b[1;3H\x1b[1mA\x1b[0m");
  // Contiguous run: no move between the cells.
  ccb_tui_screen_clear(new_s);
  ccb_tui_screen_put_text(new_s, 0, 0, "AB", def);
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB, "\x1b[HAB");
  // Gap on one row: short CUF hop instead of CUP.
  ccb_tui_screen_clear(new_s);
  ccb_tui_screen_put(new_s, 0, 0, 'A', def);
  ccb_tui_screen_put(new_s, 3, 0, 'B', def);
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB, "\x1b[HA\x1b[2CB");
  // Style change mid-run: SGR diff, reset at the end.
  ccb_tui_screen_clear(new_s);
  ccb_tui_screen_put(new_s, 0, 0, 'A', def);
  ccb_tui_screen_put(new_s, 1, 0, 'B', bold);
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB,
    "\x1b[HA\x1b[1mB\x1b[0m");
  // Two rows: CR/LF for column 0 of the next row.
  ccb_tui_screen_clear(new_s);
  ccb_tui_screen_put(new_s, 0, 0, 'A', def);
  ccb_tui_screen_put(new_s, 0, 1, 'B', def);
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB, "\x1b[HA\r\nB");
  // Truecolour / 256 / 16 / none emission of one RGB cell.
  ccb_tui_screen_clear(new_s);
  ccb_tui_screen_put(new_s, 0, 0, 'A', rgb);
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB,
    "\x1b[H\x1b[38;2;255;0;0mA\x1b[0m");
  diff_case(old_s, new_s, CCB_TUI_LEVEL_256,
    "\x1b[H\x1b[38;5;196mA\x1b[0m");
  diff_case(old_s, new_s, CCB_TUI_LEVEL_16, "\x1b[H\x1b[91mA\x1b[0m");
  diff_case(old_s, new_s, CCB_TUI_LEVEL_NONE, "\x1b[HA");
  // Trailing blank run: EL instead of spaces.
  {
    ccb_tui_screen *o10 = ccb_tui_screen_new(10, 1);
    ccb_tui_screen *n10 = ccb_tui_screen_new(10, 1);
    ccb_tui_screen_fill(o10, 0, 0, 10, 1, 'X', def);
    ccb_tui_screen_put_text(n10, 0, 0, "AB", def);
    diff_case(o10, n10, CCB_TUI_LEVEL_RGB, "\x1b[HAB\x1b[K");
    ccb_tui_screen_free(o10);
    ccb_tui_screen_free(n10);
  }
  // Wide glyph: cursor re-asserted with CUP after it.
  ccb_tui_screen_clear(old_s);
  ccb_tui_screen_clear(new_s);
  ccb_tui_screen_put(new_s, 0, 0, 0x4E2D, def);
  ccb_tui_screen_put(new_s, 2, 0, 'B', def);
  diff_case(old_s, new_s, CCB_TUI_LEVEL_RGB,
    "\x1b[H\xE4\xB8\xAD\x1b[1;3HB");
  // Mismatched dimensions and NULLs: nothing appended.
  {
    ccb_tui_screen *small = ccb_tui_screen_new(3, 1);
    ccb_str out;
    ccb_str_init(&out);
    ccb_tui_diff(small, new_s, CCB_TUI_LEVEL_RGB, &out);
    CHECK(out.len == 0);
    ccb_str_free(&out);
    ccb_tui_screen_free(small);
  }
  ccb_tui_screen_free(old_s);
  ccb_tui_screen_free(new_s);
}

// Feed bytes into a feed-only parser and expect one event.
static void feed_event(const unsigned char *bytes, size_t n,
    ccb_tui_event *ev) {
  ccb_tui_input *in = ccb_tui_input_new(-1);
  int r = ccb_tui_input_feed(in, bytes, n, ev);
  CHECK(r == 1);
  ccb_tui_input_free(in);
}

static void feed_key(const char *bytes, ccb_tui_key key,
    uint32_t ch, uint16_t mods) {
  ccb_tui_event ev;
  feed_event((const unsigned char *)bytes, strlen(bytes), &ev);
  CHECK(ev.kind == CCB_TUI_EV_KEY);
  CHECK(ev.key == key);
  CHECK(ev.ch == ch);
  CHECK(ev.mods == mods);
}

static void test_input(void) {
  ccb_tui_input *in = NULL;
  ccb_tui_event ev;
  int r = 0;

  // Plain chars, controls, arrows, Home/End, PgUp/PgDn.
  feed_key("a", CCB_TUI_KEY_CHAR, 'a', 0);
  feed_key("\r", CCB_TUI_KEY_ENTER, 0, 0);
  feed_key("\t", CCB_TUI_KEY_TAB, 0, 0);
  feed_key("\x7F", CCB_TUI_KEY_BACKSPACE, 0, 0);
  feed_key("\x1b[A", CCB_TUI_KEY_UP, 0, 0);
  feed_key("\x1b[B", CCB_TUI_KEY_DOWN, 0, 0);
  feed_key("\x1b[C", CCB_TUI_KEY_RIGHT, 0, 0);
  feed_key("\x1b[D", CCB_TUI_KEY_LEFT, 0, 0);
  feed_key("\x1b[H", CCB_TUI_KEY_HOME, 0, 0);
  feed_key("\x1b[F", CCB_TUI_KEY_END, 0, 0);
  feed_key("\x1b[5~", CCB_TUI_KEY_PAGE_UP, 0, 0);
  feed_key("\x1b[6~", CCB_TUI_KEY_PAGE_DOWN, 0, 0);
  feed_key("\x1b[3~", CCB_TUI_KEY_DELETE, 0, 0);
  feed_key("\x1b[2~", CCB_TUI_KEY_INSERT, 0, 0);
  feed_key("\x1b[11~", CCB_TUI_KEY_F1, 0, 0);
  feed_key("\x1b[Z", CCB_TUI_KEY_TAB, 0, CCB_TUI_MOD_SHIFT);
  // SS3 forms.
  feed_key("\x1bOA", CCB_TUI_KEY_UP, 0, 0);
  feed_key("\x1bOP", CCB_TUI_KEY_F1, 0, 0);
  // Modifiers: Ctrl+Up, Shift+Up, Alt+x, Ctrl+C.
  feed_key("\x1b[1;5A", CCB_TUI_KEY_UP, 0, CCB_TUI_MOD_CTRL);
  feed_key("\x1b[1;2A", CCB_TUI_KEY_UP, 0, CCB_TUI_MOD_SHIFT);
  feed_key("\x1bx", CCB_TUI_KEY_CHAR, 'x', CCB_TUI_MOD_ALT);
  feed_key("\x03", CCB_TUI_KEY_CHAR, 'c', CCB_TUI_MOD_CTRL);
  // Kitty keyboard: Shift+Enter, release of 'a', Esc as CSI-u.
  feed_key("\x1b[13;2u", CCB_TUI_KEY_ENTER, 0, CCB_TUI_MOD_SHIFT);
  {
    ccb_tui_event e2;
    feed_event((const unsigned char *)"\x1b[97;1:3u", 9, &e2);
    CHECK(e2.kind == CCB_TUI_EV_KEY);
    CHECK(e2.key == CCB_TUI_KEY_CHAR && e2.ch == 'a');
    CHECK(e2.evtype == 3); // release, not press
  }
  feed_key("\x1b[27u", CCB_TUI_KEY_ESC, 0, 0);
  // UTF-8 char, split across two feeds; invalid byte -> U+FFFD.
  in = ccb_tui_input_new(-1);
  r = ccb_tui_input_feed(in, (const unsigned char *)"\xC3", 1, &ev);
  CHECK(r == 0);
  r = ccb_tui_input_feed(in, (const unsigned char *)"\xA9", 1, &ev);
  CHECK(r == 1 && ev.kind == CCB_TUI_EV_KEY && ev.ch == 0xE9);
  ccb_tui_input_free(in);
  feed_key("\xFF", CCB_TUI_KEY_CHAR, 0xFFFD, 0);
  // Lone ESC: need-more, then the timeout turns it into Esc.
  in = ccb_tui_input_new(-1);
  r = ccb_tui_input_feed(in, (const unsigned char *)"\x1b", 1, &ev);
  CHECK(r == 0);
  CHECK(ccb_tui_input_esc_pending(in));
  CHECK(ccb_tui_input_esc_wait_ms(in) >= 0);
  CHECK(ccb_tui_input_esc_timeout(in, &ev) == 0); // grace not over
  usleep(60000);
  CHECK(ccb_tui_input_esc_timeout(in, &ev) == 1);
  CHECK(ev.kind == CCB_TUI_EV_KEY && ev.key == CCB_TUI_KEY_ESC);
  CHECK(!ccb_tui_input_esc_pending(in));
  CHECK(ccb_tui_input_esc_wait_ms(in) == -1);
  ccb_tui_input_free(in);
  // Bracketed paste: one event, split across feeds, terminator
  // excluded from the text.
  in = ccb_tui_input_new(-1);
  r = ccb_tui_input_feed(in,
    (const unsigned char *)"\x1b[200~hel", 9, &ev);
  CHECK(r == 0);
  r = ccb_tui_input_feed(in,
    (const unsigned char *)"lo\x1b[201~", 8, &ev);
  CHECK(r == 1 && ev.kind == CCB_TUI_EV_PASTE);
  CHECK_STR_EQ(ev.paste, "hello");
  free(ev.paste);
  ccb_tui_input_free(in);
  // Mouse reports and focus events are consumed, never keys.
  in = ccb_tui_input_new(-1);
  r = ccb_tui_input_feed(in,
    (const unsigned char *)"\x1b[<0;10;20M",
    strlen("\x1b[<0;10;20M"), &ev);
  CHECK(r == 0);
  r = ccb_tui_input_feed(in, (const unsigned char *)"a", 1, &ev);
  CHECK(r == 1 && ev.kind == CCB_TUI_EV_KEY && ev.ch == 'a');
  ccb_tui_input_free(in);
  // Poll on a feed-only parser never blocks; EOF from a pipe.
  in = ccb_tui_input_new(-1);
  CHECK(ccb_tui_input_poll(in, 0, &ev) == CCB_OK);
  CHECK(ev.kind == CCB_TUI_EV_NONE);
  ccb_tui_input_free(in);
  {
    int fds[2];
    ccb_tui_input *pin = NULL;
    CHECK(pipe(fds) == 0);
    close(fds[1]); // write end closed: read hits EOF
    pin = ccb_tui_input_new(fds[0]);
    CHECK(ccb_tui_input_poll(pin, 200, &ev) == CCB_OK);
    CHECK(ev.kind == CCB_TUI_EV_EOF);
    ccb_tui_input_free(pin);
    close(fds[0]);
  }
  ccb_tui_input_free(NULL); // NULL-safe
}

// Read whatever is available on fd within timeout_ms (0 on none).
static int tui_read_avail(int fd, char *buf, size_t cap,
    int timeout_ms) {
  fd_set rfds;
  struct timeval tv;
  ssize_t m = 0;
  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
    return 0;
  }
  m = read(fd, buf, cap);
  return m < 0 ? 0 : (int)m;
}

static void test_term(void) {
  int master = -1;
  int slave = -1;
  char *slave_name = NULL;
  struct winsize ws;
  ccb_tui_term *t = NULL;
  ccb_tui_term_opts opts;
  ccb_tui_screen *s = NULL;
  ccb_tui_style def = ccb_tui_style_default();
  char buf[4096];
  int n = 0;
  int fds[2];
  ccb_tui_term *second = NULL;

  // Non-TTY fds are refused.
  CHECK(pipe(fds) == 0);
  ccb_tui_term_opts_default(&opts);
  opts.in_fd = fds[0];
  opts.out_fd = fds[1];
  CHECK(ccb_tui_term_open(&opts, &t) == CCB_ERR_IO);
  CHECK(t == NULL);
  close(fds[0]);
  close(fds[1]);
  CHECK(ccb_tui_term_open(NULL, &t) == CCB_ERR_USAGE);
  CHECK(ccb_tui_term_close(NULL) == CCB_OK);

  // A pty pair stands in for the terminal.
  master = posix_openpt(O_RDWR);
  CHECK(master >= 0);
  CHECK(grantpt(master) == 0);
  CHECK(unlockpt(master) == 0);
  slave_name = ptsname(master);
  CHECK(slave_name != NULL);
  slave = open(slave_name, O_RDWR);
  CHECK(slave >= 0);
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 10;
  CHECK(ioctl(master, TIOCSWINSZ, &ws) == 0);
  {
    const char *old_ct = getenv("COLORTERM");
    char *keep_ct = old_ct != NULL ? ccb_xstrdup(old_ct) : NULL;
    setenv("COLORTERM", "truecolor", 1);
    ccb_tui_term_opts_default(&opts);
    opts.in_fd = slave;
    opts.out_fd = slave;
    CHECK(ccb_tui_term_open(&opts, &t) == CCB_OK);
    if (keep_ct != NULL) {
      setenv("COLORTERM", keep_ct, 1);
      free(keep_ct);
    } else {
      unsetenv("COLORTERM");
    }
  }
  CHECK(t != NULL);
  CHECK(ccb_tui_term_cols(t) == 20);
  CHECK(ccb_tui_term_rows(t) == 10);
  CHECK(ccb_tui_term_color_level(t) == CCB_TUI_LEVEL_RGB);
  // A second open while one is active is refused.
  CHECK(ccb_tui_term_open(&opts, &second) == CCB_ERR_IO);
  CHECK(second == NULL);
  // Open wrote the alt-screen + bracketed-paste entries.
  n = tui_read_avail(master, buf, sizeof(buf) - 1, 200);
  CHECK(n > 0);
  buf[n] = '\0';
  CHECK(ccb_test_contains(buf, "\x1b[?1049h"));
  CHECK(ccb_test_contains(buf, "\x1b[?2004h"));
  // Present a frame: sync output + hidden cursor + the text.
  s = ccb_tui_screen_new(20, 10);
  ccb_tui_screen_put_text(s, 0, 0, "hi", def);
  CHECK(ccb_tui_term_present(t, s) == CCB_OK);
  n = tui_read_avail(master, buf, sizeof(buf) - 1, 200);
  CHECK(n > 0);
  buf[n] = '\0';
  CHECK(ccb_test_contains(buf, "\x1b[?2026h"));
  CHECK(ccb_test_contains(buf, "\x1b[?25l"));
  CHECK(ccb_test_contains(buf, "hi"));
  // Presenting the same frame again writes nothing.
  CHECK(ccb_tui_term_present(t, s) == CCB_OK);
  CHECK(tui_read_avail(master, buf, sizeof(buf) - 1, 100) == 0);
  // Resize via the master: query_size reports the change.
  ws.ws_col = 30;
  ws.ws_row = 12;
  CHECK(ioctl(master, TIOCSWINSZ, &ws) == 0);
  CHECK(ccb_tui_term_query_size(t) == 1);
  CHECK(ccb_tui_term_cols(t) == 30);
  CHECK(ccb_tui_term_rows(t) == 12);
  CHECK(ccb_tui_term_query_size(t) == 0); // unchanged now
  // A key typed on the master arrives as an event.
  {
    ccb_tui_event ev;
    CHECK(write(master, "q", 1) == 1);
    CHECK(ccb_tui_term_poll_event(t, 500, &ev) == CCB_OK);
    CHECK(ev.kind == CCB_TUI_EV_KEY);
    CHECK(ev.key == CCB_TUI_KEY_CHAR && ev.ch == 'q');
    CHECK(ccb_tui_term_poll_event(t, 0, &ev) == CCB_OK);
    CHECK(ev.kind == CCB_TUI_EV_NONE);
  }
  // Present after a resize repaints at the new size.
  CHECK(ccb_tui_screen_resize(s, 30, 12) == CCB_OK);
  CHECK(ccb_tui_term_present(t, s) == CCB_OK);
  n = tui_read_avail(master, buf, sizeof(buf) - 1, 200);
  CHECK(n > 0);
  buf[n] = '\0';
  CHECK(ccb_test_contains(buf, "hi"));
  CHECK(ccb_tui_term_present(t, NULL) == CCB_ERR_USAGE);
  CHECK(ccb_tui_term_close(t) == CCB_OK);
  ccb_tui_screen_free(s);
  close(slave);
  close(master);
}

int main(void) {
  test_cell();
  test_style();
  test_screen();
  test_diff();
  test_input();
  test_term();
  TEST_MAIN_END();
}
