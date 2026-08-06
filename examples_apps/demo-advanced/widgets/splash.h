#ifndef DEMO_ADVANCED_WIDGETS_SPLASH_H
#define DEMO_ADVANCED_WIDGETS_SPLASH_H

#include "ctui.h"

typedef struct {
  int src_w, src_h;    /* logical resolution the splash is authored at --
                        * deliberately small and fixed. ctui_splash_render()
                        * nearest-neighbor resamples it up or down to
                        * whatever self->w x self->h its layout gives it, via
                        * ctui_util_rescale_i() -- the same integer-only
                        * coordinate mapping ctui_dump_palette_render() uses
                        * for its 256-color ramp, just walked over both axes
                        * instead of one. */
  unsigned char *rgba; /* src_w*src_h*4 bytes, owned -- see
                        * ctui_splash_free(). alpha 0 marks a cell outside
                        * the aura; ctui_splash_render() skips painting those
                        * so whatever's already in the compositor (the main
                        * area's cleared background) shows through instead
                        * of a hard-edged box. */
} CTUI_SPLASH;

/* procedurally builds a small src_w x src_h radial "aura" (no external image
 * dependency, same spirit as ctui_kitty_image_make()): a full-saturation hue
 * sweep by angle around the center, brightness fading toward the rim,
 * transparent outside it. */
CTUI_SPLASH ctui_splash_make(int src_w, int src_h);
void ctui_splash_free(CTUI_SPLASH *splash);

/* renders widget_data's (a CTUI_SPLASH*) pattern into self's current w x h
 * box, nearest-neighbor resampled to fill it -- shrinks when the panel grid
 * below it opens up, fills the whole main area again once every navbar
 * panel is toggled back off. Requires CTUI_GFX_TRUECOLOR to have been
 * negotiated (paints via ctui_widget_putc_rgb()); nothing at this layer
 * enforces that, same convention as every other ctui_widget_putc_rgb()
 * consumer. */
void ctui_splash_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
