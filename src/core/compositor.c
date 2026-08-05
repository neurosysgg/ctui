#include "compositor.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

static void compositor_alloc(CTUI_COMPOSITOR *comp, int rows, int cols) {
  comp->rows = rows;
  comp->cols = cols;
  comp->cells = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));

  for (int i = 0; i < rows * cols; i++) {
    comp->cells[i].ch = ' ';
  }
}

CTUI_COMPOSITOR *ctui_compositor_create(int rows, int cols) {
  ctui_logf(E_INF, "[CTUI:COMPOSITOR] - creating %dx%d compositor @ tick %d\n",
            cols, rows, ctui_tick_advance());
  CTUI_COMPOSITOR *comp = malloc(sizeof(CTUI_COMPOSITOR));
  compositor_alloc(comp, rows, cols);
  return comp;
}

void ctui_compositor_free(CTUI_COMPOSITOR *comp) {
  ctui_logf(E_INF, "[CTUI:COMPOSITOR] - freeing %dx%d compositor @ tick %d\n",
            comp->cols, comp->rows, ctui_tick_advance());
  free(comp->cells);
  free(comp);
}

void ctui_compositor_clear(CTUI_COMPOSITOR *comp) {
  ctui_logf(E_DBG, "[CTUI:COMPOSITOR] - clearing %dx%d compositor @ tick %d\n",
            comp->cols, comp->rows, ctui_tick_advance());
  for (int i = 0; i < comp->rows * comp->cols; i++) {
    comp->cells[i].ch = ' ';
    comp->cells[i].fg = CTUI_COLOR_DEFAULT;
    comp->cells[i].bg = CTUI_COLOR_DEFAULT;
    /* a cell drawn via ctui_widget_putc_256()/rgb() last frame must not
     * keep that color_mode if this frame's widget draws it via plain
     * putc() instead -- see GFX_DESIGN.md's "Resolved open questions" */
    comp->cells[i].color_mode = CTUI_COLOR_MODE_BASIC;
  }
}

void ctui_compositor_resize(CTUI_COMPOSITOR *comp, int rows, int cols) {
  ctui_logf(E_INF,
            "[CTUI:COMPOSITOR] - resizing %dx%d -> %dx%d @ tick %d\n",
            comp->cols, comp->rows, cols, rows, ctui_tick_advance());
  free(comp->cells);
  compositor_alloc(comp, rows, cols);
}

void ctui_compositor_blit(CTUI_COMPOSITOR *comp, CTUI_SCREEN *screen) {
  if (comp->rows != screen->rows || comp->cols != screen->cols) {
    ctui_logf(E_WRN,
              "[CTUI:COMPOSITOR] - blit size mismatch @ tick %d (compositor "
              "%dx%d, screen %dx%d)\n",
              ctui_tick_advance(), comp->cols, comp->rows, screen->cols,
              screen->rows);
    return;
  }
  ctui_logf(E_DBG, "[CTUI:COMPOSITOR] - blit @ tick %d\n",
            ctui_tick_advance());
  memcpy(screen->cells, comp->cells,
         sizeof(CTUI_CELL) * (size_t)comp->rows * (size_t)comp->cols);
}
