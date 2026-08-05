#include "ctui.h"
#include "widgets/matrix.h"
#include "widgets/periodic.h"

#include <stdio.h>

/* full-screen digital rain: one CTUI_MATRIX widget the size of the whole
 * terminal, reseeded/advanced by a single periodic timer. See
 * widgets/matrix.h for how a frame turns into falling columns -- it's the
 * same per-cell-hash, no-stored-buffer trick as examples_apps/flicker's
 * CTUI_FLICKER, just with the head/trail geometry layered on top instead
 * of a flat re-randomize every fire. */

static void matrix_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = comp->rows;
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
  ctui_logf(E_INF, "[MATRIX:APP] - startup @ tick %d\n", ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  CTUI_MATRIX matrix_data = ctui_matrix_make(
      CTUI_COLOR_WHITE, CTUI_COLOR_GREEN, CTUI_COLOR_BLACK, 10, 1);

  CTUI_WIDGET matrix_widget = ctui_widget_make(
      0, 0, 0, 0, &matrix_data, ctui_matrix_render, matrix_layout);

  CTUI_WIDGET *widgets[] = {&matrix_widget};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);

  ctui_periodic_register(&matrix_widget, 80, 0, ctui_matrix_handle_timer);

  ctui_logf(E_INF,
            "[MATRIX:APP] - widgets wired @ tick %d, entering event loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 50);
  ctui_logf(E_INF, "[MATRIX:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
