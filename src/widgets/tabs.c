#include "tabs.h"

#include <stdio.h>

static int selectable(const CTUI_TABS *t, int i) {
  return i >= 0 && i < t->count && !(t->vertical && t->items[i].heading);
}

/* the width a bar item takes, its separator included */
static int bar_width(const CTUI_TABS *t, int i) {
  return ctui_utf8_width(t->items[i].label) + 2 + (i + 1 < t->count);
}

/* scroll so the selection shows: rows for a sidebar, columns w for a bar */
static void scroll_into_view(CTUI_TABS *t, int w) {
  if (t->selected < t->scroll) {
    t->scroll = t->selected;
  }
  if (t->vertical) {
    if (t->page > 0 && t->selected >= t->scroll + t->page) {
      t->scroll = t->selected - t->page + 1;
    }
    if (t->scroll > 0 && t->scroll == t->selected &&
        t->items[t->scroll - 1].heading) {
      t->scroll--; /* its heading comes along */
    }
    return;
  }
  for (;;) {
    int used = 0;
    for (int i = t->scroll; i <= t->selected; i++) {
      used += bar_width(t, i);
    }
    if (used <= w || t->scroll >= t->selected) {
      return;
    }
    t->scroll++;
  }
}

void ctui_tabs_select(CTUI_TABS *t, int i) {
  for (i = i < 0 ? 0 : i; i < t->count; i++) {
    if (selectable(t, i)) {
      t->selected = i;
      return;
    }
  }
}

static int move(CTUI_TABS *t, int dir, int from) {
  for (int i = from + dir; i >= 0 && i < t->count; i += dir) {
    if (selectable(t, i)) {
      t->selected = i;
      return CTUI_TABS_MOVED;
    }
  }
  return CTUI_TABS_NONE;
}

int ctui_tabs_key(CTUI_TABS *t, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  CTUI_KEYTYPE back = t->vertical ? CTUI_KEY_UP : CTUI_KEY_LEFT;
  CTUI_KEYTYPE fwd = t->vertical ? CTUI_KEY_DOWN : CTUI_KEY_RIGHT;
  if (kp->type == back) {
    return move(t, -1, t->selected);
  }
  if (kp->type == fwd) {
    return move(t, 1, t->selected);
  }
  if (kp->type == CTUI_KEY_HOME) {
    int was = t->selected;
    return move(t, 1, -1) && t->selected != was ? CTUI_TABS_MOVED
                                                : CTUI_TABS_NONE;
  }
  if (kp->type == CTUI_KEY_END) {
    int was = t->selected;
    return move(t, -1, t->count) && t->selected != was ? CTUI_TABS_MOVED
                                                       : CTUI_TABS_NONE;
  }
  if (kp->type == CTUI_KEY_ENTER && selectable(t, t->selected)) {
    return CTUI_TABS_ACTIVATE;
  }
  return CTUI_TABS_NONE;
}

int ctui_tabs_mouse(CTUI_TABS *t, const CTUI_WIDGET *self,
                    const CTUI_MOUSE_EVENT_DATA *m) {
  if (!ctui_widget_contains(self, m->row, m->col)) {
    return CTUI_TABS_NONE;
  }
  if (m->action == CTUI_MOUSE_SCROLL_UP || m->action == CTUI_MOUSE_SCROLL_DOWN) {
    return move(t, m->action == CTUI_MOUSE_SCROLL_UP ? -1 : 1, t->selected);
  }
  if (m->action != CTUI_MOUSE_PRESS || (m->button != 0 && m->button != 2)) {
    return CTUI_TABS_NONE;
  }
  int hit = -1;
  if (t->vertical) {
    hit = t->scroll + m->row - self->y;
  } else {
    for (int i = t->scroll, x = 0; i < t->count; i++) {
      int w = bar_width(t, i);
      if (m->col - self->x < x + w) {
        hit = i;
        break;
      }
      x += w;
    }
  }
  if (!selectable(t, hit)) {
    return CTUI_TABS_NONE;
  }
  if (m->button == 2) {
    t->menu_item = hit;
    return CTUI_TABS_MENU;
  }
  if (hit == t->selected) {
    return CTUI_TABS_NONE;
  }
  t->selected = hit;
  return CTUI_TABS_MOVED;
}

void ctui_tabs_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_TABS *t = self->widget_data;
  const CTUI_STYLE *st = ctui_style_of(t->style);
  t->page = self->h;
  if (!selectable(t, t->selected)) {
    ctui_tabs_select(t, 0);
  }
  scroll_into_view(t, self->w);
  if (t->vertical) {
    for (int i = t->scroll, y = 0; i < t->count && y < self->h; i++, y++) {
      const CTUI_TABS_ITEM *it = &t->items[i];
      if (it->heading) {
        ctui_widget_puts_cut(self, comp, y, 0, it->label, self->w,
                             st->title_fg, st->bg);
        continue;
      }
      int sel = i == t->selected;
      unsigned char fg = sel ? st->sel_fg : st->fg, bg = sel ? st->sel_bg
                                                             : st->bg;
      for (int c = 0; sel && c < self->w; c++) {
        ctui_widget_putc(self, comp, y, c, ' ', fg, bg);
      }
      ctui_widget_puts_cut(self, comp, y, 1, it->label, self->w - 2, fg, bg);
    }
    return;
  }
  int x = 0;
  for (int i = t->scroll; i < t->count && x < self->w; i++) {
    int sel = i == t->selected;
    char label[160];
    snprintf(label, sizeof label, " %s ", t->items[i].label);
    x += ctui_widget_puts_cut(self, comp, 0, x, label, self->w - x,
                              sel ? st->sel_fg : st->fg,
                              sel ? st->sel_bg : st->bg);
    if (i + 1 < t->count && x < self->w) {
      ctui_widget_putc(self, comp, 0, x++, 0x2502, st->dim_fg, st->bg); /* │ */
    }
  }
}

static int emit(CTUI_WIDGET *self, int what) {
  if (what == CTUI_TABS_NONE) {
    return 0;
  }
  CTUI_TABS *t = self->widget_data;
  const char *name = what == CTUI_TABS_MOVED      ? "moved"
                     : what == CTUI_TABS_ACTIVATE ? "activate"
                                                  : "menu";
  ctui_logf(E_INF, "[CTUI:TABS] - %s '%s' @ tick %d\n", name,
            t->items[t->selected].label, ctui_tick_advance());
  CTUI_VALUE_CHANGED_EVENT_DATA changed = {
      .value = name,
      .enabled = what == CTUI_TABS_MENU ? t->menu_item : t->selected};
  CTUI_EVENT out = {.type = CTUI_VALUE_CHANGED_EVENT,
                    .scope = CTUI_EVENT_SCOPE_GLOBAL,
                    .ev_source = "tabs",
                    .event_data = &changed,
                    .origin = self};
  ctui_handle_event(&out);
  return 1;
}

int ctui_tabs_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  return emit(self, ctui_tabs_key(self->widget_data, ev->event_data));
}

int ctui_tabs_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  return emit(self, ctui_tabs_mouse(self->widget_data, self, ev->event_data));
}
