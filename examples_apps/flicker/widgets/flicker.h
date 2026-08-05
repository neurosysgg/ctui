#ifndef CTUI_WIDGETS_FLICKER_H
#define CTUI_WIDGETS_FLICKER_H

#include "ctui.h"

typedef struct {
  unsigned char fg, bg;
  unsigned int seed; /* reseeded on each timer fire -- render() derives
                      * every cell from it, so redrawing without a new fire
                      * reproduces the same frame instead of re-rolling on
                      * every render pass */
} CTUI_FLICKER;

/* seed distinguishes this instance's random content from any other
 * CTUI_FLICKER's -- widgets sharing a synchronized timer group (see
 * ctui_periodic_register() in widgets/periodic.h) reseed on the exact
 * same fire, so without different starting seeds they'd render
 * bit-for-bit identical patterns forever (same LCG step applied to the
 * same starting value each tick) despite being logically independent
 * regions; any distinct value per instance (e.g. 1, 2, 3, ...) is enough */
CTUI_FLICKER ctui_flicker_make(unsigned char fg, unsigned char bg,
                               unsigned int seed);

/* fills self with, per cell, either a random character from a small fixed
 * palette or a blank -- both chosen by hashing `seed` together with each
 * cell's own (row, col), so the whole area re-randomizes as one frame
 * whenever seed changes, with no stored w*h buffer that would have to
 * track resizes */
void ctui_flicker_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* reseeds so the next render() produces a new frame. Always returns 1 --
 * every fire is a visible change by design, unlike e.g. CTUI_CLOCK's tick
 * handler which skips a redraw when the second hasn't changed. Pass this
 * as ctui_periodic_register()'s handler to wire a CTUI_FLICKER up to the
 * timer subsystem. */
int ctui_flicker_handle_timer(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
