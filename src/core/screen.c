#include "screen.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void screen_alloc(CTUI_SCREEN *s, int rows, int cols) {
  s->rows = rows;
  s->cols = cols;
  s->cells = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));
  s->buffer = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));

  for (int i = 0; i < rows * cols; i++) {
    s->cells[i].ch = ' ';
    s->buffer[i].ch = '\0'; /* force buffer flush on next draw */
  }
}

CTUI_SCREEN *ctui_screen_create(int rows, int cols) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - creating %dx%d screen @ tick %d\n", cols,
            rows, ctui_tick_advance());
  CTUI_SCREEN *screen = malloc(sizeof(CTUI_SCREEN));
  screen_alloc(screen, rows, cols);
  return screen;
}

void ctui_screen_free(CTUI_SCREEN *s) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - freeing %dx%d screen @ tick %d\n",
            s->cols, s->rows, ctui_tick_advance());
  free(s->cells);
  free(s->buffer);
  free(s);
}

void ctui_screen_resize(CTUI_SCREEN *s, int rows, int cols) {
  ctui_logf(E_INF,
            "[CTUI:SCREEN] - resizing %dx%d -> %dx%d @ tick %d\n", s->cols,
            s->rows, cols, rows, ctui_tick_advance());
  free(s->cells);
  free(s->buffer);
  screen_alloc(s, rows, cols);

  /* clear the real terminal too -- a shrink could otherwise leave stale
   * content from the old (larger) frame outside the new bounds. Guarded by
   * isatty() so a headless caller (tools/ctui_test.h, which drives resize
   * through ctui_app_resize() without a real terminal on stdout) doesn't
   * spew a raw escape sequence into a pipe/log. */
  if (isatty(STDOUT_FILENO)) {
    printf("\x1b[2J");
    fflush(stdout);
  }
}

void ctui_screen_clear(CTUI_SCREEN *s) {
  ctui_logf(E_DBG, "[CTUI:SCREEN] - clearing %dx%d screen @ tick %d\n",
            s->cols, s->rows, ctui_tick_advance());
  for (int i = 0; i < s->rows * s->cols; i++) {
    s->cells[i].ch = ' ';
    s->cells[i].fg = CTUI_COLOR_DEFAULT;
    s->cells[i].bg = CTUI_COLOR_DEFAULT;
  }
}

void ctui_screen_putc(CTUI_SCREEN *s, int row, int col, char ch,
                      unsigned char fg, unsigned char bg) {
  if (row < 0 || row >= s->rows || col < 0 || col >= s->cols) {
    ctui_logf(E_WRN,
              "[CTUI:SCREEN] - putc out of bounds @ tick %d (row=%d, col=%d, "
              "size=%dx%d)\n",
              ctui_tick_advance(), row, col, s->cols, s->rows);
    return;
  }
  ctui_logf(E_DBG,
            "[CTUI:SCREEN] - putc @ tick %d (row=%d, col=%d, ch='%c')\n",
            ctui_tick_advance(), row, col, ch);
  CTUI_CELL *cell = &s->cells[row * s->cols + col];
  cell->ch = ch;
  cell->bg = bg;
  cell->fg = fg;
}

void ctui_screen_puts(CTUI_SCREEN *s, int row, int col, const char *str,
                      unsigned char fg, unsigned char bg) {
  ctui_logf(E_DBG,
            "[CTUI:SCREEN] - puts @ tick %d (row=%d, col=%d, len=%zu): "
            "\"%s\"\n",
            ctui_tick_advance(), row, col, strlen(str), str);
  for (int i = 0; str[i] != '\0'; i++) {
    ctui_screen_putc(s, row, col + i, str[i], fg, bg);
  }
}

static int ansi_fg_code(unsigned char c) {
  return c == CTUI_COLOR_DEFAULT ? 39 : 30 + (c - 1);
}
static int ansi_bg_code(unsigned char c) {
  return c == CTUI_COLOR_DEFAULT ? 49 : 40 + (c - 1);
}

static int ctui_compare_ctuicell(CTUI_CELL *lhs, CTUI_CELL *rhs) {
  return (lhs->ch == rhs->ch && lhs->bg == rhs->bg && lhs->fg == rhs->fg);
}

void ctui_screen_flush(CTUI_SCREEN *s) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - flush starting @ tick %d\n",
            ctui_tick_advance());
  size_t cap = (size_t)s->rows * (size_t)s->cols * 24 + 64;
  char *out = malloc(cap);
  size_t len = 0;

  int last_row = -1, last_col = -1;
  int last_fg = -1, last_bg = -1;

  // iterate over cells, compare to our buffer and rewrite accordingly
  for (int r = 0; r < s->rows; r++) {
    for (int c = 0; c < s->cols; c++) {
      CTUI_CELL *cur = &s->cells[r * s->cols + c];
      CTUI_CELL *buf = &s->buffer[r * s->cols + c];

      if (ctui_compare_ctuicell(cur, buf) == 1)
        continue;

      if (r != last_row || c != last_col) {
        len +=
            (size_t)snprintf(out + len, cap - len, "\x1b[%d;%dH", r + 1, c + 1);
      }

      if (cur->fg != last_fg || cur->bg != last_bg) {
        len += (size_t)snprintf(out + len, cap - len, "\x1b[%d;%dm",
                                ansi_fg_code(cur->fg), ansi_bg_code(cur->bg));
        last_fg = cur->fg;
        last_bg = cur->bg;
      }

      out[len++] = cur->ch;
      last_row = r;
      last_col = c + 1;
    }
  }

  len += (size_t)snprintf(out + len, cap - len, "\x1b[0m");

  write(STDOUT_FILENO, out, len);
  ctui_logf(E_INF, "[CTUI:SCREEN] - flush wrote %zu bytes @ tick %d\n", len,
            ctui_tick_advance());
  free(out);
  memcpy(s->buffer, s->cells,
         sizeof(CTUI_CELL) * (size_t)s->rows * (size_t)s->cols);
}
