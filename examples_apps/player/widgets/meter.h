#ifndef PLAYER_WIDGETS_METER_H
#define PLAYER_WIDGETS_METER_H

#include "ctui.h"

/* single VU-style level bar -- decided over multi-band bars in DESIGN.md
 * ("Resolved"): RMS/peak alone needs no FFT, matches "lean" for v1. */
typedef struct {
  float level; /* 0.0-1.0; the app's playback timer handler updates this
               * directly each tick (see calculator's CALC_RESULT ->
               * CTUI_DISPLAY precedent -- main.c owns both sides, no
               * event needed) */
  unsigned char fg_low, fg_mid, fg_high; /* color by column position, not
                                          * by current level -- a classic
                                          * VU meter's green/yellow/red
                                          * zones stay fixed, only how far
                                          * the bar reaches into them
                                          * changes */
  unsigned char bg;
} CTUI_METER;

CTUI_METER ctui_meter_make(unsigned char fg_low, unsigned char fg_mid,
                          unsigned char fg_high, unsigned char bg);

/* fills self->w * self->h with a horizontal bar: level * self->w columns
 * lit (fg_low/mid/high by column position, zone breakpoints at 60%/85% of
 * width), the rest bg. */
void ctui_meter_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
