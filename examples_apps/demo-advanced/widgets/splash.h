#ifndef DEMO_ADVANCED_WIDGETS_SPLASH_H
#define DEMO_ADVANCED_WIDGETS_SPLASH_H

#include "ctui.h"

typedef struct {
  int src_w, src_h;    /* logical resolution the char-cell aura is authored
                        * at -- deliberately small and fixed. ctui_splash_render()
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

  int px_w, px_h;         /* pixel resolution of the Kitty-tier image below --
                           * independent of src_w/src_h and much higher, since
                           * a real raster image isn't limited to one color
                           * per character cell. ctui_gfx_kitty_display()
                           * scales this to whatever cell box self->w x
                           * self->h currently is, so it doesn't need to match
                           * any particular terminal's pixel-per-cell size. */
  unsigned char *px_rgba; /* px_w*px_h*4 bytes, owned -- see
                           * ctui_splash_free(). Always fully opaque: unlike
                           * rgba above, this isn't masked to a circle -- a
                           * real pixel image can just fill its whole
                           * rectangle instead of needing transparency to
                           * read as non-blocky. */
  unsigned int image_id;  /* passed to ctui_gfx_kitty_display() so every
                           * redraw replaces the same image in place instead
                           * of layering a new one on top -- caller-chosen,
                           * must differ from any other CTUI_GFX_KITTY
                           * widget's id live at the same time (see
                           * kitty_image_data's id=1 in main.c). */
} CTUI_SPLASH;

/* procedurally builds both of CTUI_SPLASH's owned buffers (no external image
 * dependency, same spirit as ctui_kitty_image_make()): a swirling
 * plasma-perturbed hue nebula, sampled once at src_w x src_h (masked to a
 * circle, for the char-cell degrade path) and once at px_w x px_h (unmasked,
 * with scattered star sparkles, for the Kitty pixel path). image_id is a
 * caller-chosen nonzero id, stable across this widget's lifetime, used only
 * by the Kitty path. */
CTUI_SPLASH ctui_splash_make(int src_w, int src_h, int px_w, int px_h,
                             unsigned int image_id);
void ctui_splash_free(CTUI_SPLASH *splash);

/* renders widget_data's (a CTUI_SPLASH*) char-cell aura into self's current
 * w x h box, nearest-neighbor resampled to fill it -- shrinks when the panel
 * grid below it opens up, fills the whole main area again once every navbar
 * panel is toggled back off. Requires CTUI_GFX_TRUECOLOR to have been
 * negotiated (paints via ctui_widget_putc_rgb()); nothing at this layer
 * enforces that, same convention as every other ctui_widget_putc_rgb()
 * consumer. Used as the plain render(), the degrade path
 * ctui_widget_dispatch_render() falls back to whenever CTUI_GFX_KITTY isn't
 * the negotiated mode -- see ctui_splash_gfx_render() below. */
void ctui_splash_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* the Kitty-tier alternative: transmits/displays widget_data's px_rgba
 * buffer via ctui_gfx_kitty_display(), scaled to cover self's w x h cells at
 * self's (x,y) compositor origin. Registered via
 * ctui_widget_set_gfx_renderer(), called directly by ctui_app_run() after
 * each frame's screen flush -- see core/app.c's render_gfx_widgets(). Same
 * "w/h <= 0 means not currently visible" convention as
 * ctui_kitty_image_gfx_render(). */
void ctui_splash_gfx_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
