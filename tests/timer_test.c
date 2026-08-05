/* Exercises core/timer.c's real registry -- ctui_timer_register_synchronized()
 * and ctui_timer_register() wired through ctui_timer_tick() exactly like
 * ctui_app_run() calls it, driven by real wall-clock sleeps (ctui_timer_tick()
 * reads CLOCK_MONOTONIC internally, so there's no shortcut around actually
 * waiting). Proves: two widgets sharing a synchronized duration always fire
 * together, and an independent widget on a different duration fires on its
 * own schedule, decoupled from any group. See tools/ctui_test.h's header
 * comment for what this harness can/can't reach. */

/* must precede every #include -- nanosleep() is POSIX, not C11 (see
 * core/term.c's identical comment for why this has to come before the
 * first system header, even a transitive one). */
#define _POSIX_C_SOURCE 200809L

#include "ctui.h"

#include "ctui_test.h"

#include <time.h>

typedef struct {
  int fire_count;
} COUNTER;

static int handle_timer(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  COUNTER *c = self->widget_data;
  c->fire_count++;
  return 1;
}

static void noop_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  (void)comp;
}

static void sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
  nanosleep(&ts, NULL);
}

int main(void) {
  ctui_log_init(E_ALL);

  int rows = 24, cols = 80;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  COUNTER a = {0}, b = {0}, c = {0};
  CTUI_WIDGET widget_a = ctui_widget_make(0, 0, 1, 1, &a, noop_render, NULL);
  CTUI_WIDGET widget_b = ctui_widget_make(0, 0, 1, 1, &b, noop_render, NULL);
  CTUI_WIDGET widget_c = ctui_widget_make(0, 0, 1, 1, &c, noop_render, NULL);

  CTUI_WIDGET *widgets[] = {&widget_a, &widget_b, &widget_c};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 3, rows, cols);

  /* a and b share an 80ms synchronized group; c is independent at 150ms */
  ctui_timer_register_synchronized(80, &widget_a, handle_timer);
  ctui_timer_register_synchronized(80, &widget_b, handle_timer);
  ctui_timer_register(150, &widget_c, handle_timer);

  CTUI_TEST_ASSERT(a.fire_count == 0 && b.fire_count == 0 && c.fire_count == 0,
                   "nothing fires before any time has passed");

  sleep_ms(110);
  ctui_timer_tick();
  CTUI_TEST_ASSERT(a.fire_count == 1 && b.fire_count == 1,
                   "synchronized group fires both members together once "
                   "past its 80ms deadline");
  CTUI_TEST_ASSERT(c.fire_count == 0,
                   "independent 150ms timer hasn't fired yet at ~110ms");

  sleep_ms(90); /* ~200ms total; group's 2nd deadline is ~110+80=190ms,
                 * rescheduled from its actual (slightly late) first fire
                 * rather than the ideal 160ms mark -- see ctui_timer_tick()'s
                 * "next_fire_ms = now + duration_ms" rescheduling */
  ctui_timer_tick();
  CTUI_TEST_ASSERT(c.fire_count == 1,
                   "independent timer fires on its own 150ms schedule, "
                   "decoupled from the synchronized group");
  CTUI_TEST_ASSERT(a.fire_count == 2 && b.fire_count == 2,
                   "synchronized group has fired a second time in lockstep "
                   "by ~200ms");

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
