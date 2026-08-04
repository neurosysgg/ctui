#include "menu.h"

#include <stdio.h>

void ctui_menu_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_MENU *data = self->widget_data;
  ctui_logf(E_INF, "[CTUI:MENU] - rendering @ tick %d (selected=%d)\n",
            ctui_tick_advance(), data->selected);

  ctui_widget_puts(self, comp, 0, 0,
                   "Pick a tool (Enter to select, Esc to quit)",
                   CTUI_COLOR_CYAN, CTUI_COLOR_DEFAULT);

  for (int i = 0; i < data->count; i++) {
    int row = 2 + i;
    CTUI_MENU_ITEM *item = &data->items[i];
    int is_selected = (i == data->selected);
    int highlighted = is_selected || item->enabled;

    unsigned char fg = highlighted ? CTUI_COLOR_BLACK : CTUI_COLOR_DEFAULT;
    unsigned char bg = highlighted ? CTUI_COLOR_GREEN : CTUI_COLOR_DEFAULT;

    char line[64];
    snprintf(line, sizeof(line), "%c %-20s", is_selected ? '>' : ' ',
             item->label);
    ctui_widget_puts(self, comp, row, 0, line, fg, bg);
  }
}

int ctui_menu_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_MENU *data = self->widget_data;
  CTUI_KEYPRESS_EVENT_DATA *ev_data = (CTUI_KEYPRESS_EVENT_DATA *)ev->event_data;

  ctui_logf(E_INF, "[CTUI:MENU] - keypress %s\n",
            ctui_keytype_name(ev_data->type));

  if (ev_data->type == CTUI_KEY_UP) {
    if (data->selected > 0) {
      data->selected--;
      ctui_logf(E_INF, "[CTUI:MENU] - selection moved up to %d ('%s')\n",
                data->selected, data->items[data->selected].label);
      return 1;
    }
    ctui_log(E_INF, "[CTUI:MENU] - already at top, ignoring UP\n");
  } else if (ev_data->type == CTUI_KEY_DOWN) {
    if (data->selected < data->count - 1) {
      data->selected++;
      ctui_logf(E_INF, "[CTUI:MENU] - selection moved down to %d ('%s')\n",
                data->selected, data->items[data->selected].label);
      return 1;
    }
    ctui_log(E_INF, "[CTUI:MENU] - already at bottom, ignoring DOWN\n");
  } else if (ev_data->type == CTUI_KEY_ENTER) {
    CTUI_MENU_ITEM *item = &data->items[data->selected];
    item->enabled = !item->enabled;

    ctui_logf(E_INF,
              "[CTUI:MENU] - '%s' toggled %s, emitting value-changed\n",
              item->label, item->enabled ? "on" : "off");

    CTUI_VALUE_CHANGED_EVENT_DATA changed = {.value = item->label,
                                             .enabled = item->enabled};
    CTUI_EVENT changed_ev = {.type = CTUI_VALUE_CHANGED_EVENT,
                             .scope = CTUI_EVENT_SCOPE_GLOBAL,
                             .ev_source = "menu",
                             .event_data = &changed};
    ctui_handle_event(&changed_ev);

    return 1;
  }

  ctui_log(E_INF, "[CTUI:MENU] - keypress not handled\n");
  return 0;
}
