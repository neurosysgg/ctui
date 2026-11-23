#ifndef CTUI_LOOK_PAINT_H
#define CTUI_LOOK_PAINT_H

#include "look.h"

/* The primitives the controls (control.h) are painted with: rectangles,
 * bevelled boxes, chunked fills, role masks and lines on an RGBA buffer.
 * Every call clips to the canvas; only the polyline is anti-aliased (kitty
 * shows the pixels as they are when the image is exactly its cells' size,
 * so a box's edges stay sharp and a slanted line's is smoothed here). */

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

/* a rectangle of the canvas, in px */
typedef struct {
  int x, y, w, h;
} CTUI_LOOK_RECT;

/* an anti-aliased line through n points (xy: x0, y0, x1, y1, ... in px,
 * a pixel's centre at its whole coordinates), width px thick with round
 * ends and joins, in rgb over what's there by how much of each pixel it
 * covers (a whole pixel's centre on it: rgb itself, so a level line on
 * whole rows stays sharp); inside clip (NULL: the canvas). The line is one
 * coverage mask, its joins counted once, blended in one pass. */
void ctui_look_paint_polyline(CTUI_LOOK_CANVAS *c, const float *xy, int n,
                              float width, const CTUI_LOOK_RECT *clip,
                              const unsigned char rgb[3]);

/* an anti-aliased disc of radius r around (cx, cy) (px, as the polyline's),
 * hollow inside r_in (0: whole; a ring between the two), in rgb over what's
 * there by how much of each pixel it covers; inside clip (NULL: the
 * canvas) */
void ctui_look_paint_disc(CTUI_LOOK_CANVAS *c, float cx, float cy, float r,
                          float r_in, const CTUI_LOOK_RECT *clip,
                          const unsigned char rgb[3]);

#endif
