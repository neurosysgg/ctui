#include "flicker.h"

static const char CHARSET[] = "01#@%&*+=~.:";

static unsigned int cell_hash(unsigned int seed, int row, int col) {
  unsigned int h = seed;
  h ^= (unsigned int)row * 73856093u;
  h ^= (unsigned int)col * 19349663u;
  h *= 2654435761u;
  h ^= h >> 15;
  return h;
}

CTUI_FLICKER ctui_flicker_make(unsigned char fg, unsigned char bg,
                               unsigned int seed) {
  ctui_logf(E_INF, "[CTUI:FLICKER] - creating flicker @ tick %d (seed=%u)\n",
            ctui_tick_advance(), seed);
  return (CTUI_FLICKER){.fg = fg, .bg = bg, .seed = seed};
}

void ctui_flicker_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_FLICKER *data = self->widget_data;
  ctui_logf(E_INF, "[CTUI:FLICKER] - rendering @ tick %d (seed=%u)\n",
            ctui_tick_advance(), data->seed);

  for (int row = 0; row < self->h; row++) {
    for (int col = 0; col < self->w; col++) {
      unsigned int h = cell_hash(data->seed, row, col);
      char ch = (h & 1) ? CHARSET[(h >> 8) % (sizeof(CHARSET) - 1)] : ' ';
      ctui_widget_putc(self, comp, row, col, ch, data->fg, data->bg);
    }
  }
}

int ctui_flicker_handle_timer(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  CTUI_FLICKER *data = self->widget_data;
  data->seed = data->seed * 1103515245u + 12345u;
  ctui_logf(E_INF, "[CTUI:FLICKER] - reseeded @ tick %d (seed=%u)\n",
            ctui_tick_advance(), data->seed);
  return 1;
}
