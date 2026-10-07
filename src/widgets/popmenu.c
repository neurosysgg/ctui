#include "popmenu.h"

#include <stdio.h>
#include <string.h>

#define POPMENU_MARK_COLS 2 /* "✓ " before every label */
#define POPMENU_KEYS_GAP 3  /* between a label and its keys */

static int selectable(const CTUI_POPMENU_ITEM *it) {
  return !(it->flags & (CTUI_POPMENU_SEP | CTUI_POPMENU_OFF));
}

void ctui_popmenu_label(const char *label, char *out, size_t cap, int *letter) {
  size_t n = 0;
  *letter = -1;
  for (const char *s = label ? label : ""; *s && n + 1 < cap; s++) {
    if (*s == '&' && s[1] == '&') {
      s++;
    } else if (*s == '&' && s[1]) {
      *letter = *letter < 0 ? (int)n : *letter;
      continue;
    }
    out[n++] = *s;
  }
  if (cap) {
    out[n] = '\0';
  }
  *letter = *letter >= (int)n ? -1 : *letter;
}

static int label_width(const char *label) {
  char s[256];
  int letter;
  ctui_popmenu_label(label, s, sizeof s, &letter);
  return ctui_utf8_width(s);
}

static int keys_width(const CTUI_POPMENU_LEVEL *l) {
  int w = 0;
  for (int i = 0; i < l->count; i++) {
    int k = l->items[i].keys ? ctui_utf8_width(l->items[i].keys) : 0;
    w = k > w ? k : w;
  }
  return w;
}

static int has_sub(const CTUI_POPMENU_LEVEL *l) {
  for (int i = 0; i < l->count; i++) {
    if (l->items[i].sub) {
      return 1;
    }
  }
  return 0;
}

/* a level's size: the frame, a column of room either side, the marks, the
 * labels, the keys after a gap, a submenu's arrow */
static void level_size(CTUI_POPMENU_LEVEL *l, int rows, int cols) {
  int lw = 0;
  for (int i = 0; i < l->count; i++) {
    int w = (l->items[i].flags & CTUI_POPMENU_SEP)
                ? 0
                : label_width(l->items[i].label);
    lw = w > lw ? w : lw;
  }
  int kw = keys_width(l);
  l->w = 4 + POPMENU_MARK_COLS + lw + (kw ? POPMENU_KEYS_GAP + kw : 0) +
         (has_sub(l) ? 2 : 0);
  l->h = l->count + 2;
  l->w = l->w > cols ? cols : l->w;
  l->h = l->h > rows ? rows : l->h;
}

/* from at, size long, on a screen of n: kept on it, else flipped to end at
 * back (left of / above the point) */
static int fit(int at, int size, int back, int n) {
  if (at + size <= n) {
    return at;
  }
  at = back - size + 1;
  if (at < 0) {
    at = n - size;
  }
  return at < 0 ? 0 : at;
}

void ctui_popmenu_layout(CTUI_POPMENU *m, int rows, int cols) {
  m->rows = rows;
  m->cols = cols;
  for (int k = 0; k < m->depth; k++) {
    CTUI_POPMENU_LEVEL *l = &m->level[k];
    level_size(l, rows, cols);
    if (k == 0) {
      l->x = fit(m->col, l->w, m->col, cols);
      l->y = fit(m->row, l->h, m->row, rows);
      continue;
    }
    /* beside its item, its first item level with it */
    const CTUI_POPMENU_LEVEL *p = &m->level[k - 1];
    l->x = fit(p->x + p->w, l->w, p->x - 1, cols);
    int y = p->y + (p->sel > 0 ? p->sel : 0);
    l->y = y + l->h <= rows ? y : rows - l->h < 0 ? 0 : rows - l->h;
  }
}

void ctui_popmenu_open(CTUI_POPMENU *m, const CTUI_POPMENU_ITEM *items,
                       int count, int row, int col, int rows, int cols) {
  m->depth = 1;
  m->level[0] = (CTUI_POPMENU_LEVEL){.items = items, .count = count, .sel = -1};
  m->row = row;
  m->col = col;
  m->chosen = NULL;
  ctui_popmenu_layout(m, rows, cols);
  ctui_logf(E_INF, "[CTUI:POPMENU] - opened at %d,%d (%d items) @ tick %d\n",
            row, col, count, ctui_tick_advance());
}

void ctui_popmenu_close(CTUI_POPMENU *m) { m->depth = 0; }

/* the cursor moved by dir (+1 / -1) to the next item that can be chosen,
 * wrapping; 1 if it moved */
static int move(CTUI_POPMENU_LEVEL *l, int dir) {
  int at = l->sel;
  for (int n = 0; n < l->count; n++) {
    at = at < 0 ? (dir > 0 ? 0 : l->count - 1)
                : (at + dir + l->count) % l->count;
    if (selectable(&l->items[at])) {
      int moved = at != l->sel;
      l->sel = at;
      return moved;
    }
  }
  return 0;
}

/* item i's submenu at level k + 1 (those deeper closed), the cursor on its
 * first (keys) or none (the pointer) */
static int open_sub(CTUI_POPMENU *m, int k, int i, int first) {
  const CTUI_POPMENU_ITEM *it = &m->level[k].items[i];
  if (k + 1 >= CTUI_POPMENU_DEPTH || !it->sub) {
    return CTUI_POPMENU_NONE;
  }
  m->level[k].sel = i;
  m->level[k + 1] =
      (CTUI_POPMENU_LEVEL){.items = it->sub, .count = it->sub_count, .sel = -1};
  m->depth = k + 2;
  if (first) {
    move(&m->level[k + 1], 1);
  }
  ctui_popmenu_layout(m, m->rows, m->cols);
  return CTUI_POPMENU_REDRAW;
}

static int choose(CTUI_POPMENU *m, const CTUI_POPMENU_ITEM *it) {
  m->chosen = it;
  m->depth = 0;
  return CTUI_POPMENU_CHOSEN;
}

/* item i of level k chosen: its submenu opened, else it */
static int activate(CTUI_POPMENU *m, int k, int i) {
  const CTUI_POPMENU_ITEM *it = &m->level[k].items[i];
  if (!selectable(it)) {
    return CTUI_POPMENU_NONE;
  }
  return it->sub ? open_sub(m, k, i, 1) : choose(m, it);
}

static uint32_t fold(uint32_t c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static uint32_t item_letter(const CTUI_POPMENU_ITEM *it) {
  char s[256];
  int letter;
  uint32_t cp = 0;
  ctui_popmenu_label(it->label, s, sizeof s, &letter);
  if (letter >= 0) {
    ctui_utf8_decode(s + letter, &cp);
  }
  return fold(cp);
}

/* a letter: its one item chosen, else the cursor to the next with it */
static int by_letter(CTUI_POPMENU *m, int k, uint32_t ch) {
  CTUI_POPMENU_LEVEL *l = &m->level[k];
  int n = 0, only = -1, next = -1;
  for (int j = 1; j <= l->count; j++) {
    int i = ((l->sel < 0 ? -1 : l->sel) + j) % l->count;
    if (selectable(&l->items[i]) && item_letter(&l->items[i]) == fold(ch)) {
      n++;
      only = i;
      next = next < 0 ? i : next;
    }
  }
  if (n == 1) {
    return activate(m, k, only);
  }
  if (n > 1) {
    l->sel = next;
    return CTUI_POPMENU_REDRAW;
  }
  return CTUI_POPMENU_NONE;
}

/* one level closed (the menu at the top) */
static int back(CTUI_POPMENU *m) {
  if (m->depth > 1) {
    m->depth--;
    return CTUI_POPMENU_REDRAW;
  }
  m->chosen = NULL;
  m->depth = 0;
  return CTUI_POPMENU_CLOSED;
}

int ctui_popmenu_key(CTUI_POPMENU *m, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  if (m->depth == 0) {
    return CTUI_POPMENU_NONE;
  }
  int k = m->depth - 1;
  CTUI_POPMENU_LEVEL *l = &m->level[k];
  switch (kp->type) {
  case CTUI_KEY_UP:
  case CTUI_KEY_DOWN:
    return move(l, kp->type == CTUI_KEY_UP ? -1 : 1) ? CTUI_POPMENU_REDRAW
                                                     : CTUI_POPMENU_NONE;
  case CTUI_KEY_HOME:
  case CTUI_KEY_END: {
    int was = l->sel;
    l->sel = -1;
    move(l, kp->type == CTUI_KEY_HOME ? 1 : -1);
    return l->sel != was ? CTUI_POPMENU_REDRAW : CTUI_POPMENU_NONE;
  }
  case CTUI_KEY_RIGHT:
    return l->sel >= 0 && l->items[l->sel].sub ? activate(m, k, l->sel)
                                               : CTUI_POPMENU_NONE;
  case CTUI_KEY_LEFT:
    return m->depth > 1 ? back(m) : CTUI_POPMENU_NONE;
  case CTUI_KEY_ESC:
    return back(m);
  case CTUI_KEY_ENTER:
    return l->sel >= 0 ? activate(m, k, l->sel) : CTUI_POPMENU_NONE;
  case CTUI_KEY_CHAR:
    if (kp->ch == ' ') {
      return l->sel >= 0 ? activate(m, k, l->sel) : CTUI_POPMENU_NONE;
    }
    return by_letter(m, k, kp->ch);
  default:
    return CTUI_POPMENU_NONE;
  }
}

/* the level (deepest first) and item at a screen cell: the level, or -1;
 * *item -1 on its frame */
static int hit(const CTUI_POPMENU *m, int row, int col, int *item) {
  for (int k = m->depth - 1; k >= 0; k--) {
    const CTUI_POPMENU_LEVEL *l = &m->level[k];
    if (row < l->y || row >= l->y + l->h || col < l->x || col >= l->x + l->w) {
      continue;
    }
    int i = row - l->y - 1;
    *item = col > l->x && col < l->x + l->w - 1 && i >= 0 && i < l->count &&
                    row < l->y + l->h - 1
                ? i
                : -1;
    return k;
  }
  return -1;
}

/* the pointer on item i of level k: it selected, the levels past it closed
 * unless they're its submenu, its submenu opened */
static int hover(CTUI_POPMENU *m, int k, int i) {
  CTUI_POPMENU_LEVEL *l = &m->level[k];
  const CTUI_POPMENU_ITEM *it = &l->items[i];
  int changed = 0;
  if (m->depth > k + 1 && !(l->sel == i && m->level[k + 1].items == it->sub)) {
    m->depth = k + 1;
    changed = 1;
  }
  if (!selectable(it)) {
    return changed ? CTUI_POPMENU_REDRAW : CTUI_POPMENU_NONE;
  }
  if (l->sel != i) {
    l->sel = i;
    changed = 1;
  }
  if (it->sub && m->depth == k + 1) {
    open_sub(m, k, i, 0);
    changed = 1;
  }
  return changed ? CTUI_POPMENU_REDRAW : CTUI_POPMENU_NONE;
}

int ctui_popmenu_mouse(CTUI_POPMENU *m, const CTUI_MOUSE_EVENT_DATA *ms) {
  if (m->depth == 0 || ms->action == CTUI_MOUSE_SCROLL_UP ||
      ms->action == CTUI_MOUSE_SCROLL_DOWN) {
    return CTUI_POPMENU_NONE;
  }
  int i, k = hit(m, ms->row, ms->col, &i);
  if (k < 0) {
    if (ms->action != CTUI_MOUSE_PRESS) {
      return CTUI_POPMENU_NONE;
    }
    m->chosen = NULL;
    m->depth = 0;
    return CTUI_POPMENU_CLOSED;
  }
  if (i < 0) {
    return CTUI_POPMENU_NONE;
  }
  if (ms->action == CTUI_MOUSE_RELEASE) {
    const CTUI_POPMENU_ITEM *it = &m->level[k].items[i];
    return selectable(it) && !it->sub ? choose(m, it) : CTUI_POPMENU_NONE;
  }
  return hover(m, k, i);
}

/* box drawing round the level, for when the style's hook didn't draw it */
static void glyph_frame(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int y, int x,
                        int w, int h, const CTUI_CELL *pen) {
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c += (r == 0 || r == h - 1) ? 1 : w - 1) {
      uint32_t ch = r == 0 || r == h - 1 ? 0x2500 : 0x2502; /* ─ │ */
      if (c == 0 || c == w - 1) {
        ch = r == 0       ? (c ? 0x2510 : 0x250c) /* ┐ ┌ */
             : r == h - 1 ? (c ? 0x2518 : 0x2514) /* ┘ └ */
                          : ch;
      }
      ctui_widget_putc_cell(self, comp, y + r, x + c, ch, pen);
      if (w == 1) {
        break;
      }
    }
  }
}

static void render_item(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp,
                        const CTUI_POPMENU_LEVEL *l, int i, int y, int x,
                        const CTUI_CELL *pen, const CTUI_CELL *dim, int kw,
                        int sub) {
  const CTUI_POPMENU_ITEM *it = &l->items[i];
  int row = y + 1 + i, inner = l->w - 2;
  if (it->flags & CTUI_POPMENU_SEP) {
    for (int c = 0; c < inner; c++) {
      ctui_widget_putc_cell(self, comp, row, x + 1 + c, 0x2500, dim); /* ─ */
    }
    return;
  }
  for (int c = 0; c < inner; c++) {
    ctui_widget_putc_cell(self, comp, row, x + 1 + c, ' ', pen);
  }
  if (it->flags & CTUI_POPMENU_CHECKED) {
    ctui_widget_putc_cell(self, comp, row, x + 2,
                          it->flags & CTUI_POPMENU_RADIO ? 0x2022 : 0x2713,
                          pen); /* • ✓ */
  }
  /* the label, then the keys right-aligned before the arrow's room */
  int col = x + 2 + POPMENU_MARK_COLS;
  int end = x + l->w - 2 - (sub ? 2 : 0);
  int keys_at = kw ? end - kw : end;
  int room = (kw ? keys_at - POPMENU_KEYS_GAP : end) - col;
  char s[256];
  int letter;
  ctui_popmenu_label(it->label, s, sizeof s, &letter);
  if (room > 0) {
    ctui_widget_puts_cut_cell(self, comp, row, col, s, room, pen);
  }
  if (letter >= 0) {
    char pre[256];
    memcpy(pre, s, (size_t)letter);
    pre[letter] = '\0';
    int at = ctui_utf8_width(pre);
    uint32_t cp = 0;
    ctui_utf8_decode(s + letter, &cp);
    if (at + ctui_utf8_cpwidth(cp) <= room) {
      CTUI_CELL u = *pen;
      u.attr |= CTUI_ATTR_UNDERLINE;
      ctui_widget_putc_cell(self, comp, row, col + at, cp, &u);
    }
  }
  if (it->keys && kw && keys_at > col) {
    ctui_widget_puts_cut_cell(self, comp, row, keys_at, it->keys, kw, pen);
  }
  if (it->sub) {
    ctui_widget_putc_cell(self, comp, row, x + l->w - 3, 0x25b8, pen); /* ▸ */
  }
}

void ctui_popmenu_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_POPMENU *m = self->widget_data;
  if (m->depth == 0) {
    return;
  }
  const CTUI_STYLE *st = ctui_style_of(m->style);
  ctui_popmenu_layout(m, comp->rows, comp->cols);
  CTUI_CELL text =
      ctui_style_cell(st, CTUI_STYLE_FG, CTUI_STYLE_BG, st->fg, st->bg);
  CTUI_CELL dim =
      ctui_style_cell(st, CTUI_STYLE_DIM, CTUI_STYLE_BG, st->dim_fg, st->bg);
  CTUI_CELL sel = ctui_style_cell(st, CTUI_STYLE_SEL_FG, CTUI_STYLE_SEL_BG,
                                  st->sel_fg, st->sel_bg);
  for (int k = 0; k < m->depth; k++) {
    const CTUI_POPMENU_LEVEL *l = &m->level[k];
    int y = l->y - self->y, x = l->x - self->x;
    if (l->w < 3 || l->h < 3) {
      continue;
    }
    CTUI_CONTROL frame = {.kind = CTUI_CONTROL_FRAME, .rows = l->h};
    if (ctui_style_control(st, self, comp, y, x, l->w, &frame, st->bg)) {
      for (int r = 0; r < l->h; r++) {
        for (int c = 0; c < l->w; c++) {
          ctui_widget_putc_cell(self, comp, y + r, x + c, ' ', &text);
        }
      }
    } else {
      glyph_frame(self, comp, y, x, l->w, l->h, &dim);
    }
    int kw = keys_width(l), sub = has_sub(l);
    for (int i = 0; i < l->count && i < l->h - 2; i++) {
      const CTUI_CELL *pen = i == l->sel                ? &sel
                             : selectable(&l->items[i]) ? &text
                                                        : &dim;
      render_item(self, comp, l, i, y, x, pen, &dim, kw, sub);
    }
  }
}

static int emit(CTUI_WIDGET *self, int what) {
  CTUI_POPMENU *m = self->widget_data;
  if (what != CTUI_POPMENU_CHOSEN && what != CTUI_POPMENU_CLOSED) {
    return 1;
  }
  const CTUI_POPMENU_ITEM *it = what == CTUI_POPMENU_CHOSEN ? m->chosen : NULL;
  ctui_logf(E_INF, "[CTUI:POPMENU] - %s%s @ tick %d\n",
            it ? "chose " : "cancelled", it && it->label ? it->label : "",
            ctui_tick_advance());
  CTUI_VALUE_CHANGED_EVENT_DATA changed = {.value = it ? it->label : NULL,
                                           .enabled = it ? it->act : -1};
  CTUI_EVENT out = {.type = CTUI_VALUE_CHANGED_EVENT,
                    .scope = CTUI_EVENT_SCOPE_GLOBAL,
                    .ev_source = "popmenu",
                    .event_data = &changed,
                    .origin = self};
  ctui_handle_event(&out);
  return 1;
}

int ctui_popmenu_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_POPMENU *m = self->widget_data;
  if (m->depth == 0) {
    return 0;
  }
  return emit(self, ctui_popmenu_key(m, ev->event_data));
}

int ctui_popmenu_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_POPMENU *m = self->widget_data;
  if (m->depth == 0) {
    return 0;
  }
  return emit(self, ctui_popmenu_mouse(m, ev->event_data));
}
