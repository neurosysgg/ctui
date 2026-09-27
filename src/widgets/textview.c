#include "textview.h"

#include <stdlib.h>
#include <string.h>

#define TAB 8

int ctui_textview_set(CTUI_TEXTVIEW *tv, const char *text, size_t len) {
  ctui_textview_free(tv);
  int n = 0;
  for (size_t i = 0; i < len; i++) {
    n += text[i] == '\n' && i + 1 < len; /* a last '\n' ends, not starts */
  }
  n += len > 0;
  tv->lines = malloc(((size_t)n + 1) * sizeof *tv->lines);
  if (!tv->lines) {
    ctui_logf(E_ERR, "[CTUI:TEXTVIEW] - no memory for %d lines\n", n);
    return -1;
  }
  int k = 0;
  if (len > 0) {
    tv->lines[k++] = 0;
  }
  for (size_t i = 0; i + 1 < len; i++) {
    if (text[i] == '\n') {
      tv->lines[k++] = i + 1;
    }
  }
  tv->lines[k] = len + (len > 0 && text[len - 1] == '\n' ? 0 : 1);
  tv->text = text;
  tv->len = len;
  tv->line_count = n;
  ctui_logf(E_INF, "[CTUI:TEXTVIEW] - %zu bytes, %d lines @ tick %d\n", len,
            n, ctui_tick_advance());
  return 0;
}

void ctui_textview_free(CTUI_TEXTVIEW *tv) {
  free(tv->lines);
  tv->lines = NULL;
  tv->text = NULL;
  tv->len = 0;
  tv->line_count = 0;
  tv->top = tv->top_row = tv->hscroll = 0;
}

/* line i's bytes, without its '\n' (and a '\r' before it) */
static void line_span(const CTUI_TEXTVIEW *tv, int i, const char **s,
                      const char **e) {
  *s = tv->text + tv->lines[i];
  *e = tv->text + tv->lines[i + 1] - 1;
  if (*e > *s && (*e)[-1] == '\r') {
    (*e)--;
  }
}

/* one row of [s, e): its glyphs from column skip on, width columns of
 * them, drawn at row when self is set. Returns where the row stopped: the
 * next wrapped row's start. Takes at least one glyph. */
static const char *row(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int r,
                       const char *s, const char *e, int skip, int width,
                       const CTUI_STYLE *st) {
  int col = 0;
  while (s < e) {
    uint32_t cp = 0;
    int gw;
    /* measuring (no self) needs no cell value, so interns nothing */
    size_t k = ctui_utf8_cluster(s, (size_t)(e - s), self ? &cp : NULL, &gw);
    if (*s == '\t') {
      cp = '\t';
      gw = TAB - col % TAB;
    } else if ((unsigned char)*s < 0x20 || *s == 0x7f) {
      cp = '?';
      gw = 1;
    }
    if (col + gw > skip + width && col > skip) {
      break;
    }
    if (self) {
      for (int c = col; c < col + gw; c++) {
        int x = c - skip;
        if (x < 0 || x >= width) {
          continue;
        }
        int whole = cp != '\t' && c == col && col >= skip &&
                    col + gw <= skip + width;
        if (whole) {
          ctui_widget_putc(self, comp, r, x, cp, st->fg, st->bg);
          break;
        }
        ctui_widget_putc(self, comp, r, x, ' ', st->fg, st->bg);
      }
    }
    col += gw;
    s += k;
  }
  return s;
}

/* one wrapped row of [s, e): sets *end to where its glyphs stop and
 * returns where the next row starts. Breaks after the last blank that
 * fits (the blanks at the break go), else cuts the word. */
static const char *wrap_row(const char *s, const char *e, int width,
                            const char **end) {
  const char *cut = row(NULL, NULL, 0, s, e, 0, width, NULL);
  *end = cut;
  if (cut >= e || *cut == ' ' || *cut == '\t') { /* broke at a blank */
    while (cut < e && (*cut == ' ' || *cut == '\t')) {
      cut++;
    }
    return cut;
  }
  const char *b = cut;
  while (b > s && b[-1] != ' ' && b[-1] != '\t') {
    b--;
  }
  if (b == s) { /* one word wider than the row */
    return cut;
  }
  *end = b;
  return b;
}

/* rows line i takes wrapped at width */
static int line_rows(const CTUI_TEXTVIEW *tv, int i, int width) {
  const char *s, *e, *end;
  line_span(tv, i, &s, &e);
  int n = 1;
  for (s = wrap_row(s, e, width, &end); s < e;
       s = wrap_row(s, e, width, &end)) {
    n++;
  }
  return n;
}

static int wrapped(const CTUI_TEXTVIEW *tv) {
  return tv->wrap && tv->width > 0;
}

/* rows from the top of the view to the end of the text, counting up to
 * limit */
static int rows_below(const CTUI_TEXTVIEW *tv, int limit) {
  if (!wrapped(tv)) {
    return tv->line_count - tv->top;
  }
  int n = -tv->top_row;
  for (int i = tv->top; i < tv->line_count && n < limit; i++) {
    n += line_rows(tv, i, tv->width);
  }
  return n;
}

static int down(CTUI_TEXTVIEW *tv) {
  if (rows_below(tv, tv->page + 1) <= tv->page) {
    return 0;
  }
  if (wrapped(tv) && tv->top_row + 1 < line_rows(tv, tv->top, tv->width)) {
    tv->top_row++;
  } else {
    tv->top++;
    tv->top_row = 0;
  }
  return 1;
}

static int up(CTUI_TEXTVIEW *tv) {
  if (tv->top_row > 0) {
    tv->top_row--;
    return 1;
  }
  if (tv->top == 0) {
    return 0;
  }
  tv->top--;
  tv->top_row = wrapped(tv) ? line_rows(tv, tv->top, tv->width) - 1 : 0;
  return 1;
}

static int scroll(CTUI_TEXTVIEW *tv, int by) {
  int moved = 0;
  for (; by > 0 && down(tv); by--) {
    moved = 1;
  }
  for (; by < 0 && up(tv); by++) {
    moved = 1;
  }
  return moved;
}

/* the last page: the last row at the bottom (walking up from it, not
 * down from wherever the view is) */
static int end(CTUI_TEXTVIEW *tv) {
  int top = tv->top, top_row = tv->top_row;
  if (tv->line_count == 0) {
    return 0;
  }
  if (wrapped(tv)) {
    tv->top = tv->line_count - 1;
    tv->top_row = line_rows(tv, tv->top, tv->width) - 1;
    for (int i = 1; i < tv->page && up(tv); i++) {
    }
  } else {
    tv->top = tv->line_count - (tv->page > 0 ? tv->page : 1);
    tv->top = tv->top > 0 ? tv->top : 0;
    tv->top_row = 0;
  }
  return tv->top != top || tv->top_row != top_row;
}

int ctui_textview_key(CTUI_TEXTVIEW *tv, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  int page = tv->page > 1 ? tv->page - 1 : 1;
  switch (kp->type) {
  case CTUI_KEY_UP:
    return scroll(tv, -1);
  case CTUI_KEY_DOWN:
    return scroll(tv, 1);
  case CTUI_KEY_PGUP:
    return scroll(tv, -page);
  case CTUI_KEY_PGDN:
    return scroll(tv, page);
  case CTUI_KEY_CHAR:
    return kp->ch == ' ' && !kp->mods ? scroll(tv, page) : 0;
  case CTUI_KEY_HOME: {
    int moved = tv->top || tv->top_row || tv->hscroll;
    tv->top = tv->top_row = tv->hscroll = 0;
    return moved;
  }
  case CTUI_KEY_END:
    return end(tv);
  case CTUI_KEY_LEFT:
  case CTUI_KEY_RIGHT:
    if (tv->wrap) {
      return 0;
    }
    if (kp->type == CTUI_KEY_LEFT) {
      int was = tv->hscroll;
      tv->hscroll = tv->hscroll > TAB ? tv->hscroll - TAB : 0;
      return tv->hscroll != was;
    }
    tv->hscroll += TAB;
    return 1;
  default:
    return 0;
  }
}

int ctui_textview_mouse(CTUI_TEXTVIEW *tv, const CTUI_WIDGET *self,
                        const CTUI_MOUSE_EVENT_DATA *m) {
  if (!ctui_widget_contains(self, m->row, m->col)) {
    return 0;
  }
  if (m->action == CTUI_MOUSE_SCROLL_UP) {
    return scroll(tv, -3);
  }
  if (m->action == CTUI_MOUSE_SCROLL_DOWN) {
    return scroll(tv, 3);
  }
  return 0;
}

void ctui_textview_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_TEXTVIEW *tv = self->widget_data;
  const CTUI_STYLE *st = ctui_style_of(tv->style);
  tv->page = self->h;
  tv->width = self->w;
  if (self->w <= 0 || !tv->lines) {
    return;
  }
  if (tv->top >= tv->line_count) {
    tv->top = tv->line_count > 0 ? tv->line_count - 1 : 0;
    tv->top_row = 0;
  }
  if (wrapped(tv)) { /* a resize may have left fewer rows in top */
    int n = line_rows(tv, tv->top, self->w);
    tv->top_row = tv->top_row < n ? tv->top_row : n - 1;
  } else {
    tv->top_row = 0;
  }
  int r = 0;
  for (int i = tv->top; i < tv->line_count && r < self->h; i++) {
    const char *s, *e;
    line_span(tv, i, &s, &e);
    if (!tv->wrap) {
      row(self, comp, r++, s, e, tv->hscroll, self->w, st);
      continue;
    }
    int skip = i == tv->top ? tv->top_row : 0;
    do {
      const char *end, *next = wrap_row(s, e, self->w, &end);
      if (skip) {
        skip--;
      } else {
        row(self, comp, r++, s, end, 0, self->w, st);
      }
      s = next;
    } while (s < e && r < self->h);
  }
}

int ctui_textview_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  return ctui_textview_key(self->widget_data, ev->event_data);
}

int ctui_textview_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  return ctui_textview_mouse(self->widget_data, self, ev->event_data);
}
