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

/* per-widget cache of the last-transmitted frame, keyed by the widget's own
 * pointer identity -- same reasoning as border.c's ctui_border_kitty_cache
 * (PROCESS.md's "Addendum 2" root-caused this exact pattern there first):
 * a caller with a periodic redraw tick (e.g. ctui-mus's 20ms tick) would
 * otherwise retransmit this widget's full pixel buffer every single frame
 * even when nothing changed -- wasted CPU, and a visible flicker each time
 * the terminal recomposites an identical image. Unlike CTUI_BORDER, whose
 * pixels are entirely *derived* from w/h (a fresh per-pixel SDF computed
 * every call), CTUI_KITTY_IMAGE's content is an opaque, externally-supplied
 * `rgba` buffer with no relationship to geometry at all -- geometry alone
 * is therefore the wrong cache key here: a caller swapping in a same-sized
 * new image (e.g. ctui-mus's now_playing cover art landing in the same
 * cell box as the previous track's) must still retransmit. Keyed on the
 * `rgba` pointer identity as well as cell geometry, not a content hash --
 * every current caller (this demo's static gradient, ctui-mus's cover art)
 * already replaces the whole buffer wholesale via a fresh malloc rather
 * than mutating one in place on content change, so pointer identity is a
 * correct, far cheaper proxy for "did the pixels change" than hashing
 * width*height*4 bytes every frame would be. */
typedef struct {
  const CTUI_WIDGET *widget;
  const unsigned char *rgba;
  int px_w, px_h;
  unsigned int image_id;
  int cell_x, cell_y, cell_w, cell_h;
} ctui_kitty_image_cache_entry;

static ctui_kitty_image_cache_entry *ctui_kitty_image_cache = NULL;
static int ctui_kitty_image_cache_count = 0;
static int ctui_kitty_image_cache_cap = 0;

/* true (and updates the cache) the first time widget is seen or whenever
 * its transmitted content/placement differ from the last call; false if
 * unchanged, so the caller can skip retransmitting entirely. */
static int ctui_kitty_image_frame_changed(const CTUI_WIDGET *widget,
                                          const CTUI_KITTY_IMAGE *img) {
  for (int i = 0; i < ctui_kitty_image_cache_count; i++) {
    ctui_kitty_image_cache_entry *e = &ctui_kitty_image_cache[i];
    if (e->widget != widget) {
      continue;
    }
    if (e->rgba == img->rgba && e->px_w == img->px_w && e->px_h == img->px_h &&
        e->image_id == img->image_id && e->cell_x == widget->x &&
        e->cell_y == widget->y && e->cell_w == widget->w &&
        e->cell_h == widget->h) {
      return 0;
    }
    e->rgba = img->rgba;
    e->px_w = img->px_w;
    e->px_h = img->px_h;
    e->image_id = img->image_id;
    e->cell_x = widget->x;
    e->cell_y = widget->y;
    e->cell_w = widget->w;
    e->cell_h = widget->h;
    return 1;
  }
  if (ctui_kitty_image_cache_count == ctui_kitty_image_cache_cap) {
    ctui_kitty_image_cache_cap =
        ctui_kitty_image_cache_cap ? ctui_kitty_image_cache_cap * 2 : 4;
    ctui_kitty_image_cache =
        realloc(ctui_kitty_image_cache,
               sizeof(*ctui_kitty_image_cache) *
                   (size_t)ctui_kitty_image_cache_cap);
  }
  ctui_kitty_image_cache[ctui_kitty_image_cache_count++] =
      (ctui_kitty_image_cache_entry){.widget = widget,
                                     .rgba = img->rgba,
                                     .px_w = img->px_w,
                                     .px_h = img->px_h,
                                     .image_id = img->image_id,
                                     .cell_x = widget->x,
                                     .cell_y = widget->y,
                                     .cell_w = widget->w,
                                     .cell_h = widget->h};
  return 1;
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
  if (!ctui_kitty_image_frame_changed(self, img)) {
    ctui_logf(E_DBG,
              "[CTUI:KITTY_IMAGE] - gfx_render @ tick %d skipped, frame "
              "unchanged (image_id=%u)\n",
              ctui_tick_advance(), img->image_id);
    return;
  }
  ctui_logf(E_DBG,
            "[CTUI:KITTY_IMAGE] - gfx_render @ tick %d (row=%d, col=%d, "
            "cells=%dx%d)\n",
            ctui_tick_advance(), self->y + 1, self->x + 1, self->w, self->h);
  ctui_gfx_kitty_display(self->y + 1, self->x + 1, self->w, self->h,
                         img->rgba, img->px_w, img->px_h, img->image_id);
}
