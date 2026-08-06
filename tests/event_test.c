/* Exercises core/event.c's registry semantics directly -- multiple handlers
 * on the same (source, type) all fire, in registration order; a mismatched
 * source or type is skipped; ctui_handle_event()'s return aggregates via
 * OR across every matching handler, not just the last one. menu_status_test
 * already proves a real single-handler wiring works end to end; this covers
 * the registry mechanics that test doesn't exercise. Also covers the
 * enum-to-string helpers and the "no app yet" rejection paths in
 * ctui_event_register()/ctui_handle_event() -- only reachable before any
 * ctui_app_init() call has run in this process, since g_app has no public
 * way to be unset afterward. */
#include "ctui.h"

#include "ctui_test.h"

#include <string.h>

static void test_enum_names(void) {
  for (CTUI_KEYTYPE t = CTUI_KEY_NONE; t <= CTUI_KEY_CHAR; t++) {
    CTUI_TEST_ASSERT(strcmp(ctui_keytype_name(t), "UNKNOWN") != 0,
                     "ctui_keytype_name(%d) has a real name, not the "
                     "UNKNOWN fallback",
                     t);
  }
  for (CTUI_EVENTTYPE t = CTUI_KEYPRESS_EVENT; t <= CTUI_DUMMY_EVENT; t++) {
    CTUI_TEST_ASSERT(strcmp(ctui_eventtype_name(t), "UNKNOWN") != 0,
                     "ctui_eventtype_name(%d) has a real name, not the "
                     "UNKNOWN fallback",
                     t);
  }
}

static void noop_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  (void)comp;
}

static int should_not_fire_before_init(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  CTUI_TEST_ASSERT(0, "a handler registered before ctui_app_init() must "
                      "never fire -- registration itself should have been "
                      "dropped");
  return 1;
}

static void test_no_app_yet(void) {
  CTUI_WIDGET w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);

  /* no ctui_app_init() has run yet in this process -- g_app is still NULL,
   * so both of these must reject-and-log rather than crash or silently
   * succeed */
  ctui_event_register("src", CTUI_VALUE_CHANGED_EVENT, &w,
                      should_not_fire_before_init);

  CTUI_EVENT ev = {.type = CTUI_VALUE_CHANGED_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "src",
                   .event_data = NULL};
  int changed = ctui_handle_event(&ev);
  CTUI_TEST_ASSERT(changed == 0,
                   "handle_event() with no app registered yet returns 0 "
                   "and dispatches nothing (the registration above was "
                   "dropped, not queued)");
}

static char fire_order[8];
static int fire_len = 0;

static int handler_a(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  fire_order[fire_len++] = 'a';
  return 0;
}

static int handler_b(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  fire_order[fire_len++] = 'b';
  return 1;
}

static int handler_should_not_fire(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  fire_order[fire_len++] = '!';
  return 1;
}

int main(void) {
  ctui_log_init(E_ALL);

  test_enum_names();
  test_no_app_yet();

  int rows = 10, cols = 10;
  CTUI_WIDGET w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);

  ctui_event_register("src", CTUI_VALUE_CHANGED_EVENT, &w, handler_a);
  ctui_event_register("src", CTUI_VALUE_CHANGED_EVENT, &w, handler_b);
  ctui_event_register("other-src", CTUI_VALUE_CHANGED_EVENT, &w,
                      handler_should_not_fire);
  ctui_event_register("src", CTUI_TICK_EVENT, &w, handler_should_not_fire);

  CTUI_EVENT ev = {.type = CTUI_VALUE_CHANGED_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "src",
                   .event_data = NULL};
  int changed = ctui_handle_event(&ev);

  CTUI_TEST_ASSERT(fire_len == 2 && fire_order[0] == 'a' &&
                       fire_order[1] == 'b',
                   "both handlers registered for (\"src\", "
                   "VALUE_CHANGED) fire, in registration order");
  CTUI_TEST_ASSERT(changed == 1,
                   "handle_event() returns 1 if ANY matching handler "
                   "returned 1, even though handler_a (fired first) "
                   "returned 0");

  fire_len = 0;
  CTUI_EVENT mismatched_source = {.type = CTUI_VALUE_CHANGED_EVENT,
                                  .scope = CTUI_EVENT_SCOPE_GLOBAL,
                                  .ev_source = "unregistered",
                                  .event_data = NULL};
  changed = ctui_handle_event(&mismatched_source);
  CTUI_TEST_ASSERT(fire_len == 0 && changed == 0,
                   "no handler fires for a source nothing registered "
                   "under");

  ctui_app_free(&app);
  return ctui_test_summary();
}
