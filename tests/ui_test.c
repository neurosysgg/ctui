/* core/keyname.h's key names, core/input.h's posted events and wait hook,
 * node/node.h's deferred answers, and node/ui.h's tree driving a small
 * app from a fake daemon inside the app's own run loop: keys, text and
 * clicks posted, each write answered after the frame it made (the rows'
 * new text sent before the answer), refusals answered at once, ESC quitting
 * as the terminal's would, the answer still waiting then cancelled. */
#define _GNU_SOURCE

#include "node/ui.h"

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
  (void)ctx, (void)col;
  snprintf(scratch, cap, "row%d", row);
  return scratch;
}

static int tbl_changed(CTUI_WIDGET *w, void *user) {
  (void)w, (void)user;
  return 0;
}

static int always_busy(void *user) {
  (void)user;
  return 1;
}

static void a_wait(void) {
  CTUI_EVENT ev;
  ctui_input_loop(&ev, 1);
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

  CTUI_WIDGET w = ctui_widget_make(1, 2, 20, 4, NULL, toy_render, NULL);
  CTUI_WIDGET *ws[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, ws, 1, 6, 30);
  ctui_input_set_source(&NONE);
  CTUI_UI *ui = ctui_ui_open(ns, &app);
  CTUI_TABLE t = {.count = 3, .selected = 1, .cell = tbl_cell};
  static const CTUI_TABS_ITEM items[] = {{"one", 0}, {"two", 0}};
  CTUI_TABS tabs = {.items = items, .count = 2, .selected = 1};
  CTUI_TEST_ASSERT(ctui_ui_table(NULL, "x", &w, &t, 0, NULL, NULL) == 0,
                   "no ui: nothing, no error");
  CTUI_TEST_ASSERT(
      ctui_ui_table(ui, "tbl", &w, &t, CTUI_UI_RDWR, tbl_changed, NULL) == 0 &&
          ctui_ui_tabs(ui, "tabs", &w, &tabs, 0, NULL, NULL) == 0,
      "a table and tabs published");
  CTUI_TEST_ASSERT(
      ctui_ui_tabs(ui, "tabs", &w, &tabs, 0, NULL, NULL) == -1 &&
          ctui_ui_table(ui, "rw", &w, &t, CTUI_UI_RDWR, NULL, NULL) == -1 &&
          ctui_ui_table(ui, "bad name", &w, &t, 0, NULL, NULL) == -1,
      "refused: a name taken, writable without changed(), a bad "
      "name");
  CTUI_BUF got = {0};
  char buf[16384];
  ssize_t r;
  a_wait();
  while ((r = read(fd, buf, sizeof buf)) > 0) {
    ctui_buf_add(&got, buf, (size_t)r);
  }
  const char *all = got.s ? got.s : "";
  if (getenv("UI_TEST_DUMP")) {
    fputs(all, stdout);
  }
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
          strstr(all, "\"v\":\"1 2 20 4\"}"),
      "their state on publishing (in the same begin / commit): rows, the "
      "selection and its text, the label, the rect; the table's selected "
      "writable, the tabs' not");
  free(got.s);
  got = (CTUI_BUF){0};
  t.selected = 2;
  tabs.selected = 0;
  a_wait();
  while ((r = read(fd, buf, sizeof buf)) > 0) {
    ctui_buf_add(&got, buf, (size_t)r);
  }
  all = got.s ? got.s : "";
  CTUI_TEST_ASSERT(strstr(all, "\"v\":\"row2\"}") &&
                       strstr(all, "\"v\":\"one\"}") &&
                       !strstr(all, "\"v\":\"3\"}"),
                   "the app moves them: the next wait sends what changed, "
                   "only that");
  free(got.s);
  got = (CTUI_BUF){0};
  ctui_ui_forget(ui, "tbl");
  while ((r = read(fd, buf, sizeof buf)) > 0) {
    ctui_buf_add(&got, buf, (size_t)r);
  }
  all = got.s ? got.s : "";
  int rms = 0;
  for (const char *q = all; (q = strstr(q, "{\"rm\":")); q++) {
    rms++;
  }
  CTUI_TEST_ASSERT(rms == 5, "forget: its five nodes removed (%d)", rms);
  free(got.s);
  /* the app busy: ui/busy 1, ui/idle 0 though nothing is due */
  ctui_ui_busy(ui, always_busy, NULL);
  got = (CTUI_BUF){0};
  a_wait();
  while ((r = read(fd, buf, sizeof buf)) > 0) {
    ctui_buf_add(&got, buf, (size_t)r);
  }
  all = got.s ? got.s : "";
  /* (idle was 0 already: this loop's 1 ms tick is due within 50 ms) */
  CTUI_TEST_ASSERT(strstr(all, "{\"set\":3,\"v\":\"1\"}") &&
                       !strstr(all, "{\"set\":2,\"v\":\"1\"}"),
                   "busy: ui/busy 1, ui/idle not 1");
  free(got.s);
  ctui_ui_close(ui);
  ctui_nodes_close(ns);
  ctui_input_set_source(NULL);
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
