#ifndef CTUI_WIDGETS_DUMP_PALETTE_H
#define CTUI_WIDGETS_DUMP_PALETTE_H

#include "ctui.h"

/* no widget_data needed -- reads comp directly. Shows every basic ANSI
 * color (CTUI_COLOR_DEFAULT..CTUI_COLOR_WHITE) as a filled swatch, arranged
 * in the fewest rows (>=2) that evenly divide the color count into columns.
 * Each swatch's ANSI name is centered on top of it in default fg/bg, so it
 * stays legible no matter which color the swatch itself is. */
void ctui_dump_palette_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
