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
#define _POSIX_C_SOURCE 200809L /* fileno() */
#include "ctui.h"

#include "ctui_test.h"

#include <stdio.h>
#include <stdlib.h>
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
  const char *want = "\x1b[1;1H\x1b[0;1;39;49mB\x1b[0;3;4;39;49mI"
                     "\x1b[0;39;49mpq\x1b[0m";
  CTUI_TEST_ASSERT(n == strlen(want) && strcmp(out, want) == 0,
                   "flush resets and sets each run's attributes in the "
                   "colour escape, back to plain with a reset only, and "
                   "repeats nothing within a run (got %s)", out + 1);

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

/* the reference for flush's bytes: its loop as it was with snprintf()
 * doing the formatting, before the escapes were written by hand
 * (2026-10-15). Kept simple and slow on purpose; flush must match it
 * byte for byte on any frame. */
static int ref_same(const CTUI_CELL *a, const CTUI_CELL *b, int with_ch) {
  if ((with_ch && (a->ch != b->ch || a->kitty_row != b->kitty_row)) ||
      a->color_mode != b->color_mode || a->attr != b->attr) {
    return 0;
  }
  int fg_rgb = a->color_mode == CTUI_COLOR_MODE_RGB ||
               a->color_mode == CTUI_COLOR_MODE_RGB_FG;
  if (fg_rgb &&
      (a->fg_r != b->fg_r || a->fg_g != b->fg_g || a->fg_b != b->fg_b)) {
    return 0;
  }
  if (!fg_rgb && a->fg != b->fg) {
    return 0;
  }
  if (a->color_mode == CTUI_COLOR_MODE_RGB) {
    return a->bg_r == b->bg_r && a->bg_g == b->bg_g && a->bg_b == b->bg_b;
  }
  return a->bg == b->bg;
}

static size_t ref_flush(const CTUI_CELL *cells, const CTUI_CELL *prev,
                        int rows, int cols, char *out, size_t cap) {
  static const char *const sgr[] = {"1;", "2;", "3;", "4;", "9;"};
  size_t len = 0;
  int last_row = -1, last_col = -1;
  CTUI_CELL last = {.color_mode = 0xff};
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const CTUI_CELL *cur = &cells[r * cols + c];
      if (ref_same(cur, &prev[r * cols + c], 1) || cur->ch == CTUI_CELL_CONT) {
        continue;
      }
      if (r != last_row || c != last_col) {
        len += (size_t)snprintf(out + len, cap - len, "\x1b[%d;%dH", r + 1,
                                c + 1);
      }
      if (!ref_same(cur, &last, 0)) {
        int fg = cur->fg == CTUI_COLOR_DEFAULT ? 39 : 30 + (cur->fg - 1);
        int bg = cur->bg == CTUI_COLOR_DEFAULT ? 49 : 40 + (cur->bg - 1);
        len += (size_t)snprintf(out + len, cap - len, "\x1b[");
        if (cur->attr != last.attr) {
          len += (size_t)snprintf(out + len, cap - len, "0;");
          for (int i = 0; i < 5; i++) {
            if (cur->attr & (1 << i)) {
              len += (size_t)snprintf(out + len, cap - len, "%s", sgr[i]);
            }
          }
        }
        if (cur->color_mode == CTUI_COLOR_MODE_256) {
          len += (size_t)snprintf(out + len, cap - len, "38;5;%d;48;5;%dm",
                                  cur->fg, cur->bg);
        } else if (cur->color_mode == CTUI_COLOR_MODE_RGB_FG) {
          len += (size_t)snprintf(out + len, cap - len, "38;2;%d;%d;%d;%dm",
                                  cur->fg_r, cur->fg_g, cur->fg_b, bg);
        } else if (cur->color_mode == CTUI_COLOR_MODE_RGB) {
          len += (size_t)snprintf(out + len, cap - len,
                                  "38;2;%d;%d;%d;48;2;%d;%d;%dm", cur->fg_r,
                                  cur->fg_g, cur->fg_b, cur->bg_r, cur->bg_g,
                                  cur->bg_b);
        } else {
          len += (size_t)snprintf(out + len, cap - len, "%d;%dm", fg, bg);
        }
        last = *cur;
      }
      len += (size_t)ctui_cell_encode(cur->ch, out + len);
      if (cur->ch == CTUI_GFX_KITTY_PLACEHOLDER && cur->kitty_row) {
        len += (size_t)ctui_utf8_encode(
            ctui_gfx_kitty_diacritic(cur->kitty_row - 1), out + len);
      }
      last_row = r;
      last_col = c + ctui_cell_width(cur->ch);
    }
  }
  if (len) {
    len += (size_t)snprintf(out + len, cap - len, "\x1b[0m");
  }
  return len;
}

/* flush into a temporary file: frames here pass a pipe's 64 KiB */
static size_t capture_big(CTUI_SCREEN *s, char *out, size_t cap) {
  FILE *f = tmpfile();
  int saved = dup(STDOUT_FILENO);
  dup2(fileno(f), STDOUT_FILENO);
  ctui_screen_flush(s);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  rewind(f);
  size_t n = fread(out, 1, cap, f);
  fclose(f);
  return n;
}

static unsigned g_rng = 2026;
static unsigned rnd(void) {
  g_rng = g_rng * 1103515245u + 12345u;
  return g_rng >> 8;
}

/* random frames, each partly over the last: every colour mode, out-of-
 * range BASIC colours (codes past 255), attributes, wide glyphs,
 * clusters, kitty placeholders with rows, a screen past 255 rows and
 * columns (positions past the digit table) */
static void test_flush_matches_reference(void) {
  static const char *const text[] = {"a", "Z", " ", "\xe4\xb8\x80",
                                     "e\xcc\x81", "\xf0\x9f\x91\x8d",
                                     "\xe2\x94\x80"};
  static const int sizes[][2] = {{3, 7}, {24, 80}, {12, 300}, {270, 4}};
  int bad = 0, frames = 0;
  size_t bytes = 0;
  for (size_t z = 0; z < sizeof sizes / sizeof sizes[0]; z++) {
    int rows = sizes[z][0], cols = sizes[z][1];
    size_t n = (size_t)rows * (size_t)cols;
    size_t cap = n * 100 + 64;
    CTUI_SCREEN *s = ctui_screen_create(rows, cols);
    CTUI_CELL *prev = malloc(n * sizeof *prev);
    char *got = malloc(cap), *want = malloc(cap);
    for (int f = 0; f < 6; f++) {
      memcpy(prev, s->buffer, n * sizeof *prev);
      unsigned touch = f == 0 ? 1 : 2 + rnd() % 6; /* 1 in touch changes */
      for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
          if (rnd() % touch) {
            continue;
          }
          if (rnd() % 16 == 0) {
            ctui_screen_putc(s, r, c, CTUI_GFX_KITTY_PLACEHOLDER, 0, 0);
            s->cells[r * cols + c].kitty_row = (unsigned char)(rnd() % 4);
          } else {
            ctui_screen_puts(s, r, c, text[rnd() % 7], 0, 0);
          }
          CTUI_CELL *cell = &s->cells[r * cols + c];
          cell->color_mode = (unsigned char)(rnd() % 4);
          cell->fg = (unsigned char)(rnd() % 3 ? rnd() % 9 : rnd());
          cell->bg = (unsigned char)(rnd() % 3 ? rnd() % 9 : rnd());
          cell->fg_r = (unsigned char)rnd();
          cell->fg_g = (unsigned char)(rnd() % 2 ? rnd() : rnd() % 10);
          cell->fg_b = (unsigned char)(rnd() % 2 ? rnd() : 99 + rnd() % 2);
          cell->bg_r = (unsigned char)rnd();
          cell->bg_g = (unsigned char)rnd();
          cell->bg_b = (unsigned char)rnd();
          cell->attr = (unsigned char)(rnd() % 3 ? 0 : rnd() % 32);
        }
      }
      size_t wn = ref_flush(s->cells, prev, rows, cols, want, cap);
      size_t gn = capture_big(s, got, cap);
      bad += gn != wn || memcmp(got, want, wn) != 0;
      frames++;
      bytes += gn;
    }
    free(prev);
    free(got);
    free(want);
    ctui_screen_free(s);
  }
  CTUI_TEST_ASSERT(bad == 0,
                   "flush writes the reference's bytes on %d random frames "
                   "(%zu bytes; %d differ)",
                   frames, bytes, bad);
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
  test_flush_matches_reference();
  test_resize_forces_redraw();
  test_sink();

  return ctui_test_summary();
}
