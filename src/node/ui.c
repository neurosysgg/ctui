#define _POSIX_C_SOURCE 200809L

#include "ui_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* nothing due sooner than this at a wait: idle */
#define IDLE_MS 50
#define ROWS_MAX 1000

static const CTUI_NODE_TYPE FRAME = {CTUI_NODE_INT,    NULL, 1, 0, 0,
                                     CTUI_NODE_LINEAR, NULL, 0, 0};
const CTUI_NODE_TYPE ctui_ui_t_flag = {CTUI_NODE_BOOL,   NULL, 1, 0, 0,
                                       CTUI_NODE_LINEAR, NULL, 0, 0};
const CTUI_NODE_TYPE ctui_ui_t_text = {CTUI_NODE_STRING, NULL, 1, 0, 0,
                                       CTUI_NODE_LINEAR, NULL, 0, 0};
static const CTUI_NODE_TYPE INPUT = {
    CTUI_NODE_TRIGGER, NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE FOCUS = {
    CTUI_NODE_BOOL,    NULL, 1, 0, 1, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
/* (ui_internal.h's, shared with ui_widgets.c) */
#define FLAG ctui_ui_t_flag
#define TEXT ctui_ui_t_text

/* --- the screen ---------------------------------------------------------- */

/* row r of the compositor as text into out (CTUI_NODE_TEXT_MAX + 1 bytes),
 * trailing blanks dropped, cut to what a node holds: printable ASCII as
 * is, the rest encoded */
static void row_text(const CTUI_COMPOSITOR *comp, int r, char *out) {
  const CTUI_CELL *row = comp->cells + (size_t)r * (size_t)comp->cols;
  size_t len = 0, keep = 0;
  for (int c = 0; c < comp->cols; c++) {
    uint32_t ch = row[c].ch ? row[c].ch : ' ';
    if (ch >= 0x20 && ch < 0x7f) {
      if (len == CTUI_NODE_TEXT_MAX) {
        break;
      }
      out[len++] = (char)ch;
      if (ch != ' ') {
        keep = len;
      }
      continue;
    }
    if (ch == CTUI_CELL_CONT) {
      continue;
    }
    char enc[CTUI_CLUSTER_MAX * 4];
    int n = ctui_cell_encode(ch, enc);
    if (len + (size_t)n > CTUI_NODE_TEXT_MAX) {
      break;
    }
    memcpy(out + len, enc, (size_t)n);
    len += (size_t)n;
    keep = len;
  }
  out[keep] = '\0';
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
  char text[CTUI_NODE_TEXT_MAX + 1];
  for (int r = 0; r < ui->nrows; r++) {
    row_text(comp, r, text);
    /* most rows are as they were: no check, copy or compare of the value */
    if (ui->rows[r] && strcmp(ctui_node_text(ui->rows[r]), text) != 0) {
      ctui_node_set_str(ui->rows[r], text);
    }
  }
  ctui_node_set_int(ui->frame, (int64_t)ui->app->frames);
}

static void sync_watch(CTUI_UI *ui);

/* the loop is about to wait: whatever was posted is handled and drawn */
static void on_wait(void *ctx, long timeout_ms) {
  CTUI_UI *ui = ctx;
  ctui_nodes_begin(ui->ns);
  update_screen(ui);
  ctui_ui_widgets_sync(ui);
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
  ctui_ui_widgets_answer(ui);
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
int ctui_ui_hold(CTUI_UI *ui, CTUI_NODE *n) {
  if (ui->nlater == LATER_MAX) {
    return EBUSY;
  }
  if (ctui_node_defer(n, &ui->later[ui->nlater]) == 0) {
    ui->nlater++;
  }
  return 0;
}

int ctui_ui_post_keys(const CTUI_KEYPRESS_EVENT_DATA *keys, int count) {
  if (ctui_input_posted() + count > CTUI_INPUT_POST_MAX) {
    return EAGAIN;
  }
  for (int i = 0; i < count; i++) {
    post_key(&keys[i]);
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
  return ctui_ui_hold(ui, n);
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
  return ctui_ui_hold(ui, n);
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
    int err = ctui_ui_widget_cell(ui, w[1], wl[1], dc, dr, &x, &y);
    if (err) {
      return err;
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
  return ctui_ui_hold(ui, n);
}

static int focus_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                       void *user) {
  (void)reply;
  CTUI_UI *ui = user;
  CTUI_FOCUS_EVENT_DATA fd = {.focused = v->b};
  CTUI_EVENT ev = {
      .type = CTUI_FOCUS_EVENT, .ev_source = "input", .event_data = &fd};
  int err = post(&ev);
  return err ? err : ctui_ui_hold(ui, n);
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
  ui->synced_frame = app->frames;
  ui->drawn_frame = (unsigned)-1;
  ctui_widget_drawn_hook(ctui_ui_on_drawn, ui);
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
  ctui_widget_drawn_hook(NULL, NULL);
  if (ui->watch) {
    ctui_io_unwatch(ui->watch);
  }
  if (ui->retry) {
    ctui_timer_cancel(ui->retry);
  }
  ctui_ui_widgets_free(ui);
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
