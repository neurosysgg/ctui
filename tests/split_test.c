/* Exercises core/split.c's layout math for all three CTUI_SPLIT_MODE
 * values, driven through the real ctui_widget_init()/ctui_app_render() path
 * (a split's layout() is ctui_split_layout() itself, assigned as the split
 * widget's own layout callback) rather than calling ctui_split_layout()
 * directly, so this also proves the "assign as a widget's layout()"
 * pattern documented in split.h. */
#include "ctui.h"

#include "ctui_test.h"

typedef struct {
  char label;
} PANE;

static void pane_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  PANE *p = self->widget_data;
  ctui_widget_putc(self, comp, 0, 0, p->label, CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
}

static void split_layout_cb(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_split_layout(self, comp);
}

int main(void) {
  ctui_log_init(E_ALL);

  int rows = 10, cols = 10;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  PANE pa = {'a'}, pb = {'b'}, pc = {'c'};
  CTUI_WIDGET child_a =
      ctui_widget_make(0, 0, 0, 0, &pa, pane_render, NULL);
  CTUI_WIDGET child_b =
      ctui_widget_make(0, 0, 0, 0, &pb, pane_render, NULL);
  CTUI_WIDGET child_c =
      ctui_widget_make(0, 0, 0, 0, &pc, pane_render, NULL);
  CTUI_WIDGET *children[] = {&child_a, &child_b, &child_c};

  CTUI_SPLIT split = {.mode = CTUI_SPLIT_V, .children = children, .count = 2};
  CTUI_WIDGET split_w =
      ctui_widget_make(0, 0, 10, 10, &split, ctui_split_render,
                       split_layout_cb);

  CTUI_WIDGET *widgets[] = {&split_w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);

  /* V: 10 rows / 2 children -> 5 and 5, full width each */
  CTUI_TEST_ASSERT(child_a.x == 0 && child_a.y == 0 && child_a.w == 10 &&
                       child_a.h == 5,
                   "SPLIT_V gives the first of 2 children the top half, "
                   "full width");
  CTUI_TEST_ASSERT(child_b.x == 0 && child_b.y == 5 && child_b.w == 10 &&
                       child_b.h == 5,
                   "SPLIT_V gives the second child the bottom half, "
                   "stacked directly below the first");

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 0, 0) == 'a',
                   "SPLIT_V's first child actually renders into its own "
                   "region");
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 5, 0) == 'b',
                   "SPLIT_V's second child renders into its own region, "
                   "not the first's");

  /* switch to 3 children on an 10-row split: base=3, remainder=1 absorbed
   * by the last child */
  split.count = 3;
  ctui_split_layout(&split_w, app.comp);
  CTUI_TEST_ASSERT(child_a.h == 3 && child_b.h == 3 && child_c.h == 4,
                   "SPLIT_V's height remainder (10 rows / 3) is absorbed "
                   "entirely by the last active child");
  CTUI_TEST_ASSERT(child_c.y == 6,
                   "the last child starts right after the second child's "
                   "region ends");

  /* H mode: divide width instead */
  split.mode = CTUI_SPLIT_H;
  split.count = 2;
  ctui_split_layout(&split_w, app.comp);
  CTUI_TEST_ASSERT(child_a.x == 0 && child_a.w == 5 && child_a.h == 10,
                   "SPLIT_H gives the first child the left half, full "
                   "height");
  CTUI_TEST_ASSERT(child_b.x == 5 && child_b.w == 5,
                   "SPLIT_H gives the second child the right half");

  /* GRID mode: 3 panes -> ceil(sqrt(3))=2 cols, 2 rows, row-major */
  split.mode = CTUI_SPLIT_GRID;
  split.count = 3;
  ctui_split_layout(&split_w, app.comp);
  CTUI_TEST_ASSERT(child_a.x == 0 && child_a.y == 0,
                   "GRID places pane 0 at the top-left");
  CTUI_TEST_ASSERT(child_b.x == 5 && child_b.y == 0,
                   "GRID places pane 1 to the right of pane 0 (row-major, "
                   "2 cols for 3 panes)");
  CTUI_TEST_ASSERT(child_c.x == 0 && child_c.y == 5,
                   "GRID wraps pane 2 to the start of the second row");

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
