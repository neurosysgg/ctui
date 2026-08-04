#ifndef CTUI_WIDGETS_BORDER_H
#define CTUI_WIDGETS_BORDER_H

#include "../ctui.h"

typedef struct {
  CTUI_CELL edge;   /* used along the four sides */
  CTUI_CELL corner; /* used at the four corners; defaults to `edge` via
                     * ctui_border_make(), so a border is a uniform block
                     * unless corner is overridden after construction */
} CTUI_BORDER;

/* defaults to a filled block, like a terminal cursor: a plain space cell
 * with `block_color` as its background, used for both edges and corners */
CTUI_BORDER ctui_border_make(unsigned char block_color);

/* generic border widget: repeats widget_data (a CTUI_BORDER) around the
 * perimeter of self's w x h box -- edge cell along the sides, corner cell at
 * the four corners. Content meant to be a separate, inset widget so it never
 * shares a cell with the border (see the ctui demo for the pattern). */
void ctui_border_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
