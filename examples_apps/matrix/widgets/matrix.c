#include "matrix.h"

static const char CHARSET[] = "01#@%&*+=~.:";

static unsigned int cell_hash(unsigned int seed, int row, int col) {
  unsigned int h = seed;
  h ^= (unsigned int)row * 73856093u;
  h ^= (unsigned int)col * 19349663u;
  h *= 2654435761u;
  h ^= h >> 15;
  return h;
}

CTUI_MATRIX ctui_matrix_make(unsigned char fg_head, unsigned char fg_trail,
                             unsigned char bg, int trail_len,
                             unsigned int seed) {
  ctui_logf(E_INF, "[CTUI:MATRIX] - creating matrix @ tick %d (seed=%u)\n",
            ctui_tick_advance(), seed);
  return (CTUI_MATRIX){.fg_head = fg_head,
                       .fg_trail = fg_trail,
                       .bg = bg,
                       .trail_len = trail_len,
                       .seed = seed,
                       .frame = 0};
}

void ctui_matrix_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_MATRIX *data = self->widget_data;
  ctui_logf(E_INF, "[CTUI:MATRIX] - rendering @ tick %d (frame=%u)\n",
            ctui_tick_advance(), data->frame);

  int period = self->h + data->trail_len;
  if (period <= 0)
    return;

  for (int col = 0; col < self->w; col++) {
    unsigned int colh = cell_hash(data->seed, 0, col);
    int speed = 1 + (int)(colh & 1u);
    int phase = (int)(colh % (unsigned int)period);
    int head =
        (int)(((unsigned int)data->frame * (unsigned int)speed + (unsigned int)phase) %
              (unsigned int)period) -
        data->trail_len;

    for (int row = 0; row < self->h; row++) {
      int dist = head - row;
      if (dist < 0 || dist > data->trail_len) {
        ctui_widget_putc(self, comp, row, col, ' ', data->bg, data->bg);
        continue;
      }
      unsigned int gh = cell_hash(data->seed ^ data->frame, row, col);
      char ch = CHARSET[(gh >> 8) % (sizeof(CHARSET) - 1)];
      unsigned char fg = dist == 0 ? data->fg_head : data->fg_trail;
      ctui_widget_putc(self, comp, row, col, ch, fg, data->bg);
    }
  }
}

int ctui_matrix_handle_timer(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  CTUI_MATRIX *data = self->widget_data;
  data->frame++;
  return 1;
}
