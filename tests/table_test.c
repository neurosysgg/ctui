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
static int span_row = -1, span_asked_col1;
static char got[32];
static int got_row;

static const char *cell(void *ctx, int row, int col, char *scratch,
                        size_t cap) {
  (void)ctx;
  calls++;
  if (row == span_row) {
    span_asked_col1 |= col != 0;
    return "the gamma group, wide";
  }
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

static int row_span(void *ctx, int row) {
  (void)ctx;
  return row == span_row;
}

static int on_value(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_VALUE_CHANGED_EVENT_DATA *d = ev->event_data;
  snprintf(got, sizeof got, "%s", d->value);
  got_row = d->enabled;
  return 0;
}

/* a style's control hook: '#' down the bar, what was asked kept */
static CTUI_CONTROL bar_asked;
static int bar_row, bar_col, bar_declines;

static int draw_bar(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                    int cols, const CTUI_CONTROL *c, unsigned char bg,
                    void *arg) {
  (void)cols;
  (void)arg;
  if (bar_declines) {
    return 0;
  }
  bar_asked = *c;
  bar_row = row;
  bar_col = col;
  for (int r = 0; r < c->rows; r++) {
    ctui_widget_putc(self, comp, row + r, col, '#', 0, bg);
  }
  return 1;
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
  int sx[2], sw[2];
  ctui_table_column_span(&t, 20, 0, &sx[0], &sw[0]);
  ctui_table_column_span(&t, 20, 1, &sx[1], &sw[1]);
  CTUI_TEST_ASSERT(sx[0] == 0 && sw[0] == 13 && sx[1] == 14 && sw[1] == 6,
                   "the columns' spans, for an app drawing over a cell "
                   "(%d+%d, %d+%d)", sx[0], sw[0], sx[1], sw[1]);

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

  /* a style with a control hook: a scrollbar in the last column, rows 2-6
   * of column 21; 11 rows, 5 shown: a thumb of 2 */
  CTUI_STYLE hooked = ctui_style_default;
  hooked.control = draw_bar;
  t.style = &hooked;
  ctui_test_key(&app, screen, CTUI_KEY_HOME, 0);
  CTUI_TEST_ASSERT(bar_asked.kind == CTUI_CONTROL_SCROLLBAR &&
                       bar_asked.value == 0 && bar_asked.max == N &&
                       bar_asked.span == 5 && bar_asked.rows == 5 &&
                       bar_row == 1 && bar_col == 19,
                   "the hook is asked for the bar: first 0 of %d, 5 shown, "
                   "5 rows at (1, 19) (%d of %d, %d, %d at (%d, %d))",
                   N, bar_asked.value, bar_asked.max, bar_asked.span,
                   bar_asked.rows, bar_row, bar_col);
  CTUI_TEST_ASSERT(ctui_table_width(&t, 20, 6) == 19 &&
                       ctui_test_cell(screen, 3, 20) == '0' &&
                       ctui_test_cell(screen, 2, 21) == '#' &&
                       ctui_test_cell(screen, 6, 21) == '#' &&
                       ctui_test_cell(screen, 1, 21) != '#',
                   "the columns give it their last one, the header row "
                   "stays");
  click(&app, screen, 5, 21, 0, CTUI_MOUSE_PRESS);
  CTUI_TEST_ASSERT(t.scroll == 4 && t.selected == 4 && !strcmp(got, "moved"),
                   "a click under the thumb: a page less one, the cursor "
                   "along (%d, %d)",
                   t.scroll, t.selected);
  click(&app, screen, 1 + 1 + 2, 21, 0, CTUI_MOUSE_PRESS); /* the thumb */
  CTUI_MOUSE_EVENT_DATA away = {.action = CTUI_MOUSE_MOTION, .row = 0};
  CTUI_TEST_ASSERT(ctui_scrollbar_held(&away),
                   "held: a motion anywhere is the bar's");
  click(&app, screen, 6, 21, 0, CTUI_MOUSE_MOTION);
  CTUI_TEST_ASSERT(t.scroll == N - 5 && t.selected == 6,
                   "dragging the thumb to the bottom: the last page (%d, %d)",
                   t.scroll, t.selected);
  click(&app, screen, 0, 5, 0, CTUI_MOUSE_MOTION);
  CTUI_TEST_ASSERT(t.scroll == 0 && t.selected == 0,
                   "the drag goes on above the table: the top (%d, %d)",
                   t.scroll, t.selected);
  click(&app, screen, 0, 5, 0, CTUI_MOUSE_RELEASE);
  click(&app, screen, 6, 21, 0, CTUI_MOUSE_MOTION);
  CTUI_TEST_ASSERT(t.scroll == 0 && !t.bar.dragging &&
                       !ctui_scrollbar_held(&away),
                   "released: motion no longer scrolls");
  click(&app, screen, 1 + 1, 21, 0, CTUI_MOUSE_PRESS);
  away.action = CTUI_MOUSE_PRESS;
  CTUI_TEST_ASSERT(t.bar.dragging && !ctui_scrollbar_held(&away) &&
                       !t.bar.dragging,
                   "a press elsewhere (its release never seen) lets go");
  bar_declines = 1;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 2, 21) == 0x2503 &&
                       ctui_test_cell(screen, 4, 21) == 0x2502,
                   "a hook that declines leaves it to the glyphs");
  bar_declines = 0;
  t.style = NULL;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_table_width(&t, 20, 6) == 20 &&
                       ctui_test_cell(screen, 3, 21) == '0',
                   "no hook: no bar, the full width");
  ctui_table_select(&t, 6);

  /* a spanning row (a group's header): row 2, line 4 */
  span_row = 2;
  t.row_span = row_span;
  ctui_table_select(&t, 0);
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 4, "the gamma group, w") &&
                       !span_asked_col1,
                   "a spanning row: column 0's text across the columns, "
                   "the others not asked");
  CTUI_TEST_ASSERT(screen->cells[4 * cols + 2].fg == ctui_style_default.title_fg &&
                       screen->cells[5 * cols + 2].fg == ctui_style_default.fg,
                   "in the title colour, the rows under it as before");
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(t.selected == 2 && !strcmp(got, "moved") &&
                       screen->cells[4 * cols + 21].bg == CTUI_COLOR_CYAN,
                   "the cursor stops on it, highlighted across");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!strcmp(got, "activate") && got_row == 2,
                   "and activates it, as any row");
  t.row_span = NULL;
  span_row = -1;

  /* 24-bit colours in the style: the slots that have one take it */
  CTUI_STYLE tinted = ctui_style_default;
  tinted.rgb[CTUI_STYLE_FG] = CTUI_STYLE_RGB | 0x102030;
  tinted.rgb[CTUI_STYLE_TITLE] = CTUI_STYLE_RGB | 0x405060;
  tinted.rgb[CTUI_STYLE_SEL_BG] = CTUI_STYLE_RGB | 0x000080;
  tinted.rgb[CTUI_STYLE_SEL_FG] = CTUI_STYLE_RGB | 0xffffff;
  t.style = &tinted;
  int was = t.selected;
  marks[0] = 0;
  ctui_test_key(&app, screen, CTUI_KEY_HOME, 0);
  const CTUI_CELL *head = &screen->cells[1 * cols + 2];
  const CTUI_CELL *cur = &screen->cells[2 * cols + 2];
  const CTUI_CELL *own = &screen->cells[3 * cols + 2];
  const CTUI_CELL *plain = &screen->cells[4 * cols + 2];
  CTUI_TEST_ASSERT(head->color_mode == CTUI_COLOR_MODE_RGB_FG && head->fg_b == 0x60 &&
                       head->bg == CTUI_COLOR_DEFAULT,
                   "rgb: the header in the title's, over the terminal's background");
  CTUI_TEST_ASSERT(cur->color_mode == CTUI_COLOR_MODE_RGB && cur->bg_b == 0x80 &&
                       cur->fg_r == 0xff && screen->cells[2 * cols + 21].bg_b == 0x80,
                   "rgb: the cursor row across, in the selection's");
  CTUI_TEST_ASSERT(plain->color_mode == CTUI_COLOR_MODE_RGB_FG && plain->fg_g == 0x20,
                   "rgb: a plain row in the text's");
  CTUI_TEST_ASSERT(own->color_mode == CTUI_COLOR_MODE_BASIC && own->fg == CTUI_COLOR_RED,
                   "rgb: a row's own colour (row_fg) stays basic");
  tinted.rgb[CTUI_STYLE_SEL_FG] = 0;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(cur->color_mode == CTUI_COLOR_MODE_RGB && cur->bg_b == 0x80 &&
                       cur->fg_r == 0 && cur->fg_g == 0 && cur->fg_b == 0,
                   "rgb: an rgb background takes a basic fg as rgb (black)");
  t.style = NULL;
  ctui_table_select(&t, was);

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
