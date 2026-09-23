#include "clock.h"

#include <string.h>
#include <time.h>

static void format_now(const char *format, char *out, size_t out_size) {
  time_t now = time(NULL);
  struct tm *tmv = localtime(&now);
  if (strftime(out, out_size, format ? format : "%H:%M:%S", tmv) == 0) {
    out[0] = '\0';
  }
}

CTUI_CLOCK ctui_clock_make(unsigned char fg, unsigned char bg) {
  ctui_logf(E_INF, "[CTUI:CLOCK] - creating clock @ tick %d\n",
            ctui_tick_advance());
  CTUI_CLOCK clock = {.fg = fg, .bg = bg};
  format_now(NULL, clock.text, sizeof(clock.text));
  return clock;
}

void ctui_clock_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_CLOCK *data = self->widget_data;
  int row = (self->h - 1) / 2;
  ctui_logf(E_INF, "[CTUI:CLOCK] - rendering @ tick %d: \"%s\"\n",
            ctui_tick_advance(), data->text);

  int width;
  size_t n = ctui_utf8_prefix(data->text, self->w, &width);
  char fit[n + 1];
  memcpy(fit, data->text, n);
  fit[n] = '\0';
  ctui_widget_puts(self, comp, row, (self->w - width) / 2, fit, data->fg,
                   data->bg);
}

int ctui_clock_handle_tick(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  CTUI_CLOCK *data = self->widget_data;
  char text[sizeof(data->text)];
  format_now(data->format, text, sizeof(text));

  if (strcmp(text, data->text) == 0) {
    return 0;
  }

  strcpy(data->text, text);
  ctui_logf(E_INF, "[CTUI:CLOCK] - tick @ tick %d: \"%s\"\n",
            ctui_tick_advance(), data->text);
  return 1;
}
