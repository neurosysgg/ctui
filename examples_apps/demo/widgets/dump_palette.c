#include "dump_palette.h"

#include <string.h>

/* HSV(h, 1, 1) -> RGB, h in [0,1) -- same full-saturation hue sweep as
 * debug_info's truecolor demo row; duplicated here rather than shared since
 * dump_palette is a demo-app-local widget, not library code */
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

static const struct {
  unsigned char color;
  const char *name;
} PALETTE[] = {
    {CTUI_COLOR_DEFAULT, "DEFAULT"}, {CTUI_COLOR_BLACK, "BLACK"},
    {CTUI_COLOR_RED, "RED"},         {CTUI_COLOR_GREEN, "GREEN"},
    {CTUI_COLOR_YELLOW, "YELLOW"},   {CTUI_COLOR_BLUE, "BLUE"},
    {CTUI_COLOR_MAGENTA, "MAGENTA"}, {CTUI_COLOR_CYAN, "CYAN"},
    {CTUI_COLOR_WHITE, "WHITE"},
};
#define PALETTE_COUNT ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

void ctui_dump_palette_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_logf(E_INF,
            "[CTUI:DUMP_PALETTE] - rendering @ tick %d (%d colors, %dx%d)\n",
            ctui_tick_advance(), PALETTE_COUNT, self->w, self->h);

  /* bottom row is a 256-color ramp (ctui_widget_putc_256()) when there's
   * room for it AND main() actually got at least CTUI_GFX_ANSI256 out of
   * ctui_init() -- widget_data is the same CTUI_GFX_MODE main() passed
   * ctui_init(), post-negotiation, so this only draws in 256-color codes
   * once the terminal has actually proven it supports them. main() now
   * requests CTUI_GFX_TRUECOLOR (debug_info's proving ground), so the
   * granted tier can land on either CTUI_GFX_ANSI256 or CTUI_GFX_TRUECOLOR
   * (never below -- ctui_init() only negotiates down from what was asked)
   * -- both qualify here, since a truecolor terminal is a 256-color
   * terminal too (see ctui_gfx_detect_caps()). */
  CTUI_GFX_MODE *gfx_mode = self->widget_data;
  int ramp_h = (gfx_mode != NULL &&
               (*gfx_mode == CTUI_GFX_ANSI256 ||
                *gfx_mode == CTUI_GFX_TRUECOLOR) &&
               self->h >= 4)
                  ? 1
                  : 0;
  /* a second bottom row, below the 256-color ramp, when the granted tier
   * is CTUI_GFX_TRUECOLOR specifically -- a smooth hue sweep via
   * ctui_widget_putc_rgb() the 256-color ramp above it can't reproduce
   * without banding, same demonstration debug_info's truecolor row makes */
  int truecolor_h = (gfx_mode != NULL && *gfx_mode == CTUI_GFX_TRUECOLOR &&
                     self->h >= 4 + ramp_h + 1)
                        ? 1
                        : 0;
  int grid_h = self->h - ramp_h - truecolor_h;

  /* fewest rows (>=2) that evenly divide PALETTE_COUNT into columns -- for
   * the current 9 basic colors that's 3x3, but this stays correct if the
   * palette ever grows/shrinks */
  int rows = 2;
  while (rows < PALETTE_COUNT && PALETTE_COUNT % rows != 0) {
    rows++;
  }
  int cols = PALETTE_COUNT / rows;

  /* same even-division-plus-remainder convention as ctui_split_layout()/
   * ctui_grid_render(): base cell size, remainder absorbed into the last
   * row/col so the grid fills self exactly */
  int base_w = self->w / cols;
  int rem_w = self->w - base_w * cols;
  int base_h = grid_h / rows;
  int rem_h = grid_h - base_h * rows;

  int base_row = 0;
  for (int r = 0; r < rows; r++) {
    int cell_h = base_h + (r == rows - 1 ? rem_h : 0);
    int base_col = 0;
    for (int c = 0; c < cols; c++) {
      int cell_w = base_w + (c == cols - 1 ? rem_w : 0);
      int idx = r * cols + c;
      unsigned char color = PALETTE[idx].color;

      for (int row = 0; row < cell_h; row++) {
        for (int col = 0; col < cell_w; col++) {
          ctui_widget_putc(self, comp, base_row + row, base_col + col, ' ',
                           CTUI_COLOR_DEFAULT, color);
        }
      }

      const char *name = PALETTE[idx].name;
      int name_len = (int)strlen(name);
      int name_row = base_row + cell_h / 2;
      int name_col = base_col + (cell_w - name_len) / 2;
      ctui_widget_puts(self, comp, name_row, name_col, name,
                       CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);

      base_col += cell_w;
    }
    base_row += cell_h;
  }

  if (ramp_h > 0) {
    /* 16..231 is the 6x6x6 color cube of the 256-color palette -- skips
     * the 16 basic-equivalent indices (0-15) and the grayscale ramp
     * (232-255) so this visibly demonstrates color_mode == 256 rather
     * than just reproducing the swatches above in a different encoding */
    for (int col = 0; col < self->w; col++) {
      unsigned char idx = (unsigned char)(16 + col * 216 / self->w);
      ctui_widget_putc_256(self, comp, grid_h, col, ' ', CTUI_COLOR_DEFAULT,
                           idx);
    }
    ctui_logf(E_DBG,
              "[CTUI:DUMP_PALETTE] - 256-color ramp drawn @ tick %d (row=%d, "
              "w=%d)\n",
              ctui_tick_advance(), grid_h, self->w);
  }

  if (truecolor_h > 0) {
    int row = grid_h + ramp_h;
    for (int col = 0; col < self->w; col++) {
      unsigned char r, g, b;
      hue_to_rgb((double)col / self->w, &r, &g, &b);
      ctui_widget_putc_rgb(self, comp, row, col, ' ', 0, 0, 0, r, g, b);
    }
    ctui_logf(E_DBG,
              "[CTUI:DUMP_PALETTE] - truecolor hue sweep drawn @ tick %d "
              "(row=%d, w=%d)\n",
              ctui_tick_advance(), row, self->w);
  }
}
