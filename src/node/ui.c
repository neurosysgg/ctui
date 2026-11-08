#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* nothing due sooner than this at a wait: idle */
#define IDLE_MS 50
/* writes waiting for their frame at once; more are refused (EBUSY) */
#define LATER_MAX 64
#define ROWS_MAX 1000

static const CTUI_NODE_TYPE FRAME = {CTUI_NODE_INT,    NULL, 1, 0, 0,
                                     CTUI_NODE_LINEAR, NULL, 0, 0};
static const CTUI_NODE_TYPE FLAG = {CTUI_NODE_BOOL,   NULL, 1, 0, 0,
                                    CTUI_NODE_LINEAR, NULL, 0, 0};
static const CTUI_NODE_TYPE TEXT = {CTUI_NODE_STRING, NULL, 1, 0, 0,
                                    CTUI_NODE_LINEAR, NULL, 0, 0};
static const CTUI_NODE_TYPE INPUT = {
    CTUI_NODE_TRIGGER, NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE FOCUS = {
    CTUI_NODE_BOOL,    NULL, 1, 0, 1, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};

typedef enum { W_ENTRY, W_TABLE, W_LIST, W_TABS } W_KIND;

/* a widget published under ui/w/NAME/ */
typedef struct {
  CTUI_UI *ui;
  char *name;
  W_KIND kind;
  CTUI_WIDGET *w;
  void *obj;
  unsigned flags;
  CTUI_UI_CHANGED changed;
  void *user;
  CTUI_NODE *rect, *text, *cursor, *rows, *selected, *selected_text, *top,
      *label;
} UIW;

struct CTUI_UI {
  CTUI_APP *app;
  CTUI_NODES *ns;
  int own;          /* ns is ours (ctui_ui_auto()) */
  CTUI_WIDGET self; /* what the watch and the timer hand back */
  CTUI_IO_WATCH *watch;
  int watched_fd;
  unsigned watched_events;
  CTUI_TIMER *retry;

  CTUI_NODE *frame, *idle, *busy_n, *size, *key, *text, *mouse, *focus;
  int (*busy)(void *user);
  void *busy_user;
  CTUI_NODE **rows;
  int nrows;
  unsigned frames_seen;
  int first; /* nothing published from the screen yet */

  CTUI_NODE_LATER later[LATER_MAX];
  int nlater;

  UIW **ws;
  int nws;
  CTUI_TIMER *frame_soon; /* a widget written: a frame asked for */
};

/* --- the screen ---------------------------------------------------------- */

/* row r of the compositor as text, trailing blanks dropped, cut to what a
 * node holds */
static void row_text(const CTUI_COMPOSITOR *comp, int r, CTUI_BUF *b) {
  const CTUI_CELL *row = comp->cells + (size_t)r * (size_t)comp->cols;
  size_t keep = 0;
  for (int c = 0; c < comp->cols; c++) {
    if (row[c].ch == CTUI_CELL_CONT) {
      continue;
    }
    char enc[CTUI_CLUSTER_MAX * 4];
    int n = ctui_cell_encode(row[c].ch ? row[c].ch : ' ', enc);
    if (b->len + (size_t)n > CTUI_NODE_TEXT_MAX) {
      break;
    }
    ctui_buf_add(b, enc, (size_t)n);
    if (!(n == 1 && enc[0] == ' ')) {
      keep = b->len;
    }
  }
  b->len = keep;
  if (b->s) {
    b->s[keep] = '\0';
  }
}

static void publish_rows(CTUI_UI *ui, int rows) {
  if (rows > ROWS_MAX) {
    rows = ROWS_MAX;
  }
  if (rows == ui->nrows) {
    return;
  }
  for (int r = rows; r < ui->nrows; r++) {
    ctui_node_remove(ui->rows[r]);
  }
  if (rows > ui->nrows) {
    CTUI_NODE **a = realloc(ui->rows, (size_t)rows * sizeof *a);
    if (!a) {
      return;
    }
    ui->rows = a;
    for (int r = ui->nrows; r < rows; r++) {
      ui->rows[r] =
          ctui_node_publishf(ui->ns, &TEXT, NULL, NULL, "ui/screen/%03d", r);
    }
  }
  ui->nrows = rows;
}

static void update_screen(CTUI_UI *ui) {
  const CTUI_COMPOSITOR *comp = ui->app->comp;
  if (!comp || (!ui->first && ui->app->frames == ui->frames_seen)) {
    return;
  }
  ui->first = 0;
  ui->frames_seen = ui->app->frames;
  char size[32];
  snprintf(size, sizeof size, "%dx%d", comp->rows, comp->cols);
  publish_rows(ui, comp->rows);
  ctui_node_set_str(ui->size, size);
  for (int r = 0; r < ui->nrows; r++) {
    CTUI_BUF b = {0};
    row_text(comp, r, &b);
    if (ui->rows[r]) {
      ctui_node_set_str(ui->rows[r], b.s ? b.s : "");
    }
    free(b.s);
  }
  ctui_node_set_int(ui->frame, (int64_t)ui->app->frames);
}

static void sync_watch(CTUI_UI *ui);
static void sync_widgets(CTUI_UI *ui);

/* the loop is about to wait: whatever was posted is handled and drawn */
static void on_wait(void *ctx, long timeout_ms) {
  CTUI_UI *ui = ctx;
  ctui_nodes_begin(ui->ns);
  update_screen(ui);
  sync_widgets(ui);
  int busy = ui->busy && ui->busy(ui->busy_user);
  ctui_node_set_bool(ui->busy_n, busy);
  ctui_node_set_bool(ui->idle,
                     !busy && (timeout_ms < 0 || timeout_ms > IDLE_MS));
  ctui_nodes_commit(ui->ns);
  /* a widget written waits for the frame it asked for */
  if (ui->nlater && !ui->frame_soon) {
    char reply[24];
    snprintf(reply, sizeof reply, "%u", ui->app->frames);
    for (int i = 0; i < ui->nlater; i++) {
      ctui_nodes_answer(ui->ns, &ui->later[i], 0, reply);
    }
    ui->nlater = 0;
  }
  if (ui->own) {
    sync_watch(ui);
  }
}

/* --- input --------------------------------------------------------------- */

static int post(const CTUI_EVENT *ev) {
  return ctui_input_post(ev) == 0 ? 0 : EAGAIN;
}

static int post_key(const CTUI_KEYPRESS_EVENT_DATA *k) {
  CTUI_KEYPRESS_EVENT_DATA copy = *k;
  CTUI_EVENT ev = {
      .type = CTUI_KEYPRESS_EVENT, .ev_source = "input", .event_data = &copy};
  return post(&ev);
}

/* the answer waits for the frame; out of room, it's refused */
static int hold(CTUI_UI *ui, CTUI_NODE *n) {
  if (ui->nlater == LATER_MAX) {
    return EBUSY;
  }
  if (ctui_node_defer(n, &ui->later[ui->nlater]) == 0) {
    ui->nlater++;
  }
  return 0;
}

/* the words of s (len bytes) split on spaces, at most max: how many, or -1
 * for more */
static int words(const char *s, size_t len, const char **w, size_t *wl,
                 int max) {
  int n = 0;
  size_t i = 0;
  while (i < len) {
    while (i < len && s[i] == ' ') {
      i++;
    }
    if (i == len) {
      break;
    }
    size_t start = i;
    while (i < len && s[i] != ' ') {
      i++;
    }
    if (n == max) {
      return -1;
    }
    w[n] = s + start;
    wl[n] = i - start;
    n++;
  }
  return n;
}

static int key_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                     void *user) {
  (void)reply;
  CTUI_UI *ui = user;
  const char *w[CTUI_INPUT_POST_MAX];
  size_t wl[CTUI_INPUT_POST_MAX];
  int count = words(v->s, v->len, w, wl, CTUI_INPUT_POST_MAX);
  if (count <= 0) {
    return count < 0 ? EAGAIN : EINVAL;
  }
  CTUI_KEYPRESS_EVENT_DATA keys[CTUI_INPUT_POST_MAX];
  for (int i = 0; i < count; i++) {
    if (ctui_key_parse(w[i], wl[i], &keys[i]) != 0) {
      return EINVAL;
    }
  }
  if (ctui_input_posted() + count > CTUI_INPUT_POST_MAX) {
    return EAGAIN;
  }
  for (int i = 0; i < count; i++) {
    post_key(&keys[i]);
  }
  ctui_logf(E_INF, "[CTUI:UI] - %d key(s) posted @ tick %d\n", count,
            ctui_tick_advance());
  return hold(ui, n);
}

static int text_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                      void *user) {
  (void)reply;
  CTUI_UI *ui = user;
  CTUI_KEYPRESS_EVENT_DATA keys[CTUI_INPUT_POST_MAX];
  int count = 0;
  for (size_t i = 0; i < v->len;) {
    uint32_t cp;
    int used = ctui_utf8_decode(v->s + i, &cp);
    i += (size_t)used;
    if (count == CTUI_INPUT_POST_MAX) {
      return EAGAIN;
    }
    CTUI_KEYPRESS_EVENT_DATA k = {.type = CTUI_KEY_CHAR, .ch = cp};
    if (cp == '\n') {
      k = (CTUI_KEYPRESS_EVENT_DATA){.type = CTUI_KEY_ENTER};
    } else if (cp == '\t') {
      k = (CTUI_KEYPRESS_EVENT_DATA){.type = CTUI_KEY_TAB};
    } else if (cp < 0x20 || cp == 0x7f) {
      return EINVAL;
    }
    keys[count++] = k;
  }
  if (count == 0) {
    return EINVAL;
  }
  if (ctui_input_posted() + count > CTUI_INPUT_POST_MAX) {
    return EAGAIN;
  }
  for (int i = 0; i < count; i++) {
    post_key(&keys[i]);
  }
  ctui_logf(E_INF, "[CTUI:UI] - %d character(s) posted @ tick %d\n", count,
            ctui_tick_advance());
  return hold(ui, n);
}

/* a number word: 0 with *out, -1 */
static int number(const char *s, size_t len, int *out) {
  if (len == 0 || len > 6) {
    return -1;
  }
  int v = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return -1;
    }
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return 0;
}

/* "[MODS+]VERB": the verb's word and the mods before it */
static int verb_mods(const char *s, size_t len, const char **verb, size_t *vlen,
                     unsigned *mods) {
  *mods = 0;
  const char *plus;
  while ((plus = memchr(s, '+', len))) {
    size_t m = (size_t)(plus - s);
    if (m == 4 && strncasecmp(s, "ctrl", 4) == 0) {
      *mods |= CTUI_MOD_CTRL;
    } else if (m == 3 && strncasecmp(s, "alt", 3) == 0) {
      *mods |= CTUI_MOD_ALT;
    } else if (m == 5 && strncasecmp(s, "shift", 5) == 0) {
      *mods |= CTUI_MOD_SHIFT;
    } else {
      return -1;
    }
    s += m + 1;
    len -= m + 1;
  }
  *verb = s;
  *vlen = len;
  return 0;
}

static int is_word(const char *w, size_t wl, const char *lit) {
  return strlen(lit) == wl && memcmp(w, lit, wl) == 0;
}

static int widget_cell(CTUI_UI *ui, const char *path, size_t len, int dc,
                       int dr, int *x, int *y);

static int mouse_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                       void *user) {
  (void)reply;
  CTUI_UI *ui = user;
  const char *w[7];
  size_t wl[7];
  int count = words(v->s, v->len, w, wl, 7);
  const char *verb;
  size_t vlen;
  unsigned mods;
  if (count < 2 || verb_mods(w[0], wl[0], &verb, &vlen, &mods) != 0) {
    return EINVAL;
  }
  /* "VERB w/NAME [C R]": the widget's cells for COL ROW */
  char at[2][8];
  if (count >= 2 && memchr(w[1], '/', wl[1])) {
    int x, y;
    int extra = count - 2;
    if (extra != 0 && extra != 2) {
      return EINVAL;
    }
    int dc = -1, dr = -1;
    if (extra == 2 && (number(w[2], wl[2], &dc) || number(w[3], wl[3], &dr))) {
      return EINVAL;
    }
    if (widget_cell(ui, w[1], wl[1], dc, dr, &x, &y) != 0) {
      return ENOENT;
    }
    snprintf(at[0], sizeof at[0], "%d", x);
    snprintf(at[1], sizeof at[1], "%d", y);
    w[1] = at[0], wl[1] = strlen(at[0]);
    w[2] = at[1], wl[2] = strlen(at[1]);
    count = 3;
  }
  if (count < 3) {
    return EINVAL;
  }
  CTUI_MOUSE_EVENT_DATA md[3];
  int nev = 0;
  int c1, r1, c2 = 0, r2 = 0, button = 0;
  CTUI_MOUSE_EVENT_DATA base = {.px = -1, .py = -1, .mods = mods};
  if (is_word(verb, vlen, "scroll")) {
    if (count != 4 || number(w[2], wl[2], &c1) || number(w[3], wl[3], &r1)) {
      return EINVAL;
    }
    base.button = -1;
    base.col = c1;
    base.row = r1;
    if (is_word(w[1], wl[1], "up")) {
      base.action = CTUI_MOUSE_SCROLL_UP;
    } else if (is_word(w[1], wl[1], "down")) {
      base.action = CTUI_MOUSE_SCROLL_DOWN;
    } else {
      return EINVAL;
    }
    md[nev++] = base;
  } else if (is_word(verb, vlen, "drag")) {
    if ((count != 5 && count != 6) || number(w[1], wl[1], &c1) ||
        number(w[2], wl[2], &r1) || number(w[3], wl[3], &c2) ||
        number(w[4], wl[4], &r2) ||
        (count == 6 && (number(w[5], wl[5], &button) || button > 6))) {
      return EINVAL;
    }
    base.button = button;
    md[0] = md[1] = base;
    md[0].action = CTUI_MOUSE_PRESS;
    md[0].col = c1;
    md[0].row = r1;
    md[1].action = CTUI_MOUSE_MOTION;
    md[1].col = c2;
    md[1].row = r2;
    md[2] = md[1];
    md[2].action = CTUI_MOUSE_RELEASE;
    nev = 3;
  } else {
    if ((count != 3 && count != 4) || number(w[1], wl[1], &c1) ||
        number(w[2], wl[2], &r1) ||
        (count == 4 && (number(w[3], wl[3], &button) || button > 6))) {
      return EINVAL;
    }
    base.button = button;
    base.col = c1;
    base.row = r1;
    md[0] = md[1] = base;
    nev = 1;
    if (is_word(verb, vlen, "click")) {
      md[0].action = CTUI_MOUSE_PRESS;
      md[1].action = CTUI_MOUSE_RELEASE;
      nev = 2;
    } else if (is_word(verb, vlen, "press")) {
      md[0].action = CTUI_MOUSE_PRESS;
    } else if (is_word(verb, vlen, "release")) {
      md[0].action = CTUI_MOUSE_RELEASE;
    } else if (is_word(verb, vlen, "move")) {
      md[0].action = CTUI_MOUSE_MOTION;
      if (count == 3) {
        md[0].button = -1; /* nothing held */
      }
    } else {
      return EINVAL;
    }
  }
  if (ctui_input_posted() + nev > CTUI_INPUT_POST_MAX) {
    return EAGAIN;
  }
  for (int i = 0; i < nev; i++) {
    CTUI_EVENT ev = {
        .type = CTUI_MOUSE_EVENT, .ev_source = "input", .event_data = &md[i]};
    post(&ev);
  }
  ctui_logf(E_INF, "[CTUI:UI] - %d mouse event(s) posted @ tick %d\n", nev,
            ctui_tick_advance());
  return hold(ui, n);
}

static int focus_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                       void *user) {
  (void)reply;
  CTUI_UI *ui = user;
  CTUI_FOCUS_EVENT_DATA fd = {.focused = v->b};
  CTUI_EVENT ev = {
      .type = CTUI_FOCUS_EVENT, .ev_source = "input", .event_data = &fd};
  int err = post(&ev);
  return err ? err : hold(ui, n);
}

/* --- widgets ------------------------------------------------------------ */

static const CTUI_NODE_TYPE COUNT = {CTUI_NODE_INT,    NULL, 1, 0, 0,
                                     CTUI_NODE_LINEAR, NULL, 0, 0};
static const CTUI_NODE_TYPE ROW_RW = {
    CTUI_NODE_INT,     NULL, 1, 0, -1, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE TEXT_RW = {
    CTUI_NODE_STRING,  NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};

/* w/NAME (len bytes) 's cell dc, dr into it (-1: its middle) */
static int widget_cell(CTUI_UI *ui, const char *path, size_t len, int dc,
                       int dr, int *x, int *y) {
  if (len < 3 || memcmp(path, "w/", 2) != 0) {
    return -1;
  }
  for (int i = 0; i < ui->nws; i++) {
    const UIW *u = ui->ws[i];
    if (strlen(u->name) == len - 2 && memcmp(u->name, path + 2, len - 2) == 0) {
      const CTUI_WIDGET *w = u->w;
      if ((dc >= 0 && dc >= w->w) || (dr >= 0 && dr >= w->h)) {
        return -1;
      }
      *x = w->x + (dc >= 0 ? dc : w->w / 2);
      *y = w->y + (dr >= 0 ? dr : w->h / 2);
      return 0;
    }
  }
  return -1;
}

/* a widget's row cursor moved from outside: kept in view where the widget
 * doesn't do that itself */
static void list_select(CTUI_LIST *l, int row, int h) {
  l->selected = row;
  if (row < l->scroll_offset) {
    l->scroll_offset = row;
  } else if (h > 0 && row >= l->scroll_offset + h) {
    l->scroll_offset = row - h + 1;
  }
}

static int frame_soon(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  CTUI_UI *ui = self->widget_data;
  ctui_timer_cancel(ui->frame_soon);
  ui->frame_soon = NULL;
  return 1; /* drawn after this tick */
}

static int widget_rows(const UIW *u) {
  switch (u->kind) {
  case W_TABLE:
    return ((const CTUI_TABLE *)u->obj)->count;
  case W_LIST:
    return ((const CTUI_LIST *)u->obj)->count;
  case W_TABS:
    return ((const CTUI_TABS *)u->obj)->count;
  default:
    return 0;
  }
}

static int widget_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                        void *user) {
  (void)reply;
  UIW *u = user;
  CTUI_UI *ui = u->ui;
  if (n == u->text) {
    CTUI_ENTRY *e = u->obj;
    if (v->len >= e->cap) {
      return E2BIG;
    }
    ctui_entry_set(e, v->s);
  } else {
    if (v->i < -1 || v->i >= widget_rows(u) ||
        (v->i < 0 && u->kind == W_TABS)) {
      return ERANGE;
    }
    int row = (int)v->i;
    if (u->kind == W_TABLE) {
      ctui_table_select(u->obj, row);
    } else if (u->kind == W_LIST) {
      list_select(u->obj, row, u->w->h);
    } else {
      ctui_tabs_select(u->obj, row);
    }
  }
  int err = u->changed(u->w, u->user);
  if (err) {
    return err;
  }
  if (!ui->frame_soon) {
    ui->frame_soon = ctui_timer_register(1, &ui->self, frame_soon);
  }
  ctui_logf(E_INF, "[CTUI:UI] - w/%s written @ tick %d\n", u->name,
            ctui_tick_advance());
  return hold(ui, n);
}

static const CTUI_NODE_OPS WIDGET_OPS = {widget_write, NULL};

static void sync_one(UIW *u) {
  char buf[64];
  const CTUI_WIDGET *w = u->w;
  snprintf(buf, sizeof buf, "%d %d %d %d", w->x, w->y, w->w, w->h);
  ctui_node_set_str(u->rect, buf);
  switch (u->kind) {
  case W_ENTRY: {
    const CTUI_ENTRY *e = u->obj;
    if (ctui_node_string_ok(ctui_node_type(u->text), e->buf, strlen(e->buf))) {
      ctui_node_set_str(u->text, e->buf);
    }
    ctui_node_set_int(u->cursor, (int64_t)e->cursor);
    break;
  }
  case W_TABLE: {
    const CTUI_TABLE *t = u->obj;
    int sel = t->count > 0 ? t->selected : -1;
    ctui_node_set_int(u->rows, t->count);
    ctui_node_set_int(u->selected, sel);
    ctui_node_set_int(u->top, t->scroll);
    char scratch[512];
    const char *txt = sel >= 0 && t->cell
                          ? t->cell(t->ctx, sel, 0, scratch, sizeof scratch)
                          : "";
    if (txt && ctui_node_string_ok(ctui_node_type(u->selected_text), txt,
                                   strlen(txt))) {
      ctui_node_set_str(u->selected_text, txt);
    }
    break;
  }
  case W_LIST: {
    const CTUI_LIST *l = u->obj;
    int sel = l->count > 0 ? l->selected : -1;
    ctui_node_set_int(u->rows, l->count);
    ctui_node_set_int(u->selected, sel);
    ctui_node_set_int(u->top, l->scroll_offset);
    const char *txt =
        sel >= 0 && l->items[sel].label ? l->items[sel].label : "";
    if (ctui_node_string_ok(ctui_node_type(u->selected_text), txt,
                            strlen(txt))) {
      ctui_node_set_str(u->selected_text, txt);
    }
    break;
  }
  case W_TABS: {
    const CTUI_TABS *t = u->obj;
    ctui_node_set_int(u->rows, t->count);
    ctui_node_set_int(u->selected, t->selected);
    const char *lab = t->selected >= 0 && t->selected < t->count &&
                              t->items[t->selected].label
                          ? t->items[t->selected].label
                          : "";
    if (ctui_node_string_ok(ctui_node_type(u->label), lab, strlen(lab))) {
      ctui_node_set_str(u->label, lab);
    }
    break;
  }
  }
}

static void sync_widgets(CTUI_UI *ui) {
  for (int i = 0; i < ui->nws; i++) {
    sync_one(ui->ws[i]);
  }
}

static CTUI_NODE *wnode(UIW *u, const char *leaf, const CTUI_NODE_TYPE *ro,
                        const CTUI_NODE_TYPE *rw) {
  int writable = rw && (u->flags & CTUI_UI_RDWR);
  return ctui_node_publishf(u->ui->ns, writable ? rw : ro,
                            writable ? &WIDGET_OPS : NULL, u, "ui/w/%s/%s",
                            u->name, leaf);
}

static int add_widget(CTUI_UI *ui, const char *name, CTUI_WIDGET *w, void *obj,
                      W_KIND kind, unsigned flags, CTUI_UI_CHANGED changed,
                      void *user) {
  if (!ui) {
    return 0;
  }
  char path[CTUI_NODE_PATH_MAX + 1];
  if ((flags & CTUI_UI_RDWR) && !changed) {
    ctui_logf(E_WRN, "[CTUI:UI] - w/%s: writable without changed() refused\n",
              name);
    return -1;
  }
  if ((size_t)snprintf(path, sizeof path, "ui/w/%s/selected_text", name) >=
          sizeof path ||
      !ctui_node_path_ok(path)) {
    ctui_logf(E_WRN, "[CTUI:UI] - w/%s: a bad name refused\n", name);
    return -1;
  }
  for (int i = 0; i < ui->nws; i++) {
    if (strcmp(ui->ws[i]->name, name) == 0) {
      ctui_logf(E_WRN, "[CTUI:UI] - w/%s: published already\n", name);
      return -1;
    }
  }
  UIW **a = realloc(ui->ws, (size_t)(ui->nws + 1) * sizeof *a);
  UIW *u = calloc(1, sizeof *u);
  if (a) {
    ui->ws = a;
  }
  if (!a || !u || !(u->name = strdup(name))) {
    free(u);
    return -1;
  }
  u->ui = ui;
  u->kind = kind;
  u->w = w;
  u->obj = obj;
  u->flags = flags;
  u->changed = changed;
  u->user = user;
  ctui_nodes_begin(ui->ns);
  u->rect = wnode(u, "rect", &TEXT, NULL);
  if (kind == W_ENTRY) {
    u->text = wnode(u, "text", &TEXT, &TEXT_RW);
    u->cursor = wnode(u, "cursor", &COUNT, NULL);
  } else {
    u->rows = wnode(u, "rows", &COUNT, NULL);
    u->selected = wnode(u, "selected", &COUNT, &ROW_RW);
    if (kind == W_TABS) {
      u->label = wnode(u, "label", &TEXT, NULL);
    } else {
      u->selected_text = wnode(u, "selected_text", &TEXT, NULL);
      u->top = wnode(u, "top", &COUNT, NULL);
    }
  }
  sync_one(u);
  ctui_nodes_commit(ui->ns);
  ui->ws[ui->nws++] = u;
  ctui_logf(E_INF, "[CTUI:UI] - w/%s published%s @ tick %d\n", name,
            flags & CTUI_UI_RDWR ? " (writable)" : "", ctui_tick_advance());
  return 0;
}

int ctui_ui_entry(CTUI_UI *ui, const char *name, CTUI_WIDGET *w, CTUI_ENTRY *e,
                  unsigned flags, CTUI_UI_CHANGED changed, void *user) {
  return add_widget(ui, name, w, e, W_ENTRY, flags, changed, user);
}

int ctui_ui_table(CTUI_UI *ui, const char *name, CTUI_WIDGET *w, CTUI_TABLE *t,
                  unsigned flags, CTUI_UI_CHANGED changed, void *user) {
  return add_widget(ui, name, w, t, W_TABLE, flags, changed, user);
}

int ctui_ui_list(CTUI_UI *ui, const char *name, CTUI_WIDGET *w, CTUI_LIST *l,
                 unsigned flags, CTUI_UI_CHANGED changed, void *user) {
  return add_widget(ui, name, w, l, W_LIST, flags, changed, user);
}

int ctui_ui_tabs(CTUI_UI *ui, const char *name, CTUI_WIDGET *w, CTUI_TABS *t,
                 unsigned flags, CTUI_UI_CHANGED changed, void *user) {
  return add_widget(ui, name, w, t, W_TABS, flags, changed, user);
}

void ctui_ui_forget(CTUI_UI *ui, const char *name) {
  if (!ui) {
    return;
  }
  for (int i = 0; i < ui->nws; i++) {
    UIW *u = ui->ws[i];
    if (strcmp(u->name, name) == 0) {
      char prefix[CTUI_NODE_PATH_MAX + 1];
      snprintf(prefix, sizeof prefix, "ui/w/%s", name);
      ctui_nodes_remove_under(ui->ns, prefix);
      free(u->name);
      free(u);
      ui->ws[i] = ui->ws[--ui->nws];
      return;
    }
  }
}

static const CTUI_NODE_OPS KEY_OPS = {key_write, NULL};
static const CTUI_NODE_OPS TEXT_OPS = {text_write, NULL};
static const CTUI_NODE_OPS MOUSE_OPS = {mouse_write, NULL};
static const CTUI_NODE_OPS FOCUS_OPS = {focus_write, NULL};

/* --- the tree ------------------------------------------------------------ */

static void ui_noop_render(CTUI_WIDGET *w, CTUI_COMPOSITOR *comp) {
  (void)w, (void)comp;
}

static CTUI_UI *ui_new(CTUI_NODES *ns, CTUI_APP *app) {
  if (ctui_nodes_find(ns, "ui/frame")) {
    ctui_log(E_WRN, "[CTUI:UI] - ui/ is published already\n");
    return NULL;
  }
  CTUI_UI *ui = calloc(1, sizeof *ui);
  if (!ui) {
    return NULL;
  }
  ui->app = app;
  ui->ns = ns;
  ui->first = 1;
  ui->watched_fd = -1;
  ui->self = ctui_widget_make(0, 0, 0, 0, ui, ui_noop_render, NULL);
  ctui_nodes_begin(ns);
  ui->frame = ctui_node_publish(ns, "ui/frame", &FRAME, NULL, NULL);
  ui->idle = ctui_node_publish(ns, "ui/idle", &FLAG, NULL, NULL);
  ui->busy_n = ctui_node_publish(ns, "ui/busy", &FLAG, NULL, NULL);
  ui->size = ctui_node_publish(ns, "ui/size", &TEXT, NULL, NULL);
  ui->key = ctui_node_publish(ns, "ui/input/key", &INPUT, &KEY_OPS, ui);
  ui->text = ctui_node_publish(ns, "ui/input/text", &INPUT, &TEXT_OPS, ui);
  ui->mouse = ctui_node_publish(ns, "ui/input/mouse", &INPUT, &MOUSE_OPS, ui);
  ui->focus = ctui_node_publish(ns, "ui/input/focus", &FOCUS, &FOCUS_OPS, ui);
  ctui_nodes_commit(ns);
  ctui_input_on_wait(on_wait, ui);
  ctui_logf(E_INF, "[CTUI:UI] - ui/ published @ tick %d\n",
            ctui_tick_advance());
  return ui;
}

CTUI_UI *ctui_ui_open(CTUI_NODES *ns, CTUI_APP *app) { return ui_new(ns, app); }

/* --- nodes of its own (ctui_ui_auto()) ----------------------------------- */

static int on_socket(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  CTUI_UI *ui = self->widget_data;
  ctui_nodes_dispatch(ui->ns);
  sync_watch(ui);
  return 0;
}

/* the watch follows the socket: a new one after a reconnect, writes
 * watched while output waits */
static void sync_watch(CTUI_UI *ui) {
  int fd = ctui_nodes_fd(ui->ns);
  unsigned events =
      CTUI_IO_READ | (ctui_nodes_pending(ui->ns) ? CTUI_IO_WRITE : 0u);
  if (fd != ui->watched_fd) {
    if (ui->watch) {
      ctui_io_unwatch(ui->watch);
      ui->watch = NULL;
    }
    ui->watched_fd = fd;
    if (fd >= 0) {
      ui->watch = ctui_io_watch(fd, events, &ui->self, on_socket);
      ui->watched_events = events;
    }
  } else if (ui->watch && events != ui->watched_events) {
    ctui_io_set_events(ui->watch, events);
    ui->watched_events = events;
  }
}

static int on_retry(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  CTUI_UI *ui = self->widget_data;
  if (ctui_nodes_fd(ui->ns) < 0 && ctui_nodes_connect(ui->ns) == 0) {
    /* published whole on connect: the screen again too */
    ui->first = 1;
  }
  sync_watch(ui);
  return 0;
}

CTUI_UI *ctui_ui_auto(CTUI_APP *app, const char *name) {
  const char *on = getenv("CTUI_UI_NODES");
  if (!on || !*on || strcmp(on, "0") == 0) {
    return NULL;
  }
  CTUI_NODES *ns = ctui_nodes_open(name, 0);
  if (!ns) {
    return NULL;
  }
  CTUI_UI *ui = ui_new(ns, app);
  if (!ui) {
    ctui_nodes_close(ns);
    return NULL;
  }
  ui->own = 1;
  sync_watch(ui);
  ui->retry = ctui_timer_register(1000, &ui->self, on_retry);
  return ui;
}

void ctui_ui_close(CTUI_UI *ui) {
  if (!ui) {
    return;
  }
  ctui_input_on_wait(NULL, NULL);
  if (ui->watch) {
    ctui_io_unwatch(ui->watch);
  }
  if (ui->retry) {
    ctui_timer_cancel(ui->retry);
  }
  if (ui->frame_soon) {
    ctui_timer_cancel(ui->frame_soon);
  }
  for (int i = 0; i < ui->nws; i++) {
    free(ui->ws[i]->name);
    free(ui->ws[i]);
  }
  free(ui->ws);
  /* answered, so no writer waits out the daemon's timeout */
  for (int i = 0; i < ui->nlater; i++) {
    ctui_nodes_answer(ui->ns, &ui->later[i], ECANCELED, NULL);
  }
  if (ui->own) {
    ctui_nodes_close(ui->ns);
  } else {
    ctui_nodes_remove_under(ui->ns, "ui");
  }
  free(ui->rows);
  free(ui);
}

CTUI_NODES *ctui_ui_nodes(const CTUI_UI *ui) { return ui->ns; }

void ctui_ui_busy(CTUI_UI *ui, int (*busy)(void *user), void *user) {
  if (ui) {
    ui->busy = busy;
    ui->busy_user = user;
  }
}
