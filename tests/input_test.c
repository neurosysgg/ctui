/* Exercises ctui_input_loop() end to end without a terminal: STDIN_FILENO
 * is swapped for a pipe, so the real select()/read() path runs against
 * whatever raw bytes the test writes into the other end -- the exact
 * bytes a terminal would send. Covers key decoding (CSI + modifiers, SS3,
 * alt+key, UTF-8, unknown sequences), SGR mouse reports, fd watches,
 * timer-deadline wakeups, CTUI_TICK_EVENT, and ctui_app_run()'s
 * quit_on_esc/ctui_app_quit() handling. Termios is never touched (pipes
 * have none), so ctui_init() isn't needed. */

/* clock_gettime() is POSIX, not C11 -- see core/term.c */
#define _POSIX_C_SOURCE 200809L

#include "ctui.h"

#include "ctui_test.h"

#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_stdin_w = -1;

static void feed(const char *bytes) {
  write(g_stdin_w, bytes, strlen(bytes));
}

static CTUI_KEYPRESS_EVENT_DATA *next_key(CTUI_EVENT *ev) {
  if (!ctui_input_loop(ev, 0) || ev->type != CTUI_KEYPRESS_EVENT) {
    return NULL;
  }
  return ev->event_data;
}

static void test_keys(void) {
  CTUI_EVENT ev;
  CTUI_KEYPRESS_EVENT_DATA *kp;

  feed("a");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_CHAR && kp->ch == 'a' &&
                       kp->mods == 0,
                   "a plain byte is CTUI_KEY_CHAR with no modifiers");

  feed("\xc3\xa9");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_CHAR && kp->ch == 0xE9,
                   "a 2-byte UTF-8 keypress decodes to one codepoint event, "
                   "not two byte events");

  feed("\x1b[A");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_UP && kp->mods == 0,
                   "CSI A is UP");

  feed("\x1b[1;5C");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_RIGHT &&
                       kp->mods == CTUI_MOD_CTRL,
                   "CSI 1;5C is ctrl+RIGHT (modifier param 5 = 1 + ctrl)");

  feed("\x1b[3~");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_DELETE,
                   "CSI 3~ is DELETE -- previously its '~' leaked out as a "
                   "stray CHAR");

  feed("\x1b[5~\x1b[6~\x1b[H\x1b[4~\x1b[Z");
  CTUI_KEYTYPE want[] = {CTUI_KEY_PGUP, CTUI_KEY_PGDN, CTUI_KEY_HOME,
                         CTUI_KEY_END, CTUI_KEY_BACKTAB};
  int all = 1;
  for (int i = 0; i < 5; i++) {
    kp = next_key(&ev);
    all = all && kp && kp->type == want[i];
  }
  CTUI_TEST_ASSERT(all, "PGUP/PGDN/HOME/END/BACKTAB decode from back-to-back "
                        "sequences in one read");

  feed("\x1bOB");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_DOWN,
                   "SS3 B (application cursor mode) is DOWN");

  feed("\x1bx");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_CHAR && kp->ch == 'x' &&
                       kp->mods == CTUI_MOD_ALT,
                   "ESC followed immediately by a byte is alt+that key");

  feed("\x1b[99X");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_NONE,
                   "an unknown CSI resolves to NONE, never ESC (which would "
                   "quit a default app)");
  feed("z");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_CHAR && kp->ch == 'z',
                   "...and was consumed whole: the next event is the next "
                   "real key, not the unknown sequence's tail");

  feed("\x1b");
  kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->type == CTUI_KEY_ESC,
                   "a lone ESC (nothing follows within the sequence timeout) "
                   "is ESC");
}

static CTUI_MOUSE_EVENT_DATA *next_mouse(CTUI_EVENT *ev) {
  if (!ctui_input_loop(ev, 0) || ev->type != CTUI_MOUSE_EVENT) {
    return NULL;
  }
  return ev->event_data;
}

static void test_focus(void) {
  CTUI_EVENT ev;
  feed("\x1b[O");
  CTUI_TEST_ASSERT(ctui_input_loop(&ev, 0) && ev.type == CTUI_FOCUS_EVENT &&
                       !((CTUI_FOCUS_EVENT_DATA *)ev.event_data)->focused,
                   "CSI O is a focus-out event");
  feed("\x1b[I");
  CTUI_TEST_ASSERT(ctui_input_loop(&ev, 0) && ev.type == CTUI_FOCUS_EVENT &&
                       ((CTUI_FOCUS_EVENT_DATA *)ev.event_data)->focused,
                   "CSI I is a focus-in event");
  feed("\x1b[1;5I");
  CTUI_TEST_ASSERT(ctui_input_loop(&ev, 0) && ev.type == CTUI_KEYPRESS_EVENT &&
                       ((CTUI_KEYPRESS_EVENT_DATA *)ev.event_data)->type ==
                           CTUI_KEY_NONE,
                   "an I final with parameters isn't a focus report");
}

static void test_mouse(void) {
  CTUI_EVENT ev;
  CTUI_MOUSE_EVENT_DATA *md;

  feed("\x1b[<0;10;5M");
  md = next_mouse(&ev);
  CTUI_TEST_ASSERT(md && md->action == CTUI_MOUSE_PRESS && md->button == 0 &&
                       md->row == 4 && md->col == 9,
                   "SGR press: left button, 1-based x/y become 0-based "
                   "col/row");

  feed("\x1b[<2;10;5m");
  md = next_mouse(&ev);
  CTUI_TEST_ASSERT(md && md->action == CTUI_MOUSE_RELEASE && md->button == 2,
                   "SGR lowercase m is a release, keeping its button");

  feed("\x1b[<65;1;1M");
  md = next_mouse(&ev);
  CTUI_TEST_ASSERT(md && md->action == CTUI_MOUSE_SCROLL_DOWN,
                   "button code 65 is wheel down");

  feed("\x1b[<51;3;3M");
  md = next_mouse(&ev);
  CTUI_TEST_ASSERT(md && md->action == CTUI_MOUSE_MOTION &&
                       md->button == -1 && md->mods == CTUI_MOD_CTRL,
                   "32 (motion) + 3 (no button) + 16 (ctrl) is buttonless "
                   "ctrl-motion");

  CTUI_WIDGET w = ctui_widget_make(8, 3, 4, 2, NULL, NULL, NULL);
  CTUI_TEST_ASSERT(ctui_widget_contains(&w, 4, 9) &&
                       !ctui_widget_contains(&w, 5, 9) &&
                       !ctui_widget_contains(&w, 4, 12),
                   "ctui_widget_contains() hit-tests absolute cells against "
                   "x/y/w/h, exclusive on the far edges");
}

static int g_io_calls = 0;
static int g_io_fd = -1;
static CTUI_IO_WATCH *g_io_watch = NULL;

static int on_readable(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_IO_EVENT_DATA *data = ev->event_data;
  char buf[16];
  read(data->fd, buf, sizeof buf);
  g_io_calls++;
  g_io_fd = data->fd;
  ctui_io_unwatch(g_io_watch);
  return 1;
}

static void test_io_watch(void) {
  int p[2];
  pipe(p);
  g_io_watch = ctui_io_watch(p[0], CTUI_IO_READ, NULL, on_readable);
  write(p[1], "x", 1);

  CTUI_EVENT ev;
  int got = ctui_input_loop(&ev, 0);
  CTUI_TEST_ASSERT(got && ev.type == CTUI_IO_EVENT &&
                       ((CTUI_IO_EVENT_DATA *)ev.event_data)->fd == p[0],
                   "a watched fd becoming readable wakes the loop as a "
                   "CTUI_IO_EVENT naming that fd");
  CTUI_TEST_ASSERT(ctui_io_dispatch(&ev) == 1 && g_io_calls == 1 &&
                       g_io_fd == p[0],
                   "dispatch runs the watch's own handler, returning its "
                   "changed flag");

  write(p[1], "y", 1);
  feed("k");
  CTUI_KEYPRESS_EVENT_DATA *kp = next_key(&ev);
  CTUI_TEST_ASSERT(kp && kp->ch == 'k' && g_io_calls == 1,
                   "after unwatching from inside its own handler, the fd is "
                   "no longer selected even though it's readable again");

  CTUI_TEST_ASSERT(ctui_io_watch(-1, CTUI_IO_READ, NULL, on_readable) ==
                           NULL &&
                       ctui_io_watch(p[0], 0, NULL, on_readable) == NULL,
                   "negative fds and empty event masks are rejected");
  close(p[0]);
  close(p[1]);
}

static int g_timer_fires = 0;
static int on_timer(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  g_timer_fires++;
  return 1;
}

static long ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void test_timer_wake_and_tick(void) {
  CTUI_TIMER *t = ctui_timer_register(30, NULL, on_timer);
  CTUI_EVENT ev;
  long start = ms_now();
  int got = ctui_input_loop(&ev, 0);
  long waited = ms_now() - start;
  CTUI_TEST_ASSERT(got && ev.type == CTUI_TIMER_EVENT && waited >= 25 &&
                       waited < 200,
                   "with tick_ms = 0 and no input, the loop still wakes at "
                   "the next timer deadline (waited %ldms for a 30ms timer)",
                   waited);
  CTUI_TEST_ASSERT(ctui_timer_tick() == 1 && g_timer_fires == 1,
                   "the woken loop's ctui_timer_tick() fires the timer");
  ctui_timer_cancel(t);
  CTUI_TEST_ASSERT(ctui_timer_ms_until_due() == -1,
                   "a cancelled timer no longer has a deadline");

  got = ctui_input_loop(&ev, 20);
  CTUI_TEST_ASSERT(got && ev.type == CTUI_TICK_EVENT,
                   "tick_ms > 0 with nothing else pending still produces "
                   "CTUI_TICK_EVENT");
}

static int g_saw_esc = 0;
static int on_key_quit(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_KEYPRESS_EVENT_DATA *kp = ev->event_data;
  if (kp->type == CTUI_KEY_ESC) {
    g_saw_esc = 1;
  }
  if (kp->type == CTUI_KEY_CHAR && kp->ch == 'q') {
    ctui_app_quit();
  }
  return 0;
}

/* bytes already sitting in the pipe when the loop starts would decode as
 * one sequence (ESC + q = alt+q), so the 'q' is fed from a timer instead,
 * after the lone ESC has already timed out into its own key */
static int feed_q_later(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  feed("q");
  return 0;
}

static void noop_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  (void)comp;
}

static void test_quit(CTUI_APP *app, CTUI_WIDGET *w) {
  CTUI_SCREEN *screen = ctui_screen_create(2, 4);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, w, on_key_quit);
  app->quit_on_esc = 0;

  /* ctui_app_run() flushes frames to stdout; keep them out of the report */
  int saved = dup(STDOUT_FILENO);
  int devnull = open("/dev/null", O_WRONLY);
  dup2(devnull, STDOUT_FILENO);
  feed("\x1b");
  CTUI_TIMER *t = ctui_timer_register(150, NULL, feed_q_later);
  ctui_app_run(app, screen, 0);
  ctui_timer_cancel(t);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  close(devnull);

  CTUI_TEST_ASSERT(g_saw_esc,
                   "with quit_on_esc = 0, ESC reaches handlers like any "
                   "other key instead of ending the run loop");
  CTUI_TEST_ASSERT(1, "ctui_app_quit() from a handler ended ctui_app_run() "
                      "(reaching this line at all)");
  ctui_event_unregister(w);
  ctui_screen_free(screen);
}

int main(void) {
  ctui_log_init(E_WRN | E_ERR);

  int p[2];
  pipe(p);
  dup2(p[0], STDIN_FILENO);
  close(p[0]);
  g_stdin_w = p[1];

  CTUI_WIDGET w = ctui_widget_make(0, 0, 4, 2, NULL, noop_render, NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, 2, 4);

  test_keys();
  test_mouse();
  test_focus();
  test_io_watch();
  test_timer_wake_and_tick();
  test_quit(&app, &w);

  ctui_app_free(&app);
  ctui_log_shutdown();
  return ctui_test_summary();
}
