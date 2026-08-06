#include "kitty_image.h"

#include <stdlib.h>

CTUI_KITTY_IMAGE ctui_kitty_image_make(int px_w, int px_h,
                                       unsigned int image_id) {
  unsigned char *rgba = malloc((size_t)px_w * (size_t)px_h * 4);
  for (int y = 0; y < px_h; y++) {
    for (int x = 0; x < px_w; x++) {
      unsigned char *p = rgba + ((size_t)y * (size_t)px_w + (size_t)x) * 4;
      p[0] = (unsigned char)(x * 255 / (px_w - 1 > 0 ? px_w - 1 : 1));
      p[1] = (unsigned char)(y * 255 / (px_h - 1 > 0 ? px_h - 1 : 1));
      p[2] = (unsigned char)(255 - p[0]);
      p[3] = 255;
    }
  }
  ctui_logf(E_INF,
            "[CTUI:KITTY_IMAGE] - creating %dx%d gradient @ tick %d "
            "(image_id=%u)\n",
            px_w, px_h, ctui_tick_advance(), image_id);
  return (CTUI_KITTY_IMAGE){
      .px_w = px_w, .px_h = px_h, .rgba = rgba, .image_id = image_id};
}

void ctui_kitty_image_free(CTUI_KITTY_IMAGE *img) {
  free(img->rgba);
  img->rgba = NULL;
}

void ctui_kitty_image_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_widget_puts(self, comp, 0, 0, "[kitty required]", CTUI_COLOR_RED,
                   CTUI_COLOR_DEFAULT);
}

void ctui_kitty_image_gfx_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)comp;
  /* render_gfx_widgets() (core/app.c) calls every matching widget's
   * gfx_render unconditionally, every frame -- there's no per-widget
   * "skip this frame" hook the way a plain render() can just not draw
   * anything. A caller that wants this widget hideable (e.g. a
   * toggle-driven panel, see the demo app) collapses it to 0x0 via its
   * own layout() instead; w/h <= 0 is read here as "not currently
   * visible, nothing to transmit" rather than as degenerate geometry. */
  if (self->w <= 0 || self->h <= 0) {
    return;
  }
  CTUI_KITTY_IMAGE *img = self->widget_data;
  ctui_logf(E_DBG,
            "[CTUI:KITTY_IMAGE] - gfx_render @ tick %d (row=%d, col=%d, "
            "cells=%dx%d)\n",
            ctui_tick_advance(), self->y + 1, self->x + 1, self->w, self->h);
  ctui_gfx_kitty_display(self->y + 1, self->x + 1, self->w, self->h,
                         img->rgba, img->px_w, img->px_h, img->image_id);
}
