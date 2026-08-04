#ifndef CTUI_WIDGETS_DEBUG_INFO_H
#define CTUI_WIDGETS_DEBUG_INFO_H

#include "../ctui.h"

/* no widget_data needed -- reads comp directly. Shows the compositor's
 * current dimensions, mainly useful as a quick "is my layout math right"
 * sanity check while building a screen. */
void ctui_debug_info_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
