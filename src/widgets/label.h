#ifndef CTUI_WIDGETS_LABEL_H
#define CTUI_WIDGETS_LABEL_H

#include "../ctui.h"

typedef struct {
  char *text;
  unsigned char fg, bg;
} CTUI_LABEL;

/* generic single-line label widget: centers widget_data's text both
 * horizontally (ctui_util_center_h) and vertically (single line, so just
 * middle-row math) within self's w/h */
void ctui_label_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
