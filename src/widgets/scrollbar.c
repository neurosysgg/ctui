#include "scrollbar.h"

static CTUI_SCROLLBAR *g_held; /* the thumb being dragged */

int ctui_scrollbar_held(const CTUI_MOUSE_EVENT_DATA *m) {
  if (g_held && m->action == CTUI_MOUSE_PRESS) {
    g_held->dragging = 0; /* its release went where we never saw it */
    g_held = NULL;
  }
  return g_held != NULL;
}

int ctui_scrollbar_wanted(const CTUI_STYLE *st, int total, int span) {
  return st->control && span > 0 && total > span;
}

/* a of b, scaled to of, rounded */
static int share(int a, int b, int of) {
  return b > 0 ? (int)(((long long)a * of + b / 2) / b) : 0;
}

void ctui_scrollbar_thumb(int rows, int total, int span, int first, int *at,
                          int *len) {
  int n = share(span, total, rows);
  n = n < 1 ? 1 : n > rows ? rows : n;
  int room = total - span;
  first = first < 0 ? 0 : first > room ? room : first;
  *at = room > 0 ? share(first, room, rows - n) : 0;
  *len = n;
}

void ctui_scrollbar_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp,
                           const CTUI_STYLE *st, int row, int col, int rows,
                           int total, int span, int first) {
  if (rows <= 0) {
    return;
  }
  CTUI_CONTROL c = {.kind = CTUI_CONTROL_SCROLLBAR,
                    .value = first,
                    .max = total,
                    .span = span,
                    .rows = rows};
  if (ctui_style_control(st, self, comp, row, col, 1, &c, st->bg)) {
    return;
  }
  int at, len;
  ctui_scrollbar_thumb(rows, total, span, first, &at, &len);
  for (int r = 0; r < rows; r++) {
    int thumb = r >= at && r < at + len;
    ctui_widget_putc(self, comp, row + r, col, thumb ? 0x2503 : 0x2502,
                     thumb ? st->fg : st->dim_fg, st->bg); /* ┃ │ */
  }
}

int ctui_scrollbar_mouse(CTUI_SCROLLBAR *sb, const CTUI_WIDGET *self, int row,
                         int col, int rows, int total, int span, int first,
                         const CTUI_MOUSE_EVENT_DATA *m) {
  int room = total - span, at, len;
  int y = m->row - self->y - row;
  if (m->action == CTUI_MOUSE_PRESS || room <= 0 || rows <= 0) {
    sb->dragging = 0; /* a release gone elsewhere, a view grown short */
  }
  if (!sb->dragging && g_held == sb) {
    g_held = NULL;
  }
  if (room <= 0 || rows <= 0) {
    return -1;
  }
  ctui_scrollbar_thumb(rows, total, span, first, &at, &len);
  if (sb->dragging) {
    if (m->action == CTUI_MOUSE_RELEASE) {
      sb->dragging = 0;
      g_held = NULL;
      return first;
    }
    if (m->action != CTUI_MOUSE_MOTION) {
      return -1;
    }
    int travel = rows - len, to = y - sb->grab;
    to = to < 0 ? 0 : to > travel ? travel : to;
    return to == at ? first : share(to, travel, room);
  }
  if (m->action != CTUI_MOUSE_PRESS || m->button != 0 ||
      m->col != self->x + col || y < 0 || y >= rows) {
    return -1;
  }
  int page = span > 1 ? span - 1 : 1;
  if (y < at) {
    return first > page ? first - page : 0;
  }
  if (y >= at + len) {
    return first + page < room ? first + page : room;
  }
  sb->dragging = 1;
  sb->grab = y - at;
  g_held = sb;
  return first;
}
