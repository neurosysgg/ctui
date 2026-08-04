#ifndef CTUI_WIDGETS_STATUS_H
#define CTUI_WIDGETS_STATUS_H

#include "../ctui.h"

typedef struct {
  char text[64]; /* set by ctui_status_handle_value_changed() */
} CTUI_STATUS;

void ctui_status_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* shows "you picked: <value>" from a CTUI_VALUE_CHANGED_EVENT. Register
 * against whatever source you want this status line to track, e.g.
 * ctui_event_register("menu", CTUI_VALUE_CHANGED_EVENT, status_widget,
 * ctui_status_handle_value_changed) to watch a specific menu instance. */
int ctui_status_handle_value_changed(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
