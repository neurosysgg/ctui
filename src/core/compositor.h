#ifndef CTUI_COMPOSITOR_H
#define CTUI_COMPOSITOR_H

#include "cell.h"
#include "screen.h"

typedef struct {
  int rows, cols;   /* full screen output dimensions */
  CTUI_CELL *cells; /* backing store for the whole screen output; each
                     * widget's CTUI_WIDGET.buf is a strided slice into this
                     * array, set up by ctui_widget_init() */
} CTUI_COMPOSITOR;

/* compositor: single backing buffer for the whole screen output, sliced up
 * among widgets by ctui_widget_init() */
CTUI_COMPOSITOR *ctui_compositor_create(int rows, int cols);
void ctui_compositor_free(CTUI_COMPOSITOR *comp);
/* resets every cell to blank (space, default fg/bg). Called once per render
 * pass by ctui_app_render(), before any widget draws -- without this, a
 * cell a widget drew to last frame but doesn't draw to this frame (because
 * it moved, shrank, or a widget was hidden) would keep showing whatever was
 * drawn there previously, since nothing else ever touches it again. */
void ctui_compositor_clear(CTUI_COMPOSITOR *comp);
/* copies comp's cells onto screen's frame-being-built; called once per
 * render pass by ctui_app_render(), after every widget has drawn */
void ctui_compositor_blit(CTUI_COMPOSITOR *comp, CTUI_SCREEN *screen);
/* reallocates comp to rows x cols in place (same CTUI_COMPOSITOR*, new
 * backing storage); every widget's old buf is now dangling until rebound
 * via ctui_widget_init() -- see ctui_app_resize(), which does this for you */
void ctui_compositor_resize(CTUI_COMPOSITOR *comp, int rows, int cols);

#endif
