#ifndef CTUI_WIDGETS_GRID_H
#define CTUI_WIDGETS_GRID_H

#include "../ctui.h"

typedef struct {
  const char *label; /* cell text, drawn centered */
  char code;          /* keyboard shortcut for direct typing, matched
                       * case-insensitively against a CTUI_KEY_CHAR's ch (see
                       * ctui_grid_handle_keypress()); 0 means this item has
                       * none and is only reachable via arrow-key navigation
                       * + Enter */
} CTUI_GRID_ITEM;

typedef struct {
  CTUI_GRID_ITEM *items; /* rows*cols, row-major; caller-owned, not copied */
  int rows, cols;
  int sel_row, sel_col; /* cursor position */
} CTUI_GRID;

/* divides self's w x h evenly into a rows x cols grid of filled-block
 * cells (one cell of gap on the right/bottom of each, for visual
 * separation; remainder from uneven division absorbed into the last
 * row/col, same convention as ctui_split_layout()), label centered in
 * each, cursor cell highlighted -- a calculator's keypad, a grid menu, an
 * on-screen keyboard, anywhere selection needs two dimensions instead of
 * CTUI_MENU/CTUI_LIST's one */
void ctui_grid_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* arrows move the cursor; ENTER "presses" the cursor's item. A
 * CTUI_KEY_CHAR keypress matching some item's `code` presses that item
 * directly (moving the cursor there first), so items can be picked without
 * navigating. Either path emits a CTUI_VALUE_CHANGED_EVENT (source "grid",
 * value = the pressed item's label) via ctui_handle_event(). Register
 * against ("input", CTUI_KEYPRESS_EVENT). */
int ctui_grid_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
