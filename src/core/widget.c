#include "widget.h"

#include "ctui_internal.h"
#include "gfx.h"
#include "log.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

CTUI_WIDGET ctui_widget_make(int x, int y, int w, int h, void *widget_data,
                             void (*render)(CTUI_WIDGET *self,
                                            CTUI_COMPOSITOR *comp),
                             void (*layout)(CTUI_WIDGET *self,
                                           CTUI_COMPOSITOR *comp)) {
  ctui_logf(E_INF,
            "[CTUI:WIDGET] - creating widget @ tick %d (x=%d, y=%d, w=%d, "
            "h=%d)\n",
            ctui_tick_advance(), x, y, w, h);
  return (CTUI_WIDGET){
      .x = x,
      .y = y,
      .w = w,
      .h = h,
      .widget_data = widget_data,
      .ticks = 0,
      .buf = NULL,
      .layout = layout,
      .render = render,
      .supported_gfx_modes =
          CTUI_GFX_ANSI16 | CTUI_GFX_ANSI256 | CTUI_GFX_TRUECOLOR,
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
  widget->buf = comp->cells + (size_t)widget->y * (size_t)comp->cols +
               (size_t)widget->x;
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
 * the string lands where it would have. */
static int widget_put(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, uint32_t ch, const CTUI_CELL *style) {
  CTUI_CELL *cell = widget_cell_at(widget, comp, row, col);
  if (cell == NULL) {
    int w = ctui_utf8_cpwidth(ch);
    return w == 0 ? 0 : w;
  }
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - putc @ tick %d (row=%d, col=%d, ch=U+%04X, "
            "mode=%d)\n",
            ctui_tick_advance(), row, col, ch, style->color_mode);

  int abs_col = widget->x + col;
  CTUI_CELL *comp_row = cell - abs_col;
  int limit = widget->x + widget->w;
  int w = ctui_cell_set_ch(comp_row, comp->cols, abs_col, limit, ch);
  for (int i = 0; i < w; i++) {
    uint32_t keep = cell[i].ch;
    cell[i] = *style;
    cell[i].ch = keep;
  }
  return w;
}

static int widget_puts_styled(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp,
                              int row, int col, const char *str,
                              const CTUI_CELL *style) {
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - puts @ tick %d (row=%d, col=%d, len=%zu, "
            "mode=%d): \"%s\"\n",
            ctui_tick_advance(), row, col, strlen(str), style->color_mode,
            str);
  while (*str) {
    uint32_t cp;
    str += ctui_utf8_decode(str, &cp);
    col += widget_put(widget, comp, row, col, cp, style);
  }
  return col;
}

void ctui_widget_putc(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, uint32_t ch, unsigned char fg,
                      unsigned char bg) {
  CTUI_CELL style = {.fg = fg, .bg = bg, .color_mode = CTUI_COLOR_MODE_BASIC};
  widget_put(widget, comp, row, col, ch, &style);
}

void ctui_widget_puts(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, const char *str, unsigned char fg,
                      unsigned char bg) {
  CTUI_CELL style = {.fg = fg, .bg = bg, .color_mode = CTUI_COLOR_MODE_BASIC};
  widget_puts_styled(widget, comp, row, col, str, &style);
}

int ctui_widget_puts_cut(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                         int col, const char *str, int width,
                         unsigned char fg, unsigned char bg) {
  if (width <= 0) {
    return 0;
  }
  CTUI_CELL style = {.fg = fg, .bg = bg, .color_mode = CTUI_COLOR_MODE_BASIC};
  int w;
  size_t n = ctui_utf8_prefix(str, width, &w);
  if (!str[n]) {
    widget_puts_styled(widget, comp, row, col, str, &style);
    return w;
  }
  /* the prefix one column short of width, then U+2026 right after it:
   * "Doku…", never "Doku …" */
  n = ctui_utf8_prefix(str, width - 1, &w);
  while (n > 0 && str[n - 1] == ' ') {
    n--;
    w--;
  }
  const char *end = str + n;
  int c = col;
  while (str < end) {
    uint32_t cp;
    str += ctui_utf8_decode(str, &cp);
    c += widget_put(widget, comp, row, c, cp, &style);
  }
  widget_put(widget, comp, row, col + w, 0x2026, &style);
  return w + 1;
}

void ctui_widget_putc_256(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, uint32_t ch, unsigned char fg256,
                          unsigned char bg256) {
  CTUI_CELL style = {
      .fg = fg256, .bg = bg256, .color_mode = CTUI_COLOR_MODE_256};
  widget_put(widget, comp, row, col, ch, &style);
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
  widget_put(widget, comp, row, col, ch, &style);
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
  widget_put(widget, comp, row, col, ch, &style);
}

void ctui_widget_put_kitty_placeholder(CTUI_WIDGET *widget,
                                       CTUI_COMPOSITOR *comp, int row, int col,
                                       unsigned int image_id, int cols,
                                       int rows, unsigned char bg) {
  if (rows > CTUI_GFX_KITTY_MAX_ROWS) {
    rows = CTUI_GFX_KITTY_MAX_ROWS;
  }
  /* the id rides in the fg color; a single row goes without diacritics
   * (kitty takes row 0 and counts columns up along the run) */
  CTUI_CELL style = {.fg_r = (unsigned char)(image_id >> 16),
                     .fg_g = (unsigned char)(image_id >> 8),
                     .fg_b = (unsigned char)image_id,
                     .bg = bg,
                     .color_mode = CTUI_COLOR_MODE_RGB_FG};
  for (int r = 0; r < rows; r++) {
    style.kitty_row = rows > 1 ? (unsigned char)(r + 1) : 0;
    for (int i = 0; i < cols; i++) {
      widget_put(widget, comp, row + r, col + i, CTUI_GFX_KITTY_PLACEHOLDER,
                 &style);
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
  if (widget->gfx_render_mode == 0 || widget->gfx_render_mode != g_gfx_mode) {
    widget->render(widget, comp);
    return;
  }

  if (gfx_pending_count == gfx_pending_cap) {
    gfx_pending_cap = gfx_pending_cap ? gfx_pending_cap * 2 : 4;
    gfx_pending = realloc(gfx_pending,
                          sizeof(*gfx_pending) * (size_t)gfx_pending_cap);
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
  ctui_logf(E_DBG, "[CTUI:WIDGET] - flushed %d pending gfx widget(s) @ tick %d\n",
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
