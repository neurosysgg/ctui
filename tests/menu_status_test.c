/* Exercises real CTUI_MENU + CTUI_STATUS widgets, wired through the actual
 * event registry exactly like an app's main() would -- proves out the
 * tools/ctui_test.h harness against real widget/event logic instead of a
 * toy. See tools/ctui_test.h for what each helper does. */
#include "ctui.h"
#include "widgets/menu.h"
#include "widgets/status.h"

#include "ctui_test.h"

int main(void) {
  ctui_log_init(E_ALL);

  int rows = 24, cols = 80;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  static CTUI_MENU_ITEM items[] = {
      {.label = "alpha", .enabled = 0},
      {.label = "beta", .enabled = 0},
  };
  CTUI_MENU menu_data = {.items = items, .count = 2, .selected = 0};
  CTUI_STATUS status_data = {.text = ""};

  CTUI_WIDGET menu =
      ctui_widget_make(2, 2, 50, 6, &menu_data, ctui_menu_render, NULL);
  CTUI_WIDGET status =
      ctui_widget_make(2, 12, 50, 2, &status_data, ctui_status_render, NULL);

  CTUI_WIDGET *widgets[] = {&menu, &status};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 2, rows, cols);

  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &menu,
                      ctui_menu_handle_keypress);
  ctui_event_register("menu", CTUI_VALUE_CHANGED_EVENT, &status,
                      ctui_status_handle_value_changed);

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 4, "> alpha"),
                   "menu starts with alpha selected");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 13, "(nothing yet)"),
                   "status starts empty");

  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 5, "> beta"),
                   "DOWN moves selection to beta");
  CTUI_TEST_ASSERT(!ctui_test_row_contains(screen, 4, ">"),
                   "alpha no longer shows the cursor");

  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 13, "you picked: beta"),
                   "ENTER on beta updates the status line via the real "
                   "menu -> status event wiring");

  ctui_test_resize(&app, screen, 40, 100);
  CTUI_TEST_ASSERT(screen->rows == 40 && screen->cols == 100,
                   "resize reallocates the screen to the new size");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 13, "you picked: beta"),
                   "status content survives a resize");

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
