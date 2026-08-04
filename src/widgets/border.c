#include "border.h"

CTUI_BORDER ctui_border_make(unsigned char block_color) {
  CTUI_CELL block = {.ch = ' ', .fg = CTUI_COLOR_DEFAULT, .bg = block_color};
  return (CTUI_BORDER){.edge = block, .corner = block};
}

void ctui_border_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_BORDER *border = self->widget_data;
  ctui_logf(E_INF, "[CTUI:BORDER] - rendering @ tick %d (%dx%d)\n",
            ctui_tick_advance(), self->w, self->h);

  for (int col = 1; col < self->w - 1; col++) {
    ctui_widget_putc(self, comp, 0, col, border->edge.ch, border->edge.fg,
                     border->edge.bg);
    ctui_widget_putc(self, comp, self->h - 1, col, border->edge.ch,
                     border->edge.fg, border->edge.bg);
  }
  for (int row = 1; row < self->h - 1; row++) {
    ctui_widget_putc(self, comp, row, 0, border->edge.ch, border->edge.fg,
                     border->edge.bg);
    ctui_widget_putc(self, comp, row, self->w - 1, border->edge.ch,
                     border->edge.fg, border->edge.bg);
  }

  ctui_widget_putc(self, comp, 0, 0, border->corner.ch, border->corner.fg,
                   border->corner.bg);
  ctui_widget_putc(self, comp, 0, self->w - 1, border->corner.ch,
                   border->corner.fg, border->corner.bg);
  ctui_widget_putc(self, comp, self->h - 1, 0, border->corner.ch,
                   border->corner.fg, border->corner.bg);
  ctui_widget_putc(self, comp, self->h - 1, self->w - 1, border->corner.ch,
                   border->corner.fg, border->corner.bg);
}
