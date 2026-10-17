#include "widget.h"

#include "ctui_internal.h"
#include "gfx.h"
#include "log.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

CTUI_WIDGET
ctui_widget_make(int x, int y, int w, int h, void *widget_data,
                 void (*render)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp),
                 void (*layout)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp)) {
  ctui_logf(E_INF,
            "[CTUI:WIDGET] - creating widget @ tick %d (x=%d, y=%d, w=%d, "
            "h=%d)\n",
            ctui_tick_advance(), x, y, w, h);
  return (CTUI_WIDGET){.x = x,
                       .y = y,
                       .w = w,
                       .h = h,
                       .widget_data = widget_data,
                       .ticks = 0,
                       .buf = NULL,
                       .layout = layout,
                       .render = render,
                       .supported_gfx_modes = CTUI_GFX_ANSI16 |
                                              CTUI_GFX_ANSI256 |
                                              CTUI_GFX_TRUECOLOR,
                       .gfx_render_mode = 0,
                       .gfx_render = NULL,
                       .parent = NULL,
                       .is_active_child = NULL};
}

void ctui_widget_tick_advance(CTUI_WIDGET *widget) { ++widget->ticks; }

void ctui_widget_init(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp) {
  if (widget->layout) {
    widget->layout(widget, comp);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p layout() re-run @ tick %d (x=%d, "
              "y=%d, w=%d, h=%d)\n",
              (void *)widget, ctui_tick_advance(), widget->x, widget->y,
              widget->w, widget->h);
  }

  if (widget->x < 0 || widget->x >= comp->cols || widget->y < 0 ||
      widget->y >= comp->rows) {
    ctui_logf(E_WRN,
              "[CTUI:WIDGET] - widget %p origin out of compositor bounds @ "
              "tick %d (x=%d, y=%d, compositor=%dx%d); leaving buf NULL\n",
              (void *)widget, ctui_tick_advance(), widget->x, widget->y,
              comp->cols, comp->rows);
    widget->buf = NULL;
    return;
  }
  widget->buf =
      comp->cells + (size_t)widget->y * (size_t)comp->cols + (size_t)widget->x;
  ctui_logf(E_INF,
            "[CTUI:WIDGET] - widget %p bound to compositor slice @ tick %d "
            "(x=%d, y=%d, w=%d, h=%d)\n",
            (void *)widget, ctui_tick_advance(), widget->x, widget->y,
            widget->w, widget->h);
}

int ctui_widget_contains(const CTUI_WIDGET *widget, int row, int col) {
  return row >= widget->y && row < widget->y + widget->h && col >= widget->x &&
         col < widget->x + widget->w;
}

/* shared bounds-checking/resolution for every putc variant below -- widget
 * not bound, out of widget bounds, or out of compositor bounds all log +
 * return NULL rather than write anywhere */
static CTUI_CELL *widget_cell_at(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp,
                                 int row, int col) {
  if (widget->buf == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:WIDGET] - putc rejected @ tick %d, widget %p not bound "
              "to a compositor slice (call ctui_widget_init() first)\n",
              ctui_tick_advance(), (void *)widget);
    return NULL;
  }
  if (row < 0 || row >= widget->h || col < 0 || col >= widget->w) {
    ctui_logf(E_WRN,
              "[CTUI:WIDGET] - putc out of widget bounds @ tick %d (row=%d, "
              "col=%d, size=%dx%d)\n",
              ctui_tick_advance(), row, col, widget->w, widget->h);
    return NULL;
  }
  int abs_row = widget->y + row;
  int abs_col = widget->x + col;
  if (abs_row < 0 || abs_row >= comp->rows || abs_col < 0 ||
      abs_col >= comp->cols) {
    ctui_logf(E_WRN,
              "[CTUI:WIDGET] - putc out of compositor bounds @ tick %d "
              "(row=%d, col=%d, compositor=%dx%d)\n",
              ctui_tick_advance(), abs_row, abs_col, comp->cols, comp->rows);
    return NULL;
  }
  return widget->buf + (size_t)row * (size_t)comp->cols + (size_t)col;
}

/* shared by every putc/puts variant below: writes ch plus style's colors
 * (fg/bg/color_mode/rgb -- style.ch is ignored) at (row, col), keeping
 * wide glyphs paired via ctui_cell_set_ch(). A wide glyph that would
 * cross the widget's right edge is clipped to a space there, same as it
 * would be at the compositor's edge. Returns the columns consumed (so
 * puts can advance), which is also what puts advances by for a rejected
 * write -- a rejected write still "occupies" its columns, so the rest of
 * the string lands where it would have. widget_put() takes the common
 * case first and comes here for the rest -- kept out of line, or its
 * calls' register saves land on that case too. */
#ifdef __GNUC__
__attribute__((noinline))
#endif
static int widget_put_any(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, uint32_t ch, CTUI_CELL style) {
  CTUI_CELL *cell = widget_cell_at(widget, comp, row, col);
  if (cell == NULL) {
    return ctui_cell_width(ch);
  }
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - putc @ tick %d (row=%d, col=%d, ch=U+%04X, "
            "mode=%d)\n",
            ctui_tick_advance(), row, col, ch, style.color_mode);

  int abs_col = widget->x + col;
  CTUI_CELL *comp_row = cell - abs_col;
  int limit = widget->x + widget->w;
  int w = ctui_cell_set_ch(comp_row, comp->cols, abs_col, limit, ch);
  for (int i = 0; i < w; i++) {
    uint32_t keep = cell[i].ch;
    cell[i] = style;
    cell[i].ch = keep;
  }
  return w;
}

/* narrow for sure without asking the width table: printable ASCII and
 * what boxes, bars and graphs are drawn with -- box drawing, block
 * elements, braille (widget_test checks the ranges against the table) */
static int known_narrow(uint32_t ch) {
  return (ch - 0x20u < 0x5Fu) || (ch - 0x2500u < 0xA0u) ||
         (ch - 0x2800u < 0x100u);
}

/* widget_put_any()'s common case without its calls: a narrow glyph inside
 * the widget and the compositor -- nearly every putc; ctui-top draws its
 * boxes and graphs a cell at a time -- written straight into the cell
 * with the two CONT mends ctui_cell_set_ch() would make. Logging putcs
 * (E_DBG) goes the general way, which logs them. Each bound is one
 * unsigned compare (a negative wraps past it). The style comes by value,
 * here and to widget_put_any(): with its address taken, a putc wrapper
 * built it on the stack in pieces and read it back whole, a failed
 * store-to-load forward that cost more than the rest of the put. A
 * frame of putcs ~1.8x faster with gcc (ctui-wm's
 * notes/self-improving.md, session 220); widget_test checks every
 * variant against the old path cell by cell. */
static inline int widget_put(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp,
                             int row, int col, uint32_t ch, CTUI_CELL style) {
  int abs_row = widget->y + row;
  int abs_col = widget->x + col;
  if (widget->buf != NULL && (unsigned)row < (unsigned)widget->h &&
      (unsigned)col < (unsigned)widget->w &&
      (unsigned)abs_row < (unsigned)comp->rows &&
      (unsigned)abs_col < (unsigned)comp->cols &&
      !(ctui_g_log_verbosity & E_DBG) &&
      (known_narrow(ch) || ctui_cell_width(ch) == 1)) {
    /* addressed as widget_cell_at() does: from the widget's slice */
    CTUI_CELL *cell =
        widget->buf + (size_t)row * (size_t)comp->cols + (size_t)col;
    if (cell->ch == CTUI_CELL_CONT && abs_col > 0) {
      cell[-1].ch = ' ';
    }
    if (abs_col + 1 < comp->cols && cell[1].ch == CTUI_CELL_CONT) {
      cell[1].ch = ' ';
    }
    *cell = style;
    cell->ch = ch;
    return 1;
  }
  return widget_put_any(widget, comp, row, col, ch, style);
}

/* how many bytes at str (before end, NULL: none) are printable ASCII that
 * nothing joins (the next byte ASCII too, or the end): one cell each */
static int ascii_run(const char *str, const char *end) {
  const unsigned char *u = (const unsigned char *)str;
  int n = 0;
  while ((end == NULL || str + n < end) && u[n] >= 0x20 && u[n] < 0x7F &&
         ((end != NULL && str + n + 1 == end) || u[n + 1] < 0x80)) {
    n++;
  }
  return n;
}

/* str (up to end, NULL: its NUL) at (row, col) in style, cluster by
 * cluster; returns the column after it. A run of lone printable ASCII
 * inside the widget is written straight into the row: one bounds check,
 * and the wide glyphs it cuts mended at its two edges -- what
 * ctui_cell_set_ch() does cell by cell comes to that, each write having
 * already blanked the CONT to its right. ~5x fewer instructions a cell
 * than widget_put() (ctui-wm's notes/self-improving.md, session 219);
 * widget_test checks it against widget_put() cell by cell. */
static int widget_put_str(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, const char *str, const char *end,
                          const CTUI_CELL *style) {
  while ((end == NULL || str < end) && *str) {
    int run = ascii_run(str, end);
    int abs_row = widget->y + row;
    int abs_col = widget->x + col;
    if (run > widget->w - col) {
      run = widget->w - col;
    }
    if (run > comp->cols - abs_col) {
      run = comp->cols - abs_col;
    }
    if (run > 0 && widget->buf != NULL && row >= 0 && row < widget->h &&
        col >= 0 && abs_row >= 0 && abs_row < comp->rows && abs_col >= 0) {
      /* addressed as widget_cell_at() does: from the widget's slice */
      CTUI_CELL *cell =
          widget->buf + (size_t)row * (size_t)comp->cols + (size_t)col;
      CTUI_CELL *line = cell - abs_col;
      if (line[abs_col].ch == CTUI_CELL_CONT && abs_col > 0) {
        line[abs_col - 1].ch = ' ';
      }
      if (abs_col + run < comp->cols &&
          line[abs_col + run].ch == CTUI_CELL_CONT) {
        line[abs_col + run].ch = ' ';
      }
      for (int i = 0; i < run; i++) {
        line[abs_col + i] = *style;
        line[abs_col + i].ch = (unsigned char)str[i];
      }
      str += run;
      col += run;
      continue;
    }
    uint32_t ch;
    str += ctui_utf8_cluster(str, end ? (size_t)(end - str) : (size_t)-1, &ch,
                             NULL);
    col += widget_put(widget, comp, row, col, ch, *style);
  }
  return col;
}

static int widget_puts_styled(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp,
                              int row, int col, const char *str,
                              const CTUI_CELL *style) {
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - puts @ tick %d (row=%d, col=%d, len=%zu, "
            "mode=%d): \"%s\"\n",
            ctui_tick_advance(), row, col, strlen(str), style->color_mode, str);
  return widget_put_str(widget, comp, row, col, str, NULL, style);
}

void ctui_widget_putc(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, uint32_t ch, unsigned char fg,
                      unsigned char bg) {
  CTUI_CELL style = {.fg = fg, .bg = bg, .color_mode = CTUI_COLOR_MODE_BASIC};
  widget_put(widget, comp, row, col, ch, style);
}

void ctui_widget_puts(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, const char *str, unsigned char fg,
                      unsigned char bg) {
  CTUI_CELL style = {.fg = fg, .bg = bg, .color_mode = CTUI_COLOR_MODE_BASIC};
  widget_puts_styled(widget, comp, row, col, str, &style);
}

int ctui_widget_puts_n(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                       int col, const char *str, size_t n, unsigned char fg,
                       unsigned char bg) {
  CTUI_CELL style = {.fg = fg, .bg = bg, .color_mode = CTUI_COLOR_MODE_BASIC};
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - puts_n @ tick %d (row=%d, col=%d, n=%zu): "
            "\"%.*s\"\n",
            ctui_tick_advance(), row, col, n, (int)n, str);
  return widget_put_str(widget, comp, row, col, str, str + n, &style) - col;
}

int ctui_widget_puts_cut(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                         int col, const char *str, int width, unsigned char fg,
                         unsigned char bg) {
  CTUI_CELL style = {.fg = fg, .bg = bg, .color_mode = CTUI_COLOR_MODE_BASIC};
  return ctui_widget_puts_cut_cell(widget, comp, row, col, str, width, &style);
}

int ctui_widget_puts_cut_cell(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp,
                              int row, int col, const char *str, int width,
                              const CTUI_CELL *style) {
  if (width <= 0) {
    return 0;
  }
  int w;
  size_t n = ctui_utf8_prefix(str, width, &w);
  if (!str[n]) {
    widget_puts_styled(widget, comp, row, col, str, style);
    return w;
  }
  /* the prefix one column short of width, then U+2026 right after it:
   * "Doku…", never "Doku …" */
  n = ctui_utf8_prefix(str, width - 1, &w);
  while (n > 0 && str[n - 1] == ' ') {
    n--;
    w--;
  }
  widget_put_str(widget, comp, row, col, str, str + n, style);
  widget_put(widget, comp, row, col + w, 0x2026, *style);
  return w + 1;
}

int ctui_widget_putc_cell(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, uint32_t ch, const CTUI_CELL *style) {
  return widget_put(widget, comp, row, col, ch, *style);
}

int ctui_widget_puts_cell(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, const char *str, size_t n,
                          const CTUI_CELL *style) {
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - puts_cell @ tick %d (row=%d, col=%d, n=%zu, "
            "mode=%d, attr=%d)\n",
            ctui_tick_advance(), row, col, n, style->color_mode, style->attr);
  const char *end = n == (size_t)-1 ? NULL : str + n;
  return widget_put_str(widget, comp, row, col, str, end, style) - col;
}

void ctui_widget_putc_256(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, uint32_t ch, unsigned char fg256,
                          unsigned char bg256) {
  CTUI_CELL style = {
      .fg = fg256, .bg = bg256, .color_mode = CTUI_COLOR_MODE_256};
  widget_put(widget, comp, row, col, ch, style);
}

void ctui_widget_puts_256(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, const char *str, unsigned char fg256,
                          unsigned char bg256) {
  CTUI_CELL style = {
      .fg = fg256, .bg = bg256, .color_mode = CTUI_COLOR_MODE_256};
  widget_puts_styled(widget, comp, row, col, str, &style);
}

static CTUI_CELL rgb_style(unsigned char fg_r, unsigned char fg_g,
                           unsigned char fg_b, unsigned char bg_r,
                           unsigned char bg_g, unsigned char bg_b) {
  return (CTUI_CELL){.fg_r = fg_r,
                     .fg_g = fg_g,
                     .fg_b = fg_b,
                     .bg_r = bg_r,
                     .bg_g = bg_g,
                     .bg_b = bg_b,
                     .color_mode = CTUI_COLOR_MODE_RGB};
}

void ctui_widget_putc_rgb(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, uint32_t ch, unsigned char fg_r,
                          unsigned char fg_g, unsigned char fg_b,
                          unsigned char bg_r, unsigned char bg_g,
                          unsigned char bg_b) {
  CTUI_CELL style = rgb_style(fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
  widget_put(widget, comp, row, col, ch, style);
}

void ctui_widget_puts_rgb(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, const char *str, unsigned char fg_r,
                          unsigned char fg_g, unsigned char fg_b,
                          unsigned char bg_r, unsigned char bg_g,
                          unsigned char bg_b) {
  CTUI_CELL style = rgb_style(fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
  widget_puts_styled(widget, comp, row, col, str, &style);
}

void ctui_widget_putc_rgb_fg(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp,
                             int row, int col, uint32_t ch, unsigned char fg_r,
                             unsigned char fg_g, unsigned char fg_b,
                             unsigned char bg) {
  CTUI_CELL style = {.fg_r = fg_r,
                     .fg_g = fg_g,
                     .fg_b = fg_b,
                     .bg = bg,
                     .color_mode = CTUI_COLOR_MODE_RGB_FG};
  widget_put(widget, comp, row, col, ch, style);
}

void ctui_widget_put_kitty_placeholder(CTUI_WIDGET *widget,
                                       CTUI_COMPOSITOR *comp, int row, int col,
                                       unsigned int image_id, int cols,
                                       int rows, unsigned char bg) {
  if (rows > CTUI_GFX_KITTY_MAX_ROWS) {
    rows = CTUI_GFX_KITTY_MAX_ROWS;
  }
  /* the id rides in the fg color; a single row goes without a row
   * diacritic (kitty takes row 0 and counts columns up along the run);
   * each row's first cell says its column, 0: the same image right
   * before it would carry its count on */
  CTUI_CELL style = {.fg_r = (unsigned char)(image_id >> 16),
                     .fg_g = (unsigned char)(image_id >> 8),
                     .fg_b = (unsigned char)image_id,
                     .bg = bg,
                     .color_mode = CTUI_COLOR_MODE_RGB_FG};
  for (int r = 0; r < rows; r++) {
    style.kitty_row = rows > 1 ? (unsigned char)(r + 1) : 0;
    for (int i = 0; i < cols; i++) {
      style.kitty_col = i == 0 ? 1 : 0;
      widget_put(widget, comp, row + r, col + i, CTUI_GFX_KITTY_PLACEHOLDER,
                 style);
    }
  }
}

void ctui_widget_set_gfx_renderer(CTUI_WIDGET *widget, unsigned int mode,
                                  void (*render)(CTUI_WIDGET *self,
                                                 CTUI_COMPOSITOR *comp)) {
  widget->supported_gfx_modes = mode;
  widget->gfx_render_mode = mode;
  widget->gfx_render = render;
  ctui_logf(E_INF,
            "[CTUI:WIDGET] - widget %p gfx renderer registered @ tick %d "
            "(mode=0x%x)\n",
            (void *)widget, ctui_tick_advance(), mode);
}

/* widgets ctui_widget_dispatch_render() queued this frame, drained by
 * ctui_widget_flush_gfx() -- file-static rather than living on CTUI_APP,
 * same reasoning as core/timer.c's own registries: simpler, and there's
 * only ever one live app per process anyway. Flat realloc-grown array,
 * same shape as the event handler registry -- plenty fast at this scale,
 * and it's never more than a handful of gfx-mode widgets in one frame. */
static CTUI_WIDGET **gfx_pending = NULL;
static int gfx_pending_count = 0;
static int gfx_pending_cap = 0;

void ctui_widget_dispatch_render(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp) {
  if (widget->gfx_render_mode == 0 ||
      widget->gfx_render_mode != ctui_g_gfx_mode) {
    widget->render(widget, comp);
    if (ctui_g_app && ctui_g_app->rendered) {
      ctui_g_app->rendered(widget, comp, ctui_g_app->render_arg);
    }
    return;
  }

  if (gfx_pending_count == gfx_pending_cap) {
    gfx_pending_cap = gfx_pending_cap ? gfx_pending_cap * 2 : 4;
    gfx_pending =
        realloc(gfx_pending, sizeof(*gfx_pending) * (size_t)gfx_pending_cap);
  }
  gfx_pending[gfx_pending_count++] = widget;
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - widget %p queued for deferred gfx_render @ "
            "tick %d (mode=0x%x)\n",
            (void *)widget, ctui_tick_advance(), widget->gfx_render_mode);
}

void ctui_widget_flush_gfx(CTUI_COMPOSITOR *comp) {
  for (int i = 0; i < gfx_pending_count; i++) {
    gfx_pending[i]->gfx_render(gfx_pending[i], comp);
  }
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - flushed %d pending gfx widget(s) @ tick %d\n",
            gfx_pending_count, ctui_tick_advance());
  gfx_pending_count = 0;
  /* Phase 5a: every gfx_render() call above only appended to gfx.c's
   * internal Kitty batch buffer (ctui_gfx_kitty_display()/_delete()) --
   * this is the one write() that actually puts those bytes on the wire,
   * for this frame's queued widgets and any direct kitty_delete() calls
   * an earlier event handler issued this same iteration (see
   * GFX_DESIGN.md's Phase 5a hook-point rationale). */
  ctui_gfx_kitty_flush();
}

void ctui_widget_gfx_reset(void) {
  free(gfx_pending);
  gfx_pending = NULL;
  gfx_pending_count = 0;
  gfx_pending_cap = 0;
  ctui_logf(E_INF, "[CTUI:WIDGET] - gfx dispatch queue reset @ tick %d\n",
            ctui_tick_advance());
}
