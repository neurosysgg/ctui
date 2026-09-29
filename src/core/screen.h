#ifndef CTUI_SCREEN_H
#define CTUI_SCREEN_H

#include "cell.h"

#include <stddef.h>
#include <stdint.h>

typedef struct CTUI_SCREEN CTUI_SCREEN;

/* where a screen's frames go instead of the terminal (an OLED, a test, a
 * remote viewer): ctui_screen_flush() calls frame() only when a cell
 * changed, with s->cells the new frame and s->buffer the one this sink
 * last took (all '\0' after a create or resize: redraw everything).
 * Return 0 once taken, -1 to get the same change again next flush. A
 * sink screen never writes the terminal; kitty graphics stay terminal-only
 * (a gfx widget draws its text fallback without ctui_init()). */
typedef struct {
  int (*frame)(void *ctx, const CTUI_SCREEN *s);
} CTUI_SCREEN_SINK;

struct CTUI_SCREEN {
  int rows, cols;
  CTUI_CELL *cells;  /* frame being built */
  CTUI_CELL *buffer; /* buffer currently displayed on screen */
  /* internal -- ctui_screen_flush()'s scratch buffer for the ANSI byte
   * stream, sized once (rows*cols*64 worst case) and reused every frame
   * instead of malloc/free per flush */
  char *out;
  size_t out_cap;
  const CTUI_SCREEN_SINK *sink; /* NULL: the terminal */
  void *sink_ctx;
};

CTUI_SCREEN *ctui_screen_create(int rows, int cols);
void ctui_screen_free(CTUI_SCREEN *s);
/* sends s's frames to sink (NULL: back to the terminal); the next flush
 * redraws everything */
void ctui_screen_set_sink(CTUI_SCREEN *s, const CTUI_SCREEN_SINK *sink,
                          void *ctx);
void ctui_screen_clear(CTUI_SCREEN *s);
void ctui_screen_putc(CTUI_SCREEN *s, int row, int col, uint32_t ch,
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
