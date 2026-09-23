/* Exercises core/utf8.c and the UTF-8 paths it feeds: codepoint
 * decode/encode, column widths, wide-glyph placement (lead cell +
 * CTUI_CELL_CONT) through ctui_widget_puts(), the pair-breaking rules when
 * something overwrites half a wide glyph, width-aware util helpers, and
 * the literal UTF-8 bytes ctui_screen_flush() emits. */
#include "ctui.h"

#include "ctui_test.h"

#include <string.h>
#include <unistd.h>

static void noop_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  (void)comp;
}

static void test_decode_encode(void) {
  uint32_t cp;
  CTUI_TEST_ASSERT(ctui_utf8_decode("A", &cp) == 1 && cp == 'A',
                   "ASCII decodes as itself, 1 byte");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xc3\xa9", &cp) == 2 && cp == 0xE9,
                   "2-byte sequence decodes (U+00E9)");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xe4\xbd\xa0", &cp) == 3 && cp == 0x4F60,
                   "3-byte sequence decodes (U+4F60)");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xf0\x9f\x98\x80", &cp) == 4 &&
                       cp == 0x1F600,
                   "4-byte sequence decodes (U+1F600)");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xe4\xbd", &cp) == 1 && cp == 0xFFFD,
                   "a sequence truncated by the NUL terminator decodes as "
                   "U+FFFD consuming 1 byte, never reading past the string");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xc0\xaf", &cp) == 1 && cp == 0xFFFD,
                   "an overlong encoding is rejected as U+FFFD");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xed\xa0\x80", &cp) == 1 &&
                       cp == 0xFFFD,
                   "an encoded surrogate is rejected as U+FFFD");

  char buf[4];
  CTUI_TEST_ASSERT(ctui_utf8_encode(0x1F600, buf) == 4 &&
                       memcmp(buf, "\xf0\x9f\x98\x80", 4) == 0,
                   "encode round-trips a 4-byte codepoint");
  CTUI_TEST_ASSERT(ctui_utf8_encode(0xD800, buf) == 3 &&
                       memcmp(buf, "\xef\xbf\xbd", 3) == 0,
                   "encoding a surrogate emits U+FFFD instead");
}

static void test_widths(void) {
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth('a') == 1 &&
                       ctui_utf8_cpwidth(0xE9) == 1,
                   "ASCII and Latin-1 letters are 1 column");
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth(0x4F60) == 2 &&
                       ctui_utf8_cpwidth(0x1F600) == 2,
                   "CJK and emoji are 2 columns");
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth(0x0301) == 0,
                   "a combining mark is 0 columns");
  CTUI_TEST_ASSERT(ctui_utf8_width("a\xe4\xbd\xa0" "b") == 4,
                   "string width sums per-glyph widths, not bytes (5 bytes, "
                   "4 columns)");

  int w;
  size_t n = ctui_utf8_prefix("ab\xe4\xbd\xa0" "c", 3, &w);
  CTUI_TEST_ASSERT(n == 2 && w == 2,
                   "prefix stops before a wide glyph that would straddle "
                   "the column limit, reporting the narrower width");
}

static void test_widget_wide_glyphs(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(1, 8);
  CTUI_WIDGET w = ctui_widget_make(0, 0, 6, 1, NULL, noop_render, NULL);
  ctui_widget_init(&w, comp);

  ctui_widget_puts(&w, comp, 0, 0, "a\xe4\xbd\xa0" "b", CTUI_COLOR_RED,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[0].ch == 'a' && comp->cells[1].ch == 0x4F60 &&
                       comp->cells[2].ch == CTUI_CELL_CONT &&
                       comp->cells[3].ch == 'b',
                   "puts lays a wide glyph out as lead + CONT, and the next "
                   "glyph lands after both");
  CTUI_TEST_ASSERT(comp->cells[2].fg == CTUI_COLOR_RED,
                   "the CONT cell carries the glyph's colors too, so a "
                   "background color covers both columns");

  ctui_widget_putc(&w, comp, 0, 2, 'x', CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[1].ch == ' ' && comp->cells[2].ch == 'x',
                   "writing over a CONT blanks the orphaned lead to its left");

  ctui_widget_putc(&w, comp, 0, 3, 0x4F60, CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  ctui_widget_putc(&w, comp, 0, 3, 'y', CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[3].ch == 'y' && comp->cells[4].ch == ' ',
                   "writing a narrow glyph over a lead blanks its orphaned "
                   "CONT to the right");

  ctui_widget_putc(&w, comp, 0, 5, 0x4F60, CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[5].ch == ' ' && comp->cells[6].ch == ' ',
                   "a wide glyph in the widget's last column is clipped to a "
                   "space instead of spilling past the widget's right edge");

  ctui_widget_putc(&w, comp, 0, 0, 0x0301, CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[0].ch == 'a',
                   "a zero-width codepoint is dropped, leaving the cell alone");

  ctui_compositor_free(comp);
}

static void test_util_widths(void) {
  char line[16];
  memset(line, ' ', 8);
  line[8] = '\0';
  CTUI_TEST_ASSERT(
      ctui_util_center_h("\xe4\xbd\xa0\xe5\xa5\xbd", line,
                         (CTUI_CELL){.ch = '-'}) == 0 &&
          strcmp(line, "-\xe4\xbd\xa0\xe5\xa5\xbd-") == 0,
      "center_h never writes past strlen(line): a 4-column, 6-byte string "
      "in an 8-byte line gets the 2 bytes of padding that fit, not the 4 "
      "that width-centering would want");

  char tight[] = "      "; /* 6 bytes, a caller sized for ASCII */
  CTUI_TEST_ASSERT(ctui_util_center_h("\xe4\xbd\xa0\xe5\xa5\xbd", tight,
                                      (CTUI_CELL){.ch = ' '}) == 0 &&
                       strcmp(tight, "\xe4\xbd\xa0\xe5\xa5\xbd") == 0,
                   "an exactly-ASCII-sized buffer gets the glyphs with no "
                   "padding rather than an overflow");

  CTUI_TEST_ASSERT(ctui_util_center_col("\xe4\xbd\xa0\xe5\xa5\xbd", 8) == 2 &&
                       ctui_util_center_col("abcdefghij", 8) == 0,
                   "center_col centers by columns (4 wide in 8 starts at 2), "
                   "0 when it doesn't fit");

  char s[] = "\xe4\xbd\xa0\xe5\xa5\xbd\xe5\x90\x97"; /* 3 wide glyphs */
  CTUI_TEST_ASSERT(ctui_util_truncate_str(s, 5, "..") == 0 &&
                       strcmp(s, "\xe4\xbd\xa0..") == 0,
                   "truncate_str cuts by columns at a glyph boundary: 6 "
                   "columns into 5 keeps one wide glyph (2) + \"..\" (2), "
                   "since a second wide glyph would straddle the cut");
}

static size_t capture_flush(CTUI_SCREEN *s, char *out, size_t out_cap) {
  int pipefd[2];
  pipe(pipefd);
  int saved_stdout = dup(STDOUT_FILENO);
  dup2(pipefd[1], STDOUT_FILENO);
  close(pipefd[1]);
  ctui_screen_flush(s);
  dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
  ssize_t n = read(pipefd[0], out, out_cap - 1);
  close(pipefd[0]);
  if (n < 0) {
    n = 0;
  }
  out[n] = '\0';
  return (size_t)n;
}

static void test_flush_utf8(void) {
  CTUI_SCREEN *s = ctui_screen_create(1, 5);
  char out[256];
  capture_flush(s, out, sizeof out); /* prime the shadow buffer */

  ctui_screen_puts(s, 0, 0, "\xe4\xbd\xa0" "b", CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strcmp(out, "\x1b[1;1H\x1b[39;49m\xe4\xbd\xa0"
                               "b\x1b[0m") == 0,
                   "flush emits the wide glyph's UTF-8 bytes, nothing for its "
                   "CONT cell, and the next glyph without a reposition (the "
                   "terminal cursor already advanced 2 columns)");

  ctui_screen_putc(s, 0, 4, '\x1b', CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strcmp(out, "\x1b[1;5H\x1b[39;49m\xef\xbf\xbd\x1b[0m") ==
                       0,
                   "a raw control byte in a cell is emitted as U+FFFD, never "
                   "as itself");

  ctui_screen_free(s);
}

int main(void) {
  ctui_log_init(E_WRN | E_ERR);

  test_decode_encode();
  test_widths();
  test_widget_wide_glyphs();
  test_util_widths();
  test_flush_utf8();

  ctui_log_shutdown();
  return ctui_test_summary();
}
