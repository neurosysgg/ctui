#include "grid.h"

#include <ctype.h>
#include <string.h>

void ctui_grid_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_GRID *grid = self->widget_data;
  ctui_logf(E_INF,
            "[CTUI:GRID] - rendering @ tick %d (%dx%d grid, sel=%d,%d)\n",
            ctui_tick_advance(), grid->cols, grid->rows, grid->sel_row,
            grid->sel_col);

  /* base cell size plus remainder, absorbed into the last row/col -- same
   * convention as ctui_split_layout(), so the grid fills self exactly
   * instead of leaving a dead strip on the right/bottom from integer
   * division */
  int base_w = self->w / grid->cols;
  int rem_w = self->w - base_w * grid->cols;
  int base_h = self->h / grid->rows;
  int rem_h = self->h - base_h * grid->rows;

  int base_row = 0;
  for (int r = 0; r < grid->rows; r++) {
    int cell_h = base_h + (r == grid->rows - 1 ? rem_h : 0);
    int base_col = 0;
    for (int c = 0; c < grid->cols; c++) {
      int cell_w = base_w + (c == grid->cols - 1 ? rem_w : 0);
      CTUI_GRID_ITEM *item = &grid->items[r * grid->cols + c];
      int selected = (r == grid->sel_row && c == grid->sel_col);
      unsigned char fg = selected ? CTUI_COLOR_BLACK : CTUI_COLOR_WHITE;
      unsigned char bg = selected ? CTUI_COLOR_GREEN : CTUI_COLOR_BLUE;

      int cell_inner_w = cell_w > 1 ? cell_w - 1 : cell_w;
      int cell_inner_h = cell_h > 1 ? cell_h - 1 : cell_h;

      for (int row = 0; row < cell_inner_h; row++) {
        for (int col = 0; col < cell_inner_w; col++) {
          ctui_widget_putc(self, comp, base_row + row, base_col + col, ' ',
                           fg, bg);
        }
      }

      int label_len = (int)strlen(item->label);
      int label_row = base_row + (cell_inner_h - 1) / 2;
      int label_col = base_col + (cell_inner_w - label_len) / 2;
      ctui_widget_puts(self, comp, label_row, label_col, item->label, fg, bg);
      base_col += cell_w;
    }
    base_row += cell_h;
  }
}

static void grid_press(CTUI_GRID *grid, int row, int col) {
  CTUI_GRID_ITEM *item = &grid->items[row * grid->cols + col];
  grid->sel_row = row;
  grid->sel_col = col;

  ctui_logf(E_INF, "[CTUI:GRID] - '%s' pressed @ tick %d, emitting value-changed\n",
            item->label, ctui_tick_advance());

  CTUI_VALUE_CHANGED_EVENT_DATA changed = {.value = item->label, .enabled = 0};
  CTUI_EVENT changed_ev = {.type = CTUI_VALUE_CHANGED_EVENT,
                           .scope = CTUI_EVENT_SCOPE_GLOBAL,
                           .ev_source = "grid",
                           .event_data = &changed};
  ctui_handle_event(&changed_ev);
}

int ctui_grid_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_GRID *grid = self->widget_data;
  CTUI_KEYPRESS_EVENT_DATA *kd = ev->event_data;

  ctui_logf(E_INF, "[CTUI:GRID] - keypress %s\n", ctui_keytype_name(kd->type));

  if (kd->type == CTUI_KEY_UP) {
    if (grid->sel_row > 0) {
      grid->sel_row--;
      return 1;
    }
  } else if (kd->type == CTUI_KEY_DOWN) {
    if (grid->sel_row < grid->rows - 1) {
      grid->sel_row++;
      return 1;
    }
  } else if (kd->type == CTUI_KEY_LEFT) {
    if (grid->sel_col > 0) {
      grid->sel_col--;
      return 1;
    }
  } else if (kd->type == CTUI_KEY_RIGHT) {
    if (grid->sel_col < grid->cols - 1) {
      grid->sel_col++;
      return 1;
    }
  } else if (kd->type == CTUI_KEY_ENTER) {
    grid_press(grid, grid->sel_row, grid->sel_col);
    return 1;
  } else if (kd->type == CTUI_KEY_CHAR) {
    /* most terminals send DEL (0x7f) for the physical Backspace key, some
     * send BS (0x08) -- normalize so an item registered under either code
     * matches */
    char ch = kd->ch == 127 ? 8 : kd->ch;
    for (int r = 0; r < grid->rows; r++) {
      for (int c = 0; c < grid->cols; c++) {
        CTUI_GRID_ITEM *item = &grid->items[r * grid->cols + c];
        if (item->code && tolower((unsigned char)item->code) ==
                              tolower((unsigned char)ch)) {
          grid_press(grid, r, c);
          return 1;
        }
      }
    }
  }

  ctui_log(E_INF, "[CTUI:GRID] - keypress not handled\n");
  return 0;
}
