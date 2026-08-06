#include "ctui.h"
#include "widgets/border.h"
#include "widgets/clock.h"

#include <stdio.h>

static void border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = comp->rows;
}

static void clock_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 1;
  self->w = comp->cols - 2;
  self->h = comp->rows - 2;
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
  ctui_logf(E_INF, "[CLOCK:APP] - startup @ tick %d\n", ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  border_style.corner =
      (CTUI_CELL){.ch = '+', .fg = CTUI_COLOR_YELLOW, .bg = CTUI_COLOR_DEFAULT};
  CTUI_CLOCK clock_data = ctui_clock_make(CTUI_COLOR_CYAN, CTUI_COLOR_DEFAULT);

  CTUI_WIDGET border = ctui_widget_make(0, 0, 0, 0, &border_style,
                                        ctui_border_render, border_layout);
  CTUI_WIDGET clock_widget = ctui_widget_make(
      0, 0, 0, 0, &clock_data, ctui_clock_render, clock_layout);

  CTUI_WIDGET *widgets[] = {&border, &clock_widget};
  CTUI_APP app;
  if (ctui_app_init(&app, widgets, 2, rows, cols) != 0) {
    fprintf(stderr, "failed to init ctui app\n");
    return 1;
  }

  ctui_event_register("timer", CTUI_TICK_EVENT, &clock_widget,
                      ctui_clock_handle_tick);

  ctui_logf(E_INF,
            "[CLOCK:APP] - widgets wired @ tick %d, entering event loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 1000);
  ctui_logf(E_INF, "[CLOCK:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
