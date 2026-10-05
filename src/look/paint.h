#ifndef CTUI_LOOK_PAINT_H
#define CTUI_LOOK_PAINT_H

#include "look.h"

/* The primitives the controls (control.h) are painted with: rectangles,
 * bevelled boxes, chunked fills and role masks on an RGBA buffer. Every
 * call clips to the canvas; nothing is anti-aliased (kitty shows the
 * pixels as they are when the image is exactly its cells' size). */

typedef struct {
  unsigned char *px; /* w x h RGBA, rows top down */
  int w, h;
} CTUI_LOOK_CANVAS;

typedef enum {
  CTUI_LOOK_PAINT_FLAT,   /* no edge */
  CTUI_LOOK_PAINT_RAISED, /* lit top-left, shaded bottom-right */
  CTUI_LOOK_PAINT_SUNKEN, /* the other way round */
} CTUI_LOOK_PAINT_EDGE;

/* a pixel / a rectangle in rgb, opaque */
void ctui_look_paint_px(CTUI_LOOK_CANVAS *c, int x, int y,
                        const unsigned char rgb[3]);
void ctui_look_paint_rect(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                          const unsigned char rgb[3]);

/* a box: an optional 1 px dark outline, then an edge `depth` px deep
 * (0-2: the outer ring highlight/dark, the inner light/shadow, swapped
 * for sunken; a 1 px edge is highlight/shadow), then fill inside (NULL:
 * left as it is). The look's corner cut leaves each corner's pixels
 * alone. The bottom/right edge wins the top-right and bottom-left
 * corners, as in 95. */
void ctui_look_paint_box(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                         const CTUI_LOOK *l, CTUI_LOOK_PAINT_EDGE edge,
                         int depth, int outline, const unsigned char *fill);

/* the first `fill` px of the row of boxes x..x+w: one block, or the
 * look's chunks (chunk px wide, gap px apart; a chunk shows once the
 * fill reaches its middle) */
void ctui_look_paint_fill(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                          int fill, const CTUI_LOOK *l,
                          const unsigned char rgb[3]);

/* a pictogram whose pixels are roles, not colours: w x h chars, rows
 * one after another; '.' is clear, the rest name a role: f face,
 * h highlight, l light, s shadow, d dark, a accent, A accent_text,
 * g groove, x disabled, t text, w warm */
typedef struct {
  int w, h;
  const char *px;
} CTUI_LOOK_MASK;

/* m in the look's colours, scaled by the largest whole factor that fits
 * the box (nearest neighbour) and centred in it; one that doesn't fit at
 * 1x is centred and clipped. off: every opaque pixel in disabled. */
void ctui_look_paint_mask(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                          const CTUI_LOOK_MASK *m, const CTUI_LOOK *l, int off);

#endif
