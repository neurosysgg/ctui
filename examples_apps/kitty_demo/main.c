#include "ctui.h"
#include "widgets/border.h"
#include "widgets/kitty_image.h"
#include "widgets/label.h"

#include <stdio.h>

static void border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = comp->rows;
}

static void title_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 1;
  self->w = comp->cols - 2;
  self->h = 1;
}

static void image_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 2;
  self->w = comp->cols - 2;
  self->h = comp->rows - 3;
}

int main(void) {
  /* the whole point of this app: request CTUI_GFX_KITTY specifically, not
   * just a text tier. ctui_init() still only hard-fails below the
   * mandatory ANSI16 floor, so a non-kitty terminal negotiates down to
   * its own best text tier instead of failing here -- the actual "kitty
   * or nothing" enforcement for this app's one image widget happens at
   * ctui_app_init() below, since a pixel-graphics widget has no sensible
   * text fallback to degrade to (see GFX_DESIGN.md's Phase 4). */
  CTUI_GFX_MODE gfx_mode = CTUI_GFX_KITTY;
  if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  ctui_logf(E_INF,
            "[KITTY_DEMO:APP] - startup @ tick %d (negotiated gfx mode "
            "0x%x)\n",
            ctui_tick_advance(), (unsigned int)gfx_mode);

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  CTUI_LABEL title = {.text = "ctui - kitty graphics protocol demo",
                      .fg = CTUI_COLOR_CYAN,
                      .bg = CTUI_COLOR_DEFAULT};
  CTUI_KITTY_IMAGE image_data = ctui_kitty_image_make(128, 128, 1);

  CTUI_WIDGET border = ctui_widget_make(0, 0, 0, 0, &border_style,
                                        ctui_border_render, border_layout);
  CTUI_WIDGET title_widget = ctui_widget_make(
      0, 0, 0, 0, &title, ctui_label_render, title_layout);
  CTUI_WIDGET image = ctui_widget_make(
      0, 0, 0, 0, &image_data, ctui_kitty_image_render, image_layout);
  /* narrows image's supported_gfx_modes to just CTUI_GFX_KITTY and points
   * ctui_app_render()'s Phase 4 dispatch at ctui_kitty_image_gfx_render()
   * instead of ctui_kitty_image_render() -- see widget.h. */
  ctui_widget_set_gfx_renderer(&image, CTUI_GFX_KITTY,
                               ctui_kitty_image_gfx_render);

  CTUI_WIDGET *widgets[] = {&border, &title_widget, &image};
  CTUI_APP app;
  if (ctui_app_init(&app, widgets, 3, rows, cols) != 0) {
    fprintf(stderr,
            "failed to init ctui app -- this demo needs a terminal that "
            "actually speaks the Kitty graphics protocol (negotiated gfx "
            "mode was 0x%x, not CTUI_GFX_KITTY); try kitty, wezterm, "
            "konsole, or ghostty\n",
            (unsigned int)gfx_mode);
    ctui_screen_free(screen);
    ctui_shutdown();
    return 1;
  }

  ctui_logf(E_INF,
            "[KITTY_DEMO:APP] - widgets wired @ tick %d, entering event "
            "loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 0);
  ctui_logf(E_INF, "[KITTY_DEMO:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_kitty_image_free(&image_data);
  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
