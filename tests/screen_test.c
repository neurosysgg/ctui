/* Exercises core/screen.c directly -- ctui_screen_putc()/puts()/clear(), and
 * ctui_screen_flush()'s shadow-buffer diff engine (skip unchanged cells,
 * cache the last-emitted color so a color escape isn't repeated, only
 * re-emit a cursor-position escape when the next written cell isn't
 * immediately after the last one). Nothing else in the suite calls
 * ctui_screen_flush() at all -- every other test asserts against
 * screen->cells directly (ctui_test_row_contains()) rather than the actual
 * ANSI byte stream flush() writes to STDOUT_FILENO, so this is the only
 * coverage of that logic.
 *
 * flush() writes via a raw write(STDOUT_FILENO, ...), not stdio -- captured
 * here by dup2()-ing STDOUT_FILENO to a pipe for the duration of the call,
 * same trick real shells use to capture a child's output, just without a
 * child process. Restored immediately after each capture so
 * CTUI_TEST_ASSERT's own printf()s keep going to the real stdout. */
#include "ctui.h"

#include "ctui_test.h"

#include <string.h>
#include <unistd.h>

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

static void test_putc_puts_clear(void) {
  CTUI_SCREEN *s = ctui_screen_create(3, 5);

  ctui_screen_putc(s, 1, 1, 'X', CTUI_COLOR_CYAN, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(s->cells[1 * 5 + 1].ch == 'X' &&
                       s->cells[1 * 5 + 1].fg == CTUI_COLOR_CYAN,
                   "screen_putc writes ch/fg at (row,col)");

  ctui_screen_putc(s, 10, 10, 'Y', CTUI_COLOR_RED, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(s->cells[1 * 5 + 1].ch == 'X',
                   "screen_putc rejects an out-of-bounds (row,col) without "
                   "touching any real cell");

  ctui_screen_puts(s, 0, 0, "hi", CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(s->cells[0].ch == 'h' && s->cells[1].ch == 'i',
                   "screen_puts writes each character via screen_putc in "
                   "sequence");

  ctui_screen_clear(s);
  CTUI_TEST_ASSERT(s->cells[0].ch == ' ' && s->cells[1 * 5 + 1].ch == ' ' &&
                       s->cells[1 * 5 + 1].fg == CTUI_COLOR_DEFAULT,
                   "screen_clear resets every cell back to a blank default "
                   "cell");

  ctui_screen_free(s);
}

static void test_flush_diff_and_color_cache(void) {
  CTUI_SCREEN *s = ctui_screen_create(1, 5);
  char out[256];

  /* prime: buffer starts as '\0' so the very first flush always repaints
   * everything -- burn that one and start the real assertions from an
   * already-synced buffer */
  capture_flush(s, out, sizeof out);

  ctui_screen_putc(s, 0, 0, 'A', CTUI_COLOR_RED, CTUI_COLOR_DEFAULT);
  ctui_screen_putc(s, 0, 1, 'B', CTUI_COLOR_RED, CTUI_COLOR_DEFAULT);
  /* col 2 left untouched -- still matches the buffer, so flush must skip it
   * entirely rather than repainting a blank */
  ctui_screen_putc(s, 0, 3, 'C', CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);

  size_t n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(
      n == strlen("\x1b[1;1H\x1b[31;49mAB\x1b[1;4H\x1b[32;49mC\x1b[0m") &&
          strcmp(out, "\x1b[1;1H\x1b[31;49mAB\x1b[1;4H\x1b[32;49mC\x1b[0m") ==
              0,
      "flush emits exactly one position escape for the contiguous A/B run "
      "(no repeated color escape since B's color matches A's), skips "
      "unchanged col 2 outright, then a fresh position+color escape for "
      "the non-contiguous, differently-colored C, ending with a reset");

  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n == strlen("\x1b[0m") && strcmp(out, "\x1b[0m") == 0,
                   "a flush with nothing changed since the last one writes "
                   "only the trailing reset -- every cell matched the "
                   "shadow buffer and was skipped");

  ctui_screen_free(s);
}

static void test_flush_256_and_rgb(void) {
  CTUI_SCREEN *s = ctui_screen_create(1, 2);
  char out[256];
  capture_flush(s, out, sizeof out); /* prime, same as above */

  /* screen.h only exposes BASIC-mode putc/puts; a 256/RGB cell normally
   * arrives here via ctui_compositor_blit()'s memcpy from widget-drawn
   * cells. Writing s->cells directly is the same gray-box move
   * ctui_test_cell() already makes on the read side -- CTUI_SCREEN's
   * fields aren't opaque. */
  s->cells[0] = (CTUI_CELL){
      .ch = 'Z', .fg = 200, .bg = 16, .color_mode = CTUI_COLOR_MODE_256};
  size_t n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(
      n == strlen("\x1b[1;1H\x1b[38;5;200;48;5;16mZ\x1b[0m") &&
          strcmp(out, "\x1b[1;1H\x1b[38;5;200;48;5;16mZ\x1b[0m") == 0,
      "flush emits a 256-color SGR escape (38;5;fg;48;5;bg) for a "
      "CTUI_COLOR_MODE_256 cell");

  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n == strlen("\x1b[0m"),
                   "an unchanged 256-color cell is skipped on the next "
                   "flush, same as a BASIC one");

  s->cells[0] = (CTUI_CELL){.ch = 'R',
                            .color_mode = CTUI_COLOR_MODE_RGB,
                            .fg_r = 1,
                            .fg_g = 2,
                            .fg_b = 3,
                            .bg_r = 4,
                            .bg_g = 5,
                            .bg_b = 6};
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(
      n == strlen("\x1b[1;1H\x1b[38;2;1;2;3;48;2;4;5;6mR\x1b[0m") &&
          strcmp(out, "\x1b[1;1H\x1b[38;2;1;2;3;48;2;4;5;6mR\x1b[0m") == 0,
      "flush emits a truecolor SGR escape (38;2;r;g;b;48;2;r;g;b) for a "
      "CTUI_COLOR_MODE_RGB cell, correctly distinguished from the "
      "256-color cell it replaced");

  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n == strlen("\x1b[0m"),
                   "an unchanged RGB cell (all six channels matching the "
                   "shadow buffer) is skipped on the next flush");

  s->cells[0].fg_r = 9; /* change just one of the six RGB channels */
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(
      strcmp(out, "\x1b[1;1H\x1b[38;2;9;2;3;48;2;4;5;6mR\x1b[0m") == 0,
      "a single changed RGB channel is enough for the shadow-buffer diff "
      "to treat the cell as changed and repaint it");

  ctui_screen_free(s);
}

static void test_resize_forces_redraw(void) {
  CTUI_SCREEN *s = ctui_screen_create(1, 3);
  char out[256];
  capture_flush(s, out, sizeof out); /* prime */

  ctui_screen_putc(s, 0, 0, 'X', CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  capture_flush(s, out, sizeof out); /* sync buffer to include the 'X' */

  ctui_screen_resize(s, 2, 4);
  CTUI_TEST_ASSERT(s->rows == 2 && s->cols == 4,
                   "resize reallocates to the new rows/cols");

  size_t n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n > strlen("\x1b[0m"),
                   "resize's fresh buffer (all '\\0', per screen_alloc) "
                   "never matches the all-space cells array, so the very "
                   "next flush repaints the whole new screen instead of "
                   "assuming it's already in sync");

  ctui_screen_free(s);
}

int main(void) {
  ctui_log_init(E_ALL);

  test_putc_puts_clear();
  test_flush_diff_and_color_cache();
  test_flush_256_and_rgb();
  test_resize_forces_redraw();

  return ctui_test_summary();
}
