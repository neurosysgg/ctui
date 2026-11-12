/* core/keyname.h's key names, core/input.h's posted events and wait hook,
 * node/node.h's deferred answers, and node/ui.h's tree driving a small
 * app from a fake daemon inside the app's own run loop: keys, text and
 * clicks posted, each write answered after the frame it made (the rows'
 * new text sent before the answer), refusals answered at once, ESC quitting
 * as the terminal's would, the answer still waiting then cancelled. */
#define _GNU_SOURCE

#include "node/ui.h"
#include "widgets/dialog.h"
#include "widgets/entry.h"
#include "widgets/form.h"
#include "widgets/popmenu.h"
#include "widgets/table.h"
#include "widgets/tabs.h"

#include "ctui.h"
#include "ctui_test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* --- key names ----------------------------------------------------------- */

static int key_is(const char *text, CTUI_KEYTYPE type, uint32_t ch,
                  unsigned mods) {
  CTUI_KEYPRESS_EVENT_DATA k;
  if (ctui_key_parse(text, strlen(text), &k) != 0) {
    printf("    \"%s\" refused\n", text);
    return 0;
  }
  int ok =
      k.type == type && k.ch == ch && k.mods == mods && !k.repeat && !k.base;
  if (!ok) {
    printf("    \"%s\": type %d ch 0x%x mods 0x%x\n", text, k.type, k.ch,
           k.mods);
  }
  return ok;
}

static int key_refused(const char *text) {
  CTUI_KEYPRESS_EVENT_DATA k;
  return ctui_key_parse(text, strlen(text), &k) == -1;
}

static void test_keynames(void) {
  CTUI_TEST_ASSERT(key_is("down", CTUI_KEY_DOWN, 0, 0) &&
                       key_is("PgDn", CTUI_KEY_PGDN, 0, 0) &&
                       key_is("ESC", CTUI_KEY_ESC, 0, 0) &&
                       key_is("f1", CTUI_KEY_F1, 0, 0) &&
                       key_is("F12", CTUI_KEY_F12, 0, 0),
                   "names, in any case; F1-F12");
  CTUI_TEST_ASSERT(key_is("a", CTUI_KEY_CHAR, 'a', 0) &&
                       key_is("A", CTUI_KEY_CHAR, 'A', 0) &&
                       key_is("é", CTUI_KEY_CHAR, 0xe9, 0) &&
                       key_is("+", CTUI_KEY_CHAR, '+', 0) &&
                       key_is("space", CTUI_KEY_CHAR, ' ', 0) &&
                       key_is("backspace", CTUI_KEY_CHAR, 0x7f, 0),
                   "characters as written; space, backspace");
  CTUI_TEST_ASSERT(
      key_is("ctrl+f", CTUI_KEY_CHAR, 0x06, 0) &&
          key_is("ctrl+F", CTUI_KEY_CHAR, 0x06, 0) &&
          key_is("Ctrl+Alt+x", CTUI_KEY_CHAR, 0x18, CTUI_MOD_ALT) &&
          key_is("shift+tab", CTUI_KEY_BACKTAB, 0, 0) &&
          key_is("ctrl+up", CTUI_KEY_UP, 0, CTUI_MOD_CTRL) &&
          key_is("alt+enter", CTUI_KEY_ENTER, 0, CTUI_MOD_ALT) &&
          key_is("ctrl++", CTUI_KEY_CHAR, '+', CTUI_MOD_CTRL),
      "modifiers as the terminal sends them (ctrl+letter its "
      "control byte, shift+tab BACKTAB)");
  CTUI_TEST_ASSERT(
      key_is("super+a", CTUI_KEY_CHAR, 'a', CTUI_MOD_SUPER) &&
          key_is("super+ctrl+1", CTUI_KEY_CHAR, '1',
                 CTUI_MOD_SUPER | CTUI_MOD_CTRL) &&
          key_is("super+ctrl+r", CTUI_KEY_CHAR, 0x12, CTUI_MOD_SUPER) &&
          key_is("super+shift+g", CTUI_KEY_CHAR, 'G', CTUI_MOD_SUPER) &&
          key_is("shift+a", CTUI_KEY_CHAR, 'A', 0) &&
          key_is("shift+1", CTUI_KEY_CHAR, '1', CTUI_MOD_SHIFT) &&
          key_is("ctrl+shift+alt+h", CTUI_KEY_CHAR, 0x08,
                 CTUI_MOD_SHIFT | CTUI_MOD_ALT),
      "super; shift+letter its capital (as typed), shift kept on "
      "anything else and beside ctrl");
  CTUI_TEST_ASSERT(key_refused("") && key_refused("ctrl+") &&
                       key_refused("hyper+a") && key_refused("ab") &&
                       key_refused("f13") && key_refused("f0") &&
                       key_refused("\x01") && key_refused("\xff") &&
                       key_refused("é́") && key_refused("down down"),
                   "refused: empty, unknown modifier, two characters, F13, a "
                   "control byte, bad UTF-8, a key list");
  /* not NUL-terminated: only n bytes are read */
  CTUI_KEYPRESS_EVENT_DATA k;
  CTUI_TEST_ASSERT(ctui_key_parse("upx", 3, &k) == -1 &&
                       ctui_key_parse("upx", 2, &k) == 0 &&
                       k.type == CTUI_KEY_UP &&
                       ctui_key_parse("upx", 1, &k) == 0 && k.ch == 'u',
                   "only n bytes");
}

/* --- posted events ---------------------------------------------------------
 */

static int none_next(void *ctx, CTUI_EVENT *ev, int readable) {
  (void)ctx, (void)ev, (void)readable;
  return 0;
}

static const CTUI_INPUT_SOURCE NONE = {-1, none_next, NULL};

static int waits;
static long last_timeout;

static void count_wait(void *ctx, long timeout_ms) {
  (void)ctx;
  waits++;
  last_timeout = timeout_ms;
}

static void test_post(void) {
  ctui_input_set_source(&NONE);
  CTUI_KEYPRESS_EVENT_DATA k = {.type = CTUI_KEY_CHAR, .ch = 'q'};
  CTUI_MOUSE_EVENT_DATA m = {.action = CTUI_MOUSE_PRESS, .row = 2, .col = 3};
  CTUI_RESIZE_EVENT_DATA r = {.rows = 1, .cols = 1};
  CTUI_EVENT kev = {.type = CTUI_KEYPRESS_EVENT, .event_data = &k};
  CTUI_EVENT mev = {.type = CTUI_MOUSE_EVENT, .event_data = &m};
  CTUI_EVENT rev = {.type = CTUI_RESIZE_EVENT, .event_data = &r};
  CTUI_TEST_ASSERT(ctui_input_post(&kev) == 0 && ctui_input_post(&mev) == 0 &&
                       ctui_input_post(&rev) == -1 && ctui_input_posted() == 2,
                   "keys and mouse posted, a resize refused");
  k.ch = 'z'; /* copied when posted */
  ctui_input_on_wait(count_wait, NULL);
  CTUI_EVENT ev;
  int got = ctui_input_loop(&ev, 0);
  CTUI_TEST_ASSERT(got && ev.type == CTUI_KEYPRESS_EVENT &&
                       strcmp(ev.ev_source, "input") == 0 &&
                       ((CTUI_KEYPRESS_EVENT_DATA *)ev.event_data)->ch == 'q',
                   "the first posted comes first, as it was posted");
  got = ctui_input_loop(&ev, 0);
  CTUI_TEST_ASSERT(got && ev.type == CTUI_MOUSE_EVENT &&
                       ((CTUI_MOUSE_EVENT_DATA *)ev.event_data)->col == 3 &&
                       waits == 0 && ctui_input_posted() == 0,
                   "then the next, no wait between them");
  /* nothing posted: the loop waits (a tick of 10 ms ends it) */
  got = ctui_input_loop(&ev, 10);
  CTUI_TEST_ASSERT(got && ev.type == CTUI_TICK_EVENT && waits == 1 &&
                       last_timeout >= 0 && last_timeout <= 10,
                   "the wait hook runs before a wait, with its timeout");
  int full = 0;
  for (int i = 0; i <= CTUI_INPUT_POST_MAX; i++) {
    full += ctui_input_post(&kev) != 0;
  }
  CTUI_TEST_ASSERT(full == 1 && ctui_input_posted() == CTUI_INPUT_POST_MAX,
                   "a full queue refuses");
  while (ctui_input_posted()) {
    ctui_input_loop(&ev, 0);
  }
  ctui_input_on_wait(NULL, NULL);
  ctui_input_set_source(NULL);
}

/* --- the ui/ tree, driven --------------------------------------------------
 */

typedef struct {
  int downs;
  char last[8];
  int click_col, click_row, focus;
} TOY;

static TOY toy;

static void toy_render(CTUI_WIDGET *w, CTUI_COMPOSITOR *comp) {
  char line[64];
  snprintf(line, sizeof line, "count: %d", toy.downs);
  ctui_widget_puts(w, comp, 0, 0, line, CTUI_COLOR_WHITE, CTUI_COLOR_BLACK);
  snprintf(line, sizeof line, "last: %s", toy.last);
  ctui_widget_puts(w, comp, 1, 0, line, CTUI_COLOR_WHITE, CTUI_COLOR_BLACK);
  snprintf(line, sizeof line, "click %d,%d focus %d", toy.click_col,
           toy.click_row, toy.focus);
  ctui_widget_puts(w, comp, 2, 0, line, CTUI_COLOR_WHITE, CTUI_COLOR_BLACK);
  ctui_widget_puts(w, comp, 3, 0, "宽", CTUI_COLOR_WHITE, CTUI_COLOR_BLACK);
}

static int toy_key(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_KEYPRESS_EVENT_DATA *k = ev->event_data;
  if (k->type == CTUI_KEY_DOWN) {
    toy.downs++;
  } else if (k->type == CTUI_KEY_CHAR) {
    char enc[4];
    int n = ctui_utf8_encode(k->ch, enc);
    memcpy(toy.last, enc, (size_t)n);
    toy.last[n] = '\0';
  }
  return 1;
}

static int toy_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_MOUSE_EVENT_DATA *m = ev->event_data;
  if (m->action == CTUI_MOUSE_RELEASE) {
    toy.click_col = m->col;
    toy.click_row = m->row;
  }
  return 1;
}

static int toy_focus(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  toy.focus = ((CTUI_FOCUS_EVENT_DATA *)ev->event_data)->focused;
  return 1;
}

static int sink_frame(void *ctx, const CTUI_SCREEN *s) {
  (void)ctx, (void)s;
  return 0;
}

static const CTUI_SCREEN_SINK SINK = {sink_frame};

/* the daemon's side, a watch in the same loop: each answer sends the next
 * write */
typedef struct {
  int fd;
  CTUI_BUF got;
  int acks, step;
  volatile int done;
} DRIVER;

static const char *const STEPS[] = {
    "{\"write\":101,\"node\":5,\"v\":\"down down\"}",
    "{\"write\":102,\"node\":6,\"v\":\"hé\"}",
    "{\"write\":103,\"node\":5,\"v\":\"hyper+x\"}",
    "{\"write\":104,\"node\":7,\"v\":\"click 5 1\"}",
    "{\"write\":105,\"node\":7,\"v\":\"wiggle 5 1\"}",
    "{\"write\":106,\"node\":8,\"v\":\"0\"}",
    "{\"write\":107,\"node\":5,\"v\":\"esc\"}",
    NULL,
};

static void driver_say(DRIVER *d, const char *line) {
  size_t len = strlen(line);
  if (write(d->fd, line, len) != (ssize_t)len || write(d->fd, "\n", 1) != 1) {
    perror("write");
  }
}

static int on_driver(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  DRIVER *d = self->widget_data;
  char buf[8192];
  ssize_t r;
  while ((r = read(d->fd, buf, sizeof buf)) > 0) {
    ctui_buf_add(&d->got, buf, (size_t)r);
  }
  int acks = 0;
  for (const char *p = d->got.s; p && (p = strstr(p, "{\"ack\":")); p++) {
    acks++;
  }
  /* the first wait published the screen: start; then a write per answer */
  if (d->step == 0 && d->got.s && strstr(d->got.s, "ui/screen/003")) {
    driver_say(d, STEPS[d->step++]);
  } else if (acks > d->acks && STEPS[d->step]) {
    driver_say(d, STEPS[d->step++]);
  }
  d->acks = acks;
  return 0;
}

static void toy_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->w = comp->cols;
  self->h = comp->rows;
}

static void test_ui(void) {
  char dir[] = "/tmp/ctui-ui-test-XXXXXX", path[128];
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    exit(1);
  }
  snprintf(path, sizeof path, "%s/nodes.sock", dir);
  int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strcpy(addr.sun_path, path);
  if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
      listen(lfd, 4) != 0) {
    perror("bind");
    exit(1);
  }
  setenv("CTUI_NODES_SOCK", path, 1);

  CTUI_WIDGET w = ctui_widget_make(0, 0, 30, 6, NULL, toy_render, toy_layout);
  CTUI_WIDGET *ws[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, ws, 1, 6, 30);
  CTUI_SCREEN *screen = ctui_screen_create(6, 30);
  ctui_screen_set_sink(screen, &SINK, NULL);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w, toy_key);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &w, toy_mouse);
  ctui_event_register("input", CTUI_FOCUS_EVENT, &w, toy_focus);
  ctui_input_set_source(&NONE);

  unsetenv("CTUI_UI_NODES");
  CTUI_TEST_ASSERT(ctui_ui_auto(&app, "ui-test") == NULL,
                   "auto: nothing without $CTUI_UI_NODES");
  setenv("CTUI_UI_NODES", "0", 1);
  CTUI_TEST_ASSERT(ctui_ui_auto(&app, "ui-test") == NULL, "nor with 0");
  setenv("CTUI_UI_NODES", "1", 1);
  CTUI_UI *ui = ctui_ui_auto(&app, "ui-test");
  CTUI_TEST_ASSERT(ui != NULL, "auto: on with $CTUI_UI_NODES=1");

  DRIVER d = {.fd = accept(lfd, NULL, NULL)};
  fcntl(d.fd, F_SETFL, O_NONBLOCK);
  CTUI_WIDGET dw = ctui_widget_make(0, 0, 0, 0, &d, toy_render, NULL);
  CTUI_IO_WATCH *dwatch = ctui_io_watch(d.fd, CTUI_IO_READ, &dw, on_driver);

  toy.focus = 1;
  ctui_app_run(&app, screen, 0);
  /* ESC quit the loop before its wait: that answer is still held */
  int held_before = !strstr(d.got.s ? d.got.s : "", "\"ack\":107");
  ctui_io_unwatch(dwatch);
  ctui_ui_close(ui);
  on_driver(&dw, NULL);
  const char *all = d.got.s ? d.got.s : "";
  if (getenv("UI_TEST_DUMP")) {
    fputs(all, stdout);
  }

  CTUI_TEST_ASSERT(!strstr(all, "{\"begin\":1}\n{\"commit\":1}"),
                   "no begin / commit with nothing between them");
  CTUI_TEST_ASSERT(strstr(all, "\"path\":\"ui/frame\"") &&
                       strstr(all, "\"path\":\"ui/busy\"") &&
                       strstr(all, "\"path\":\"ui/input/key\"") &&
                       strstr(all, "\"path\":\"ui/screen/005\"") &&
                       !strstr(all, "ui/screen/006") &&
                       strstr(all, "\"v\":\"6x30\""),
                   "the tree: frame, input, a node per row, the size");
  const char *count = strstr(all, "\"v\":\"count: 2\"");
  const char *ack1 = strstr(all, "{\"ack\":101,\"err\":0,\"reply\":\"");
  CTUI_TEST_ASSERT(count && ack1 && count < ack1,
                   "two downs: handled, the row sent before the answer");
  char want[64];
  /* frame 1 the run's first; each posted key its own, as typed */
  snprintf(want, sizeof want, "{\"ack\":101,\"err\":0,\"reply\":\"%d\"}", 3);
  CTUI_TEST_ASSERT(strstr(all, want) != NULL,
                   "the answer is the frame that drew the last of them");
  CTUI_TEST_ASSERT(strstr(all, "\"v\":\"last: \\u00e9\"") &&
                       strstr(all, "{\"ack\":102,\"err\":0"),
                   "text typed character by character");
  snprintf(want, sizeof want, "{\"ack\":103,\"err\":%d}", EINVAL);
  CTUI_TEST_ASSERT(strstr(all, want) != NULL, "a bad key: EINVAL at once");
  CTUI_TEST_ASSERT(strstr(all, "\"v\":\"click 5,1 focus 1\""),
                   "a click at column 5, row 1");
  snprintf(want, sizeof want, "{\"ack\":105,\"err\":%d}", EINVAL);
  CTUI_TEST_ASSERT(strstr(all, want) != NULL, "an unknown verb: EINVAL");
  CTUI_TEST_ASSERT(strstr(all, "\"v\":\"click 5,1 focus 0\"") &&
                       strstr(all, "{\"ack\":106,\"err\":0"),
                   "focus lost");
  CTUI_TEST_ASSERT(strstr(all, "\"v\":\"\\u5bbd\""),
                   "a wide glyph's row: its right half not doubled");
  snprintf(want, sizeof want, "{\"ack\":107,\"err\":%d}", ECANCELED);
  CTUI_TEST_ASSERT(held_before && strstr(all, want) != NULL,
                   "ESC quit as the terminal's would; its answer held, then "
                   "cancelled on close");
  CTUI_TEST_ASSERT(strstr(all, "{\"set\":2,\"v\":\"1\"}") != NULL,
                   "idle published (nothing due)");

  free(d.got.s);
  close(d.fd);
  ctui_input_set_source(NULL);
  ctui_screen_free(screen);
  ctui_app_free(&app);
  close(lfd);
  unlink(path);
  rmdir(dir);
}

/* --- the widgets' nodes, synced at a wait --------------------------------- */

static const char *tbl_cell(void *ctx, int row, int col, char *scratch,
                            size_t cap) {
  (void)ctx;
  snprintf(scratch, cap, col ? "%dk" : "row%d", row);
  return scratch;
}

/* a closed picker's cells: its rows freed, its count left */
static int gone_calls;
static const char *tbl_gone(void *ctx, int row, int col, char *scratch,
                            size_t cap) {
  (void)ctx, (void)row, (void)col, (void)scratch, (void)cap;
  gone_calls++;
  return "";
}

static int tbl_changed(void *obj, void *user) {
  (void)obj, (void)user;
  return 0;
}

/* what the widget test's app draws: a table and tabs from inside its own
 * render (stack widgets, as apps do), and while `entries` two entries
 * nobody named */
static struct {
  CTUI_TABLE *t;
  CTUI_TABS *tabs;
  CTUI_ENTRY e[2];
  char text[2][16];
  int entries, no_table;
  CTUI_DIALOG *dialog;
  CTUI_FORM *form;
  CTUI_POPMENU *menu;
} wt;

static void wt_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_WIDGET tw = *self;
  tw.h = 3;
  tw.widget_data = wt.t;
  if (!wt.no_table) {
    ctui_table_render(&tw, comp);
  }
  CTUI_WIDGET sw = *self;
  sw.y += 3;
  sw.h = 1;
  sw.widget_data = wt.tabs;
  ctui_tabs_render(&sw, comp);
  for (int i = 0; i < wt.entries; i++) {
    ctui_entry_draw(self, comp, 0, 10 * i, 8, &wt.e[i], 0);
  }
  CTUI_WIDGET whole = ctui_widget_make(0, 0, 30, 6, NULL, wt_render, NULL);
  whole.buf = comp->cells;
  if (wt.form) {
    whole.widget_data = wt.form;
    ctui_form_render(&whole, comp);
  }
  if (wt.dialog) {
    whole.widget_data = wt.dialog;
    ctui_dialog_render(&whole, comp);
  }
  if (wt.menu) {
    whole.widget_data = wt.menu;
    ctui_popmenu_render(&whole, comp);
  }
}

/* the keys the dialog and the form got, routed as an app does: the dialog
 * while open, else the form */
static int wt_chosen, wt_changed, wt_pressed;
static int wt_keys_elsewhere; /* 1: drawn again, 2: nothing */
static int wt_key(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  const CTUI_KEYPRESS_EVENT_DATA *kp = ev->event_data;
  if (wt_keys_elsewhere) {
    return wt_keys_elsewhere == 1;
  }
  if (wt.dialog && wt.dialog->open) {
    wt_chosen += ctui_dialog_key(wt.dialog, kp) == CTUI_DIALOG_CHOSEN;
  } else if (wt.form) {
    int r = ctui_form_key(wt.form, kp);
    wt_changed += r == CTUI_FORM_CHANGED;
    wt_pressed += r == CTUI_FORM_PRESSED;
  }
  return 1;
}

static void a_wait(void) {
  CTUI_EVENT ev;
  ctui_input_loop(&ev, 1);
}

/* everything the daemon side got so far, after a frame and a wait */
static char *wt_frame(CTUI_APP *app, CTUI_SCREEN *screen, int fd,
                      CTUI_BUF *got) {
  free(got->s);
  *got = (CTUI_BUF){0};
  if (screen) {
    ctui_app_render(app, screen);
  }
  a_wait();
  char buf[16384];
  ssize_t r;
  while ((r = read(fd, buf, sizeof buf)) > 0) {
    ctui_buf_add(got, buf, (size_t)r);
  }
  if (getenv("UI_TEST_DUMP")) {
    printf("---\n%s", got->s ? got->s : "");
  }
  return got->s ? got->s : "";
}

/* path's node id as published in all, or -1 */
static int pub_id(const char *all, const char *path) {
  char want[160];
  snprintf(want, sizeof want, ",\"path\":\"%s\"", path);
  const char *at = strstr(all, want);
  if (!at) {
    return -1;
  }
  const char *p = at;
  while (p > all && p[-1] != ':') {
    p--;
  }
  return atoi(p);
}

static int count_of(const char *all, const char *what) {
  int n = 0;
  for (const char *q = all; (q = strstr(q, what)); q++) {
    n++;
  }
  return n;
}

/* path written v as the daemon would, the keys it posted handled as the
 * app's loop handles them, a frame drawn: the answer's errno, -1 none */
static int w_write(CTUI_NODES *ns, CTUI_APP *app, CTUI_SCREEN *screen, int fd,
                   CTUI_BUF *got, const char *all, const char *path,
                   const char *v) {
  static int req = 300;
  int id = pub_id(all, path);
  char line[256];
  snprintf(line, sizeof line, "{\"write\":%d,\"node\":%d,\"v\":\"%s\"}\n",
           ++req, id, v);
  if (id < 0 || write(fd, line, strlen(line)) != (ssize_t)strlen(line)) {
    return -1;
  }
  ctui_nodes_dispatch(ns);
  CTUI_EVENT ev;
  while (ctui_input_posted() > 0 && ctui_input_loop(&ev, 0)) {
    ctui_handle_event(&ev);
  }
  const char *r = wt_frame(app, screen, fd, got);
  snprintf(line, sizeof line, "{\"ack\":%d,\"err\":", req);
  const char *at = strstr(r, line);
  return at ? atoi(at + strlen(line)) : -1;
}

static int always_busy(void *user) {
  (void)user;
  return 1;
}

static void test_widgets(void) {
  char dir[] = "/tmp/ctui-ui-test-XXXXXX", path[128];
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    exit(1);
  }
  snprintf(path, sizeof path, "%s/nodes.sock", dir);
  int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strcpy(addr.sun_path, path);
  if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
      listen(lfd, 4) != 0) {
    perror("bind");
    exit(1);
  }
  setenv("CTUI_NODES_SOCK", path, 1);
  CTUI_NODES *ns = ctui_nodes_open("w-test", 0);
  int fd = accept(lfd, NULL, NULL);
  fcntl(fd, F_SETFL, O_NONBLOCK);

  static const CTUI_TABLE_COLUMN cols[] = {{"Name", 0, 0}, {"Size", 4, 1}};
  CTUI_TABLE t = {.columns = cols,
                  .column_count = 2,
                  .count = 3,
                  .selected = 1,
                  .cell = tbl_cell};
  static const CTUI_TABS_ITEM items[] = {{"one", 0}, {"two", 0}};
  CTUI_TABS tabs = {.items = items, .count = 2, .selected = 1};
  wt.t = &t;
  wt.tabs = &tabs;
  for (int i = 0; i < 2; i++) {
    wt.e[i] = (CTUI_ENTRY){.buf = wt.text[i], .cap = sizeof wt.text[i]};
  }
  ctui_entry_set(&wt.e[1], "second");
  CTUI_WIDGET w = ctui_widget_make(1, 2, 20, 4, NULL, wt_render, NULL);
  CTUI_WIDGET *ws[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, ws, 1, 6, 30);
  CTUI_SCREEN *screen = ctui_screen_create(6, 30);
  ctui_screen_set_sink(screen, &SINK, NULL);
  ctui_input_set_source(&NONE);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w, wt_key);
  CTUI_UI *ui = ctui_ui_open(ns, &app);
  CTUI_TEST_ASSERT(ctui_ui_name(NULL, &t, "x", 0, NULL, NULL) == 0,
                   "no ui: nothing, no error");
  CTUI_TEST_ASSERT(
      ctui_ui_name(ui, &t, "tbl", CTUI_UI_RDWR, tbl_changed, NULL) == 0 &&
          ctui_ui_name(ui, &tabs, "tabs", 0, NULL, NULL) == 0,
      "a table and tabs named");
  int dummy;
  CTUI_TEST_ASSERT(
      ctui_ui_name(ui, &dummy, "tabs", 0, NULL, NULL) == -1 &&
          ctui_ui_name(ui, &tabs, "again", 0, NULL, NULL) == -1 &&
          ctui_ui_name(ui, &dummy, "rw", CTUI_UI_RDWR, NULL, NULL) == -1 &&
          ctui_ui_name(ui, &dummy, "bad name", 0, NULL, NULL) == -1 &&
          ctui_ui_name(ui, &dummy, "entry/0", 0, NULL, NULL) == -1,
      "refused: a name taken, a widget named twice, writable without "
      "changed(), a bad name, a number last (an unnamed one's)");
  CTUI_BUF got = {0};
  const char *all = wt_frame(&app, NULL, fd, &got);
  int shown_id = pub_id(all, "ui/w/tbl/shown");
  CTUI_TEST_ASSERT(shown_id > 0 && !strstr(all, "ui/w/tbl/rows"),
                   "named, not drawn yet: only shown (0)");
  char shown0[64];
  snprintf(shown0, sizeof shown0, "{\"set\":%d,\"v\":\"0\"}", shown_id);
  all = wt_frame(&app, screen, fd, &got);
  CTUI_TEST_ASSERT(
      strstr(all, "\"path\":\"ui/w/tbl/rows\"") &&
          strstr(all, "\"v\":\"3\"}") &&
          strstr(all, "\"path\":\"ui/w/tbl/selected_text\"") &&
          strstr(all, "\"v\":\"row1\"}") &&
          strstr(all, "\"path\":\"ui/w/tbl/selected\",\"type\":{\"kind\":"
                      "\"int\",\"def\":-1,\"w\":1}") &&
          strstr(all, "\"path\":\"ui/w/tabs/label\"") &&
          strstr(all, "\"v\":\"two\"}") &&
          strstr(all, "\"path\":\"ui/w/tabs/selected\",\"type\":{\"kind\":"
                      "\"int\",\"def\":0}") &&
          strstr(all, "\"v\":\"1 2 20 3\"}") &&
          strstr(all, "\"v\":\"1 5 20 1\"}") && !strstr(all, "ui/w/entry"),
      "drawn: their state (rows, the selection and its text, the label), "
      "their rects where they were drawn (inside the app's widget); the "
      "table's selected writable, the tabs' not");
  static const char *const tbl_want[][2] = {
      {"ui/w/tbl/columns", "Name|Size"},
      {"ui/w/tbl/cells/name", "row1"},
      {"ui/w/tbl/cells/size", "1k"},
      {"ui/w/tbl/view/0", "row0\\u00090k"},
      {"ui/w/tbl/view/2", "row2\\u00092k"}};
  int tbl_found = 0;
  for (size_t i = 0; i < sizeof tbl_want / sizeof tbl_want[0]; i++) {
    char line[160];
    int id = pub_id(all, tbl_want[i][0]);
    snprintf(line, sizeof line, "{\"set\":%d,\"v\":\"%s\"}", id,
             tbl_want[i][1]);
    if (id > 0 && strstr(all, line)) {
      tbl_found++;
    } else {
      printf("    %s: not \"%s\" (id %d)\n", tbl_want[i][0], tbl_want[i][1],
             id);
    }
  }
  CTUI_TEST_ASSERT(tbl_found == (int)(sizeof tbl_want / sizeof tbl_want[0]) &&
                       pub_id(all, "ui/w/tbl/view/3") < 0 &&
                       strstr(all, "\"path\":\"ui/w/tbl/find\",\"type\":{"
                                   "\"kind\":\"string\",\"w\":1}"),
                   "a table's columns, the cursor row's cells by column, the "
                   "rows on screen (tab between cells), find writable");
  /* find: the first row whose cell is that, by column name or number */
  int find_id = pub_id(all, "ui/w/tbl/find");
  static const struct {
    const char *v;
    int err, sel;
  } finds[] = {{"size=2k", 0, 2}, {"0=row0", 0, 0}, {"size=9k", ENOENT, 0},
               {"nope=1", ENOENT, 0}, {"row0", EINVAL, 0}, {"7=x", ENOENT, 0}};
  int finds_ok = 0;
  for (size_t i = 0; i < sizeof finds / sizeof finds[0]; i++) {
    char req[128], ack[64];
    snprintf(req, sizeof req, "{\"write\":%zu,\"node\":%d,\"v\":\"%s\"}\n",
             200 + i, find_id, finds[i].v);
    if (write(fd, req, strlen(req)) != (ssize_t)strlen(req)) {
      perror("write");
    }
    ctui_nodes_dispatch(ns); /* the app's loop would, */
    usleep(2000);
    ctui_timer_tick(); /* and its timer asking for a frame */
    /* answered at the wait after the frame it made */
    for (int k = 0; k < 3 && !strstr(all = wt_frame(&app, screen, fd, &got),
                                     "{\"ack\":");
         k++) {
    }
    snprintf(ack, sizeof ack, "{\"ack\":%zu,\"err\":%d", 200 + i,
             finds[i].err);
    if (strstr(all, ack) && t.selected == finds[i].sel) {
      finds_ok++;
    } else {
      printf("    find %s: selected %d, got %s\n", finds[i].v, t.selected,
             all);
    }
  }
  CTUI_TEST_ASSERT(finds_ok == (int)(sizeof finds / sizeof finds[0]),
                   "find: by a column's name or number, the row selected; no "
                   "such row or column ENOENT, no '=' EINVAL");
  t.selected = 1;
  all = wt_frame(&app, screen, fd, &got);
  t.selected = 2;
  tabs.selected = 0;
  wt.entries = 2;
  all = wt_frame(&app, screen, fd, &got);
  CTUI_TEST_ASSERT(
      strstr(all, "\"v\":\"row2\"}") && strstr(all, "\"v\":\"one\"}") &&
          !strstr(all, "\"v\":\"3\"}") &&
          strstr(all, "\"path\":\"ui/w/entry/0/text\"") &&
          strstr(all, "\"path\":\"ui/w/entry/1/text\"") &&
          strstr(all, "\"v\":\"second\"}") &&
          strstr(all, "\"v\":\"11 2 8 1\"}"),
      "the app moves them: the next frame sends what changed, only that; "
      "the unnamed entries drawn as entry/0 and entry/1, in draw order");
  wt.entries = 0;
  all = wt_frame(&app, screen, fd, &got);
  CTUI_TEST_ASSERT(count_of(all, "{\"rm\":") == 2 * 4,
                   "not drawn: the unnamed entries' nodes go (%d)",
                   count_of(all, "{\"rm\":"));
  wt.no_table = 1;
  t.cell = tbl_gone;
  all = wt_frame(&app, screen, fd, &got);
  CTUI_TEST_ASSERT(strstr(all, shown0) && count_of(all, "{\"rm\":") == 0 &&
                       gone_calls == 0,
                   "a named one not drawn: shown 0, its nodes kept, its "
                   "struct not read (a closed picker's rows may be gone)");
  t.cell = tbl_cell;
  free(got.s);
  got = (CTUI_BUF){0};
  /* a dialog and a form: what they hold, generated */
  char name[16] = "Ada", name2[16] = "", ren[16] = "old.txt";
  CTUI_ENTRY ne = {.buf = name, .cap = sizeof name},
             ne2 = {.buf = name2, .cap = sizeof name2},
             re = {.buf = ren, .cap = sizeof ren};
  static const char *const profiles[] = {"flat", "adaptive"};
  CTUI_FORM_ROW rows[] = {
      {.kind = CTUI_FORM_HEADING, .label = "Pointer"},
      {.kind = CTUI_FORM_ENTRY, .label = "Name", .entry = &ne},
      {.kind = CTUI_FORM_TOGGLE, .label = "Natural scroll", .value = 1},
      {.kind = CTUI_FORM_CHOICE,
       .label = "Accel profile",
       .value = 1,
       .options = profiles,
       .option_count = 2},
      {.kind = CTUI_FORM_SLIDER,
       .label = "Repeat rate",
       .value = 40,
       .max = 100},
      {.kind = CTUI_FORM_ENTRY, .label = "Name", .entry = &ne2},
      {.kind = CTUI_FORM_BUTTON, .label = "Apply"}};
  CTUI_FORM form = {.rows = rows, .count = 7, .focus = 3};
  CTUI_DIALOG dlg = {.open = 1,
                     .title = "Rename",
                     .text = "A new name:",
                     .buttons = {"Rename", "Cancel"},
                     .button_count = 2,
                     .focus = 1,
                     .progress = -1,
                     .entry = &re};
  CTUI_TEST_ASSERT(ctui_ui_name(ui, &form, "prefs", 0, NULL, NULL) == 0 &&
                       ctui_ui_name(ui, &dlg, "ask", 0, NULL, NULL) == 0,
                   "a form and a dialog named");
  wt.form = &form;
  wt.dialog = &dlg;
  all = wt_frame(&app, screen, fd, &got);
  static const char *const want[][2] = {
      {"ui/w/ask/title", "Rename"},
      {"ui/w/ask/text", "A new name:"},
      {"ui/w/ask/buttons", "Rename|Cancel"},
      {"ui/w/ask/focus", "Cancel"},
      {"ui/w/ask/progress", "-1"},
      {"ui/w/ask/entry", "old.txt"},
      {"ui/w/prefs/focus", "accel_profile"},
      {"ui/w/prefs/fields/name", "Ada"},
      {"ui/w/prefs/fields/natural_scroll", "1"},
      {"ui/w/prefs/fields/accel_profile", "adaptive"},
      {"ui/w/prefs/fields/repeat_rate", "40"},
      {"ui/w/prefs/fields/name_2", ""}};
  int found = 0;
  for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
    char line[160];
    int id = pub_id(all, want[i][0]);
    snprintf(line, sizeof line, "{\"set\":%d,\"v\":\"%s\"}", id, want[i][1]);
    if (id > 0 && (strstr(all, line) || !*want[i][1])) {
      found++;
    } else {
      printf("    %s: not \"%s\" (id %d)\n", want[i][0], want[i][1], id);
    }
  }
  CTUI_TEST_ASSERT(found == (int)(sizeof want / sizeof want[0]) &&
                       !strstr(all, "ui/w/entry/") &&
                       !strstr(all, "ui/w/prefs/fields/pointer") &&
                       !strstr(all, "ui/w/prefs/fields/apply"),
                   "a dialog's title, text, buttons, focused button, progress "
                   "and entry; a form's focus and its fields by label (a "
                   "repeated one _2; headings and buttons not fields); their "
                   "entries not unnamed ones too");
  /* written: typed as keys, the app's own handling of them runs */
  const char *dall = strdup(all);
  int e1 = w_write(ns, &app, screen, fd, &got, dall, "ui/w/ask/entry",
                   "new.txt");
  int e2 = w_write(ns, &app, screen, fd, &got, dall, "ui/w/ask/press", "Nope");
  int e3 = w_write(ns, &app, screen, fd, &got, dall, "ui/w/prefs/press",
                   "Apply"); /* its keys would be the dialog's */
  CTUI_TEST_ASSERT(e1 == 0 && strcmp(ren, "new.txt") == 0 && e2 == ENOENT &&
                       e3 == EBUSY && wt_pressed == 0 && dlg.open,
                   "a dialog's entry written: its line replaced, by keys "
                   "(\"%s\", %d); no such button ENOENT (%d); a form under "
                   "the open dialog EBUSY: the keys would be the dialog's "
                   "(%d)",
                   ren, e1, e2, e3);
  int e4 = w_write(ns, &app, screen, fd, &got, dall, "ui/w/ask/press",
                   "Rename");
  int e5 = w_write(ns, &app, screen, fd, &got, dall, "ui/w/ask/press",
                   "Rename");
  CTUI_TEST_ASSERT(e4 == 0 && wt_chosen == 1 && !dlg.open && dlg.focus == 0 &&
                       e5 == ENXIO,
                   "a button pressed by its label: picked, the dialog closed "
                   "(%d); then closed: ENXIO (%d)",
                   e4, e5);
  wt.dialog = NULL;
  wt_changed = 0;
  int f1 = w_write(ns, &app, screen, fd, &got, dall,
                   "ui/w/prefs/fields/repeat_rate", "60");
  int f2 = w_write(ns, &app, screen, fd, &got, dall,
                   "ui/w/prefs/fields/natural_scroll", "0");
  int f3 = w_write(ns, &app, screen, fd, &got, dall,
                   "ui/w/prefs/fields/accel_profile", "flat");
  int f4 =
      w_write(ns, &app, screen, fd, &got, dall, "ui/w/prefs/fields/name", "Bob");
  int f5 =
      w_write(ns, &app, screen, fd, &got, dall, "ui/w/prefs/press", "Apply");
  CTUI_TEST_ASSERT(f1 == 0 && rows[4].value == 60 && f2 == 0 &&
                       rows[2].value == 0 && f3 == 0 && rows[3].value == 0 &&
                       f4 == 0 && strcmp(name, "Bob") == 0 && f5 == 0 &&
                       wt_pressed == 1 && form.focus == 6,
                   "a form's fields written by keys: a slider stepped, a "
                   "toggle flipped, a choice stepped, an entry retyped (the "
                   "focus walked to each); a button pressed by label (%d %d "
                   "%d %d %d; %d changes)",
                   f1, f2, f3, f4, f5, wt_changed);
  CTUI_TEST_ASSERT(
      wt_changed == 20 + 1 + 1 + 1 + 3,
      "each key the app's: 20 slider steps, a flip, a step, the line killed, "
      "three characters typed (%d)",
      wt_changed);
  rows[5].disabled = 1;
  int r1 = w_write(ns, &app, screen, fd, &got, dall,
                   "ui/w/prefs/fields/repeat_rate", "101");
  int r2 = w_write(ns, &app, screen, fd, &got, dall,
                   "ui/w/prefs/fields/accel_profile", "fast");
  int r3 = w_write(ns, &app, screen, fd, &got, dall,
                   "ui/w/prefs/fields/natural_scroll", "2");
  int r4 = w_write(ns, &app, screen, fd, &got, dall,
                   "ui/w/prefs/fields/name_2", "x");
  int r5 =
      w_write(ns, &app, screen, fd, &got, dall, "ui/w/prefs/press", "Nope");
  rows[5].disabled = 0;
  CTUI_TEST_ASSERT(r1 == ERANGE && r2 == ENOENT && r3 == EINVAL &&
                       r4 == EPERM && r5 == ENOENT && rows[4].value == 60,
                   "refused: a slider outside its range, no such option, a "
                   "toggle not 0/1, a disabled row, no such button (%d %d %d "
                   "%d %d)",
                   r1, r2, r3, r4, r5);
  wt_keys_elsewhere = 1;
  int k1 = w_write(ns, &app, screen, fd, &got, dall,
                   "ui/w/prefs/fields/repeat_rate", "50");
  int back = strstr(got.s ? got.s : "", "\"v\":\"60\"}") != NULL;
  wt_keys_elsewhere = 2;
  int k2 = w_write(ns, &app, NULL, fd, &got, dall,
                   "ui/w/prefs/fields/repeat_rate", "50"); /* no frame */
  wt_keys_elsewhere = 0;
  CTUI_TEST_ASSERT(k1 == EIO && back && k2 == EIO && rows[4].value == 60,
                   "the keys gone to another pane: EIO, the field reading "
                   "what it holds again (%d); nothing drawn since: EIO (%d)",
                   k1, k2);
  free((void *)dall);
  wt.dialog = &dlg;
  /* the rows change: the fields follow */
  form.count = 3;
  rows[2].value = 0;
  all = wt_frame(&app, screen, fd, &got);
  CTUI_TEST_ASSERT(count_of(all, "{\"rm\":") == 5 &&
                       strstr(all, "\"path\":\"ui/w/prefs/fields/name\"") &&
                       strstr(all, "ui/w/prefs/fields/natural_scroll\""),
                   "a form's rows changed: its fields republished (%d gone)",
                   count_of(all, "{\"rm\":"));
  /* a menu: the open level's items, the cursor's */
  static const CTUI_POPMENU_ITEM menu_items[] = {
      {.label = "&Open"},
      {.flags = CTUI_POPMENU_SEP},
      {.label = "Copy && paste", .keys = "Ctrl+C"}};
  CTUI_POPMENU menu = {0};
  ctui_popmenu_open(&menu, menu_items, 3, 1, 1, 6, 30);
  menu.level[0].sel = 2;
  wt.menu = &menu;
  ctui_ui_name(ui, &menu, "menu", 0, NULL, NULL);
  all = wt_frame(&app, screen, fd, &got);
  CTUI_TEST_ASSERT(
      strstr(all, "\"v\":\"Open|Copy & paste\"}") &&
          strstr(all, "\"v\":\"Copy & paste\"}"),
      "a menu's items (no separators, no '&'s) and the cursor's item");
  wt.menu = NULL;
  ctui_ui_forget(ui, "menu");
  all = wt_frame(&app, NULL, fd, &got);
  CTUI_TEST_ASSERT(count_of(all, "{\"rm\":") == 5,
                   "forget: the menu's five nodes gone (%d)",
                   count_of(all, "{\"rm\":"));
  dlg.open = 0;
  all = wt_frame(&app, screen, fd, &got);
  CTUI_TEST_ASSERT(count_of(all, "{\"rm\":") == 0,
                   "a dialog closed: shown 0, its nodes kept");
  wt.form = NULL;
  wt.dialog = NULL;
  ctui_ui_forget(ui, "prefs");
  ctui_ui_forget(ui, "ask");
  ctui_ui_forget(ui, "tbl");
  char buf[16384];
  ssize_t r;
  while ((r = read(fd, buf, sizeof buf)) > 0) {
    ctui_buf_add(&got, buf, (size_t)r);
  }
  all = got.s ? got.s : "";
  CTUI_TEST_ASSERT(count_of(all, "{\"rm\":") == 6 + 9 + 13,
                   "forget: the form's, the dialog's and the table's nodes "
                   "removed (%d)",
                   count_of(all, "{\"rm\":"));
  /* the app busy: ui/busy 1, ui/idle 0 though nothing is due */
  ctui_ui_busy(ui, always_busy, NULL);
  all = wt_frame(&app, NULL, fd, &got);
  /* (idle was 0 already: this loop's 1 ms tick is due within 50 ms) */
  CTUI_TEST_ASSERT(strstr(all, "{\"set\":3,\"v\":\"1\"}") &&
                       !strstr(all, "{\"set\":2,\"v\":\"1\"}"),
                   "busy: ui/busy 1, ui/idle not 1");
  free(got.s);
  ctui_ui_close(ui);
  ctui_nodes_close(ns);
  ctui_input_set_source(NULL);
  ctui_screen_free(screen);
  ctui_app_free(&app);
  close(fd);
  close(lfd);
  unlink(path);
  rmdir(dir);
}

int main(void) {
  ctui_log_init(E_ALL);
  test_keynames();
  test_post();
  test_ui();
  test_widgets();
  return ctui_test_summary();
}
