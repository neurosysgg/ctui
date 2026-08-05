#include "debug_info.h"

#include <stdio.h>

static const char *gfx_mode_name(CTUI_GFX_MODE mode) {
  switch (mode) {
  case CTUI_GFX_ANSI16:
    return "ansi16";
  case CTUI_GFX_ANSI256:
    return "ansi256";
  case CTUI_GFX_TRUECOLOR:
    return "truecolor";
  case CTUI_GFX_KITTY:
    return "kitty";
  default:
    return "unknown";
  }
}

/* the CTUI_COLOR_MODE_* a cell needs in order to actually exercise the
 * negotiated CTUI_GFX_MODE -- the two are distinct enums (one terminal
 * capability, one per-cell encoding, see GFX_DESIGN.md), but this widget's
 * own demo cells pick one straight off the other, so it's worth spelling
 * out the mapping rather than just naming the gfx tier */
static unsigned char gfx_mode_color_mode(CTUI_GFX_MODE mode) {
  switch (mode) {
  case CTUI_GFX_TRUECOLOR:
  case CTUI_GFX_KITTY:
    return CTUI_COLOR_MODE_RGB;
  case CTUI_GFX_ANSI256:
    return CTUI_COLOR_MODE_256;
  default:
    return CTUI_COLOR_MODE_BASIC;
  }
}

static const char *color_mode_name(unsigned char color_mode) {
  switch (color_mode) {
  case CTUI_COLOR_MODE_BASIC:
    return "basic";
  case CTUI_COLOR_MODE_256:
    return "256";
  case CTUI_COLOR_MODE_RGB:
    return "rgb";
  default:
    return "unknown";
  }
}

/* HSV(h, 1, 1) -> RGB, h in [0,1) -- a full-saturation hue sweep, used to
 * paint the truecolor demo row below with a gradient that has no visible
 * banding, unlike a 256-color ramp would */
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

void ctui_debug_info_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  char buf[64];
  ctui_logf(E_INF, "[CTUI:DEBUG_INFO] - rendering @ tick %d (screen=%dx%d)\n",
            ctui_tick_advance(), comp->cols, comp->rows);

  ctui_widget_puts(self, comp, 0, 0, "__debug_info__", CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
  snprintf(buf, sizeof(buf), "width: %d chars/columns", comp->cols);
  ctui_widget_puts(self, comp, 1, 0, buf, CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
  snprintf(buf, sizeof(buf), "height: %d chars/rows", comp->rows);
  ctui_widget_puts(self, comp, 2, 0, buf, CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);

  CTUI_GFX_MODE *gfx_mode = self->widget_data;
  if (gfx_mode == NULL) {
    return;
  }
  snprintf(buf, sizeof(buf), "gfx: %s", gfx_mode_name(*gfx_mode));
  ctui_widget_puts(self, comp, 3, 0, buf, CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);

  unsigned char color_mode = gfx_mode_color_mode(*gfx_mode);
  snprintf(buf, sizeof(buf), "color_mode: %s", color_mode_name(color_mode));
  ctui_widget_puts(self, comp, 4, 0, buf, CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);

  if (*gfx_mode != CTUI_GFX_TRUECOLOR || self->h < 6) {
    return;
  }
  for (int col = 0; col < self->w; col++) {
    unsigned char r, g, b;
    hue_to_rgb((double)col / self->w, &r, &g, &b);
    ctui_widget_putc_rgb(self, comp, 5, col, ' ', 0, 0, 0, r, g, b);
  }
  ctui_logf(E_DBG,
            "[CTUI:DEBUG_INFO] - truecolor hue sweep drawn @ tick %d "
            "(row=5, w=%d)\n",
            ctui_tick_advance(), self->w);
}
