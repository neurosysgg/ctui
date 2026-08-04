#include "debug_info.h"

#include <stdio.h>

void ctui_debug_info_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  char buf[64];
  ctui_logf(E_INF, "[CTUI:DEBUG_INFO] - rendering @ tick %d (screen=%dx%d)\n",
            ctui_tick_advance(), comp->cols, comp->rows);

  ctui_widget_puts(self, comp, 0, 0, "__debug_info__", CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
  snprintf(buf, sizeof(buf), "width: %d chars/columns", comp->cols);
  ctui_widget_puts(self, comp, 1, 0, buf, CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
  snprintf(buf, sizeof(buf), "height: %d chars/rows", comp->rows);
  ctui_widget_puts(self, comp, 2, 0, buf, CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
}
