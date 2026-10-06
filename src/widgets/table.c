#include "table.h"

#include <stdio.h>
#include <string.h>

#define GAP 1 /* columns between two columns */

static int body_rows(const CTUI_TABLE *t, int h) {
  int n = h - (t->header ? 1 : 0);
  return n > 0 ? n : 0;
}

int ctui_table_width(const CTUI_TABLE *t, int w, int h) {
  int bar = w > 1 && ctui_scrollbar_wanted(ctui_style_of(t->style), t->count,
                                           body_rows(t, h));
  return w - bar;
}

static void scroll_into_view(CTUI_TABLE *t, int rows) {
  if (t->selected < t->scroll) {
    t->scroll = t->selected;
  } else if (rows > 0 && t->selected >= t->scroll + rows) {
    t->scroll = t->selected - rows + 1;
  }
  int top = t->count - (rows > 0 ? rows : 1);
  if (t->scroll > top) { /* no blank rows under the last one */
    t->scroll = top;
  }
  if (t->scroll < 0) {
    t->scroll = 0;
  }
}

void ctui_table_select(CTUI_TABLE *t, int row) {
  if (row >= t->count) {
    row = t->count - 1;
  }
  t->selected = row < 0 ? 0 : row;
  scroll_into_view(t, t->page);
}

static int move(CTUI_TABLE *t, int to) {
  int was = t->selected;
  ctui_table_select(t, to);
  return t->selected != was ? CTUI_TABLE_MOVED : CTUI_TABLE_NONE;
}

int ctui_table_key(CTUI_TABLE *t, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  if (t->count <= 0) {
    return CTUI_TABLE_NONE;
  }
  int page = t->page > 1 ? t->page - 1 : 1;
  switch (kp->type) {
  case CTUI_KEY_UP:
    return move(t, t->selected - 1);
  case CTUI_KEY_DOWN:
    return move(t, t->selected + 1);
  case CTUI_KEY_PGUP:
    return move(t, t->selected - page);
  case CTUI_KEY_PGDN:
    return move(t, t->selected + page);
  case CTUI_KEY_HOME:
    return move(t, 0);
  case CTUI_KEY_END:
    return move(t, t->count - 1);
  case CTUI_KEY_ENTER:
    return CTUI_TABLE_ACTIVATE;
  case CTUI_KEY_INSERT:
    break;
  case CTUI_KEY_CHAR:
    if (kp->ch == ' ' && !kp->mods) {
      break;
    }
    return CTUI_TABLE_NONE;
  default:
    return CTUI_TABLE_NONE;
  }
  if (!t->marks) {
    return CTUI_TABLE_NONE;
  }
  t->marks[t->selected] = !t->marks[t->selected];
  move(t, t->selected + 1);
  return CTUI_TABLE_MARK;
}

void ctui_table_column_span(const CTUI_TABLE *t, int w, int i, int *x,
                            int *width) {
  int fixed = 0, flex = 0;
  for (int c = 0; c < t->column_count; c++) {
    if (t->columns[c].width > 0) {
      fixed += t->columns[c].width;
    } else {
      flex++;
    }
  }
  int rest = w - fixed - GAP * (t->column_count - 1);
  rest = rest > 0 ? rest : 0;
  int at = 0, given = 0, seen = 0;
  for (int c = 0; c <= i; c++) {
    int cw = t->columns[c].width;
    if (cw <= 0) { /* the last flexible column takes the remainder */
      seen++;
      cw = seen == flex ? rest - given : rest / flex;
      given += cw;
    }
    if (c == i) {
      *x = at;
      *width = at + cw > w ? w - at : cw;
      if (*width < 0) {
        *width = 0;
      }
      return;
    }
    at += cw + GAP;
  }
}

int ctui_table_mouse(CTUI_TABLE *t, const CTUI_WIDGET *self,
                     const CTUI_MOUSE_EVENT_DATA *m) {
  int cols = ctui_table_width(t, self->w, self->h);
  if (cols < self->w) {
    t->page = body_rows(t, self->h);
    int first = ctui_scrollbar_mouse(&t->bar, self, t->header ? 1 : 0, cols,
                                     t->page, t->count, t->page, t->scroll, m);
    if (first >= 0) { /* the cursor keeps its line of the view */
      int by = first - t->scroll;
      t->scroll = first;
      return by ? move(t, t->selected + by) : CTUI_TABLE_NONE;
    }
  }
  if (!ctui_widget_contains(self, m->row, m->col)) {
    return CTUI_TABLE_NONE;
  }
  t->page = body_rows(t, self->h);
  if (m->action == CTUI_MOUSE_SCROLL_UP) {
    return move(t, t->selected - 3);
  }
  if (m->action == CTUI_MOUSE_SCROLL_DOWN) {
    return move(t, t->selected + 3);
  }
  if (m->action != CTUI_MOUSE_PRESS || (m->button != 0 && m->button != 2)) {
    return CTUI_TABLE_NONE;
  }
  int row = m->row - self->y, col = m->col - self->x;
  if (t->header && row == 0) {
    if (m->button != 0) {
      return CTUI_TABLE_NONE;
    }
    for (int c = 0; c < t->column_count; c++) {
      int x, w;
      ctui_table_column_span(t, cols, c, &x, &w);
      if (col >= x && col < x + w) {
        t->sort_desc = t->sort_col == c ? !t->sort_desc : 0;
        t->sort_col = c;
        return CTUI_TABLE_SORT;
      }
    }
    return CTUI_TABLE_NONE;
  }
  int r = t->scroll + row - (t->header ? 1 : 0);
  if (r < 0 || r >= t->count) {
    return CTUI_TABLE_NONE;
  }
  if (m->button == 2) {
    ctui_table_select(t, r);
    return CTUI_TABLE_MENU;
  }
  if (r == t->selected) {
    return CTUI_TABLE_ACTIVATE;
  }
  return move(t, r);
}

static void draw_cell(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                      int x, int w, const char *s, int right,
                      const CTUI_CELL *pen) {
  if (w <= 0 || !s) {
    return;
  }
  int sw = ctui_utf8_width(s);
  int at = right && sw < w ? x + w - sw : x;
  ctui_widget_puts_cut_cell(self, comp, row, at, s, w, pen);
}

void ctui_table_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_TABLE *t = self->widget_data;
  const CTUI_STYLE *st = ctui_style_of(t->style);
  t->page = body_rows(t, self->h);
  if (t->count > 0) {
    ctui_table_select(t, t->selected);
  }
  if (self->w <= 0) {
    return;
  }
  int cols = ctui_table_width(t, self->w, self->h);
  int row = 0;
  if (t->header && self->h > 0) {
    for (int c = 0; c < t->column_count; c++) {
      int x, w;
      ctui_table_column_span(t, cols, c, &x, &w);
      char title[128];
      snprintf(title, sizeof title, "%s%s", t->columns[c].title,
               c != t->sort_col ? ""
               : t->sort_desc   ? " \xe2\x96\xbe"   /* ▾ */
                                : " \xe2\x96\xb4"); /* ▴ */
      CTUI_CELL pen = ctui_style_cell(st, CTUI_STYLE_TITLE, CTUI_STYLE_BG, 0, 0);
      draw_cell(self, comp, 0, x, w, title, t->columns[c].align_right, &pen);
    }
    row = 1;
  }
  char scratch[256];
  for (int r = t->scroll; r < t->count && row < self->h; r++, row++) {
    int sel = r == t->selected, marked = t->marks && t->marks[r];
    int span = t->row_span && t->row_span(t->ctx, r);
    /* the row's own colour (row_fg) is a basic one */
    unsigned char own = t->row_fg ? t->row_fg(t->ctx, r) : 0;
    CTUI_STYLE_SLOT fg = marked ? CTUI_STYLE_MARK
                         : own  ? CTUI_STYLE_NONE
                         : span ? CTUI_STYLE_TITLE
                                : CTUI_STYLE_FG;
    CTUI_STYLE_SLOT bg = CTUI_STYLE_BG;
    /* a marked row filled when the style has a mark_bg */
    int fill = marked && (st->mark_bg != CTUI_COLOR_DEFAULT ||
                          (st->rgb[CTUI_STYLE_MARK_BG] & CTUI_STYLE_RGB));
    if (fill) {
      bg = CTUI_STYLE_MARK_BG;
    }
    if (sel) {
      fg = marked && !fill ? CTUI_STYLE_MARK : CTUI_STYLE_SEL_FG;
      bg = CTUI_STYLE_SEL_BG;
    }
    CTUI_CELL pen = ctui_style_cell(st, fg, bg, own, 0);
    if (sel || fill) {
      for (int c = 0; c < cols; c++) {
        ctui_widget_putc_cell(self, comp, row, c, ' ', &pen);
      }
    }
    if (span) {
      scratch[0] = '\0';
      draw_cell(self, comp, row, 0, cols,
                t->cell(t->ctx, r, 0, scratch, sizeof scratch), 0, &pen);
      continue;
    }
    for (int c = 0; c < t->column_count; c++) {
      int x, w;
      ctui_table_column_span(t, cols, c, &x, &w);
      scratch[0] = '\0';
      const char *s = t->cell(t->ctx, r, c, scratch, sizeof scratch);
      draw_cell(self, comp, row, x, w, s, t->columns[c].align_right, &pen);
    }
  }
  if (cols < self->w) {
    ctui_scrollbar_render(self, comp, st, t->header ? 1 : 0, cols, t->page,
                          t->count, t->page, t->scroll);
  }
}

static const char *const action_names[] = {"", "moved", "activate", "mark",
                                           "sort", "menu"};

static int emit(CTUI_WIDGET *self, int what) {
  if (what == CTUI_TABLE_NONE) {
    return 0;
  }
  CTUI_TABLE *t = self->widget_data;
  ctui_logf(E_INF, "[CTUI:TABLE] - %s @ tick %d (row %d of %d)\n",
            action_names[what], ctui_tick_advance(), t->selected, t->count);
  CTUI_VALUE_CHANGED_EVENT_DATA changed = {
      .value = action_names[what],
      .enabled = what == CTUI_TABLE_SORT ? t->sort_col : t->selected};
  CTUI_EVENT out = {.type = CTUI_VALUE_CHANGED_EVENT,
                    .scope = CTUI_EVENT_SCOPE_GLOBAL,
                    .ev_source = "table",
                    .event_data = &changed,
                    .origin = self};
  ctui_handle_event(&out);
  return 1;
}

int ctui_table_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  return emit(self, ctui_table_key(self->widget_data, ev->event_data));
}

int ctui_table_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  return emit(self,
              ctui_table_mouse(self->widget_data, self, ev->event_data));
}
