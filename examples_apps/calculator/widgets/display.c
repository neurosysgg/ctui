#include "display.h"

#include <string.h>

CTUI_DISPLAY ctui_display_make(unsigned char fg, unsigned char bg) {
  ctui_logf(E_INF, "[CTUI:DISPLAY] - creating display @ tick %d\n",
            ctui_tick_advance());
  return (CTUI_DISPLAY){.text = "0", .fg = fg, .bg = bg};
}

void ctui_display_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_DISPLAY *d = self->widget_data;
  int row = (self->h - 1) / 2;
  ctui_logf(E_INF, "[CTUI:DISPLAY] - rendering @ tick %d: \"%s\"\n",
            ctui_tick_advance(), d->text);

  size_t len = strlen(d->text);
  const char *shown = d->text;
  if ((int)len > self->w) {
    shown += len - (size_t)self->w;
    len = (size_t)self->w;
  }

  char line[self->w + 1];
  memset(line, ' ', (size_t)self->w);
  line[self->w] = '\0';
  memcpy(line + (self->w - (int)len), shown, len);

  ctui_widget_puts(self, comp, row, 0, line, d->fg, d->bg);
}
