#ifndef CTUI_WIDGETS_LIST_H
#define CTUI_WIDGETS_LIST_H

#include "ctui.h"

typedef struct {
  char *label;
  int is_dir; /* drives display (trailing '/') and is forwarded as
              * CTUI_VALUE_CHANGED_EVENT_DATA.enabled on ENTER, so a
              * listener can tell entries apart without inspecting the
              * label string itself */
} CTUI_LIST_ITEM;

typedef struct {
  CTUI_LIST_ITEM *items; /* caller-owned; not freed by this widget */
  int count;
  int selected;      /* cursor position */
  int scroll_offset; /* index of the first visible row; kept in sync with
                      * selected by ctui_list_handle_keypress() so the
                      * cursor always stays within the widget's h rows */
} CTUI_LIST;

/* draws items[scroll_offset .. scroll_offset+h), highlighting selected,
 * truncating each label (plus a trailing '/' for is_dir) to self->w */
void ctui_list_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* UP/DOWN move the cursor and scroll the viewport to keep it visible.
 * ENTER emits a CTUI_VALUE_CHANGED_EVENT (source "list", value = the
 * selected item's label, enabled = its is_dir) via ctui_handle_event()
 * for an app-level handler to act on -- see the file_browser example
 * app's list_handle_value_changed() for the pattern. Register against
 * ("input", CTUI_KEYPRESS_EVENT). */
int ctui_list_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
