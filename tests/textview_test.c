/* CTUI_TEXTVIEW (widgets/textview.h): the line index, word-wrapped and
 * unwrapped drawing, tabs and control bytes, scrolling both ways, end,
 * and a resize under a wrapped view. */
#include "ctui.h"
#include "widgets/textview.h"

#include "ctui_test.h"

#include <string.h>

static int key(CTUI_APP *app, CTUI_SCREEN *screen, CTUI_KEYTYPE type) {
  return ctui_test_key(app, screen, type, 0);
}

int main(void) {
  ctui_log_init(E_ALL);

  CTUI_TEXTVIEW idx = {0};
  const char *t = "a\r\nbb\n\nccc\n";
  ctui_textview_set(&idx, t, strlen(t));
  CTUI_TEST_ASSERT(idx.line_count == 4,
                   "a last newline ends a line, starts none (%d)",
                   idx.line_count);
  ctui_textview_set(&idx, "x", 1);
  CTUI_TEST_ASSERT(idx.line_count == 1, "no newline: one line");
  ctui_textview_set(&idx, "", 0);
  CTUI_TEST_ASSERT(idx.line_count == 0, "empty: no lines");
  ctui_textview_free(&idx);

  int rows = 6, cols = 20;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  const char *text = "0123456789abcdefghij\n"   /* 2 rows at 10 */
                     "tab\there\x01!\r\n"       /* one, tab + control */
                     "日本語日本語\n"           /* 12 columns: 2 rows */
                     "l3\nl4\nl5\nl6\nl7\n";
  CTUI_TEXTVIEW tv = {.wrap = 1};
  ctui_textview_set(&tv, text, strlen(text));
  CTUI_WIDGET w = ctui_widget_make(1, 1, 10, 4, &tv, ctui_textview_render,
                                   NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w,
                      ctui_textview_handle_keypress);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &w,
                      ctui_textview_handle_mouse);

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "0123456789") &&
                       ctui_test_row_contains(screen, 2, "abcdefghij"),
                   "a long line wraps");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 3, "tab") &&
                       !ctui_test_row_contains(screen, 3, "h"),
                   "a line breaks at a blank (a tab here)");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 4, "here?!"),
                   "a control byte shows as ?, the \\r is gone");

  key(&app, screen, CTUI_KEY_DOWN);
  CTUI_TEST_ASSERT(tv.top == 0 && tv.top_row == 1 &&
                       ctui_test_row_contains(screen, 1, "abcdefghij"),
                   "down moves a row inside a wrapped line");
  key(&app, screen, CTUI_KEY_DOWN);
  CTUI_TEST_ASSERT(tv.top == 1 && tv.top_row == 0, "then to the next line");
  key(&app, screen, CTUI_KEY_UP);
  CTUI_TEST_ASSERT(tv.top == 0 && tv.top_row == 1,
                   "up goes back into the last row of a line");
  key(&app, screen, CTUI_KEY_END);
  /* rows: 2 + 2 + 2 + 5 = 11; the last 4: l4..l7 */
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 4, "l7") &&
                       ctui_test_row_contains(screen, 1, "l4"),
                   "end: the last row at the bottom");
  CTUI_TEST_ASSERT(!key(&app, screen, CTUI_KEY_DOWN),
                   "no scrolling past the end");
  key(&app, screen, CTUI_KEY_PGUP);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "日本語日本") ||
                       ctui_test_row_contains(screen, 1, "語日本語"),
                   "pgup: a page less one row back");
  key(&app, screen, CTUI_KEY_HOME);
  CTUI_TEST_ASSERT(tv.top == 0 && tv.top_row == 0, "home: the top");
  CTUI_TEST_ASSERT(!key(&app, screen, CTUI_KEY_RIGHT),
                   "wrapped: no sideways scroll");

  tv.wrap = 0;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "0123456789") &&
                       ctui_test_row_contains(screen, 2, "tab     he"),
                   "unwrapped: one row per line");
  key(&app, screen, CTUI_KEY_RIGHT);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "89abcdefgh") &&
                       ctui_test_row_contains(screen, 2, "here?!"),
                   "right scrolls 8 columns");
  tv.hscroll = 9; /* 本 on columns 8-9: cut */
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 3, 1) == ' ' &&
                       ctui_test_cell(screen, 3, 2) == 0x8a9e,
                   "a wide glyph cut by the edge is a blank (語 after it)");
  key(&app, screen, CTUI_KEY_LEFT);
  CTUI_TEST_ASSERT(tv.hscroll == 1, "left scrolls back 8 (%d)", tv.hscroll);
  key(&app, screen, CTUI_KEY_LEFT);
  CTUI_TEST_ASSERT(tv.hscroll == 0 && !key(&app, screen, CTUI_KEY_LEFT),
                   "down to column 0, no further");
  key(&app, screen, CTUI_KEY_END);
  CTUI_TEST_ASSERT(tv.top == 4, "unwrapped end: the last page (%d)", tv.top);

  CTUI_MOUSE_EVENT_DATA m = {.action = CTUI_MOUSE_SCROLL_UP, .button = -1,
                             .row = 2, .col = 2};
  CTUI_EVENT ev = {.type = CTUI_MOUSE_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "input",
                   .event_data = &m};
  ctui_handle_event(&ev);
  CTUI_TEST_ASSERT(tv.top == 1, "the wheel scrolls three (%d)", tv.top);

  tv.wrap = 1;
  tv.top = 0;
  tv.top_row = 1;
  ctui_test_resize(&app, screen, 6, 30);
  w.w = 25;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(tv.top_row == 0 &&
                       ctui_test_row_contains(screen, 1, "0123456789abcdefghij"),
                   "wider: a row gone from under the view is clamped");

  ctui_textview_free(&tv);
  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
