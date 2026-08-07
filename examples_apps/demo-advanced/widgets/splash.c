#include "splash.h"

#include <math.h>
#include <stdlib.h>

#define SPLASH_TAU 6.28318530717958647692

/* HSV(h, 1, 1) -> RGB, h in [0,1) -- same full-saturation hue sweep as
 * debug_info's/dump_palette's truecolor demo rows; duplicated here rather
 * than shared since this is a demo-app-local widget, not library code */
static void hue_to_rgb(double h, unsigned char *r, unsigned char *g,
                       unsigned char *b) {
  double hh = h * 6.0;
  int i = (int)hh;
  double f = hh - i;
  double q = 1.0 - f;
  double rr, gg, bb;
  switch (i % 6) {
  case 0:
    rr = 1;
    gg = f;
    bb = 0;
    break;
  case 1:
    rr = q;
    gg = 1;
    bb = 0;
    break;
  case 2:
    rr = 0;
    gg = 1;
    bb = f;
    break;
  case 3:
    rr = 0;
    gg = q;
    bb = 1;
    break;
  case 4:
    rr = f;
    gg = 0;
    bb = 1;
    break;
  default:
    rr = 1;
    gg = 0;
    bb = q;
    break;
  }
  *r = (unsigned char)(rr * 255);
  *g = (unsigned char)(gg * 255);
  *b = (unsigned char)(bb * 255);
}

/* the actual "something cool": a swirling nebula rather than plain radial
 * hue rings -- angle still drives the base hue sweep (glhf's original
 * idea), but a 3-term sine "plasma" perturbs both hue and brightness by
 * position, so the color reads as drifting cloud wisps instead of concentric
 * bands. u/v are centered, roughly [-1,1] coordinates; masked selects
 * between the char-cell aura (circular cutoff, needed so a coarse text grid
 * still reads as round) and the Kitty pixel path (unmasked -- a real image
 * can just fill its rectangle). */
static void nebula_pixel(double u, double v, int masked, unsigned char *r,
                         unsigned char *g, unsigned char *b,
                         unsigned char *a) {
  double dist = sqrt(u * u + v * v);
  if (masked && dist > 1.0) {
    *r = *g = *b = *a = 0;
    return;
  }
  double plasma = sin(u * 3.3 + v * 2.1) + sin(v * 4.7 - u * 1.3) +
                  sin(dist * 6.0 - dist * dist * 2.0);
  plasma /= 3.0; /* [-1, 1] */

  double angle = atan2(v, u) / SPLASH_TAU + 0.5;
  double hue = angle * 0.6 + plasma * 0.2 + 0.6; /* biased toward blue/purple/pink */
  hue -= floor(hue);

  unsigned char rr, gg, bb;
  hue_to_rgb(hue, &rr, &gg, &bb);
  double glow = masked ? (1.0 - dist * 0.35)
                       : (0.5 + 0.5 * ((plasma + 1.0) / 2.0));
  if (glow < 0.0) {
    glow = 0.0;
  }
  *r = (unsigned char)(rr * glow);
  *g = (unsigned char)(gg * glow);
  *b = (unsigned char)(bb * glow);
  *a = 255;
}

/* scatters a handful of bright single-pixel star sparkles directly into
 * px_rgba -- only the Kitty path has enough resolution for a pinpoint dot to
 * read as a star rather than noise. Unseeded rand(): deterministic (same
 * scatter every run), which is fine for a demo splash and means a resize
 * doesn't need to regenerate anything since px_rgba is built once. */
static void scatter_stars(unsigned char *px_rgba, int px_w, int px_h) {
  int count = (px_w * px_h) / 140;
  for (int i = 0; i < count; i++) {
    int sx = rand() % px_w;
    int sy = rand() % px_h;
    unsigned char *p = px_rgba + ((size_t)sy * (size_t)px_w + (size_t)sx) * 4;
    unsigned char bright = (unsigned char)(180 + rand() % 76);
    p[0] = p[1] = p[2] = bright;
    p[3] = 255;
  }
}

CTUI_SPLASH ctui_splash_make(int src_w, int src_h, int px_w, int px_h,
                             unsigned int image_id) {
  /* src_w/src_h are authored around a ~2.4:1 ratio (see main()) so the rim
   * below reads as roughly circular despite terminal cells being roughly
   * twice as tall as wide -- no separate aspect-correction factor needed */
  unsigned char *rgba = malloc((size_t)src_w * (size_t)src_h * 4);
  double cx0 = (src_w - 1) / 2.0, cy0 = (src_h - 1) / 2.0;
  for (int y = 0; y < src_h; y++) {
    for (int x = 0; x < src_w; x++) {
      unsigned char *p = rgba + ((size_t)y * (size_t)src_w + (size_t)x) * 4;
      nebula_pixel((x - cx0) / cx0, (y - cy0) / cy0, 1, &p[0], &p[1], &p[2],
                  &p[3]);
    }
  }

  unsigned char *px_rgba = malloc((size_t)px_w * (size_t)px_h * 4);
  double pcx0 = (px_w - 1) / 2.0, pcy0 = (px_h - 1) / 2.0;
  for (int y = 0; y < px_h; y++) {
    for (int x = 0; x < px_w; x++) {
      unsigned char *p = px_rgba + ((size_t)y * (size_t)px_w + (size_t)x) * 4;
      nebula_pixel((x - pcx0) / pcx0, (y - pcy0) / pcy0, 0, &p[0], &p[1],
                  &p[2], &p[3]);
    }
  }
  scatter_stars(px_rgba, px_w, px_h);

  ctui_logf(E_INF,
            "[CTUI:SPLASH] - creating %dx%d aura + %dx%d nebula @ tick %d "
            "(image_id=%u)\n",
            src_w, src_h, px_w, px_h, ctui_tick_advance(), image_id);
  return (CTUI_SPLASH){.src_w = src_w,
                       .src_h = src_h,
                       .rgba = rgba,
                       .px_w = px_w,
                       .px_h = px_h,
                       .px_rgba = px_rgba,
                       .image_id = image_id};
}

void ctui_splash_free(CTUI_SPLASH *splash) {
  free(splash->rgba);
  splash->rgba = NULL;
  free(splash->px_rgba);
  splash->px_rgba = NULL;
}

void ctui_splash_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_SPLASH *splash = self->widget_data;
  ctui_logf(E_INF,
            "[CTUI:SPLASH] - rendering @ tick %d (%dx%d -> %dx%d)\n",
            ctui_tick_advance(), splash->src_w, splash->src_h, self->w,
            self->h);

  for (int row = 0; row < self->h; row++) {
    int sy = ctui_util_rescale_i(row, 0, self->h - 1, 0, splash->src_h - 1);
    for (int col = 0; col < self->w; col++) {
      int sx = ctui_util_rescale_i(col, 0, self->w - 1, 0, splash->src_w - 1);
      const unsigned char *p =
          splash->rgba +
          ((size_t)sy * (size_t)splash->src_w + (size_t)sx) * 4;
      if (p[3] == 0) {
        continue;
      }
      ctui_widget_putc_rgb(self, comp, row, col, ' ', 0, 0, 0, p[0], p[1],
                           p[2]);
    }
  }
}

void ctui_splash_gfx_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)comp;
  /* same "w/h <= 0 means not currently visible, nothing to transmit"
   * convention as ctui_kitty_image_gfx_render() -- render_gfx_widgets()
   * (core/app.c) calls this unconditionally every frame, with no per-widget
   * "skip this frame" hook of its own. In practice self->w/h never actually
   * go to 0 here (the splash pane always keeps some share of the main area,
   * see main_split_layout() in main.c), but the check costs nothing and
   * keeps this widget honoring the same contract every other Phase 4 widget
   * does. */
  if (self->w <= 0 || self->h <= 0) {
    return;
  }
  CTUI_SPLASH *splash = self->widget_data;
  ctui_logf(E_DBG,
            "[CTUI:SPLASH] - gfx_render @ tick %d (row=%d, col=%d, "
            "cells=%dx%d)\n",
            ctui_tick_advance(), self->y + 1, self->x + 1, self->w, self->h);
  ctui_gfx_kitty_display(self->y + 1, self->x + 1, self->w, self->h,
                         splash->px_rgba, splash->px_w, splash->px_h,
                         splash->image_id, 0);
}
