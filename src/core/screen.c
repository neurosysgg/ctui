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
    s->cells[i].kitty_col = 0;
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
static int ctui_compare_ctuicell(const CTUI_CELL *lhs, const CTUI_CELL *rhs) {
  if (lhs->ch != rhs->ch || lhs->color_mode != rhs->color_mode ||
      lhs->kitty_row != rhs->kitty_row || lhs->kitty_col != rhs->kitty_col ||
      lhs->attr != rhs->attr) {
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

/* equal bytes are the same cell; unequal ones may still be (fields their
 * mode doesn't use, the padding), so the per-mode compare decides those */
static int same_cell(const CTUI_CELL *a, const CTUI_CELL *b) {
  return memcmp(a, b, sizeof *a) == 0 || ctui_compare_ctuicell(a, b);
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

/* the escapes are written by hand, not with snprintf(): its vfprintf
 * machinery (format parsing, a string FILE, the locale) per number made
 * a worst-case 240x60 frame (every cell moved to and coloured) 1.2 ms,
 * this 0.12 (ctui-wm's notes/self-improving.md, s1); the bytes are the
 * same (screen_test's reference). No bounds checks per write: flush
 * keeps 128 + CTUI_CLUSTER_MAX * 4 bytes free before each cell, which
 * covers the longest cell. */
#define PUT_LIT(p, lit)                                                      \
  (memcpy((p), (lit), sizeof(lit) - 1), (p) + sizeof(lit) - 1)

/* 0..255 in decimal: the digits padded to 4 bytes, so a copy is one
 * fixed-size memcpy (the bytes past n are overwritten by what follows) */
typedef struct {
  char s[4];
  unsigned char n;
} DIGITS;

/* digit i (0 = the first) of v, which has n digits */
#define DIG_N(v) ((v) >= 100 ? 3 : (v) >= 10 ? 2 : 1)
#define DIG_AT(v, i)                                                         \
  ((i) < DIG_N(v)                                                            \
       ? (char)('0' + (v) / ((i) + 1 == DIG_N(v)   ? 1                       \
                             : (i) + 2 == DIG_N(v) ? 10                      \
                                                   : 100) %                  \
                          10)                                                \
       : 0)
#define DIG(v) {{DIG_AT(v, 0), DIG_AT(v, 1), DIG_AT(v, 2), 0}, DIG_N(v)}
#define DIG4(v) DIG(v), DIG((v) + 1), DIG((v) + 2), DIG((v) + 3)
#define DIG16(v) DIG4(v), DIG4((v) + 4), DIG4((v) + 8), DIG4((v) + 12)
#define DIG64(v) DIG16(v), DIG16((v) + 16), DIG16((v) + 32), DIG16((v) + 48)

static const DIGITS digits[256] = {DIG64(0), DIG64(64), DIG64(128),
                                   DIG64(192)};

/* v in decimal, no NUL; returns past the last digit. Rows and columns
 * can pass 255, a BASIC cell's code too (an fg out of the 8 colours) */
static char *put_uint(char *p, unsigned v) {
  if (v < 256) {
    memcpy(p, digits[v].s, 4);
    return p + digits[v].n;
  }
  char tmp[10];
  int n = 0;
  while (v) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n) {
    *p++ = tmp[--n];
  }
  return p;
}

/* "r;g;b" */
static char *put_rgb(char *p, unsigned char r, unsigned char g,
                     unsigned char b) {
  p = put_uint(p, r);
  *p++ = ';';
  p = put_uint(p, g);
  *p++ = ';';
  return put_uint(p, b);
}

/* the SGR parameters for a cell's fg, and for its bg (no separator) */
static char *put_fg(char *p, const CTUI_CELL *cell) {
  switch (cell->color_mode) {
  case CTUI_COLOR_MODE_256:
    p = PUT_LIT(p, "38;5;");
    return put_uint(p, cell->fg);
  case CTUI_COLOR_MODE_RGB_FG:
  case CTUI_COLOR_MODE_RGB:
    p = PUT_LIT(p, "38;2;");
    return put_rgb(p, cell->fg_r, cell->fg_g, cell->fg_b);
  default:
    return put_uint(p, (unsigned)ansi_fg_code(cell->fg));
  }
}

static char *put_bg(char *p, const CTUI_CELL *cell) {
  switch (cell->color_mode) {
  case CTUI_COLOR_MODE_256:
    p = PUT_LIT(p, "48;5;");
    return put_uint(p, cell->bg);
  case CTUI_COLOR_MODE_RGB:
    p = PUT_LIT(p, "48;2;");
    return put_rgb(p, cell->bg_r, cell->bg_g, cell->bg_b);
  default:
    return put_uint(p, (unsigned)ansi_bg_code(cell->bg));
  }
}

/* whether two cells' fg (bg) come out as the same SGR parameters: the
 * mode decides how each side is sent (basic, 256 or RGB) */
static int fg_kind(const CTUI_CELL *c) {
  return c->color_mode == CTUI_COLOR_MODE_RGB_FG ? CTUI_COLOR_MODE_RGB
                                                 : c->color_mode;
}

static int fg_same(const CTUI_CELL *a, const CTUI_CELL *b) {
  if (fg_kind(a) != fg_kind(b)) {
    return 0;
  }
  if (fg_kind(a) == CTUI_COLOR_MODE_RGB) {
    return a->fg_r == b->fg_r && a->fg_g == b->fg_g && a->fg_b == b->fg_b;
  }
  return a->fg == b->fg;
}

static int bg_kind(const CTUI_CELL *c) {
  return c->color_mode == CTUI_COLOR_MODE_RGB_FG ? CTUI_COLOR_MODE_BASIC
                                                 : c->color_mode;
}

static int bg_same(const CTUI_CELL *a, const CTUI_CELL *b) {
  if (bg_kind(a) != bg_kind(b)) {
    return 0;
  }
  if (bg_kind(a) == CTUI_COLOR_MODE_RGB) {
    return a->bg_r == b->bg_r && a->bg_g == b->bg_g && a->bg_b == b->bg_b;
  }
  return a->bg == b->bg;
}

/* "1;2;..." for the CTUI_ATTR_* bits in attr */
static char *put_attrs(char *p, unsigned attr) {
  static const char sgr[] = {'1', '2', '3', '4', '9'};
  for (int i = 0; i < 5; i++) {
    if (attr & (1u << i)) {
      p[0] = sgr[i];
      p[1] = ';';
      p += 2;
    }
  }
  return p;
}

/* the SGR escape taking the terminal from last's colours and attributes
 * to cell's, only what changed in it: an attribute gone is switched off
 * (22 bold and dim together, 23, 24, 29), unless both colours change
 * anyway, when a reset ("0;") is shorter. known = 0: last isn't what the
 * terminal has (a frame's first escape), so both colours are sent, after
 * a reset when there are attributes (the last frame ended on one).
 * Returns past it -- nothing written when nothing differs. */
static char *emit_color(char *p, const CTUI_CELL *cell, const CTUI_CELL *last,
                        int known) {
  char *start = p;
  int fg = !known || !fg_same(cell, last);
  int bg = !known || !bg_same(cell, last);
  unsigned gone = known ? (unsigned)(last->attr & ~cell->attr) : 0;
  p = PUT_LIT(p, "\x1b[");
  if (!known) {
    if (cell->attr) {
      p = put_attrs(PUT_LIT(p, "0;"), cell->attr);
    }
  } else if (cell->attr == last->attr) {
    /* the usual case: colours alone */
  } else if (gone && fg && bg) {
    p = put_attrs(PUT_LIT(p, "0;"), cell->attr);
  } else {
    unsigned add = (unsigned)(cell->attr & ~last->attr);
    if (gone & (CTUI_ATTR_BOLD | CTUI_ATTR_DIM)) {
      p = PUT_LIT(p, "22;");
      add |= cell->attr & (CTUI_ATTR_BOLD | CTUI_ATTR_DIM);
    }
    if (gone & CTUI_ATTR_ITALIC) {
      p = PUT_LIT(p, "23;");
    }
    if (gone & CTUI_ATTR_UNDERLINE) {
      p = PUT_LIT(p, "24;");
    }
    if (gone & CTUI_ATTR_STRIKE) {
      p = PUT_LIT(p, "29;");
    }
    p = put_attrs(p, add);
  }
  if (fg) {
    p = put_fg(p, cell);
    *p++ = ';';
  }
  if (bg) {
    p = put_bg(p, cell);
    *p++ = ';';
  }
  if (p == start + 2) {
    return start;
  }
  p[-1] = 'm';
  return p;
}

/* the digits put_uint() writes for v (5 standing for any more) */
static int digits_of(unsigned v) {
  return v < 10 ? 1 : v < 100 ? 2 : v < 1000 ? 3 : 5;
}

/* the unchanged cells line[from..to) written out again, when that is
 * shorter than limit bytes: each a plain one-column glyph in the colours
 * the terminal has now. Returns past them, or NULL (nothing kept) */
static char *refill(char *p, const CTUI_CELL *line, int from, int to,
                    const CTUI_CELL *last_color, int limit) {
  char *start = p;
  for (int c = from; c < to; c++) {
    const CTUI_CELL *cell = &line[c];
    if (p - start >= limit || color_changed(cell, last_color) ||
        cell->ch == CTUI_CELL_CONT || cell->ch == CTUI_GFX_KITTY_PLACEHOLDER ||
        ctui_cell_width(cell->ch) != 1) {
      return NULL;
    }
    p += ctui_cell_encode(cell->ch, p);
  }
  return p - start < limit ? p : NULL;
}

/* the cursor from where the last cell left it (last_row, last_col: col
 * may be cols, a deferred wrap pending) to (r, c), in the fewest bytes:
 * nothing when the wrap lands there, CR LF to the next row's start, the
 * cells between written again or CUF along a row, else CUP. line is row
 * r's cells. */
static char *move_cursor(char *p, const CTUI_CELL *line, int r, int c,
                         int last_row, int last_col,
                         const CTUI_CELL *last_color) {
  if (r == last_row + 1 && c == 0 && last_row >= 0) {
    /* a deferred wrap puts the next glyph there itself */
    if (last_col == -2) {
      return p;
    }
    return PUT_LIT(p, "\r\n");
  }
  int cup = 4 + digits_of((unsigned)r + 1) + digits_of((unsigned)c + 1);
  if (r == last_row && last_col >= 0 && c > last_col) {
    int n = c - last_col;
    int cuf = 3 + (n > 1 ? digits_of((unsigned)n) : 0);
    char *q = refill(p, line, last_col, c, last_color, cuf < cup ? cuf : cup);
    if (q) {
      return q;
    }
    if (cuf < cup) {
      p = PUT_LIT(p, "\x1b[");
      if (n > 1) {
        p = put_uint(p, (unsigned)n);
      }
      *p++ = 'C';
      return p;
    }
  }
  p = PUT_LIT(p, "\x1b[");
  p = put_uint(p, (unsigned)r + 1);
  *p++ = ';';
  p = put_uint(p, (unsigned)c + 1);
  *p++ = 'H';
  return p;
}

/* how many cells after (r, c) REP can draw: the same one-column glyph in
 * the same colours, up to the last of them that changed (the unchanged
 * ones between are drawn again, as they are). 0 for a glyph REP can't
 * repeat: a cluster (kitty repeats one codepoint), a wide one, a kitty
 * placeholder (its row is a diacritic). */
static int repeat_run(const CTUI_SCREEN *s, int r, int c) {
  const CTUI_CELL *line = &s->cells[r * s->cols];
  const CTUI_CELL *prev = &s->buffer[r * s->cols];
  uint32_t ch = line[c].ch;
  if ((ch & CTUI_CELL_CLUSTER) || ch == CTUI_GFX_KITTY_PLACEHOLDER ||
      ctui_cell_width(ch) != 1) {
    return 0;
  }
  int last = c;
  for (int j = c + 1; j < s->cols && line[j].ch == ch &&
                      line[j].kitty_row == line[c].kitty_row &&
                      !color_changed(&line[j], &line[c]);
       j++) {
    if (!ctui_compare_ctuicell(&line[j], &prev[j])) {
      last = j;
    }
  }
  return last - c;
}

/* the sink gets whole frames, and only changed ones */
static void flush_sink(CTUI_SCREEN *s) {
  size_t n = (size_t)s->rows * (size_t)s->cols;
  size_t i = 0;
  if (memcmp(s->cells, s->buffer, n * sizeof(CTUI_CELL)) == 0) {
    return;
  }
  while (i < n && same_cell(&s->cells[i], &s->buffer[i])) {
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
    /* most rows are unchanged, and bytes the same: one memcmp (libc's,
     * vectorised) skips them -- the compare was ~3/4 of flush's time on
     * real frames, this cut it ~9x (ctui-wm's notes/self-improving.md,
     * s3) */
    if (memcmp(&s->cells[r * s->cols], &s->buffer[r * s->cols],
               sizeof(CTUI_CELL) * (size_t)s->cols) == 0) {
      continue;
    }
    for (int c = 0; c < s->cols; c++) {
      CTUI_CELL *cur = &s->cells[r * s->cols + c];
      CTUI_CELL *buf = &s->buffer[r * s->cols + c];

      if (same_cell(cur, buf))
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
        len = (size_t)(move_cursor(out + len, &s->cells[r * s->cols], r, c,
                                   last_row, last_col, &last_color) -
                       out);
      }

      if (color_changed(cur, &last_color)) {
        len = (size_t)(emit_color(out + len, cur, &last_color,
                                  last_color.color_mode != 0xff) -
                       out);
        last_color = *cur;
      }

      /* ctui_cell_encode() never lets a raw control byte reach the
       * terminal -- a stray \n or ESC in cell content would corrupt the
       * whole frame, not just one cell -- nor a noncharacter (kitty draws
       * nothing for it, the row would shift left by a cell): U+FFFD */
      uint32_t ch = cur->ch;
      int bytes = ctui_cell_encode(ch, out + len);
      len += (size_t)bytes;
      if (ch == CTUI_GFX_KITTY_PLACEHOLDER &&
          (cur->kitty_row || cur->kitty_col)) {
        len += (size_t)ctui_utf8_encode(
            ctui_gfx_kitty_diacritic(cur->kitty_row ? cur->kitty_row - 1 : 0),
            out + len);
        if (cur->kitty_col) {
          len += (size_t)ctui_utf8_encode(
              ctui_gfx_kitty_diacritic(cur->kitty_col - 1), out + len);
        }
      } else if (c + 1 < s->cols && cur[1].ch == ch) {
        int k = repeat_run(s, r, c);
        if (k > 0 && k * bytes > 3 + (k > 1 ? digits_of((unsigned)k) : 0)) {
          char *p = PUT_LIT(out + len, "\x1b[");
          if (k > 1) {
            p = put_uint(p, (unsigned)k);
          }
          *p++ = 'b';
          len = (size_t)(p - out);
          c += k;
        }
      }
      last_row = r;
      last_col = c + ctui_cell_width(ch);
      /* the last column written: the terminal holds the cursor there,
       * a wrap pending -- only a glyph or CR LF go on from it */
      if (last_col >= s->cols) {
        last_col = -2;
      }
    }
  }

  /* nothing changed: nothing written (an app redrawing every frame, a
   * visualiser at rest, would otherwise wake the terminal with a lone
   * reset each time; the last frame's reset still holds) */
  if (len == 0) {
    return;
  }
  len = (size_t)(PUT_LIT(out + len, "\x1b[0m") - out);

  write(STDOUT_FILENO, out, len);
  ctui_logf(E_INF, "[CTUI:SCREEN] - flush wrote %zu bytes @ tick %d\n", len,
            ctui_tick_advance());
  memcpy(s->buffer, s->cells,
         sizeof(CTUI_CELL) * (size_t)s->rows * (size_t)s->cols);
}
