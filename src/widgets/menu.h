#ifndef CTUI_WIDGETS_MENU_H
#define CTUI_WIDGETS_MENU_H

#include "../ctui.h"

typedef struct {
  const char *label;
  int enabled; /* persistent per-item toggle (like a checkbox, independent
               * of cursor position), flipped by ENTER on whichever item
               * currently has the cursor. Renders with the same highlight
               * ctui_menu_render() uses for the selected item. Off by
               * default -- set it explicitly if an item should start on. */
} CTUI_MENU_ITEM;

typedef struct {
  CTUI_MENU_ITEM *items;
  int count;
  int selected; /* cursor position, not the same thing as enabled */
} CTUI_MENU;

void ctui_menu_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* UP/DOWN move the cursor; ENTER toggles the cursor's item's `enabled` and
 * emits a CTUI_VALUE_CHANGED_EVENT (source "menu", value = the item's
 * label, enabled = its new state) via ctui_handle_event() for any
 * interested widget to pick up -- see ctui_status_handle_value_changed()
 * for a listener that ignores enabled, or the ctui demo's
 * main_split_handle_debug_toggle() for one that acts on both directions.
 * Register against ("input", CTUI_KEYPRESS_EVENT) via ctui_event_register(),
 * since that's the source/type ctui_input_loop() emits keypresses under. */
int ctui_menu_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
