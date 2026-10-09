#include "form.h"

#include <stdio.h>
#include <string.h>

#define SLIDER_MAX 32 /* the widest slider bar */

static int takes_focus(const CTUI_FORM_ROW *r) {
  return r->kind != CTUI_FORM_HEADING && !r->disabled;
}

static void scroll_into_view(CTUI_FORM *f) {
  if (f->focus < f->scroll) {
    f->scroll = f->focus;
  } else if (f->page > 0 && f->focus >= f->scroll + f->page) {
    f->scroll = f->focus - f->page + 1;
  }
  /* a heading right above the focus comes along when scrolling up */
  if (f->scroll > 0 && f->scroll == f->focus &&
      f->rows[f->scroll - 1].kind == CTUI_FORM_HEADING) {
    f->scroll--;
  }
}

void ctui_form_focus(CTUI_FORM *f, int row) {
  for (int i = row < 0 ? 0 : row; i < f->count; i++) {
    if (takes_focus(&f->rows[i])) {
      f->focus = i;
      scroll_into_view(f);
      return;
    }
  }
}

/* the next row taking the focus by dir (wrapping), or the focus itself */
static int step_focus(CTUI_FORM *f, int dir) {
  for (int k = 1; k <= f->count; k++) {
    int i = ((f->focus + dir * k) % f->count + f->count) % f->count;
    if (takes_focus(&f->rows[i])) {
      if (i == f->focus) {
        return CTUI_FORM_NONE;
      }
      f->focus = i;
      scroll_into_view(f);
      return CTUI_FORM_MOVED;
    }
  }
  return CTUI_FORM_NONE;
}

static int slider_set(CTUI_FORM_ROW *r, int v) {
  v = v < r->min ? r->min : v > r->max ? r->max : v;
  if (v == r->value) {
    return CTUI_FORM_NONE;
  }
  r->value = v;
  return CTUI_FORM_CHANGED;
}

static int choice_step(CTUI_FORM_ROW *r, int dir) {
  if (r->option_count < 2) {
    return CTUI_FORM_NONE;
  }
  r->value = ((r->value + dir) % r->option_count + r->option_count) %
             r->option_count;
  return CTUI_FORM_CHANGED;
}

int ctui_form_key(CTUI_FORM *f, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  if (f->count <= 0 || f->focus < 0 || f->focus >= f->count) {
    return CTUI_FORM_NONE;
  }
  switch (kp->type) {
  case CTUI_KEY_UP:
  case CTUI_KEY_BACKTAB:
    return step_focus(f, -1);
  case CTUI_KEY_DOWN:
  case CTUI_KEY_TAB:
    return step_focus(f, 1);
  default:
    break;
  }
  CTUI_FORM_ROW *r = &f->rows[f->focus];
  int activate = kp->type == CTUI_KEY_ENTER ||
                 (kp->type == CTUI_KEY_CHAR && kp->ch == ' ' && !kp->mods);
  int step = r->step > 0 ? r->step : 1;
  switch (r->kind) {
  case CTUI_FORM_ENTRY:
    if (kp->type == CTUI_KEY_ENTER) {
      return CTUI_FORM_PRESSED;
    }
    if (r->entry) {
      size_t len = strlen(r->entry->buf);
      size_t cursor = r->entry->cursor;
      if (ctui_entry_key(r->entry, kp)) {
        return strlen(r->entry->buf) != len || cursor == r->entry->cursor
                   ? CTUI_FORM_CHANGED
                   : CTUI_FORM_MOVED;
      }
    }
    return CTUI_FORM_NONE;
  case CTUI_FORM_TOGGLE:
    if (!activate) {
      return CTUI_FORM_NONE;
    }
    r->value = !r->value;
    return CTUI_FORM_CHANGED;
  case CTUI_FORM_CHOICE:
    if (kp->type == CTUI_KEY_LEFT) {
      return choice_step(r, -1);
    }
    return kp->type == CTUI_KEY_RIGHT || activate ? choice_step(r, 1)
                                                  : CTUI_FORM_NONE;
  case CTUI_FORM_SLIDER:
    switch (kp->type) {
    case CTUI_KEY_LEFT:
      return slider_set(r, r->value - step);
    case CTUI_KEY_RIGHT:
      return slider_set(r, r->value + step);
    case CTUI_KEY_HOME:
      return slider_set(r, r->min);
    case CTUI_KEY_END:
      return slider_set(r, r->max);
    default:
      return CTUI_FORM_NONE;
    }
  case CTUI_FORM_BUTTON:
    return activate ? CTUI_FORM_PRESSED : CTUI_FORM_NONE;
  default:
    return CTUI_FORM_NONE;
  }
}

/* a choice's widest option: a drawn one is a field this wide, so it
 * doesn't change size as it steps */
static int choice_width(const CTUI_FORM_ROW *r) {
  int w = 0;
  for (int i = 0; i < r->option_count; i++) {
    int ow = ctui_utf8_width(r->options[i]);
    w = ow > w ? ow : w;
  }
  return w;
}

static int label_width(const CTUI_FORM *f) {
  if (f->label_width > 0) {
    return f->label_width;
  }
  int w = 0;
  for (int i = 0; i < f->count; i++) {
    const CTUI_FORM_ROW *r = &f->rows[i];
    int lw = r->kind == CTUI_FORM_HEADING || r->kind == CTUI_FORM_BUTTON
                 ? 0
                 : ctui_utf8_width(r->label);
    w = lw > w ? lw : w;
  }
  return w;
}

/* the sliders' bar width with room w: " 1234" and a hint come after it,
 * and the bar gives way to them first. One width for every slider, so
 * the bars line up. */
static int slider_width(const CTUI_FORM *f, int w) {
  int hint = 0;
  for (int i = 0; i < f->count; i++) {
    const CTUI_FORM_ROW *r = &f->rows[i];
    if (r->kind == CTUI_FORM_SLIDER && r->hint) {
      int hw = ctui_utf8_width(r->hint) + 2;
      hint = hw > hint ? hw : hint;
    }
  }
  int bar = w - 6 - hint;
  bar = bar < 8 ? w - 6 : bar;
  return bar > SLIDER_MAX ? SLIDER_MAX : bar;
}

int ctui_form_mouse(CTUI_FORM *f, const CTUI_WIDGET *self,
                    const CTUI_MOUSE_EVENT_DATA *m) {
  if (m->action == CTUI_MOUSE_RELEASE) {
    f->drag = -1;
    return CTUI_FORM_NONE;
  }
  int col = m->col - self->x - f->control_col;
  int room = self->w - f->control_col;
  if (m->action == CTUI_MOUSE_MOTION) {
    if (f->drag < 0 || f->drag != f->focus || m->button != 0) {
      return CTUI_FORM_NONE;
    }
    CTUI_FORM_ROW *r = &f->rows[f->drag];
    int bar = slider_width(f, room);
    if (bar < 2) {
      return CTUI_FORM_NONE;
    }
    col = col < 0 ? 0 : col >= bar ? bar - 1 : col;
    return slider_set(r, r->min + (r->max - r->min) * col / (bar - 1));
  }
  if (!ctui_widget_contains(self, m->row, m->col)) {
    return CTUI_FORM_NONE;
  }
  if (m->action == CTUI_MOUSE_SCROLL_UP || m->action == CTUI_MOUSE_SCROLL_DOWN) {
    return step_focus(f, m->action == CTUI_MOUSE_SCROLL_UP ? -1 : 1);
  }
  if (m->action != CTUI_MOUSE_PRESS || m->button != 0) {
    return CTUI_FORM_NONE;
  }
  int i = f->scroll + m->row - self->y;
  if (i < 0 || i >= f->count || !takes_focus(&f->rows[i])) {
    return CTUI_FORM_NONE;
  }
  int moved = i != f->focus;
  f->focus = i;
  CTUI_FORM_ROW *r = &f->rows[i];
  int what = CTUI_FORM_NONE;
  if (col >= 0) {
    switch (r->kind) {
    case CTUI_FORM_ENTRY:
      if (r->entry) {
        ctui_entry_click(r->entry, col);
      }
      what = CTUI_FORM_MOVED;
      break;
    case CTUI_FORM_TOGGLE:
      if (col < 3) {
        r->value = !r->value;
        what = CTUI_FORM_CHANGED;
      }
      break;
    case CTUI_FORM_CHOICE: {
      int w = choice_width(r);
      if (col < 2) {
        what = choice_step(r, -1);
      } else if (col < w + 4) {
        what = choice_step(r, 1);
      }
      break;
    }
    case CTUI_FORM_SLIDER: {
      int bar = slider_width(f, room);
      if (bar >= 2 && col < bar) {
        f->drag = i;
        what = slider_set(r, r->min + (r->max - r->min) * col / (bar - 1));
      }
      break;
    }
    case CTUI_FORM_BUTTON:
      what = CTUI_FORM_PRESSED;
      break;
    default:
      break;
    }
  }
  return what != CTUI_FORM_NONE ? what : moved ? CTUI_FORM_MOVED
                                               : CTUI_FORM_NONE;
}

static void render_control(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int y,
                           int x, int w, int bar, CTUI_FORM_ROW *r,
                           int focused, const CTUI_STYLE *st) {
  unsigned char fg = r->disabled ? st->dim_fg : st->fg, bg = st->bg;
  unsigned char hfg = focused ? st->sel_fg : fg, hbg = focused ? st->sel_bg
                                                               : bg;
  char buf[160];
  int used = 0;
  switch (r->kind) {
  case CTUI_FORM_ENTRY:
    if (r->entry) {
      int ew = w > 40 ? 40 : w;
      ctui_entry_draw(self, comp, y, x, ew, r->entry, focused);
      used = ew;
    }
    break;
  case CTUI_FORM_TOGGLE: {
    CTUI_CONTROL c = {.kind = CTUI_CONTROL_TOGGLE,
                      .value = !!r->value,
                      .max = 1,
                      .focused = focused,
                      .disabled = r->disabled};
    if (w >= 3 && ctui_style_control(st, self, comp, y, x, 3, &c, hbg)) {
      used = 3;
      break;
    }
    used = ctui_widget_puts_cut(self, comp, y, x, r->value ? "[x]" : "[ ]", w,
                                r->value ? (focused ? hfg : st->mark_fg) : hfg,
                                hbg);
    break;
  }
  case CTUI_FORM_CHOICE: {
    const char *v = r->value >= 0 && r->value < r->option_count
                        ? r->options[r->value]
                        : "";
    int cw = choice_width(r) + 4;
    CTUI_CONTROL c = {.kind = CTUI_CONTROL_CHOICE,
                      .value = r->value,
                      .max = r->option_count > 0 ? r->option_count - 1 : 0,
                      .focused = focused,
                      .disabled = r->disabled};
    if (cw <= w && ctui_style_control(st, self, comp, y, x, cw, &c, bg)) {
      /* a cell in: the drop button takes the field's right end */
      ctui_widget_puts_cut(self, comp, y, x + 1, v, cw - 4, fg, bg);
      used = cw;
      break;
    }
    snprintf(buf, sizeof buf, "\xe2\x80\xb9 %s \xe2\x80\xba", v); /* ‹ › */
    used = ctui_widget_puts_cut(self, comp, y, x, buf, w, hfg, hbg);
    break;
  }
  case CTUI_FORM_SLIDER: {
    if (bar < 2) {
      break;
    }
    int span = r->max - r->min, at = span > 0 ? (r->value - r->min) *
                                                    (bar - 1) / span
                                              : 0;
    CTUI_CONTROL ctl = {.kind = CTUI_CONTROL_SLIDER,
                        .value = span > 0 ? r->value - r->min : 0,
                        .max = span > 0 ? span : 0,
                        .focused = focused,
                        .disabled = r->disabled};
    int drawn = ctui_style_control(st, self, comp, y, x, bar, &ctl, bg);
    for (int c = 0; c < bar && !drawn; c++) {
      uint32_t ch = c == at ? 0x25cf : c < at ? 0x2501 : 0x2500; /* ● ━ ─ */
      ctui_widget_putc(self, comp, y, x + c, ch,
                       c == at && focused ? st->sel_bg
                       : c <= at          ? st->title_fg
                                          : st->dim_fg,
                       bg);
    }
    snprintf(buf, sizeof buf, " %d", r->value);
    used = bar + ctui_widget_puts_cut(self, comp, y, x + bar, buf, w - bar,
                                      fg, bg);
    break;
  }
  case CTUI_FORM_BUTTON: {
    int bw = ctui_utf8_width(r->label) + 4;
    CTUI_CONTROL c = {.kind = CTUI_CONTROL_BUTTON,
                      .focused = focused,
                      .disabled = r->disabled};
    if (bw <= w && ctui_style_control(st, self, comp, y, x, bw, &c, bg)) {
      used = 2 + ctui_widget_puts_cut(self, comp, y, x + 2, r->label, bw - 4,
                                      fg, bg) +
             2;
      break;
    }
    snprintf(buf, sizeof buf, "[ %s ]", r->label);
    used = ctui_widget_puts_cut(self, comp, y, x, buf, w, hfg, hbg);
    break;
  }
  default:
    break;
  }
  if (r->hint && used + 2 < w) {
    ctui_widget_puts_cut(self, comp, y, x + used + 2, r->hint, w - used - 2,
                         st->dim_fg, bg);
  }
}

void ctui_form_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_FORM *f = self->widget_data;
  const CTUI_STYLE *st = ctui_style_of(f->style);
  f->page = self->h;
  f->control_col = 2 + label_width(f) + 2;
  if (f->count > 0 && (f->focus < 0 || f->focus >= f->count ||
                       !takes_focus(&f->rows[f->focus]))) {
    ctui_form_focus(f, 0);
  }
  scroll_into_view(f);
  if (f->control_col > self->w / 2) { /* narrow: labels give way */
    f->control_col = self->w / 2;
  }
  int bar = slider_width(f, self->w - f->control_col);
  for (int i = f->scroll, y = 0; i < f->count && y < self->h; i++, y++) {
    CTUI_FORM_ROW *r = &f->rows[i];
    int focused = i == f->focus;
    if (r->kind == CTUI_FORM_HEADING) {
      /* a group: the heading and its rows down to the next one, as far
       * as they show, the heading over its top edge */
      int n = 1;
      while (i + n < f->count && y + n < self->h &&
             f->rows[i + n].kind != CTUI_FORM_HEADING) {
        n++;
      }
      int lw = ctui_utf8_width(r->label);
      lw = lw > self->w - 4 ? self->w - 4 : lw;
      CTUI_CONTROL c = {.kind = CTUI_CONTROL_GROUP,
                        .rows = n,
                        .label = 1,
                        .label_cols = lw + 2};
      if (n > 1 && lw > 0 &&
          ctui_style_control(st, self, comp, y, 0, self->w, &c, st->bg)) {
        ctui_widget_puts_cut(self, comp, y, 2, r->label, lw, st->title_fg,
                             st->bg);
        continue;
      }
      ctui_widget_puts_cut(self, comp, y, 0, r->label, self->w, st->title_fg,
                           st->bg);
      continue;
    }
    if (focused) {
      ctui_widget_puts(self, comp, y, 0, "\xe2\x80\xba", st->title_fg,
                       st->bg); /* › */
    }
    if (r->kind != CTUI_FORM_BUTTON) {
      /* a disabled row's label faint too (popmenu.c's off items) */
      CTUI_CELL pen = {.fg = r->disabled ? st->dim_fg : st->fg,
                       .bg = st->bg,
                       .attr = r->disabled ? CTUI_ATTR_DIM : 0};
      ctui_widget_puts_cut_cell(self, comp, y, 2, r->label, f->control_col - 3,
                                &pen);
    }
    render_control(self, comp, y, f->control_col, self->w - f->control_col,
                   bar, r, focused, st);
  }
}

static int emit(CTUI_WIDGET *self, int what) {
  if (what == CTUI_FORM_NONE) {
    return 0;
  }
  if (what == CTUI_FORM_MOVED) {
    return 1;
  }
  CTUI_FORM *f = self->widget_data;
  const char *name = what == CTUI_FORM_CHANGED ? "changed" : "pressed";
  ctui_logf(E_INF, "[CTUI:FORM] - '%s' %s @ tick %d\n",
            f->rows[f->focus].label, name, ctui_tick_advance());
  CTUI_VALUE_CHANGED_EVENT_DATA changed = {.value = name,
                                           .enabled = f->focus};
  CTUI_EVENT out = {.type = CTUI_VALUE_CHANGED_EVENT,
                    .scope = CTUI_EVENT_SCOPE_GLOBAL,
                    .ev_source = "form",
                    .event_data = &changed,
                    .origin = self};
  ctui_handle_event(&out);
  return 1;
}

int ctui_form_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  return emit(self, ctui_form_key(self->widget_data, ev->event_data));
}

int ctui_form_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  return emit(self, ctui_form_mouse(self->widget_data, self, ev->event_data));
}
