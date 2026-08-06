/* Exercises core/event.c's registry semantics directly -- multiple handlers
 * on the same (source, type) all fire, in registration order; a mismatched
 * source or type is skipped; ctui_handle_event()'s return aggregates via
 * OR across every matching handler, not just the last one. menu_status_test
 * already proves a real single-handler wiring works end to end; this covers
 * the registry mechanics that test doesn't exercise. Also covers the
 * enum-to-string helpers and the "no app yet" rejection paths in
 * ctui_event_register()/ctui_handle_event() -- only reachable before any
 * ctui_app_init() call has run in this process, since g_app has no public
 * way to be unset afterward.
 *
 * test_bubble() covers EVENT_DESIGN.md's CTUI_EVENT_SCOPE_BUBBLE: a
 * handler registered on one CTUI_SPLIT child never fires for its sibling
 * (the two-instance disambiguation problem), a handler registered on the
 * split itself fires when the event bubbles up from an active child, that
 * same ancestor handler is pruned once the child is no longer active
 * (split->count shrinks), the child's OWN handler still fires regardless
 * (bubbling never gates the widget it's currently on, only whether the
 * walk continues past it), and a BUBBLE-scope event with no origin is
 * dropped rather than crashing. */
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

static int handler_on_a(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  fire_order[fire_len++] = 'A';
  return 0;
}

static int handler_on_b(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  fire_order[fire_len++] = 'B';
  return 0;
}

static int handler_on_split(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  fire_order[fire_len++] = 'S';
  return 1;
}

/* CTUI_EVENT_SCOPE_BUBBLE, per EVENT_DESIGN.md: a real CTUI_SPLIT with two
 * children, wired up exactly like an app would (ctui_widget_init() runs
 * ctui_split_layout() via the split widget's own .layout, setting each
 * active child's ->parent and the split's ->is_active_child -- nothing
 * test-only about this wiring). */
static void test_bubble(CTUI_APP *app) {
  CTUI_WIDGET child_a = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
  CTUI_WIDGET child_b = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
  CTUI_WIDGET *split_children[] = {&child_a, &child_b};
  CTUI_SPLIT split_data = {
      .mode = CTUI_SPLIT_H, .children = split_children, .count = 2};
  CTUI_WIDGET split_widget = ctui_widget_make(
      0, 0, 10, 10, &split_data, ctui_split_render, ctui_split_layout);

  ctui_widget_init(&split_widget, app->comp);
  CTUI_TEST_ASSERT(
      child_a.parent == &split_widget && child_b.parent == &split_widget,
      "ctui_split_layout() sets every active child's parent to the "
      "split's own widget");

  ctui_event_register("src", CTUI_VALUE_CHANGED_EVENT, &child_a,
                      handler_on_a);
  ctui_event_register("src", CTUI_VALUE_CHANGED_EVENT, &child_b,
                      handler_on_b);
  ctui_event_register("src", CTUI_VALUE_CHANGED_EVENT, &split_widget,
                      handler_on_split);

  fire_len = 0;
  CTUI_EVENT ev = {.type = CTUI_VALUE_CHANGED_EVENT,
                   .scope = CTUI_EVENT_SCOPE_BUBBLE,
                   .ev_source = "src",
                   .event_data = NULL,
                   .origin = &child_a};
  int changed = ctui_handle_event(&ev);
  CTUI_TEST_ASSERT(
      fire_len == 2 && fire_order[0] == 'A' && fire_order[1] == 'S',
      "bubbling from child_a fires child_a's own handler then the "
      "ancestor split's, in that order, and never fires sibling "
      "child_b's handler -- the two-instance disambiguation bubbling "
      "exists for");
  CTUI_TEST_ASSERT(changed == 1,
                   "handle_event() aggregates BUBBLE-scope handler "
                   "returns the same way it does for GLOBAL");

  /* child_b (split_children[1]) drops out of the active set; its cached
   * ->parent is left untouched (Design's Phase 3: nothing eagerly clears
   * it, the active check re-evaluates split->count/children live) */
  split_data.count = 1;
  fire_len = 0;
  CTUI_EVENT ev2 = {.type = CTUI_VALUE_CHANGED_EVENT,
                    .scope = CTUI_EVENT_SCOPE_BUBBLE,
                    .ev_source = "src",
                    .event_data = NULL,
                    .origin = &child_b};
  ctui_handle_event(&ev2);
  CTUI_TEST_ASSERT(
      fire_len == 1 && fire_order[0] == 'B',
      "child_b's own handler still fires even though it's no longer an "
      "active split child -- pruning only blocks the walk from reaching "
      "further ancestors, never the origin itself");

  CTUI_EVENT no_origin = {.type = CTUI_VALUE_CHANGED_EVENT,
                          .scope = CTUI_EVENT_SCOPE_BUBBLE,
                          .ev_source = "src",
                          .event_data = NULL,
                          .origin = NULL};
  fire_len = 0;
  changed = ctui_handle_event(&no_origin);
  CTUI_TEST_ASSERT(changed == 0 && fire_len == 0,
                   "a BUBBLE-scope event with no origin is dropped "
                   "(reject-and-log), not dispatched");
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

  test_bubble(&app);

  ctui_app_free(&app);
  return ctui_test_summary();
}
