/* CTUI_DIALOG (widgets/dialog.h): the box (centred, fitted to its parts),
 * buttons by key and by click, cancel, an entry inside, a progress bar,
 * and the events. */
#include "ctui.h"
#include "widgets/dialog.h"

#include "ctui_test.h"

#include <stdio.h>
#include <string.h>

static char got[32];
static int got_index = 99;

static int on_value(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_VALUE_CHANGED_EVENT_DATA *d = ev->event_data;
  snprintf(got, sizeof got, "%s", d->value ? d->value : "(null)");
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

static int find_row(CTUI_SCREEN *screen, const char *s) {
  for (int r = 0; r < screen->rows; r++) {
    if (ctui_test_row_contains(screen, r, s)) {
      return r;
    }
  }
  return -1;
}

static int find_col(CTUI_SCREEN *screen, int row, uint32_t ch) {
  for (int c = 0; c < screen->cols; c++) {
    if (ctui_test_cell(screen, row, c) == ch) {
      return c;
    }
  }
  return -1;
}

int main(void) {
  ctui_log_init(E_ALL);
  int rows = 16, cols = 40;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  CTUI_DIALOG d = {.title = "Delete",
                   .text = "Delete 3 files for good? This can't be undone.",
                   .buttons = {"Delete", "Cancel"},
                   .button_count = 2,
                   .focus = 1,
                   .progress = -1,
                   .width = 24};
  CTUI_WIDGET w = ctui_widget_make(0, 0, cols, rows, &d, ctui_dialog_render,
                                   NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w,
                      ctui_dialog_handle_keypress);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &w, ctui_dialog_handle_mouse);
  ctui_event_register("dialog", CTUI_VALUE_CHANGED_EVENT, &w, on_value);

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(find_row(screen, "Delete") < 0, "closed: nothing drawn");
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0),
                   "closed: keys aren't taken");

  d.open = 1;
  ctui_app_render(&app, screen);
  /* 24 inside + 4 = 28 wide at (40-28)/2 = 6; text 2 lines + blank +
   * buttons + frame = 6 high at (16-6)/2 = 5 */
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 5, 6) == 0x256d &&
                       ctui_test_cell(screen, 10, 33) == 0x256f,
                   "the frame is centred and fitted");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 5, " Delete "),
                   "the title sits in the top edge");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 6, "Delete 3 files for good?") &&
                       ctui_test_row_contains(screen, 7, "This can't be undone."),
                   "the text wraps inside");
  int br = find_row(screen, " Cancel ");
  CTUI_TEST_ASSERT(br == 9, "buttons on the last inner row (%d)", br);
  int cc = find_col(screen, br, 'C');
  CTUI_TEST_ASSERT(cc >= 0 && screen->cells[br * cols + cc].bg ==
                                  CTUI_COLOR_CYAN,
                   "the focused button is highlighted");

  ctui_test_key(&app, screen, CTUI_KEY_TAB, 0);
  CTUI_TEST_ASSERT(d.focus == 0, "tab moves the focus (wrapping)");
  ctui_test_key(&app, screen, CTUI_KEY_LEFT, 0);
  CTUI_TEST_ASSERT(d.focus == 1, "so does left without an entry");
  CTUI_TEST_ASSERT(ctui_test_key(&app, screen, CTUI_KEY_CHAR, 'x'),
                   "any key counts as handled while open");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!d.open && !strcmp(got, "Cancel") && got_index == 1,
                   "enter picks the focused button and closes");

  d.open = 1;
  ctui_app_render(&app, screen);
  int dc = find_col(screen, br, 'D');
  click(&app, screen, br, dc);
  CTUI_TEST_ASSERT(!d.open && !strcmp(got, "Delete") && got_index == 0,
                   "a click picks a button (%s)", got);

  d.open = 1;
  ctui_test_key(&app, screen, CTUI_KEY_ESC, 0);
  CTUI_TEST_ASSERT(!d.open && !strcmp(got, "(null)") && got_index == -1,
                   "esc cancels");

  char buf[32] = "";
  CTUI_ENTRY e = {.buf = buf, .cap = sizeof buf};
  ctui_entry_set(&e, "old.txt");
  CTUI_DIALOG r = {.open = 1,
                   .title = "Rename",
                   .buttons = {"Rename", "Cancel"},
                   .button_count = 2,
                   .progress = -1,
                   .entry = &e,
                   .width = 20};
  w.widget_data = &r;
  ctui_app_render(&app, screen);
  /* entry + blank + buttons + frame = 5 rows at 5 */
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 6, "old.txt"),
                   "the entry's line");
  ctui_test_key(&app, screen, CTUI_KEY_LEFT, 0);
  CTUI_TEST_ASSERT(r.focus == 0 && e.cursor == 6,
                   "left edits the entry, not the buttons");
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, 'X');
  CTUI_TEST_ASSERT(!strcmp(buf, "old.txXt"), "typing goes to the entry");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!strcmp(got, "Rename"), "enter picks with an entry too");

  CTUI_DIALOG p = {.open = 1, .title = "Copying", .text = "a.iso",
                   .progress = 50, .width = 20};
  w.widget_data = &p;
  ctui_app_render(&app, screen);
  int pr = find_row(screen, "50%");
  CTUI_TEST_ASSERT(pr >= 0, "the bar shows its percentage");
  int full = 0, empty = 0;
  for (int c = 0; c < cols; c++) {
    full += ctui_test_cell(screen, pr, c) == 0x2588;
    empty += ctui_test_cell(screen, pr, c) == 0x2591;
  }
  CTUI_TEST_ASSERT(full == 7 && empty == 8, "half full (%d, %d)", full, empty);
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0) ||
                       p.open,
                   "no buttons: enter picks nothing");
  ctui_test_key(&app, screen, CTUI_KEY_ESC, 0);
  CTUI_TEST_ASSERT(!p.open, "esc still cancels it");

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
