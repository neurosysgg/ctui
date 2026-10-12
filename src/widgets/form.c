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
  f->list = 0;
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

/* the list's option i lit, kept in view (rows: how many it shows) */
static void list_pick(CTUI_FORM *f, int i) {
  const CTUI_FORM_ROW *r = &f->rows[f->list - 1];
  i = i < 0 ? 0 : i >= r->option_count ? r->option_count - 1 : i;
  f->list_pick = i;
  int rows = f->list_h > 2 ? f->list_h - 2 : 1;
  if (i < f->list_top) {
    f->list_top = i;
  } else if (i >= f->list_top + rows) {
    f->list_top = i - rows + 1;
  }
  if (f->list_top > r->option_count - rows) { /* no blank rows at its end */
    f->list_top = r->option_count - rows > 0 ? r->option_count - rows : 0;
  }
}

/* the lit option taken: CHANGED when it's another */
static int list_take(CTUI_FORM *f) {
  CTUI_FORM_ROW *r = &f->rows[f->list - 1];
  f->list = 0;
  if (r->value == f->list_pick) {
    return CTUI_FORM_MOVED;
  }
  r->value = f->list_pick;
  return CTUI_FORM_CHANGED;
}

static char fold_ascii(uint32_t c) {
  return c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c;
}

/* a key while the list is down: it takes them all */
static int list_key(CTUI_FORM *f, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  const CTUI_FORM_ROW *r = &f->rows[f->list - 1];
  int page = f->list_h > 2 ? f->list_h - 2 : 1;
  switch (kp->type) {
  case CTUI_KEY_UP:
    list_pick(f, f->list_pick - 1);
    return CTUI_FORM_MOVED;
  case CTUI_KEY_DOWN:
    list_pick(f, f->list_pick + 1);
    return CTUI_FORM_MOVED;
  case CTUI_KEY_PGUP:
    list_pick(f, f->list_pick - page);
    return CTUI_FORM_MOVED;
  case CTUI_KEY_PGDN:
    list_pick(f, f->list_pick + page);
    return CTUI_FORM_MOVED;
  case CTUI_KEY_HOME:
    list_pick(f, 0);
    return CTUI_FORM_MOVED;
  case CTUI_KEY_END:
    list_pick(f, r->option_count - 1);
    return CTUI_FORM_MOVED;
  case CTUI_KEY_ESC:
    f->list = 0;
    return CTUI_FORM_MOVED;
  case CTUI_KEY_TAB:
  case CTUI_KEY_BACKTAB:
    return step_focus(f, kp->type == CTUI_KEY_TAB ? 1 : -1);
  case CTUI_KEY_ENTER:
    return list_take(f);
  case CTUI_KEY_CHAR:
    if (kp->ch == ' ' && !kp->mods) {
      return list_take(f);
    }
    /* a letter: the next option starting with it */
    for (int k = 1; k <= r->option_count && kp->ch < 0x80; k++) {
      int i = (f->list_pick + k) % r->option_count;
      if (fold_ascii((unsigned char)r->options[i][0]) == fold_ascii(kp->ch)) {
        list_pick(f, i);
        return CTUI_FORM_MOVED;
      }
    }
    return CTUI_FORM_NONE;
  default:
    return CTUI_FORM_NONE;
  }
}

static int list_drop(CTUI_FORM *f) {
  CTUI_FORM_ROW *r = &f->rows[f->focus];
  if (r->option_count < 1) {
    return CTUI_FORM_NONE;
  }
  f->list = f->focus + 1;
  f->list_h = 0; /* until drawn: its size */
  f->list_top = 0;
  list_pick(f, r->value);
  return CTUI_FORM_MOVED;
}

int ctui_form_key(CTUI_FORM *f, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  if (f->count <= 0 || f->focus < 0 || f->focus >= f->count) {
    return CTUI_FORM_NONE;
  }
  if (f->list > 0 && f->list <= f->count) {
    return list_key(f, kp);
  }
  f->list = 0;
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
    if (kp->type == CTUI_KEY_RIGHT) {
      return choice_step(r, 1);
    }
    return activate ? list_drop(f) : CTUI_FORM_NONE;
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

/* the pointer's shape over row i, col cells into its control (< 0: on the
 * label): what a click there would do */
static const char *shape_at(const CTUI_FORM *f, int i, int col, int room) {
  const CTUI_FORM_ROW *r = &f->rows[i];
  if (r->kind == CTUI_FORM_HEADING) {
    return NULL;
  }
  if (r->disabled) {
    return "not-allowed";
  }
  if (col < 0) {
    return NULL;
  }
  switch (r->kind) {
  case CTUI_FORM_ENTRY:
    return "text";
  case CTUI_FORM_TOGGLE:
    return col < 3 ? "pointer" : NULL;
  case CTUI_FORM_CHOICE:
    return col < choice_width(r) + 4 ? "pointer" : NULL;
  case CTUI_FORM_SLIDER:
    return col < slider_width(f, room) ? "grab" : NULL;
  case CTUI_FORM_BUTTON:
    return col < ctui_utf8_width(r->label) + 4 ? "pointer" : NULL;
  default:
    return NULL;
  }
}

/* what's under the pointer followed (its shape claimed): 1 if the row
 * under it changed */
static int hover(CTUI_FORM *f, const CTUI_WIDGET *self,
                 const CTUI_MOUSE_EVENT_DATA *m, int col, int room) {
  int i = ctui_widget_contains(self, m->row, m->col)
              ? f->scroll + m->row - self->y
              : -1;
  i = i >= 0 && i < f->count ? i : -1;
  if (f->drag >= 0 && m->action != CTUI_MOUSE_RELEASE) {
    ctui_pointer_shape("grabbing");
  } else if (i >= 0) {
    ctui_pointer_shape(shape_at(f, i, col, room));
  }
  int changed = i + 1 != f->hover;
  f->hover = i + 1;
  return changed;
}

/* the list's option under (row, col) of the screen, or -1 */
static int list_at(const CTUI_FORM *f, const CTUI_WIDGET *self, int row,
                   int col) {
  int y = row - self->y - f->list_y - 1, x = col - self->x - f->list_x;
  const CTUI_FORM_ROW *r = &f->rows[f->list - 1];
  if (x < 1 || x >= f->list_w - 1 || y < 0 || y >= f->list_h - 2 ||
      f->list_top + y >= r->option_count) {
    return -1;
  }
  return f->list_top + y;
}

/* the mouse while the list is down: it owns it */
static int list_mouse(CTUI_FORM *f, const CTUI_WIDGET *self,
                      const CTUI_MOUSE_EVENT_DATA *m) {
  int i = list_at(f, self, m->row, m->col);
  const CTUI_FORM_ROW *r = &f->rows[f->list - 1];
  if (m->action == CTUI_MOUSE_SCROLL_UP || m->action == CTUI_MOUSE_SCROLL_DOWN) {
    int rows = f->list_h > 2 ? f->list_h - 2 : 1;
    int top = f->list_top + (m->action == CTUI_MOUSE_SCROLL_UP ? -1 : 1);
    top = top > r->option_count - rows ? r->option_count - rows : top;
    f->list_top = top < 0 ? 0 : top;
    return CTUI_FORM_MOVED;
  }
  if (m->action == CTUI_MOUSE_MOTION || m->action == CTUI_MOUSE_RELEASE) {
    if (i >= 0) {
      ctui_pointer_shape("pointer");
    }
    if (i >= 0 && i != f->list_pick) {
      f->list_pick = i;
      return CTUI_FORM_HOVER;
    }
    return CTUI_FORM_NONE;
  }
  if (m->action != CTUI_MOUSE_PRESS) {
    return CTUI_FORM_NONE;
  }
  if (i < 0) { /* beside it: closed, nothing taken */
    f->list = 0;
    return CTUI_FORM_MOVED;
  }
  f->list_pick = i;
  return list_take(f);
}

int ctui_form_mouse(CTUI_FORM *f, const CTUI_WIDGET *self,
                    const CTUI_MOUSE_EVENT_DATA *m) {
  if (f->list > 0 && f->list <= f->count) {
    return list_mouse(f, self, m);
  }
  int col = m->col - self->x - f->control_col;
  int room = self->w - f->control_col;
  int hovered = hover(f, self, m, col, room);
  if (m->action == CTUI_MOUSE_RELEASE) {
    f->drag = -1;
    return hovered ? CTUI_FORM_HOVER : CTUI_FORM_NONE;
  }
  if (m->action == CTUI_MOUSE_MOTION) {
    if (f->drag < 0 || f->drag != f->focus || m->button != 0) {
      return hovered ? CTUI_FORM_HOVER : CTUI_FORM_NONE;
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
    case CTUI_FORM_CHOICE:
      if (col < choice_width(r) + 4) {
        what = list_drop(f);
      }
      break;
    case CTUI_FORM_SLIDER: {
      int bar = slider_width(f, room);
      if (bar >= 2 && col < bar) {
        f->drag = i;
        ctui_pointer_shape("grabbing");
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
                           int focused, int hover, const CTUI_STYLE *st) {
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
                      .disabled = r->disabled,
                      .hover = hover};
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
                      .disabled = r->disabled,
                      .hover = hover};
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
                        .disabled = r->disabled,
                        .hover = hover};
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
                      .disabled = r->disabled,
                      .hover = hover};
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

/* box drawing round h rows of w cells from (y, x): where the look has no
 * frame */
static void glyph_frame(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int y, int x,
                        int w, int h, const CTUI_CELL *pen) {
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      int top = r == 0, bottom = r == h - 1, left = c == 0, right = c == w - 1;
      uint32_t ch = top && left       ? 0x256d /* ╭ */
                    : top && right    ? 0x256e /* ╮ */
                    : bottom && left  ? 0x2570 /* ╰ */
                    : bottom && right ? 0x256f /* ╯ */
                    : top || bottom   ? 0x2500 /* ─ */
                    : left || right   ? 0x2502 /* │ */
                                      : ' ';
      ctui_widget_putc_cell(self, comp, y + r, x + c, ch, pen);
    }
  }
}

/* the list dropped from row y (widget rows): under it if it fits, else
 * above, else on the roomier side and scrolled; over whatever is there */
static void render_list(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, CTUI_FORM *f,
                        int y, const CTUI_STYLE *st) {
  const CTUI_FORM_ROW *r = &f->rows[f->list - 1];
  int w = choice_width(r) + 4;
  w = w < 8 ? 8 : w;
  int x = f->control_col;
  if (x + w > self->w) {
    x = self->w - w > 0 ? self->w - w : 0;
    w = w > self->w ? self->w : w;
  }
  int below = self->h - y - 1, above = y;
  int h = r->option_count + 2;
  int down = h <= below || below >= above;
  int room = down ? below : above;
  h = h > room ? room : h;
  if (h < 3 || w < 5) {
    f->list_h = 0;
    return;
  }
  f->list_x = x;
  f->list_y = down ? y + 1 : y - h;
  f->list_w = w;
  f->list_h = h;
  list_pick(f, f->list_pick); /* in view at this size */
  CTUI_CELL text =
      ctui_style_cell(st, CTUI_STYLE_FG, CTUI_STYLE_BG, st->fg, st->bg);
  CTUI_CELL dim =
      ctui_style_cell(st, CTUI_STYLE_DIM, CTUI_STYLE_BG, st->dim_fg, st->bg);
  CTUI_CELL sel = ctui_style_cell(st, CTUI_STYLE_SEL_FG, CTUI_STYLE_SEL_BG,
                                  st->sel_fg, st->sel_bg);
  CTUI_CELL cur = ctui_style_cell(st, CTUI_STYLE_TITLE, CTUI_STYLE_BG,
                                  st->title_fg, st->bg);
  CTUI_CONTROL frame = {.kind = CTUI_CONTROL_FRAME, .rows = h};
  if (ctui_style_control(st, self, comp, f->list_y, x, w, &frame, st->bg)) {
    for (int rr = 0; rr < h; rr++) {
      for (int c = 0; c < w; c++) {
        ctui_widget_putc_cell(self, comp, f->list_y + rr, x + c, ' ', &text);
      }
    }
  } else {
    glyph_frame(self, comp, f->list_y, x, w, h, &dim);
  }
  for (int k = 0; k < h - 2 && f->list_top + k < r->option_count; k++) {
    int i = f->list_top + k;
    /* the lit one a bar; the one it has now in the heading's colour */
    const CTUI_CELL *pen = i == f->list_pick ? &sel
                           : i == r->value   ? &cur
                                             : &text;
    for (int c = 1; c < w - 1; c++) {
      ctui_widget_putc_cell(self, comp, f->list_y + 1 + k, x + c, ' ', pen);
    }
    ctui_widget_puts_cut_cell(self, comp, f->list_y + 1 + k, x + 2,
                              r->options[i], w - 4, pen);
  }
  if (f->list_top > 0 || f->list_top + h - 2 < r->option_count) {
    /* more than shows: a mark on the edge where they are */
    if (f->list_top > 0) {
      ctui_widget_putc_cell(self, comp, f->list_y, x + w - 2, 0x25b4, &dim);
    }
    if (f->list_top + h - 2 < r->option_count) {
      ctui_widget_putc_cell(self, comp, f->list_y + h - 1, x + w - 2, 0x25be,
                            &dim);
    }
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
                   bar, r, focused, f->hover == i + 1, st);
  }
  /* last: over the rows it drops onto */
  int ly = f->list - 1 - f->scroll;
  if (f->list > 0 && f->list <= f->count &&
      f->rows[f->list - 1].kind == CTUI_FORM_CHOICE && ly >= 0 && ly < self->h) {
    render_list(self, comp, f, ly, st);
  } else {
    f->list = 0; /* its row went out of view, or isn't a choice now */
  }
}

static int emit(CTUI_WIDGET *self, int what) {
  if (what == CTUI_FORM_NONE) {
    return 0;
  }
  if (what == CTUI_FORM_MOVED || what == CTUI_FORM_HOVER) {
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
