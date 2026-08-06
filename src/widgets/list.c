#include "list.h"

#include <stdio.h>

void ctui_list_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_LIST *data = self->widget_data;
  ctui_logf(E_INF,
            "[CTUI:LIST] - rendering @ tick %d (%d items, selected=%d, "
            "scroll_offset=%d)\n",
            ctui_tick_advance(), data->count, data->selected,
            data->scroll_offset);

  for (int row = 0; row < self->h; row++) {
    int idx = data->scroll_offset + row;
    if (idx >= data->count) {
      break;
    }
    CTUI_LIST_ITEM *item = &data->items[idx];
    int is_selected = (idx == data->selected);

    char line[128];
    snprintf(line, sizeof(line), "%s%s", item->label,
             item->is_dir ? "/" : "");
    ctui_util_truncate_str(line, (size_t)self->w, "...");

    unsigned char fg = is_selected ? CTUI_COLOR_BLACK : CTUI_COLOR_DEFAULT;
    unsigned char bg = is_selected ? CTUI_COLOR_GREEN : CTUI_COLOR_DEFAULT;
    ctui_widget_puts(self, comp, row, 0, line, fg, bg);
  }
}

static void adjust_scroll(CTUI_WIDGET *self, CTUI_LIST *data) {
  if (data->selected < data->scroll_offset) {
    data->scroll_offset = data->selected;
  } else if (self->h > 0 && data->selected >= data->scroll_offset + self->h) {
    data->scroll_offset = data->selected - self->h + 1;
  }
}

int ctui_list_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_LIST *data = self->widget_data;
  CTUI_KEYPRESS_EVENT_DATA *ev_data = (CTUI_KEYPRESS_EVENT_DATA *)ev->event_data;

  ctui_logf(E_INF, "[CTUI:LIST] - keypress %s\n",
            ctui_keytype_name(ev_data->type));

  if (ev_data->type == CTUI_KEY_UP) {
    if (data->selected > 0) {
      data->selected--;
      adjust_scroll(self, data);
      ctui_logf(E_INF, "[CTUI:LIST] - selection moved up to %d ('%s')\n",
                data->selected, data->items[data->selected].label);
      return 1;
    }
    ctui_log(E_INF, "[CTUI:LIST] - already at top, ignoring UP\n");
  } else if (ev_data->type == CTUI_KEY_DOWN) {
    if (data->selected < data->count - 1) {
      data->selected++;
      adjust_scroll(self, data);
      ctui_logf(E_INF, "[CTUI:LIST] - selection moved down to %d ('%s')\n",
                data->selected, data->items[data->selected].label);
      return 1;
    }
    ctui_log(E_INF, "[CTUI:LIST] - already at bottom, ignoring DOWN\n");
  } else if (ev_data->type == CTUI_KEY_ENTER) {
    if (data->count == 0) {
      ctui_log(E_INF, "[CTUI:LIST] - ENTER with no items, ignoring\n");
      return 0;
    }
    CTUI_LIST_ITEM *item = &data->items[data->selected];
    ctui_logf(E_INF,
              "[CTUI:LIST] - '%s' selected, emitting value-changed\n",
              item->label);

    CTUI_VALUE_CHANGED_EVENT_DATA changed = {.value = item->label,
                                             .enabled = item->is_dir};
    CTUI_EVENT changed_ev = {.type = CTUI_VALUE_CHANGED_EVENT,
                             .scope = CTUI_EVENT_SCOPE_GLOBAL,
                             .ev_source = "list",
                             .event_data = &changed,
                             .origin = self};
    ctui_handle_event(&changed_ev);

    return 1;
  }

  ctui_log(E_INF, "[CTUI:LIST] - keypress not handled\n");
  return 0;
}
