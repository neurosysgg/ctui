#ifndef CTUI_WIDGETS_MATRIX_H
#define CTUI_WIDGETS_MATRIX_H

#include "ctui.h"

typedef struct {
  unsigned char fg_head, fg_trail, bg;
  int trail_len; /* rows behind the head that still show a (dimmer) glyph
                  * before fading to bg */
  unsigned int seed;  /* distinguishes this instance's column phases/glyphs
                       * from any other CTUI_MATRIX's, same role as
                       * CTUI_FLICKER's seed */
  unsigned int frame; /* advanced once per timer fire; the only mutable
                       * state -- a column's head row and every glyph are
                       * re-derived from (seed, frame, row, col) each
                       * render() rather than stored, so there's no w*h
                       * buffer to keep in sync across resizes, same
                       * approach ctui_flicker_render() uses */
} CTUI_MATRIX;

/* trail_len must be >= 0. Larger values make drops longer (and, since a
 * drop's period is h + trail_len, slower to loop back to the top). */
CTUI_MATRIX ctui_matrix_make(unsigned char fg_head, unsigned char fg_trail,
                             unsigned char bg, int trail_len,
                             unsigned int seed);

/* per column, derives a falling drop's head row from a hash of (seed, col)
 * -- giving each column its own fixed speed (1 or 2 rows/frame) and start
 * phase -- plus the shared frame counter, so drops advance in lockstep
 * with each timer fire but never line up between columns. A cell within
 * trail_len rows behind the head renders a glyph (re-hashed every frame,
 * so it flickers like the ones still ahead of it); anything else renders
 * blank. Head cell uses fg_head, trail cells use fg_trail. */
void ctui_matrix_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* advances frame so the next render() drops every column one step
 * further. Always returns 1 -- every fire is a visible change by design,
 * same convention as ctui_flicker_handle_timer(). Pass this as
 * ctui_periodic_register()'s handler. */
int ctui_matrix_handle_timer(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
