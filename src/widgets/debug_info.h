#ifndef CTUI_WIDGETS_DEBUG_INFO_H
#define CTUI_WIDGETS_DEBUG_INFO_H

#include "../ctui.h"

/* widget_data is optional: NULL (reads comp directly), or a CTUI_GFX_MODE*
 * -- main() passes the address of the same variable it gave ctui_init(), so
 * this widget can report which graphics tier actually got negotiated (same
 * convention as demo's dump_palette widget). Shows the compositor's current
 * dimensions, mainly useful as a quick "is my layout math right" sanity
 * check while building a screen. When widget_data is non-NULL, also shows
 * the negotiated CTUI_GFX_MODE and the CTUI_COLOR_MODE_* it maps to
 * (BASIC/256/RGB -- a different enum, see GFX_DESIGN.md: one's a terminal
 * capability, the other's how a cell is actually encoded). When
 * *widget_data reads back CTUI_GFX_TRUECOLOR (and self->h >= 6), an extra
 * bottom row sweeps a full-spectrum hue gradient via
 * ctui_widget_putc_rgb() -- a smooth 24-bit gradient that CTUI_GFX_ANSI256
 * can only render banded, so it's a direct visual proof truecolor is
 * actually in effect, not just requested. */
void ctui_debug_info_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
