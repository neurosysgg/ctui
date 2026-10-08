#include "screen.h"

#include "gfx.h"
#include "log.h"
#include "utf8.h"

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

  /* worst case is an RGB cell's escape, "\x1b[0;1;2;3;4;9;38;2;255;255;
   * 255;48;2;255;255;255m" (~50 bytes) plus a position escape (~11) plus
   * the glyph itself -- 80/cell budget covers that with room to spare */
  s->out_cap = (size_t)rows * (size_t)cols * 80 + 64;
  s->out = malloc(s->out_cap);
}

CTUI_SCREEN *ctui_screen_create(int rows, int cols) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - creating %dx%d screen @ tick %d\n", cols,
            rows, ctui_tick_advance());
  CTUI_SCREEN *screen = malloc(sizeof(CTUI_SCREEN));
  screen->sink = NULL;
  screen->sink_ctx = NULL;
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

static unsigned g_clears;

unsigned ctui_screen_clears(void) { return g_clears; }

void ctui_screen_resize(CTUI_SCREEN *s, int rows, int cols) {
  ctui_logf(E_INF,
            "[CTUI:SCREEN] - resizing %dx%d -> %dx%d @ tick %d\n", s->cols,
            s->rows, cols, rows, ctui_tick_advance());
  free(s->cells);
  free(s->buffer);
  free(s->out);
  screen_alloc(s, rows, cols);
  if (s->sink) {
    return;
  }

  /* clear the real terminal too -- a shrink could otherwise leave stale
   * content from the old (larger) frame outside the new bounds. Guarded by
   * isatty() so a headless caller (tools/ctui_test.h, which drives resize
   * through ctui_app_resize() without a real terminal on stdout) doesn't
   * spew a raw escape sequence into a pipe/log. */
  if (isatty(STDOUT_FILENO)) {
    printf("\x1b[2J");
    fflush(stdout);
    g_clears++;
  }
}

static void screen_invalidate(CTUI_SCREEN *s) {
  for (int i = 0; i < s->rows * s->cols; i++) {
    s->buffer[i].ch = '\0';
  }
}

void ctui_screen_set_sink(CTUI_SCREEN *s, const CTUI_SCREEN_SINK *sink,
                          void *ctx) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - frames go to %s @ tick %d\n",
            sink ? "a sink" : "the terminal", ctui_tick_advance());
  s->sink = sink;
  s->sink_ctx = ctx;
  screen_invalidate(s);
}

void ctui_screen_clear(CTUI_SCREEN *s) {
  ctui_logf(E_DBG, "[CTUI:SCREEN] - clearing %dx%d screen @ tick %d\n",
            s->cols, s->rows, ctui_tick_advance());
  for (int i = 0; i < s->rows * s->cols; i++) {
    s->cells[i].ch = ' ';
    s->cells[i].fg = CTUI_COLOR_DEFAULT;
    s->cells[i].bg = CTUI_COLOR_DEFAULT;
    s->cells[i].color_mode = CTUI_COLOR_MODE_BASIC;
    s->cells[i].kitty_row = 0;
    s->cells[i].attr = 0;
  }
}

static int screen_put(CTUI_SCREEN *s, int row, int col, uint32_t ch,
                      unsigned char fg, unsigned char bg) {
  if (row < 0 || row >= s->rows || col < 0 || col >= s->cols) {
    ctui_logf(E_WRN,
              "[CTUI:SCREEN] - putc out of bounds @ tick %d (row=%d, col=%d, "
              "size=%dx%d)\n",
              ctui_tick_advance(), row, col, s->cols, s->rows);
    return ctui_cell_width(ch);
  }
  ctui_logf(E_DBG,
            "[CTUI:SCREEN] - putc @ tick %d (row=%d, col=%d, ch=U+%04X)\n",
            ctui_tick_advance(), row, col, ch);
  CTUI_CELL *line = &s->cells[row * s->cols];
  int w = ctui_cell_set_ch(line, s->cols, col, s->cols, ch);
  for (int i = col; i < col + w; i++) {
    line[i].fg = fg;
    line[i].bg = bg;
    line[i].color_mode = CTUI_COLOR_MODE_BASIC;
    line[i].attr = 0;
  }
  return w;
}

void ctui_screen_putc(CTUI_SCREEN *s, int row, int col, uint32_t ch,
                      unsigned char fg, unsigned char bg) {
  screen_put(s, row, col, ch, fg, bg);
}

void ctui_screen_puts(CTUI_SCREEN *s, int row, int col, const char *str,
                      unsigned char fg, unsigned char bg) {
  ctui_logf(E_DBG,
            "[CTUI:SCREEN] - puts @ tick %d (row=%d, col=%d, len=%zu): "
            "\"%s\"\n",
            ctui_tick_advance(), row, col, strlen(str), str);
  while (*str) {
    uint32_t ch;
    str += ctui_utf8_cluster(str, (size_t)-1, &ch, NULL);
    col += screen_put(s, row, col, ch, fg, bg);
  }
}

static int ansi_fg_code(unsigned char c) {
  return c == CTUI_COLOR_DEFAULT ? 39 : 30 + (c - 1);
}
static int ansi_bg_code(unsigned char c) {
  return c == CTUI_COLOR_DEFAULT ? 49 : 40 + (c - 1);
}

/* whichever fields matter for a cell's color_mode -- BASIC and 256 both
 * key off plain fg/bg (just different index spaces), RGB off fg_r../bg_r..,
 * RGB_FG off fg_r.. and plain bg */
static int ctui_compare_ctuicell(CTUI_CELL *lhs, CTUI_CELL *rhs) {
  if (lhs->ch != rhs->ch || lhs->color_mode != rhs->color_mode ||
      lhs->kitty_row != rhs->kitty_row || lhs->attr != rhs->attr) {
    return 0;
  }
  if (lhs->color_mode == CTUI_COLOR_MODE_RGB_FG) {
    return lhs->fg_r == rhs->fg_r && lhs->fg_g == rhs->fg_g &&
           lhs->fg_b == rhs->fg_b && lhs->bg == rhs->bg;
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
  if (cur->color_mode != last->color_mode || cur->attr != last->attr) {
    return 1;
  }
  if (cur->color_mode == CTUI_COLOR_MODE_RGB_FG) {
    return cur->fg_r != last->fg_r || cur->fg_g != last->fg_g ||
           cur->fg_b != last->fg_b || cur->bg != last->bg;
  }
  if (cur->color_mode == CTUI_COLOR_MODE_RGB) {
    return cur->fg_r != last->fg_r || cur->fg_g != last->fg_g ||
          cur->fg_b != last->fg_b || cur->bg_r != last->bg_r ||
          cur->bg_g != last->bg_g || cur->bg_b != last->bg_b;
  }
  return cur->fg != last->fg || cur->bg != last->bg;
}

/* one SGR escape for cell's colours; when its attributes differ from
 * last_attr (what the terminal has now) it starts with a reset and sets
 * them again ("0;1;4;..."), since the colours follow in the same escape */
static size_t emit_color(char *out, size_t cap, size_t len,
                         const CTUI_CELL *cell, unsigned char last_attr) {
  static const char *const sgr[] = {"1;", "2;", "3;", "4;", "9;"};
  len += (size_t)snprintf(out + len, cap - len, "\x1b[");
  if (cell->attr != last_attr) {
    len += (size_t)snprintf(out + len, cap - len, "0;");
    for (int i = 0; i < 5; i++) {
      if (cell->attr & (1 << i)) {
        len += (size_t)snprintf(out + len, cap - len, "%s", sgr[i]);
      }
    }
  }
  switch (cell->color_mode) {
  case CTUI_COLOR_MODE_256:
    return len + (size_t)snprintf(out + len, cap - len, "38;5;%d;48;5;%dm",
                                  cell->fg, cell->bg);
  case CTUI_COLOR_MODE_RGB_FG:
    return len + (size_t)snprintf(out + len, cap - len, "38;2;%d;%d;%d;%dm",
                                  cell->fg_r, cell->fg_g, cell->fg_b,
                                  ansi_bg_code(cell->bg));
  case CTUI_COLOR_MODE_RGB:
    return len + (size_t)snprintf(out + len, cap - len,
                                  "38;2;%d;%d;%d;48;2;%d;%d;%dm", cell->fg_r,
                                  cell->fg_g, cell->fg_b, cell->bg_r,
                                  cell->bg_g, cell->bg_b);
  default:
    return len + (size_t)snprintf(out + len, cap - len, "%d;%dm",
                                  ansi_fg_code(cell->fg),
                                  ansi_bg_code(cell->bg));
  }
}

/* the sink gets whole frames, and only changed ones */
static void flush_sink(CTUI_SCREEN *s) {
  size_t n = (size_t)s->rows * (size_t)s->cols;
  size_t i = 0;
  while (i < n && ctui_compare_ctuicell(&s->cells[i], &s->buffer[i])) {
    i++;
  }
  if (i == n) {
    return;
  }
  if (s->sink->frame(s->sink_ctx, s) != 0) {
    ctui_logf(E_WRN, "[CTUI:SCREEN] - sink refused a frame @ tick %d\n",
              ctui_tick_advance());
    return;
  }
  ctui_logf(E_INF, "[CTUI:SCREEN] - frame handed to the sink @ tick %d\n",
            ctui_tick_advance());
  memcpy(s->buffer, s->cells, sizeof(CTUI_CELL) * n);
}

void ctui_screen_flush(CTUI_SCREEN *s) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - flush starting @ tick %d\n",
            ctui_tick_advance());
  if (s->sink) {
    flush_sink(s);
    return;
  }
  /* s->out is sized once (screen_alloc()) to the same worst-case-per-cell
   * budget this used to malloc() fresh on every call -- reused here rather
   * than allocated per flush */
  char *out = s->out;
  size_t cap = s->out_cap;
  size_t len = 0;

  int last_row = -1, last_col = -1;
  /* 0xff isn't a real CTUI_COLOR_MODE_* value, so the first emitted cell
   * always mismatches and gets its own color escape; attr 0 is what the
   * last flush's closing reset left */
  CTUI_CELL last_color = {.color_mode = 0xff};

  // iterate over cells, compare to our buffer and rewrite accordingly
  for (int r = 0; r < s->rows; r++) {
    for (int c = 0; c < s->cols; c++) {
      CTUI_CELL *cur = &s->cells[r * s->cols + c];
      CTUI_CELL *buf = &s->buffer[r * s->cols + c];

      if (ctui_compare_ctuicell(cur, buf) == 1)
        continue;

      /* the lead cell to its left draws both columns; if the lead
       * itself didn't change, neither did this (ctui_cell_set_ch() keeps
       * the pair consistent in both frames) */
      if (cur->ch == CTUI_CELL_CONT)
        continue;

      /* the per-cell budget (screen_alloc()) covers a codepoint with the
       * longest escapes; clusters can take more */
      if (cap - len < 128 + CTUI_CLUSTER_MAX * 4) {
        size_t ncap = cap + cap / 4 + 4096;
        char *p = realloc(out, ncap);
        if (p == NULL) {
          /* send what fits; the shadow buffer stays, so the next flush
           * repeats the rest */
          ctui_log(E_ERR, "[CTUI:SCREEN] - flush buffer: out of memory\n");
          write(STDOUT_FILENO, out, len);
          return;
        }
        s->out = out = p;
        s->out_cap = cap = ncap;
      }

      if (r != last_row || c != last_col) {
        len +=
            (size_t)snprintf(out + len, cap - len, "\x1b[%d;%dH", r + 1, c + 1);
      }

      if (color_changed(cur, &last_color)) {
        len = emit_color(out, cap, len, cur, last_color.attr);
        last_color = *cur;
      }

      /* ctui_cell_encode() never lets a raw control byte reach the
       * terminal -- a stray \n or ESC in cell content would corrupt the
       * whole frame, not just one cell -- nor a noncharacter (kitty draws
       * nothing for it, the row would shift left by a cell): U+FFFD */
      uint32_t ch = cur->ch;
      len += (size_t)ctui_cell_encode(ch, out + len);
      if (ch == CTUI_GFX_KITTY_PLACEHOLDER && cur->kitty_row) {
        len += (size_t)ctui_utf8_encode(
            ctui_gfx_kitty_diacritic(cur->kitty_row - 1), out + len);
      }
      last_row = r;
      last_col = c + ctui_cell_width(ch);
    }
  }

  len += (size_t)snprintf(out + len, cap - len, "\x1b[0m");

  write(STDOUT_FILENO, out, len);
  ctui_logf(E_INF, "[CTUI:SCREEN] - flush wrote %zu bytes @ tick %d\n", len,
            ctui_tick_advance());
  memcpy(s->buffer, s->cells,
         sizeof(CTUI_CELL) * (size_t)s->rows * (size_t)s->cols);
}
