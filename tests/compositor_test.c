/* Exercises core/compositor.c directly -- create/clear/resize/blit -- since
 * every other test only touches the compositor indirectly through
 * ctui_app_render()/ctui_widget_init(). No app/screen wiring needed for
 * create/clear/resize; blit needs a CTUI_SCREEN as its destination. */
#include "ctui.h"

#include "ctui_test.h"

int main(void) {
  ctui_log_init(E_ALL);

  CTUI_COMPOSITOR *comp = ctui_compositor_create(4, 6);
  CTUI_TEST_ASSERT(comp->rows == 4 && comp->cols == 6,
                   "create() sets rows/cols");
  CTUI_TEST_ASSERT(comp->cells[0].ch == ' ' &&
                       comp->cells[4 * 6 - 1].ch == ' ',
                   "create() blanks every cell to a space");

  comp->cells[0].ch = 'X';
  comp->cells[0].fg = CTUI_COLOR_RED;
  comp->cells[0].color_mode = CTUI_COLOR_MODE_256;
  ctui_compositor_clear(comp);
  CTUI_TEST_ASSERT(comp->cells[0].ch == ' ' &&
                       comp->cells[0].fg == CTUI_COLOR_DEFAULT &&
                       comp->cells[0].bg == CTUI_COLOR_DEFAULT,
                   "clear() resets a previously-drawn cell's glyph/colors");
  CTUI_TEST_ASSERT(comp->cells[0].color_mode == CTUI_COLOR_MODE_BASIC,
                   "clear() also resets color_mode back to BASIC, so a "
                   "cell drawn via putc_256/rgb last frame doesn't keep "
                   "that mode if this frame's widget uses plain putc()");

  ctui_compositor_resize(comp, 2, 3);
  CTUI_TEST_ASSERT(comp->rows == 2 && comp->cols == 3,
                   "resize() updates rows/cols");
  CTUI_TEST_ASSERT(comp->cells[0].ch == ' ' &&
                       comp->cells[2 * 3 - 1].ch == ' ',
                   "resize() reallocates a freshly-blanked buffer at the "
                   "new size");

  CTUI_SCREEN *screen = ctui_screen_create(2, 3);
  comp->cells[0].ch = 'A';
  comp->cells[1].ch = 'B';
  ctui_compositor_blit(comp, screen);
  CTUI_TEST_ASSERT(screen->cells[0].ch == 'A' && screen->cells[1].ch == 'B',
                   "blit() copies compositor cells onto the screen's frame");

  CTUI_COMPOSITOR *mismatched = ctui_compositor_create(5, 5);
  mismatched->cells[0].ch = 'Z';
  ctui_compositor_blit(mismatched, screen);
  CTUI_TEST_ASSERT(screen->cells[0].ch == 'A',
                   "blit() rejects (no-ops) a size mismatch between "
                   "compositor and screen instead of copying garbage");

  ctui_compositor_free(mismatched);
  ctui_compositor_free(comp);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
