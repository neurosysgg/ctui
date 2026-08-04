#include "status.h"

#include <stdio.h>

void ctui_status_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_STATUS *data = self->widget_data;
  ctui_logf(E_INF, "[CTUI:STATUS] - rendering @ tick %d (text=\"%s\")\n",
            ctui_tick_advance(), data->text);

  ctui_widget_puts(self, comp, 0, 0, "Status:", CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
  ctui_widget_puts(self, comp, 1, 0,
                   data->text[0] ? data->text : "(nothing yet)",
                   CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
}

int ctui_status_handle_value_changed(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_STATUS *data = self->widget_data;
  CTUI_VALUE_CHANGED_EVENT_DATA *changed = ev->event_data;

  snprintf(data->text, sizeof(data->text), "you picked: %s", changed->value);
  ctui_logf(E_INF, "[CTUI:STATUS] - value changed @ tick %d: \"%s\"\n",
            ctui_tick_advance(), changed->value);
  return 1;
}
