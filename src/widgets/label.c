#include "label.h"

#include <string.h>

void ctui_label_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_LABEL *label = self->widget_data;
  int row = (self->h - 1) / 2;
  ctui_logf(E_INF, "[CTUI:LABEL] - rendering @ tick %d (%dx%d): \"%s\"\n",
            ctui_tick_advance(), self->w, self->h, label->text);

  /* text wider than the label is clipped to what fits (it used to be
   * dropped entirely -- center_h rejected it and the row stayed blank) */
  int width;
  size_t n = ctui_utf8_prefix(label->text, self->w, &width);
  char fit[n + 1];
  memcpy(fit, label->text, n);
  fit[n] = '\0';
  ctui_widget_puts(self, comp, row, (self->w - width) / 2, fit, label->fg,
                   label->bg);
}
