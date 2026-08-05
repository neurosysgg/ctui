#ifndef CTUI_SCREEN_H
#define CTUI_SCREEN_H

#include "cell.h"

typedef struct {
  int rows, cols;
  CTUI_CELL *cells;  /* frame being built */
  CTUI_CELL *buffer; /* buffer currently displayed on screen */
} CTUI_SCREEN;

CTUI_SCREEN *ctui_screen_create(int rows, int cols);
void ctui_screen_free(CTUI_SCREEN *s);
void ctui_screen_clear(CTUI_SCREEN *s);
void ctui_screen_putc(CTUI_SCREEN *s, int row, int col, char ch,
                      unsigned char fg, unsigned char bg);
void ctui_screen_puts(CTUI_SCREEN *s, int row, int col, const char *str,
                      unsigned char fg, unsigned char bg);
void ctui_screen_flush(
    CTUI_SCREEN *s); /* diff against prev frame, write only the changes */
/* reallocates s to rows x cols in place (same CTUI_SCREEN*, new backing
 * storage) and forces a full redraw on the next flush. Also clears the
 * real terminal outright, since a shrink could otherwise leave stale
 * content lingering outside the new (smaller) bounds. */
void ctui_screen_resize(CTUI_SCREEN *s, int rows, int cols);

#endif
