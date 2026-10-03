#include "entry.h"

#include <string.h>

void ctui_entry_set(CTUI_ENTRY *e, const char *text) {
  size_t old = strlen(e->buf), n = strlen(text);
  if (n >= e->cap) { /* cut before the glyph that doesn't fit */
    n = e->cap - 1;
    while (n > 0 && ((unsigned char)text[n] & 0xc0) == 0x80) {
      n--;
    }
  }
  memmove(e->buf, text, n);
  e->buf[n] = '\0';
  if (old > n) {
    memset(e->buf + n, 0, old - n);
  }
  e->cursor = n;
  e->scroll = 0;
}

void ctui_entry_clear(CTUI_ENTRY *e) {
  memset(e->buf, 0, e->cap);
  e->cursor = 0;
  e->scroll = 0;
}

/* the cursor steps by grapheme cluster: é, a flag or 👍🏽 is one glyph */
static size_t prev_char(const char *s, size_t at) {
  size_t i = 0, start = 0;
  while (i < at && s[i]) {
    start = i;
    i += ctui_utf8_cluster(s + i, (size_t)-1, NULL, NULL);
  }
  return start;
}

static size_t next_char(const char *s, size_t at) {
  return at + ctui_utf8_cluster(s + at, (size_t)-1, NULL, NULL);
}

static int word_sep(char c) { return c == ' ' || c == '/'; }

static size_t prev_word(const char *s, size_t at) {
  while (at > 0 && word_sep(s[at - 1])) {
    at--;
  }
  while (at > 0 && !word_sep(s[at - 1])) {
    at--;
  }
  return at;
}

static size_t next_word(const char *s, size_t at) {
  while (s[at] && word_sep(s[at])) {
    at++;
  }
  while (s[at] && !word_sep(s[at])) {
    at++;
  }
  return at;
}

/* removes buf[from, to), zeroing the bytes freed at the end */
static void cut(CTUI_ENTRY *e, size_t from, size_t to) {
  if (from >= to) {
    return;
  }
  size_t len = strlen(e->buf);
  memmove(e->buf + from, e->buf + to, len - to + 1);
  memset(e->buf + len - (to - from) + 1, 0, to - from);
  if (e->cursor >= to) {
    e->cursor -= to - from;
  } else if (e->cursor > from) {
    e->cursor = from;
  }
}

static int insert(CTUI_ENTRY *e, uint32_t ch) {
  char enc[8];
  int n = ctui_utf8_encode(ch, enc);
  size_t len = strlen(e->buf);
  if (n <= 0 || len + (size_t)n + 1 > e->cap) {
    ctui_logf(E_INF, "[CTUI:ENTRY] - full (%zu of %zu bytes), U+%04X dropped\n",
              len, e->cap, ch);
    return 0;
  }
  memmove(e->buf + e->cursor + n, e->buf + e->cursor, len - e->cursor + 1);
  memcpy(e->buf + e->cursor, enc, (size_t)n);
  e->cursor += (size_t)n;
  return 1;
}

static int move_to(CTUI_ENTRY *e, size_t at) {
  if (at == e->cursor) {
    return 0;
  }
  e->cursor = at;
  return 1;
}

int ctui_entry_key(CTUI_ENTRY *e, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  char *s = e->buf;
  size_t at = e->cursor;
  int ctrl = kp->mods & CTUI_MOD_CTRL;
  switch (kp->type) {
  case CTUI_KEY_LEFT:
    return move_to(e, ctrl ? prev_word(s, at) : prev_char(s, at));
  case CTUI_KEY_RIGHT:
    return move_to(e, ctrl ? next_word(s, at) : next_char(s, at));
  case CTUI_KEY_HOME:
    return move_to(e, 0);
  case CTUI_KEY_END:
    return move_to(e, strlen(s));
  case CTUI_KEY_DELETE:
    if (!s[at]) {
      return 0;
    }
    cut(e, at, next_char(s, at));
    return 1;
  case CTUI_KEY_CHAR:
    break;
  default:
    return 0;
  }
  uint32_t ch = kp->ch;
  if (ch == 0x7f || ch == 0x08) {
    if (!at) {
      return 0;
    }
    cut(e, kp->mods & (CTUI_MOD_CTRL | CTUI_MOD_ALT) ? prev_word(s, at)
                                                    : prev_char(s, at),
        at);
  } else if (ch == 0x17) { /* ctrl+w */
    if (!at) {
      return 0;
    }
    cut(e, prev_word(s, at), at);
  } else if (ch == 0x15) { /* ctrl+u */
    if (!at) {
      return 0;
    }
    cut(e, 0, at);
  } else if (ch == 0x0b) { /* ctrl+k */
    if (!s[at]) {
      return 0;
    }
    cut(e, at, strlen(s));
  } else if (ch == 0x01) { /* ctrl+a */
    return move_to(e, 0);
  } else if (ch == 0x05) { /* ctrl+e */
    return move_to(e, strlen(s));
  } else if (ch >= 0x20 && !(kp->mods & (CTUI_MOD_ALT | CTUI_MOD_CTRL))) {
    return insert(e, ch);
  } else {
    return 0;
  }
  return 1;
}

/* the next glyph (cluster) at s: its bytes, its width as drawn (a dot
 * for a secret) in *w, its cell value in *ch when ch is non-NULL */
static size_t glyph(const CTUI_ENTRY *e, const char *s, int *w, uint32_t *ch) {
  size_t k = ctui_utf8_cluster(s, (size_t)-1, e->secret ? NULL : ch, w);
  if (e->secret) {
    *w = 1;
    if (ch) {
      *ch = 0x2022;
    }
  }
  return k;
}

static int columns(const CTUI_ENTRY *e, size_t bytes) {
  int col = 0;
  for (size_t i = 0; i < bytes && e->buf[i];) {
    int w;
    i += glyph(e, e->buf + i, &w, NULL);
    col += w;
  }
  return col;
}

void ctui_entry_click(CTUI_ENTRY *e, int col) {
  int target = e->scroll + (col > 0 ? col : 0), c = 0;
  size_t i = 0;
  while (e->buf[i]) {
    int w;
    size_t k = glyph(e, e->buf + i, &w, NULL);
    if (c + w > target) {
      break;
    }
    c += w;
    i += k;
  }
  e->cursor = i;
}

void ctui_entry_draw(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                     int col, int width, CTUI_ENTRY *e, int focused) {
  const CTUI_STYLE *st = ctui_style_of(e->style);
  if (width <= 0) {
    return;
  }
  if (e->cursor > strlen(e->buf)) {
    e->cursor = strlen(e->buf);
  }
  int cur = columns(e, e->cursor);
  if (cur < e->scroll) {
    e->scroll = cur;
  } else if (cur - e->scroll > width - 1) {
    e->scroll = cur - width + 1;
  }
  /* drawn as a field under the text, or not: the text is the same (the
   * cursor's cell keeps sel_bg either way) */
  CTUI_CONTROL field = {.kind = CTUI_CONTROL_FIELD, .focused = focused};
  ctui_style_control(st, self, comp, row, col, width, &field, st->bg);
  if (!e->buf[0] && e->placeholder && !focused) {
    ctui_widget_puts_cut(self, comp, row, col, e->placeholder, width,
                         st->dim_fg, st->bg);
    return;
  }
  int c = 0;
  for (size_t i = 0; e->buf[i];) {
    uint32_t ch;
    int w;
    size_t k = glyph(e, e->buf + i, &w, &ch);
    int x = c - e->scroll;
    if (x + w > width) {
      break;
    }
    if (x >= 0 && w > 0) {
      int here = focused && i == e->cursor;
      ctui_widget_putc(self, comp, row, col + x, ch, here ? st->sel_fg : st->fg,
                       here ? st->sel_bg : st->bg);
    }
    c += w;
    i += k;
  }
  if (focused && !e->buf[e->cursor] && cur - e->scroll < width) {
    ctui_widget_putc(self, comp, row, col + cur - e->scroll, ' ', st->sel_fg,
                     st->sel_bg);
  }
}

void ctui_entry_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  if (self->h > 0) {
    ctui_entry_draw(self, comp, 0, 0, self->w, self->widget_data, 1);
  }
}

int ctui_entry_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_ENTRY *e = self->widget_data;
  CTUI_KEYPRESS_EVENT_DATA *kp = ev->event_data;
  if (kp->type != CTUI_KEY_ENTER) {
    return ctui_entry_key(e, kp);
  }
  ctui_logf(E_INF, "[CTUI:ENTRY] - enter @ tick %d, emitting value-changed\n",
            ctui_tick_advance());
  CTUI_VALUE_CHANGED_EVENT_DATA changed = {.value = e->buf};
  CTUI_EVENT out = {.type = CTUI_VALUE_CHANGED_EVENT,
                    .scope = CTUI_EVENT_SCOPE_GLOBAL,
                    .ev_source = "entry",
                    .event_data = &changed,
                    .origin = self};
  ctui_handle_event(&out);
  return 1;
}
