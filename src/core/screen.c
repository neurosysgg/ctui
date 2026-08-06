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

  /* worst case is an RGB cell's escape, "\x1b[38;2;255;255;255;48;2;255;
   * 255;255m" (~38 bytes) plus a position escape (~11) plus the glyph
   * itself -- 64/cell budget covers that with room to spare */
  s->out_cap = (size_t)rows * (size_t)cols * 64 + 64;
  s->out = malloc(s->out_cap);
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
  free(s->out);
  free(s);
}

void ctui_screen_resize(CTUI_SCREEN *s, int rows, int cols) {
  ctui_logf(E_INF,
            "[CTUI:SCREEN] - resizing %dx%d -> %dx%d @ tick %d\n", s->cols,
            s->rows, cols, rows, ctui_tick_advance());
  free(s->cells);
  free(s->buffer);
  free(s->out);
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
    s->cells[i].color_mode = CTUI_COLOR_MODE_BASIC;
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
  cell->color_mode = CTUI_COLOR_MODE_BASIC;
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

/* whichever fields matter for a cell's color_mode -- BASIC and 256 both
 * key off plain fg/bg (just different index spaces), RGB off fg_r../bg_r.. */
static int ctui_compare_ctuicell(CTUI_CELL *lhs, CTUI_CELL *rhs) {
  if (lhs->ch != rhs->ch || lhs->color_mode != rhs->color_mode) {
    return 0;
  }
  if (lhs->color_mode == CTUI_COLOR_MODE_RGB) {
    return lhs->fg_r == rhs->fg_r && lhs->fg_g == rhs->fg_g &&
          lhs->fg_b == rhs->fg_b && lhs->bg_r == rhs->bg_r &&
          lhs->bg_g == rhs->bg_g && lhs->bg_b == rhs->bg_b;
  }
  return lhs->fg == rhs->fg && lhs->bg == rhs->bg;
}

/* same field-set-per-mode logic as ctui_compare_ctuicell(), but against the
 * last cell actually emitted this flush (to decide whether a fresh SGR
 * escape is needed), not the previous frame's shadow buffer */
static int color_changed(const CTUI_CELL *cur, const CTUI_CELL *last) {
  if (cur->color_mode != last->color_mode) {
    return 1;
  }
  if (cur->color_mode == CTUI_COLOR_MODE_RGB) {
    return cur->fg_r != last->fg_r || cur->fg_g != last->fg_g ||
          cur->fg_b != last->fg_b || cur->bg_r != last->bg_r ||
          cur->bg_g != last->bg_g || cur->bg_b != last->bg_b;
  }
  return cur->fg != last->fg || cur->bg != last->bg;
}

static size_t emit_color(char *out, size_t cap, size_t len,
                         const CTUI_CELL *cell) {
  switch (cell->color_mode) {
  case CTUI_COLOR_MODE_256:
    return len + (size_t)snprintf(out + len, cap - len,
                                  "\x1b[38;5;%d;48;5;%dm", cell->fg,
                                  cell->bg);
  case CTUI_COLOR_MODE_RGB:
    return len +
          (size_t)snprintf(out + len, cap - len,
                            "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm", cell->fg_r,
                            cell->fg_g, cell->fg_b, cell->bg_r, cell->bg_g,
                            cell->bg_b);
  default:
    return len + (size_t)snprintf(out + len, cap - len, "\x1b[%d;%dm",
                                  ansi_fg_code(cell->fg),
                                  ansi_bg_code(cell->bg));
  }
}

void ctui_screen_flush(CTUI_SCREEN *s) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - flush starting @ tick %d\n",
            ctui_tick_advance());
  /* s->out is sized once (screen_alloc()) to the same worst-case-per-cell
   * budget this used to malloc() fresh on every call -- reused here rather
   * than allocated per flush */
  char *out = s->out;
  size_t cap = s->out_cap;
  size_t len = 0;

  int last_row = -1, last_col = -1;
  /* 0xff isn't a real CTUI_COLOR_MODE_* value, so the first emitted cell
   * always mismatches and gets its own color escape */
  CTUI_CELL last_color = {.color_mode = 0xff};

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

      if (color_changed(cur, &last_color)) {
        len = emit_color(out, cap, len, cur);
        last_color = *cur;
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
  memcpy(s->buffer, s->cells,
         sizeof(CTUI_CELL) * (size_t)s->rows * (size_t)s->cols);
}
