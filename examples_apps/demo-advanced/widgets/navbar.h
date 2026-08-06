#ifndef DEMO_ADVANCED_WIDGETS_NAVBAR_H
#define DEMO_ADVANCED_WIDGETS_NAVBAR_H

#include "ctui.h"
#include "widgets/menu.h" /* reuses CTUI_MENU_ITEM -- same label+enabled
                          * shape ctui_widget's vertical CTUI_MENU already
                          * uses, just walked left-to-right and highlighted
                          * in truecolor here instead of top-to-bottom in
                          * basic ANSI green */

typedef struct {
  CTUI_MENU_ITEM *items;
  int count;
  int selected; /* cursor position, not the same thing as an item's enabled
                * -- same distinction CTUI_MENU makes */
} CTUI_NAVBAR;

/* horizontal item bar (row 0) plus a full-width truecolor hue-sweep rule
 * (row 1, drawn only if self->h >= 2) marking the band off from the main
 * area below it */
void ctui_navbar_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* LEFT/RIGHT move the cursor, ENTER toggles the cursor's item's `enabled`
 * and emits a CTUI_VALUE_CHANGED_EVENT (source "navbar", value = the item's
 * label, enabled = its new state) via ctui_handle_event() -- same emission
 * contract as ctui_menu_handle_keypress(), just LEFT/RIGHT in place of
 * UP/DOWN since items lay out horizontally here. Register against ("input",
 * CTUI_KEYPRESS_EVENT). */
int ctui_navbar_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
