#include "ctui.h"
#include "widgets/border.h"
#include "widgets/label.h"

#include <stdio.h>

static void border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = comp->rows;
}

static void label_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 1;
  self->w = comp->cols - 2;
  self->h = comp->rows - 2;
}

int main(void) {
  CTUI_GFX_MODE gfx_mode = CTUI_GFX_ANSI16;
  if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  ctui_logf(E_INF, "[HELLO:APP] - startup @ tick %d\n", ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  CTUI_LABEL label_data = {
      .text = "hello, ctui! (ESC to quit)",
      .fg = CTUI_COLOR_CYAN,
      .bg = CTUI_COLOR_DEFAULT,
  };

  CTUI_WIDGET border = ctui_widget_make(0, 0, 0, 0, &border_style,
                                        ctui_border_render, border_layout);
  CTUI_WIDGET label = ctui_widget_make(0, 0, 0, 0, &label_data,
                                       ctui_label_render, label_layout);

  CTUI_WIDGET *widgets[] = {&border, &label};
  CTUI_APP app;
  if (ctui_app_init(&app, widgets, 2, rows, cols) != 0) {
    fprintf(stderr, "failed to init ctui app\n");
    return 1;
  }

  ctui_logf(E_INF, "[HELLO:APP] - widgets wired @ tick %d, entering event loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 0);
  ctui_logf(E_INF, "[HELLO:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
