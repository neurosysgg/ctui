#include "dump_palette.h"

#include <string.h>

static const struct {
  unsigned char color;
  const char *name;
} PALETTE[] = {
    {CTUI_COLOR_DEFAULT, "DEFAULT"}, {CTUI_COLOR_BLACK, "BLACK"},
    {CTUI_COLOR_RED, "RED"},         {CTUI_COLOR_GREEN, "GREEN"},
    {CTUI_COLOR_YELLOW, "YELLOW"},   {CTUI_COLOR_BLUE, "BLUE"},
    {CTUI_COLOR_MAGENTA, "MAGENTA"}, {CTUI_COLOR_CYAN, "CYAN"},
    {CTUI_COLOR_WHITE, "WHITE"},
};
#define PALETTE_COUNT ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

void ctui_dump_palette_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_logf(E_INF,
            "[CTUI:DUMP_PALETTE] - rendering @ tick %d (%d colors, %dx%d)\n",
            ctui_tick_advance(), PALETTE_COUNT, self->w, self->h);

  /* fewest rows (>=2) that evenly divide PALETTE_COUNT into columns -- for
   * the current 9 basic colors that's 3x3, but this stays correct if the
   * palette ever grows/shrinks */
  int rows = 2;
  while (rows < PALETTE_COUNT && PALETTE_COUNT % rows != 0) {
    rows++;
  }
  int cols = PALETTE_COUNT / rows;

  /* same even-division-plus-remainder convention as ctui_split_layout()/
   * ctui_grid_render(): base cell size, remainder absorbed into the last
   * row/col so the grid fills self exactly */
  int base_w = self->w / cols;
  int rem_w = self->w - base_w * cols;
  int base_h = self->h / rows;
  int rem_h = self->h - base_h * rows;

  int base_row = 0;
  for (int r = 0; r < rows; r++) {
    int cell_h = base_h + (r == rows - 1 ? rem_h : 0);
    int base_col = 0;
    for (int c = 0; c < cols; c++) {
      int cell_w = base_w + (c == cols - 1 ? rem_w : 0);
      int idx = r * cols + c;
      unsigned char color = PALETTE[idx].color;

      for (int row = 0; row < cell_h; row++) {
        for (int col = 0; col < cell_w; col++) {
          ctui_widget_putc(self, comp, base_row + row, base_col + col, ' ',
                           CTUI_COLOR_DEFAULT, color);
        }
      }

      const char *name = PALETTE[idx].name;
      int name_len = (int)strlen(name);
      int name_row = base_row + cell_h / 2;
      int name_col = base_col + (cell_w - name_len) / 2;
      ctui_widget_puts(self, comp, name_row, name_col, name,
                       CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);

      base_col += cell_w;
    }
    base_row += cell_h;
  }
}
