#include "label.h"

#include <string.h>

void ctui_label_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_LABEL *label = self->widget_data;
  int row = (self->h - 1) / 2;
  ctui_logf(E_INF, "[CTUI:LABEL] - rendering @ tick %d (%dx%d): \"%s\"\n",
            ctui_tick_advance(), self->w, self->h, label->text);

  char line[self->w + 1];
  memset(line, ' ', (size_t)self->w);
  line[self->w] = '\0';
  ctui_util_center_h(label->text, line, (CTUI_CELL){.ch = ' '});

  ctui_widget_puts(self, comp, row, 0, line, label->fg, label->bg);
}
