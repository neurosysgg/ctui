#ifndef CTUI_WIDGETS_DUMP_PALETTE_H
#define CTUI_WIDGETS_DUMP_PALETTE_H

#include "ctui.h"

/* widget_data is a CTUI_GFX_MODE* -- main() passes the address of the same
 * variable it gave ctui_init(), so this widget can see whether at least
 * ANSI256 was actually granted (ctui_init() negotiates down instead of
 * failing, so demo keeps working on a plain terminal; see main()). Shows
 * every basic ANSI color (CTUI_COLOR_DEFAULT..CTUI_COLOR_WHITE) as a filled
 * swatch, arranged in the fewest rows (>=2) that evenly divide the color
 * count into columns. Each swatch's ANSI name is centered on top of it in
 * default fg/bg, so it stays legible no matter which color the swatch
 * itself is. When there's room (self->h >= 4) AND *widget_data reads back
 * CTUI_GFX_ANSI256 or CTUI_GFX_TRUECOLOR (a truecolor terminal is a
 * 256-color terminal too), the row above the swatches' bottom edge is a
 * 256-color ramp across the 6x6x6 color cube (indices 16-231), drawn via
 * ctui_widget_putc_256(). When there's room for that AND ONE more row (self->h
 * >= 6) AND *widget_data reads back CTUI_GFX_TRUECOLOR specifically, a
 * further bottom row sweeps a full-saturation HSV hue gradient via
 * ctui_widget_putc_rgb() -- a smooth 24-bit gradient the 256-color ramp
 * above it can only band. */
void ctui_dump_palette_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
