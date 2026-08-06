#include "navbar.h"

#include <stdio.h>
#include <string.h>

/* HSV(h, 1, 1) -> RGB, h in [0,1) -- same full-saturation hue sweep as
 * debug_info's/dump_palette's truecolor demo rows; duplicated here rather
 * than shared since this is a demo-app-local widget, not library code */
static void hue_to_rgb(double h, unsigned char *r, unsigned char *g,
                       unsigned char *b) {
  double hh = h * 6.0;
  int i = (int)hh;
  double f = hh - i;
  double q = 1.0 - f;
  double rr, gg, bb;
  switch (i % 6) {
  case 0:
    rr = 1;
    gg = f;
    bb = 0;
    break;
  case 1:
    rr = q;
    gg = 1;
    bb = 0;
    break;
  case 2:
    rr = 0;
    gg = 1;
    bb = f;
    break;
  case 3:
    rr = 0;
    gg = q;
    bb = 1;
    break;
  case 4:
    rr = f;
    gg = 0;
    bb = 1;
    break;
  default:
    rr = 1;
    gg = 0;
    bb = q;
    break;
  }
  *r = (unsigned char)(rr * 255);
  *g = (unsigned char)(gg * 255);
  *b = (unsigned char)(bb * 255);
}

void ctui_navbar_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_NAVBAR *data = self->widget_data;
  ctui_logf(E_INF, "[CTUI:NAVBAR] - rendering @ tick %d (selected=%d)\n",
            ctui_tick_advance(), data->selected);

  int col = 1;
  for (int i = 0; i < data->count && col < self->w; i++) {
    CTUI_MENU_ITEM *item = &data->items[i];
    int is_selected = (i == data->selected);

    char label[24];
    snprintf(label, sizeof(label), " %c %s ", item->enabled ? '*' : ' ',
             item->label);
    int len = (int)strlen(label);
    if (col + len > self->w) {
      break;
    }

    if (is_selected) {
      /* deep-navy fg over a bright cyan-accent bg -- the cursor position,
       * distinct from an item's own persistent enabled state below */
      ctui_widget_puts_rgb(self, comp, 0, col, label, 15, 20, 30, 80, 200,
                           220);
    } else if (item->enabled) {
      ctui_widget_puts(self, comp, 0, col, label, CTUI_COLOR_CYAN,
                       CTUI_COLOR_DEFAULT);
    } else {
      ctui_widget_puts(self, comp, 0, col, label, CTUI_COLOR_DEFAULT,
                       CTUI_COLOR_DEFAULT);
    }
    col += len;
  }

  if (self->h < 2) {
    return;
  }
  for (int c = 0; c < self->w; c++) {
    unsigned char r, g, b;
    hue_to_rgb((double)c / self->w, &r, &g, &b);
    ctui_widget_putc_rgb(self, comp, 1, c, ' ', 0, 0, 0, r, g, b);
  }
}

int ctui_navbar_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_NAVBAR *data = self->widget_data;
  CTUI_KEYPRESS_EVENT_DATA *ev_data = (CTUI_KEYPRESS_EVENT_DATA *)ev->event_data;

  ctui_logf(E_INF, "[CTUI:NAVBAR] - keypress %s\n",
            ctui_keytype_name(ev_data->type));

  if (ev_data->type == CTUI_KEY_LEFT) {
    if (data->selected > 0) {
      data->selected--;
      ctui_logf(E_INF, "[CTUI:NAVBAR] - selection moved left to %d ('%s')\n",
                data->selected, data->items[data->selected].label);
      return 1;
    }
    ctui_log(E_INF, "[CTUI:NAVBAR] - already at leftmost, ignoring LEFT\n");
  } else if (ev_data->type == CTUI_KEY_RIGHT) {
    if (data->selected < data->count - 1) {
      data->selected++;
      ctui_logf(E_INF,
                "[CTUI:NAVBAR] - selection moved right to %d ('%s')\n",
                data->selected, data->items[data->selected].label);
      return 1;
    }
    ctui_log(E_INF, "[CTUI:NAVBAR] - already at rightmost, ignoring RIGHT\n");
  } else if (ev_data->type == CTUI_KEY_ENTER) {
    CTUI_MENU_ITEM *item = &data->items[data->selected];
    item->enabled = !item->enabled;

    ctui_logf(E_INF,
              "[CTUI:NAVBAR] - '%s' toggled %s, emitting value-changed\n",
              item->label, item->enabled ? "on" : "off");

    CTUI_VALUE_CHANGED_EVENT_DATA changed = {.value = item->label,
                                             .enabled = item->enabled};
    CTUI_EVENT changed_ev = {.type = CTUI_VALUE_CHANGED_EVENT,
                             .scope = CTUI_EVENT_SCOPE_GLOBAL,
                             .ev_source = "navbar",
                             .event_data = &changed};
    ctui_handle_event(&changed_ev);

    return 1;
  }

  ctui_log(E_INF, "[CTUI:NAVBAR] - keypress not handled\n");
  return 0;
}
