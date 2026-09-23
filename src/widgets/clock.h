#ifndef CTUI_WIDGETS_CLOCK_H
#define CTUI_WIDGETS_CLOCK_H

#include "../ctui.h"

typedef struct {
  char text[64]; /* formatted time, refreshed by ctui_clock_handle_tick() */
  const char *format; /* strftime() format; NULL (ctui_clock_make()'s
                       * default) means "%H:%M:%S". Set it right after
                       * ctui_clock_make(), then call ctui_clock_handle_tick()
                       * once (or wait a tick) to reformat. */
  unsigned char fg, bg;
} CTUI_CLOCK;

/* seeds text from the current wall-clock time so the first frame isn't
 * blank before the first tick arrives */
CTUI_CLOCK ctui_clock_make(unsigned char fg, unsigned char bg);

void ctui_clock_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* recomputes text from the current wall-clock time; returns 0 (no visible
 * change) if it's identical to last time, e.g. a tick_ms shorter than
 * 1000ms firing within the same second. Register against
 * ("timer", CTUI_TICK_EVENT), or as a ctui_timer_register() handler. */
int ctui_clock_handle_tick(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
