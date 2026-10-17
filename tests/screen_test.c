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

#include <stddef.h>
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
  const char *want = "\x1b[1;1H\x1b[31;49mAB\x1b[C\x1b[32mC\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "flush emits exactly one position escape for the "
                   "contiguous A/B run (no repeated color escape since B's "
                   "color matches A's), steps over unchanged col 2 (CUF), "
                   "then only the fg that changed for C, ending with a "
                   "reset");

  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n == 0,
                   "a flush with nothing changed since the last one writes "
                   "nothing, not even the trailing reset -- every cell "
                   "matched the shadow buffer and was skipped");

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
  CTUI_TEST_ASSERT(n == 0, "an unchanged 256-color cell is skipped on the next "
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
  CTUI_TEST_ASSERT(n == 0,
                   "an unchanged RGB cell (all six channels matching the "
                   "shadow buffer) is skipped on the next flush");

  s->cells[0].fg_r = 9; /* change just one of the six RGB channels */
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(
      strcmp(out, "\x1b[1;1H\x1b[38;2;9;2;3;48;2;4;5;6mR\x1b[0m") == 0,
      "a single changed RGB channel is enough for the shadow-buffer diff "
      "to treat the cell as changed and repaint it");

  s->cells[0] = (CTUI_CELL){.ch = 'F',
                            .bg = CTUI_COLOR_BLUE,
                            .color_mode = CTUI_COLOR_MODE_RGB_FG,
                            .fg_r = 1,
                            .fg_g = 2,
                            .fg_b = 3};
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strcmp(out, "\x1b[1;1H\x1b[38;2;1;2;3;44mF\x1b[0m") == 0,
                   "flush emits a truecolor fg with a basic bg code for a "
                   "CTUI_COLOR_MODE_RGB_FG cell");
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n == 0,
                   "an unchanged RGB_FG cell is skipped on the next flush");
  s->cells[0].bg = CTUI_COLOR_DEFAULT;
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strcmp(out, "\x1b[1;1H\x1b[38;2;1;2;3;49mF\x1b[0m") == 0,
                   "... a changed bg alone repaints it");

  s->cells[0] = (CTUI_CELL){.ch = CTUI_GFX_KITTY_PLACEHOLDER,
                            .color_mode = CTUI_COLOR_MODE_RGB_FG,
                            .fg_b = 5,
                            .kitty_row = 2};
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strcmp(out, "\x1b[1;1H\x1b[38;2;0;0;5;49m\xf4\x8e\xbb\xae"
                               "\xcc\x8d\x1b[0m") == 0,
                   "a placeholder with kitty_row 2 is followed by kitty's "
                   "diacritic for row 1 (U+030D)");
  s->cells[0].kitty_row = 3;
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n > strlen("\x1b[0m") && strstr(out, "\xcc\x8e"),
                   "... a changed kitty_row alone repaints the cell");
  /* a run's first cell names its column: the same image right after
   * another doesn't take its columns on from it */
  s->cells[0].kitty_row = 0;
  s->cells[0].kitty_col = 1;
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strstr(out, "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85") != NULL,
                   "kitty_col 1 alone: row 0's and column 0's diacritics "
                   "(U+0305 twice)");
  s->cells[0].kitty_row = 2;
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strstr(out, "\xf4\x8e\xbb\xae\xcc\x8d\xcc\x85") != NULL,
                   "... with a row: the row's, then the column's");
  s->cells[0].kitty_col = 0;
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n > 0 && !strstr(out, "\xcc\x8d\xcc\x85"),
                   "... a changed kitty_col alone repaints the cell");
  CTUI_TEST_ASSERT(ctui_gfx_kitty_diacritic(0) == 0x0305 &&
                       ctui_gfx_kitty_diacritic(CTUI_GFX_KITTY_MAX_ROWS - 1) ==
                           0xA8E4,
                   "the diacritic table runs from U+0305 to kitty's 255th "
                   "entry, U+A8E4");

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

static void test_flush_attrs(void) {
  CTUI_SCREEN *s = ctui_screen_create(1, 4);
  char out[256];
  capture_flush(s, out, sizeof out); /* prime, same as above */

  s->cells[0] = (CTUI_CELL){.ch = 'B', .attr = CTUI_ATTR_BOLD};
  s->cells[1] = (CTUI_CELL){.ch = 'I',
                            .attr = CTUI_ATTR_ITALIC | CTUI_ATTR_UNDERLINE};
  s->cells[2] = (CTUI_CELL){.ch = 'p'};
  s->cells[3] = (CTUI_CELL){.ch = 'q'};
  size_t n = capture_flush(s, out, sizeof out);
  const char *want = "\x1b[1;1H\x1b[0;1;39;49mB\x1b[22;3;4mI"
                     "\x1b[23;24mpq\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "flush sets a frame's first attributes after a reset, "
                   "then switches off only those that go (22: bold and "
                   "dim, 23 italic, 24 underline) and on those that come, "
                   "colours unchanged left out, nothing repeated within a "
                   "run (got %s)", out + 1);

  s->cells[3].attr = CTUI_ATTR_STRIKE | CTUI_ATTR_DIM;
  n = capture_flush(s, out, sizeof out);
  want = "\x1b[1;4H\x1b[0;2;9;39;49mq\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "a cell whose attributes alone changed is redrawn "
                   "(dim + strike)");

  ctui_screen_putc(s, 0, 0, 'B', CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(s->cells[0].attr == 0,
                   "screen_putc writes a plain cell over a bold one");
  s->cells[1].attr = CTUI_ATTR_BOLD;
  ctui_screen_clear(s);
  CTUI_TEST_ASSERT(s->cells[1].attr == 0, "screen_clear drops attributes");

  ctui_screen_free(s);
}

/* the bytes saved beside the plain encoding: moves, refills, REP, the
 * wrap, SGR deltas */
static void test_flush_fewer_bytes(void) {
  CTUI_SCREEN *s = ctui_screen_create(2, 12);
  char out[256];
  capture_flush(s, out, sizeof out); /* prime */

  ctui_screen_puts(s, 0, 0, "--------", CTUI_COLOR_RED, CTUI_COLOR_DEFAULT);
  size_t n = capture_flush(s, out, sizeof out);
  const char *want = "\x1b[1;1H\x1b[31;49m-\x1b[7b\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "a run of one glyph in one style is the glyph and a REP "
                   "(got %s)", out + 1);

  ctui_screen_puts(s, 0, 0, "--------", CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);
  s->cells[6].fg = s->cells[7].fg = CTUI_COLOR_RED;
  n = capture_flush(s, out, sizeof out);
  want = "\x1b[1;1H\x1b[32;49m-\x1b[5b\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "a REP takes in only what changed (got %s)", out + 1);

  ctui_screen_puts(s, 0, 0, "ab", CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);
  ctui_screen_puts(s, 0, 4, "cd", CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);
  ctui_screen_puts(s, 0, 8, "e", CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);
  n = capture_flush(s, out, sizeof out);
  want = "\x1b[1;1H\x1b[32;49mab--cd\x1b[2Ce\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "unchanged cells in the colours the terminal has are "
                   "written again when that's shorter than a move; ones in "
                   "others are stepped over (CUF) (got %s)", out + 1);

  ctui_screen_putc(s, 0, 11, 'x', 0, 0);
  ctui_screen_putc(s, 1, 0, 'y', 0, 0);
  n = capture_flush(s, out, sizeof out);
  want = "\x1b[1;12H\x1b[39;49mxy\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "after the last column the terminal's pending wrap "
                   "places the next row's first glyph (got %s)", out + 1);

  ctui_screen_putc(s, 0, 10, 'w', 0, 0);
  ctui_screen_putc(s, 1, 0, 'v', 0, 0);
  ctui_screen_putc(s, 1, 5, 'u', 0, 0);
  n = capture_flush(s, out, sizeof out);
  want = "\x1b[1;11H\x1b[39;49mw\r\nv\x1b[4Cu\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "short of it, CR LF to the next row's start (got %s)",
                   out + 1);

  ctui_screen_putc(s, 0, 0, 'p', 0, 0);
  ctui_screen_putc(s, 1, 7, 'q', 0, 0);
  n = capture_flush(s, out, sizeof out);
  want = "\x1b[1;1H\x1b[39;49mp\x1b[2;8Hq\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "anywhere else a CUP (got %s)", out + 1);

  ctui_screen_free(s);
}

/* the compare goes bytes first; cells whose bytes differ only where their
 * colour mode doesn't look (an RGB cell's fg/bg, a BASIC cell's channels,
 * the padding) are still the same cells */
static void test_flush_unused_fields(void) {
  CTUI_SCREEN *s = ctui_screen_create(2, 6);
  char out[256];
  for (int i = 0; i < 12; i++) {
    s->cells[i] = (CTUI_CELL){.ch = (uint32_t)('a' + i),
                              .color_mode = (unsigned char)(i % 4),
                              .fg = 3,
                              .bg = 4,
                              .fg_r = 5,
                              .bg_b = 6};
  }
  capture_flush(s, out, sizeof out);
  for (int i = 0; i < 12; i++) {
    CTUI_CELL *c = &s->cells[i];
    switch (c->color_mode) {
    case CTUI_COLOR_MODE_RGB:
      c->fg = 99, c->bg = 98;
      break;
    case CTUI_COLOR_MODE_RGB_FG:
      c->fg = 97, c->bg_r = 1, c->bg_g = 2, c->bg_b = 3;
      break;
    default:
      c->fg_r = 7, c->fg_g = 8, c->bg_r = 9, c->bg_b = 10;
      break;
    }
    /* the padding, if the fields leave any (kitty_col took the last) */
    for (size_t b = offsetof(CTUI_CELL, attr) + 1; b < sizeof *c; b++) {
      ((unsigned char *)c)[b] ^= 0x5a;
    }
  }
  size_t n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n == 0, "cells differing only in fields their mode "
                           "doesn't use, and the padding, write nothing");
  s->cells[7].fg_g ^= 1; /* RGB_FG: used */
  s->cells[5].fg ^= 1;   /* 256: used */
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n > 0 && strchr(out, 'h') && strchr(out, 'f') &&
                       !strchr(out, 'a') && !strchr(out, 'g'),
                   "a field its mode uses still repaints just that cell "
                   "(got %s)", out + 1);
  ctui_screen_free(s);
}

static int g_frames = 0, g_refuse = 0;
static uint32_t g_prev_ch = 0, g_new_ch = 0;

static int count_frame(void *ctx, const CTUI_SCREEN *s) {
  (void)ctx;
  g_frames++;
  g_prev_ch = s->buffer[0].ch;
  g_new_ch = s->cells[0].ch;
  return g_refuse ? -1 : 0;
}

static void test_sink(void) {
  static const CTUI_SCREEN_SINK sink = {count_frame};
  CTUI_SCREEN *s = ctui_screen_create(2, 4);
  ctui_screen_set_sink(s, &sink, NULL);

  char out[256];
  ctui_screen_puts(s, 0, 0, "ab", CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  size_t n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n == 0 && g_frames == 1 && g_prev_ch == '\0' &&
                       g_new_ch == 'a',
                   "a sink screen's flush hands the frame over (its previous "
                   "one all '\\0': draw everything) and writes nothing to "
                   "stdout");

  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(g_frames == 1, "an unchanged frame isn't handed over");

  ctui_screen_putc(s, 1, 3, 'z', CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  g_refuse = 1;
  capture_flush(s, out, sizeof out);
  g_refuse = 0;
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(g_frames == 3 && g_prev_ch == 'a',
                   "a refused frame comes again on the next flush, against "
                   "the last frame the sink took");

  ctui_screen_resize(s, 3, 5);
  ctui_screen_puts(s, 0, 0, "x", CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  n = capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(n == 0 && g_frames == 4 && g_prev_ch == '\0',
                   "after a resize the sink's next frame is a full one");

  ctui_screen_set_sink(s, NULL, NULL);
  n = capture_flush(s, out, sizeof out);
  out[n] = '\0';
  CTUI_TEST_ASSERT(g_frames == 4 && strstr(out, "x") != NULL,
                   "back on the terminal the whole frame is written again");
  ctui_screen_free(s);
}

int main(void) {
  ctui_log_init(E_ALL);

  test_putc_puts_clear();
  test_flush_diff_and_color_cache();
  test_flush_256_and_rgb();
  test_flush_attrs();
  test_flush_fewer_bytes();
  test_flush_unused_fields();
  test_resize_forces_redraw();
  test_sink();

  return ctui_test_summary();
}
