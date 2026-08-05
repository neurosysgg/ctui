#include "meter.h"

CTUI_METER ctui_meter_make(unsigned char fg_low, unsigned char fg_mid,
                          unsigned char fg_high, unsigned char bg) {
  return (CTUI_METER){
      .level = 0.0f, .fg_low = fg_low, .fg_mid = fg_mid, .fg_high = fg_high,
      .bg = bg};
}

void ctui_meter_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_METER *meter = self->widget_data;
  float level = meter->level < 0.0f ? 0.0f : (meter->level > 1.0f ? 1.0f : meter->level);
  int filled = (int)(level * (float)self->w + 0.5f);

  ctui_logf(E_INF, "[PLAYER:METER] - rendering @ tick %d, level=%.3f (%d/%d)\n",
           ctui_tick_advance(), level, filled, self->w);

  int low_end = (self->w * 6) / 10;
  int mid_end = (self->w * 85) / 100;

  for (int col = 0; col < self->w; col++) {
    unsigned char zone_fg = col < low_end
                                ? meter->fg_low
                                : (col < mid_end ? meter->fg_mid : meter->fg_high);
    unsigned char bg = col < filled ? zone_fg : meter->bg;

    for (int row = 0; row < self->h; row++) {
      ctui_widget_putc(self, comp, row, col, ' ', meter->bg, bg);
    }
  }
}
