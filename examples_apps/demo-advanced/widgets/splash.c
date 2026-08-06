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

CTUI_SPLASH ctui_splash_make(int src_w, int src_h) {
  unsigned char *rgba = malloc((size_t)src_w * (size_t)src_h * 4);
  /* src_w/src_h are authored around a ~2.4:1 ratio (see main()) so the rim
   * below reads as roughly circular despite terminal cells being roughly
   * twice as tall as wide -- no separate aspect-correction factor needed */
  double cx0 = (src_w - 1) / 2.0, cy0 = (src_h - 1) / 2.0;

  for (int y = 0; y < src_h; y++) {
    for (int x = 0; x < src_w; x++) {
      unsigned char *p = rgba + ((size_t)y * (size_t)src_w + (size_t)x) * 4;
      double dx = (x - cx0) / cx0;
      double dy = (y - cy0) / cy0;
      double r = sqrt(dx * dx + dy * dy);
      if (r > 1.0) {
        p[0] = p[1] = p[2] = p[3] = 0;
        continue;
      }
      double hue = atan2(dy, dx) / SPLASH_TAU + 0.5;
      unsigned char rr, gg, bb;
      hue_to_rgb(hue, &rr, &gg, &bb);
      double glow = 1.0 - r * 0.35; /* dims toward the rim */
      p[0] = (unsigned char)(rr * glow);
      p[1] = (unsigned char)(gg * glow);
      p[2] = (unsigned char)(bb * glow);
      p[3] = 255;
    }
  }

  ctui_logf(E_INF,
            "[CTUI:SPLASH] - creating %dx%d aura @ tick %d\n", src_w,
            src_h, ctui_tick_advance());
  return (CTUI_SPLASH){.src_w = src_w, .src_h = src_h, .rgba = rgba};
}

void ctui_splash_free(CTUI_SPLASH *splash) {
  free(splash->rgba);
  splash->rgba = NULL;
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
