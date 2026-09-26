/* CTUI_TABLE (widgets/table.h): columns (fixed, shared, right-aligned),
 * the header's sort mark, the cursor and scrolling, marks, mouse, and the
 * value-changed events it emits. */
#include "ctui.h"
#include "widgets/table.h"

#include "ctui_test.h"

#include <stdio.h>
#include <string.h>

static const char *names[] = {"alpha", "beta",  "gamma", "delta",
                              "eps",   "zeta",  "eta",   "theta",
                              "iota",  "kappa", "lambda"};
#define N (int)(sizeof names / sizeof *names)
static int calls; /* cell() calls: only the visible rows */
static char got[32];
static int got_row;

static const char *cell(void *ctx, int row, int col, char *scratch,
                        size_t cap) {
  (void)ctx;
  calls++;
  if (col == 0) {
    return names[row];
  }
  snprintf(scratch, cap, "%d", row * 100);
  return scratch;
}

static unsigned char row_fg(void *ctx, int row) {
  (void)ctx;
  return row == 1 ? CTUI_COLOR_RED : 0;
}

static int on_value(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_VALUE_CHANGED_EVENT_DATA *d = ev->event_data;
  snprintf(got, sizeof got, "%s", d->value);
  got_row = d->enabled;
  return 0;
}

static void click(CTUI_APP *app, CTUI_SCREEN *screen, int row, int col,
                  int button, CTUI_MOUSE_ACTION action) {
  CTUI_MOUSE_EVENT_DATA m = {
      .action = action, .button = button, .row = row, .col = col};
  CTUI_EVENT ev = {.type = CTUI_MOUSE_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "input",
                   .event_data = &m};
  if (ctui_handle_event(&ev)) {
    ctui_app_render(app, screen);
  }
}

int main(void) {
  ctui_log_init(E_ALL);
  int rows = 10, cols = 30;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  CTUI_TABLE_COLUMN columns[] = {{.title = "Name"},
                                 {.title = "Size", .width = 6,
                                  .align_right = 1}};
  unsigned char marks[N] = {0};
  CTUI_TABLE t = {.columns = columns,
                  .column_count = 2,
                  .count = N,
                  .header = 1,
                  .sort_col = 0,
                  .marks = marks,
                  .cell = cell,
                  .row_fg = row_fg};
  /* 20 wide, 6 high at (2, 1): Name gets 20 - 6 - 1 = 13 columns */
  CTUI_WIDGET w = ctui_widget_make(2, 1, 20, 6, &t, ctui_table_render, NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w,
                      ctui_table_handle_keypress);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &w, ctui_table_handle_mouse);
  ctui_event_register("table", CTUI_VALUE_CHANGED_EVENT, &w, on_value);

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "Name ▴") &&
                       ctui_test_row_contains(screen, 1, "  Size"),
                   "header: titles, the sort mark, a right-aligned title");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 2, "alpha") &&
                       ctui_test_cell(screen, 2, 21) == '0' &&
                       ctui_test_cell(screen, 3, 21) == '0' &&
                       ctui_test_cell(screen, 3, 19) == '1',
                   "rows: numbers right-aligned in their column");
  CTUI_TEST_ASSERT(calls == 5 * 2, "cell() only for the 5 rows shown (%d)",
                   calls);
  CTUI_TEST_ASSERT(screen->cells[2 * cols + 2].bg == CTUI_COLOR_CYAN &&
                       screen->cells[2 * cols + 21].bg == CTUI_COLOR_CYAN,
                   "the cursor row is highlighted across");
  CTUI_TEST_ASSERT(screen->cells[3 * cols + 2].fg == CTUI_COLOR_RED,
                   "row_fg colours a row");

  ctui_test_key(&app, screen, CTUI_KEY_END, 0);
  CTUI_TEST_ASSERT(t.selected == N - 1 && t.scroll == N - 5,
                   "end: the last row, scrolled to it (%d, %d)", t.selected,
                   t.scroll);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 6, "lambda") &&
                       !strcmp(got, "moved") && got_row == N - 1,
                   "shown, and reported as moved");
  ctui_test_key(&app, screen, CTUI_KEY_PGUP, 0);
  CTUI_TEST_ASSERT(t.selected == N - 5, "pgup moves a page less one (%d)",
                   t.selected);
  ctui_test_key(&app, screen, CTUI_KEY_HOME, 0);
  CTUI_TEST_ASSERT(t.selected == 0 && t.scroll == 0, "home: back up");
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_UP, 0),
                   "up at the top changes nothing");

  ctui_test_key(&app, screen, CTUI_KEY_CHAR, ' ');
  CTUI_TEST_ASSERT(marks[0] && t.selected == 1 && !strcmp(got, "mark"),
                   "space marks and moves down");
  CTUI_TEST_ASSERT(screen->cells[2 * cols + 2].fg == CTUI_COLOR_YELLOW,
                   "a marked row shows it");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!strcmp(got, "activate") && got_row == 1,
                   "enter activates the cursor row");

  click(&app, screen, 4, 5, 0, CTUI_MOUSE_PRESS);
  CTUI_TEST_ASSERT(t.selected == 2 && !strcmp(got, "moved"),
                   "a click selects a row (%d)", t.selected);
  click(&app, screen, 4, 5, 0, CTUI_MOUSE_PRESS);
  CTUI_TEST_ASSERT(!strcmp(got, "activate") && got_row == 2,
                   "a click on the selected row activates it");
  click(&app, screen, 5, 5, 2, CTUI_MOUSE_PRESS);
  CTUI_TEST_ASSERT(t.selected == 3 && !strcmp(got, "menu"),
                   "a right click selects and asks for the menu");
  click(&app, screen, 1, 20, 0, CTUI_MOUSE_PRESS);
  CTUI_TEST_ASSERT(t.sort_col == 1 && !t.sort_desc && !strcmp(got, "sort") &&
                       got_row == 1,
                   "a header click sorts by that column");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "Size ▴"),
                   "the mark follows");
  click(&app, screen, 1, 20, 0, CTUI_MOUSE_PRESS);
  CTUI_TEST_ASSERT(t.sort_desc && ctui_test_row_contains(screen, 1, "Size ▾"),
                   "again: the other way");
  click(&app, screen, 5, 5, -1, CTUI_MOUSE_SCROLL_DOWN);
  CTUI_TEST_ASSERT(t.selected == 6, "the wheel moves three rows (%d)",
                   t.selected);
  strcpy(got, "none");
  click(&app, screen, 9, 28, 0, CTUI_MOUSE_PRESS);
  CTUI_TEST_ASSERT(!strcmp(got, "none"), "a click outside is ignored");

  t.count = 2;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(t.selected == 1 && t.scroll == 0,
                   "fewer rows: the cursor and scroll are clamped");
  t.count = 0;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0) &&
                       !ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0),
                   "no rows: keys do nothing");

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
