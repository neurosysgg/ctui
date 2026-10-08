/* CTUI_TABS (widgets/tabs.h): a sidebar with headings and a tab bar --
 * selection by key and click, headings skipped, scrolling both ways, and
 * the events. */
#include "ctui.h"
#include "widgets/tabs.h"

#include "ctui_test.h"

#include <stdio.h>
#include <string.h>

static char got[32];
static int got_index = -1;

static int on_value(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_VALUE_CHANGED_EVENT_DATA *d = ev->event_data;
  snprintf(got, sizeof got, "%s", d->value);
  got_index = d->enabled;
  return 0;
}

static void click(CTUI_APP *app, CTUI_SCREEN *screen, int row, int col) {
  CTUI_MOUSE_EVENT_DATA m = {
      .action = CTUI_MOUSE_PRESS, .button = 0, .row = row, .col = col};
  CTUI_EVENT ev = {.type = CTUI_MOUSE_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "input",
                   .event_data = &m};
  if (ctui_handle_event(&ev)) {
    ctui_app_render(app, screen);
  }
}

/* a hook taking tabs and the selection, each call kept */
static CTUI_CONTROL asked[8];
static int asked_row[8], asked_col[8], asked_cols[8], nasked;

static int draw_chrome(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                       int col, int cols, const CTUI_CONTROL *c,
                       unsigned char bg, void *arg) {
  (void)self, (void)comp, (void)bg, (void)arg;
  if (nasked == 8) {
    return 0;
  }
  asked[nasked] = *c;
  asked_row[nasked] = row;
  asked_col[nasked] = col;
  asked_cols[nasked++] = cols;
  return 1;
}

int main(void) {
  ctui_log_init(E_ALL);
  int rows = 8, cols = 40;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  static const CTUI_TABS_ITEM side_items[] = {
      {.label = "Desktop", .heading = 1}, {.label = "ctui-wm"},
      {.label = "Input"},                 {.label = "Hardware", .heading = 1},
      {.label = "Sound"},                 {.label = "Network"},
  };
  CTUI_TABS side = {.items = side_items, .count = 6, .vertical = 1};
  static const CTUI_TABS_ITEM bar_items[] = {
      {.label = "Files"}, {.label = "Sound"}, {.label = "Network"},
      {.label = "Bluetooth"}};
  CTUI_TABS bar = {.items = bar_items, .count = 4};
  CTUI_WIDGET sw = ctui_widget_make(0, 1, 12, 6, &side, ctui_tabs_render,
                                    NULL);
  CTUI_WIDGET bw = ctui_widget_make(14, 0, 26, 1, &bar, ctui_tabs_render,
                                    NULL);
  CTUI_WIDGET *widgets[] = {&sw, &bw};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 2, rows, cols);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &sw,
                      ctui_tabs_handle_keypress);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &sw, ctui_tabs_handle_mouse);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &bw, ctui_tabs_handle_mouse);
  ctui_event_register("tabs", CTUI_VALUE_CHANGED_EVENT, &sw, on_value);

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(side.selected == 1, "a heading is never selected");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "Desktop") &&
                       ctui_test_row_contains(screen, 2, " ctui-wm") &&
                       screen->cells[2 * cols + 11].bg == CTUI_COLOR_CYAN,
                   "sidebar: heading, the selection highlighted across");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 0, " Files │ Sound │"),
                   "bar: labels and separators");

  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(side.selected == 4 && !strcmp(got, "moved") &&
                       got_index == 4,
                   "down skips a heading (%d)", side.selected);
  ctui_test_key(&app, screen, CTUI_KEY_END, 0);
  CTUI_TEST_ASSERT(side.selected == 5, "end: the last");
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_END, 0),
                   "end again: nothing");
  ctui_test_key(&app, screen, CTUI_KEY_HOME, 0);
  CTUI_TEST_ASSERT(side.selected == 1, "home: the first selectable");
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_UP, 0),
                   "up past the first: nothing");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!strcmp(got, "activate") && got_index == 1,
                   "enter activates");

  click(&app, screen, 3, 4);
  CTUI_TEST_ASSERT(side.selected == 2, "a click selects");
  click(&app, screen, 4, 4);
  CTUI_TEST_ASSERT(side.selected == 2, "a click on a heading doesn't");
  CTUI_MOUSE_EVENT_DATA right = {
      .action = CTUI_MOUSE_PRESS, .button = 2, .row = 5, .col = 4};
  CTUI_TEST_ASSERT(
      ctui_tabs_mouse(&side, &sw, &right) == CTUI_TABS_MENU &&
          side.menu_item == 4 && side.selected == 2,
      "a right click asks for that item's menu, the selection kept");
  right.row = 4;
  side.menu_item = -1;
  CTUI_TEST_ASSERT(ctui_tabs_mouse(&side, &sw, &right) == CTUI_TABS_NONE &&
                       side.menu_item == -1,
                   "a right click on a heading: nothing");

  sw.h = 2;
  side.selected = 5;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(side.scroll == 4 &&
                       ctui_test_row_contains(screen, 2, "Network"),
                   "short: scrolled to the selection (%d)", side.scroll);
  side.selected = 4;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(side.scroll == 3 &&
                       ctui_test_row_contains(screen, 1, "Hardware"),
                   "scrolling up brings the heading along");

  click(&app, screen, 0, 14 + 9);
  CTUI_TEST_ASSERT(bar.selected == 1, "bar: a click selects (%d)",
                   bar.selected);
  bar.selected = 3; /* " Bluetooth " doesn't fit after the first three */
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(bar.scroll > 0 &&
                       ctui_test_row_contains(screen, 0, " Bluetooth "),
                   "bar: scrolled to the selection (%d)", bar.scroll);
  CTUI_KEYPRESS_EVENT_DATA left = {.type = CTUI_KEY_LEFT};
  CTUI_TEST_ASSERT(ctui_tabs_key(&bar, &left) == CTUI_TABS_MOVED &&
                       bar.selected == 2,
                   "bar: left moves");

  /* a hook: the sidebar's selection and the bar's tabs asked */
  CTUI_STYLE chrome = ctui_style_default;
  chrome.control = draw_chrome;
  side.style = bar.style = &chrome;
  sw.h = 6;
  side.selected = 2;
  side.scroll = 0;
  bar.selected = 1;
  bar.scroll = 0;
  nasked = 0;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(nasked == 4 && asked[0].kind == CTUI_CONTROL_SELECTION &&
                       asked_row[0] == 2 && asked_col[0] == 0 &&
                       asked_cols[0] == 12 &&
                       screen->cells[3 * cols + 2].bg == CTUI_COLOR_DEFAULT,
                   "sidebar: the selection asked across, its label plain "
                   "(%d; %d at %d+%d)",
                   nasked, asked_row[0], asked_col[0], asked_cols[0]);
  CTUI_TEST_ASSERT(asked[1].kind == CTUI_CONTROL_TAB && asked[1].value == 0 &&
                       asked_col[1] == 0 && asked_cols[1] == 7 &&
                       asked[2].value == 1 && asked_col[2] == 8 &&
                       asked_cols[2] == 7 && asked[3].value == 0,
                   "bar: a tab per label, the shown one says so (%d+%d, "
                   "%d+%d)",
                   asked_col[1], asked_cols[1], asked_col[2], asked_cols[2]);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 0, " Files   Sound   ") &&
                       screen->cells[0 * cols + 14 + 9].bg ==
                           CTUI_COLOR_DEFAULT,
                   "bar: a gap for the separators, the shown label plain");

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
