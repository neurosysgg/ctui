#ifndef CTUI_WIDGETS_DISPLAY_H
#define CTUI_WIDGETS_DISPLAY_H

#include "ctui.h"

typedef struct {
  char text[32];
  unsigned char fg, bg;
} CTUI_DISPLAY;

/* seeds text to "0", the natural idle state for a numeric readout */
CTUI_DISPLAY ctui_display_make(unsigned char fg, unsigned char bg);

/* right-aligns text within self's w, on a single vertically centered row --
 * mirrors ctui_label_render()/ctui_clock_render()'s centering math, but
 * right- instead of center-aligned, since that's the readable convention for
 * a numeric readout. If text is wider than self->w, shows its rightmost
 * self->w chars (the least-significant end matters most for a number). */
void ctui_display_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
