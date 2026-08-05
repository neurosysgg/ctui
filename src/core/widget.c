#include "widget.h"

#include "log.h"

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
  return (CTUI_WIDGET){.x = x,
                       .y = y,
                       .w = w,
                       .h = h,
                       .widget_data = widget_data,
                       .ticks = 0,
                       .buf = NULL,
                       .layout = layout,
                       .render = render};
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

void ctui_widget_putc(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, char ch, unsigned char fg, unsigned char bg) {
  CTUI_CELL *cell = widget_cell_at(widget, comp, row, col);
  if (cell == NULL) {
    return;
  }
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - putc @ tick %d (row=%d, col=%d, ch='%c')\n",
            ctui_tick_advance(), row, col, ch);
  cell->ch = ch;
  cell->bg = bg;
  cell->fg = fg;
  cell->color_mode = CTUI_COLOR_MODE_BASIC;
}

void ctui_widget_puts(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, const char *str, unsigned char fg,
                      unsigned char bg) {
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - puts @ tick %d (row=%d, col=%d, len=%zu): "
            "\"%s\"\n",
            ctui_tick_advance(), row, col, strlen(str), str);
  for (int i = 0; str[i] != '\0'; i++) {
    ctui_widget_putc(widget, comp, row, col + i, str[i], fg, bg);
  }
}

void ctui_widget_putc_256(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, char ch, unsigned char fg256,
                          unsigned char bg256) {
  CTUI_CELL *cell = widget_cell_at(widget, comp, row, col);
  if (cell == NULL) {
    return;
  }
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - putc_256 @ tick %d (row=%d, col=%d, ch='%c')\n",
            ctui_tick_advance(), row, col, ch);
  cell->ch = ch;
  cell->fg = fg256;
  cell->bg = bg256;
  cell->color_mode = CTUI_COLOR_MODE_256;
}

void ctui_widget_puts_256(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, const char *str, unsigned char fg256,
                          unsigned char bg256) {
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - puts_256 @ tick %d (row=%d, col=%d, len=%zu): "
            "\"%s\"\n",
            ctui_tick_advance(), row, col, strlen(str), str);
  for (int i = 0; str[i] != '\0'; i++) {
    ctui_widget_putc_256(widget, comp, row, col + i, str[i], fg256, bg256);
  }
}
