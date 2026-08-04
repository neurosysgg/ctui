#include "clock.h"

#include <string.h>
#include <time.h>

static void format_now(char *out, size_t out_size) {
  time_t now = time(NULL);
  struct tm *tmv = localtime(&now);
  strftime(out, out_size, "%H:%M:%S", tmv);
}

CTUI_CLOCK ctui_clock_make(unsigned char fg, unsigned char bg) {
  ctui_logf(E_INF, "[CTUI:CLOCK] - creating clock @ tick %d\n",
            ctui_tick_advance());
  CTUI_CLOCK clock = {.fg = fg, .bg = bg};
  format_now(clock.text, sizeof(clock.text));
  return clock;
}

void ctui_clock_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_CLOCK *data = self->widget_data;
  int row = (self->h - 1) / 2;
  ctui_logf(E_INF, "[CTUI:CLOCK] - rendering @ tick %d: \"%s\"\n",
            ctui_tick_advance(), data->text);

  char line[self->w + 1];
  memset(line, ' ', (size_t)self->w);
  line[self->w] = '\0';
  ctui_util_center_h(data->text, line, (CTUI_CELL){.ch = ' '});

  ctui_widget_puts(self, comp, row, 0, line, data->fg, data->bg);
}

int ctui_clock_handle_tick(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  CTUI_CLOCK *data = self->widget_data;
  char text[sizeof(data->text)];
  format_now(text, sizeof(text));

  if (strcmp(text, data->text) == 0) {
    return 0;
  }

  strcpy(data->text, text);
  ctui_logf(E_INF, "[CTUI:CLOCK] - tick @ tick %d: \"%s\"\n",
            ctui_tick_advance(), data->text);
  return 1;
}
