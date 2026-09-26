#ifndef CTUI_WIDGETS_TABS_H
#define CTUI_WIDGETS_TABS_H

#include "../ctui.h"
#include "style.h"

/* Page switching: a sidebar (vertical: an item a row, headings between
 * groups of them) or a tab bar (one row, " Files │ Sound │ ... ",
 * scrolled to keep the selection in view). The pages are the app's; it
 * shows the one selected.
 *
 * Keys: up/down (vertical) or left/right (a bar), home/end move the
 * selection; Enter activates it (the app moves its focus into the page).
 * Mouse: a click selects, the wheel moves. */
typedef struct {
  const char *label;
  int heading; /* vertical only: a group's title, not selectable */
} CTUI_TABS_ITEM;

typedef struct {
  const CTUI_TABS_ITEM *items;
  int count;
  int selected;
  int vertical;
  int scroll; /* the first item shown */
  const CTUI_STYLE *style;
  int page; /* the last render's rows (vertical) */
} CTUI_TABS;

enum {
  CTUI_TABS_NONE = 0,
  CTUI_TABS_MOVED,    /* selected changed: show that page */
  CTUI_TABS_ACTIVATE, /* Enter on it */
};

/* selects item i, or the next selectable one after it */
void ctui_tabs_select(CTUI_TABS *t, int i);

int ctui_tabs_key(CTUI_TABS *t, const CTUI_KEYPRESS_EVENT_DATA *kp);
int ctui_tabs_mouse(CTUI_TABS *t, const CTUI_WIDGET *self,
                    const CTUI_MOUSE_EVENT_DATA *m);

void ctui_tabs_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* ("input", CTUI_KEYPRESS_EVENT / CTUI_MOUSE_EVENT): MOVED and ACTIVATE
 * emit a CTUI_VALUE_CHANGED_EVENT (source "tabs", origin self), value =
 * "moved" | "activate", enabled = the selected item */
int ctui_tabs_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);
int ctui_tabs_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
