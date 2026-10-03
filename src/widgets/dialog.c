#include "dialog.h"

#include <stdio.h>
#include <string.h>

static int step(CTUI_DIALOG *d, int by) {
  if (d->button_count < 2) {
    return CTUI_DIALOG_NONE;
  }
  d->focus = (d->focus + by + d->button_count) % d->button_count;
  return CTUI_DIALOG_REDRAW;
}

static int finish(CTUI_DIALOG *d, int what) {
  d->open = 0;
  return what;
}

int ctui_dialog_key(CTUI_DIALOG *d, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  if (!d->open) {
    return CTUI_DIALOG_NONE;
  }
  switch (kp->type) {
  case CTUI_KEY_ESC:
    return finish(d, CTUI_DIALOG_CANCEL);
  case CTUI_KEY_ENTER:
    return d->button_count ? finish(d, CTUI_DIALOG_CHOSEN) : CTUI_DIALOG_NONE;
  case CTUI_KEY_TAB:
    return step(d, 1);
  case CTUI_KEY_BACKTAB:
    return step(d, -1);
  case CTUI_KEY_LEFT:
  case CTUI_KEY_RIGHT:
    if (!d->entry) {
      return step(d, kp->type == CTUI_KEY_LEFT ? -1 : 1);
    }
    break;
  default:
    break;
  }
  if (d->entry && ctui_entry_key(d->entry, kp)) {
    return CTUI_DIALOG_REDRAW;
  }
  return CTUI_DIALOG_NONE;
}

int ctui_dialog_mouse(CTUI_DIALOG *d, const CTUI_MOUSE_EVENT_DATA *m) {
  if (!d->open || m->action != CTUI_MOUSE_PRESS || m->button != 0 ||
      m->row != d->button_row) {
    return CTUI_DIALOG_NONE;
  }
  for (int i = 0; i < d->button_count; i++) {
    if (m->col >= d->button_from[i] && m->col < d->button_to[i]) {
      d->focus = i;
      return finish(d, CTUI_DIALOG_CHOSEN);
    }
  }
  return CTUI_DIALOG_NONE;
}

static int text_lines(const char *s, int w) {
  int n = 0;
  while (s && *s) {
    ctui_util_wrap(s, w, &s);
    n++;
  }
  return n;
}

static int buttons_width(const CTUI_DIALOG *d) {
  int w = 0;
  for (int i = 0; i < d->button_count; i++) {
    w += ctui_utf8_width(d->buttons[i]) + 2 + (i ? 2 : 0);
  }
  return w;
}

void ctui_dialog_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_DIALOG *d = self->widget_data;
  const CTUI_STYLE *st = ctui_style_of(d->style);
  d->button_row = -1;
  if (!d->open || self->w < 6 || self->h < 3) {
    return;
  }
  if (d->focus >= d->button_count) {
    d->focus = 0;
  }
  int iw = d->width > 0 ? d->width : 48;
  int bw = buttons_width(d);
  iw = iw < bw ? bw : iw;
  iw = iw > self->w - 4 ? self->w - 4 : iw;
  int lines = text_lines(d->text, iw);
  /* the parts, a blank line between two: text, entry, bar, buttons */
  int parts = (lines > 0) + (d->entry != NULL) + (d->progress >= 0) +
              (d->button_count > 0);
  int h = 2 + lines + (d->entry != NULL) + (d->progress >= 0) +
          (d->button_count > 0) + (parts > 1 ? parts - 1 : 0);
  h = h > self->h ? self->h : h;
  int x = (self->w - iw - 4) / 2, y = (self->h - h) / 2, w = iw + 4;
  unsigned char fg = st->fg, bg = st->bg, dim = st->dim_fg;

  int tw = d->title && d->title[0] ? ctui_utf8_width(d->title) + 2 : 0;
  tw = tw > w - 4 ? w - 4 : tw;
  CTUI_CONTROL frame = {
      .kind = CTUI_CONTROL_FRAME, .rows = h, .label = 2, .label_cols = tw};
  int drawn = ctui_style_control(st, self, comp, y, x, w, &frame, bg);
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      uint32_t ch = ' ';
      if (drawn) {
        /* the frame is under the cells: they're blank */
      } else if (r == 0 || r == h - 1) {
        ch = c == 0       ? (r ? 0x2570 : 0x256d)  /* ╰ ╭ */
             : c == w - 1 ? (r ? 0x256f : 0x256e)  /* ╯ ╮ */
                          : 0x2500;                /* ─ */
      } else if (c == 0 || c == w - 1) {
        ch = 0x2502; /* │ */
      }
      ctui_widget_putc(self, comp, y + r, x + c, ch, dim, bg);
    }
  }
  if (d->title && d->title[0]) {
    char t[256];
    snprintf(t, sizeof t, " %s ", d->title);
    ctui_widget_puts_cut(self, comp, y, x + 2, t, w - 4, st->title_fg, bg);
  }
  int row = y + 1, last = y + h - 2; /* the rows inside the frame */
  const char *s = d->text;
  while (s && *s && row <= last) {
    const char *line = s;
    size_t n = ctui_util_wrap(s, iw, &s);
    ctui_widget_puts_n(self, comp, row++, x + 2, line, n, fg, bg);
  }
  if (lines && row <= last) {
    row++;
  }
  if (d->entry && row <= last) {
    ctui_entry_draw(self, comp, row, x + 2, iw, d->entry, 1);
    row += 2;
  }
  if (d->progress >= 0 && row <= last) {
    int p = d->progress > 100 ? 100 : d->progress;
    char pct[16];
    snprintf(pct, sizeof pct, " %3d%%", p);
    int bar = iw - (int)strlen(pct), full = bar * p / 100;
    CTUI_CONTROL ctl = {.kind = CTUI_CONTROL_PROGRESS,
                        .value = p < 0 ? 0 : p,
                        .max = 100};
    int drawn = ctui_style_control(st, self, comp, row, x + 2, bar, &ctl, bg);
    for (int c = 0; c < bar && !drawn; c++) {
      ctui_widget_putc(self, comp, row, x + 2 + c, c < full ? 0x2588 : 0x2591,
                       c < full ? st->title_fg : dim, bg); /* █ ░ */
    }
    ctui_widget_puts(self, comp, row, x + 2 + bar, pct, fg, bg);
    row += 2;
  }
  if (d->button_count) {
    row = last; /* the buttons stay when the text is cut */
    d->button_row = self->y + row;
    int col = x + 2 + iw - bw;
    for (int i = 0; i < d->button_count; i++) {
      char label[128];
      snprintf(label, sizeof label, " %s ", d->buttons[i]);
      int focused = i == d->focus;
      d->button_from[i] = self->x + col;
      int lw = ctui_utf8_width(label);
      CTUI_CONTROL b = {.kind = CTUI_CONTROL_BUTTON, .focused = focused};
      if (lw <= x + w - 2 - col &&
          ctui_style_control(st, self, comp, row, col, lw, &b, bg)) {
        col += ctui_widget_puts_cut(self, comp, row, col, label, lw, fg, bg);
      } else {
        col += ctui_widget_puts_cut(self, comp, row, col, label,
                                    x + w - 2 - col, focused ? st->sel_fg : fg,
                                    focused ? st->sel_bg : bg);
      }
      d->button_to[i] = self->x + col;
      col += 2;
    }
  }
}

static int emit(CTUI_WIDGET *self, int what) {
  CTUI_DIALOG *d = self->widget_data;
  if (what != CTUI_DIALOG_CHOSEN && what != CTUI_DIALOG_CANCEL) {
    return what != CTUI_DIALOG_NONE;
  }
  int chosen = what == CTUI_DIALOG_CHOSEN;
  ctui_logf(E_INF, "[CTUI:DIALOG] - '%s' %s @ tick %d\n",
            d->title ? d->title : "", chosen ? d->buttons[d->focus] : "cancel",
            ctui_tick_advance());
  CTUI_VALUE_CHANGED_EVENT_DATA changed = {
      .value = chosen ? d->buttons[d->focus] : NULL,
      .enabled = chosen ? d->focus : -1};
  CTUI_EVENT out = {.type = CTUI_VALUE_CHANGED_EVENT,
                    .scope = CTUI_EVENT_SCOPE_GLOBAL,
                    .ev_source = "dialog",
                    .event_data = &changed,
                    .origin = self};
  ctui_handle_event(&out);
  return 1;
}

int ctui_dialog_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_DIALOG *d = self->widget_data;
  if (!d->open) {
    return 0;
  }
  emit(self, ctui_dialog_key(d, ev->event_data));
  return 1;
}

int ctui_dialog_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_DIALOG *d = self->widget_data;
  if (!d->open) {
    return 0;
  }
  return emit(self, ctui_dialog_mouse(d, ev->event_data));
}
