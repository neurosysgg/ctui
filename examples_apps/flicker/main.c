#include "ctui.h"
#include "widgets/border.h"
#include "widgets/flicker.h"
#include "widgets/periodic.h"

#include <stdio.h>

/* six panes across two rows: the top row's three panes are all registered
 * synchronized at the same duration, so ctui_timer_tick() fires them all
 * off one shared deadline and they visibly reseed in lockstep. The bottom
 * row mixes a second synchronized pair (a different, slower duration --
 * its own separate group) with one independent pane on a third duration
 * that shares no group with anything, so it flickers on its own schedule
 * and never lines up with the other five. */

static void border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = comp->rows;
}

static void content_split_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 1;
  self->w = comp->cols - 2;
  self->h = comp->rows - 2;
  ctui_split_layout(self, comp);
}

int main(void) {
  /* ANSI16 is the mandatory floor -- ctui_init() can only negotiate
   * *down* to it, never below, so this app never needs to inspect
   * gfx_mode again after the call */
  CTUI_GFX_MODE gfx_mode = CTUI_GFX_ANSI16;
  if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  ctui_logf(E_INF, "[FLICKER:APP] - startup @ tick %d\n",
            ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  border_style.corner =
      (CTUI_CELL){.ch = '+', .fg = CTUI_COLOR_YELLOW, .bg = CTUI_COLOR_DEFAULT};
  CTUI_WIDGET border =
      ctui_widget_make(0, 0, 0, 0, &border_style, ctui_border_render,
                       border_layout);

  CTUI_FLICKER flicker_a1 = ctui_flicker_make(CTUI_COLOR_GREEN, CTUI_COLOR_BLACK, 1);
  CTUI_FLICKER flicker_a2 = ctui_flicker_make(CTUI_COLOR_CYAN, CTUI_COLOR_BLACK, 2);
  CTUI_FLICKER flicker_a3 = ctui_flicker_make(CTUI_COLOR_MAGENTA, CTUI_COLOR_BLACK, 3);
  CTUI_FLICKER flicker_b1 = ctui_flicker_make(CTUI_COLOR_YELLOW, CTUI_COLOR_BLACK, 4);
  CTUI_FLICKER flicker_b2 = ctui_flicker_make(CTUI_COLOR_WHITE, CTUI_COLOR_BLACK, 5);
  CTUI_FLICKER flicker_c1 = ctui_flicker_make(CTUI_COLOR_RED, CTUI_COLOR_BLACK, 6);

  CTUI_WIDGET pane_a1 =
      ctui_widget_make(0, 0, 0, 0, &flicker_a1, ctui_flicker_render, NULL);
  CTUI_WIDGET pane_a2 =
      ctui_widget_make(0, 0, 0, 0, &flicker_a2, ctui_flicker_render, NULL);
  CTUI_WIDGET pane_a3 =
      ctui_widget_make(0, 0, 0, 0, &flicker_a3, ctui_flicker_render, NULL);
  CTUI_WIDGET pane_b1 =
      ctui_widget_make(0, 0, 0, 0, &flicker_b1, ctui_flicker_render, NULL);
  CTUI_WIDGET pane_b2 =
      ctui_widget_make(0, 0, 0, 0, &flicker_b2, ctui_flicker_render, NULL);
  CTUI_WIDGET pane_c1 =
      ctui_widget_make(0, 0, 0, 0, &flicker_c1, ctui_flicker_render, NULL);

  CTUI_WIDGET *row1_children[] = {&pane_a1, &pane_a2, &pane_a3};
  CTUI_SPLIT row1_split = {
      .mode = CTUI_SPLIT_H, .children = row1_children, .count = 3};
  CTUI_WIDGET row1_widget = ctui_widget_make(
      0, 0, 0, 0, &row1_split, ctui_split_render, ctui_split_layout);

  CTUI_WIDGET *row2_children[] = {&pane_b1, &pane_b2, &pane_c1};
  CTUI_SPLIT row2_split = {
      .mode = CTUI_SPLIT_H, .children = row2_children, .count = 3};
  CTUI_WIDGET row2_widget = ctui_widget_make(
      0, 0, 0, 0, &row2_split, ctui_split_render, ctui_split_layout);

  CTUI_WIDGET *content_children[] = {&row1_widget, &row2_widget};
  CTUI_SPLIT content_split = {
      .mode = CTUI_SPLIT_V, .children = content_children, .count = 2};
  CTUI_WIDGET content_split_widget = ctui_widget_make(
      0, 0, 0, 0, &content_split, ctui_split_render, content_split_layout);

  CTUI_WIDGET *widgets[] = {&border, &content_split_widget};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 2, rows, cols);

  ctui_periodic_register(&pane_a1, 300, 1, ctui_flicker_handle_timer);
  ctui_periodic_register(&pane_a2, 300, 1, ctui_flicker_handle_timer);
  ctui_periodic_register(&pane_a3, 300, 1, ctui_flicker_handle_timer);
  ctui_periodic_register(&pane_b1, 600, 1, ctui_flicker_handle_timer);
  ctui_periodic_register(&pane_b2, 600, 1, ctui_flicker_handle_timer);
  ctui_periodic_register(&pane_c1, 450, 0, ctui_flicker_handle_timer);

  ctui_logf(E_INF,
            "[FLICKER:APP] - widgets wired @ tick %d, entering event loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 50);
  ctui_logf(E_INF, "[FLICKER:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
