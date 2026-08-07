#ifndef CTUI_WIDGETS_BORDER_H
#define CTUI_WIDGETS_BORDER_H

#include "../ctui.h"

typedef struct {
  CTUI_CELL edge;   /* used along the four sides */
  CTUI_CELL corner; /* used at the four corners; defaults to `edge` via
                     * ctui_border_make(), so a border is a uniform block
                     * unless corner is overridden after construction */

  /* Kitty pixel tier only -- see ctui_border_kitty_gfx_render()'s doc
   * comment for why these have no equivalent in the ANSI16/256/TRUECOLOR
   * text render above: CTUI_CELL.ch is a single byte, so a text-tier
   * corner is always exactly one glyph, and there's no smaller-than-a-
   * cell shape a text renderer can draw. A pixel renderer has neither
   * limit, so rounding and a soft two-tone stroke are opt-in here without
   * touching the text path at all. */
  int rounded; /* 0 = square corners (default), nonzero = rounded */
  unsigned char stroke_r, stroke_g, stroke_b; /* primary stroke color;
                                               * seeded from block_color by
                                               * ctui_border_make() */
  unsigned char accent_r, accent_g, accent_b; /* secondary stroke color,
                                               * only used when dither is
                                               * set; defaults to stroke */
  int dither; /* 0 = solid stroke (default), nonzero = ordered-dither
              * between stroke and accent along the stroke */
} CTUI_BORDER;

/* defaults to a filled block, like a terminal cursor: a plain space cell
 * with `block_color` as its background, used for both edges and corners.
 * Kitty-tier fields default to a solid, square-cornered stroke in
 * block_color's approximate RGB (ctui_gfx_ansi16_rgb()) -- inert unless a
 * caller opts into rounded/dither and registers
 * ctui_border_kitty_gfx_render() via ctui_widget_set_gfx_renderer(). */
CTUI_BORDER ctui_border_make(unsigned char block_color);

/* generic border widget: repeats widget_data (a CTUI_BORDER) around the
 * perimeter of self's w x h box -- edge cell along the sides, corner cell at
 * the four corners. Content meant to be a separate, inset widget so it never
 * shares a cell with the border (see the ctui demo for the pattern). Honors
 * edge/corner's own CTUI_CELL.color_mode (BASIC/256/RGB), dispatching to
 * the matching ctui_widget_putc*() variant -- a caller that builds edge/
 * corner with putc_256/putc_rgb-style fields gets that richer tier drawn
 * automatically, same as any other widget that just says "this cell is
 * $COLOR" and lets the encoding follow from color_mode. */
void ctui_border_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* pixel-perfect outline of self's box via the Kitty graphics protocol:
 * a soft-edged (~1px antialiased), few-pixels-wide stroke, traced around a
 * rounded-rect signed-distance field when `rounded` is
 * set (square corners otherwise), solid `stroke` color or ordered-dithered
 * between `stroke`/`accent` when `dither` is set. Everything outside the
 * stroke band -- interior AND exterior -- stays alpha 0, so whatever the
 * text-tier render() already drew underneath (or whatever content a
 * separately-positioned, inset widget draws inside the border) shows
 * through untouched; this only ever adds a pixel-precise outline on top,
 * never a filled panel. Register via ctui_widget_set_gfx_renderer(widget,
 * CTUI_GFX_KITTY, ctui_border_kitty_gfx_render) only once CTUI_GFX_KITTY is
 * the actually-negotiated mode (same conditional-registration pattern as
 * any other optional top-level Kitty widget -- see docs/protocol.md) so
 * ctui_border_render() stays the fallback everywhere else. image_id is
 * derived internally from the widget's own pointer identity (not stored on
 * CTUI_BORDER), so multiple border widgets sharing one CTUI_BORDER instance
 * (a common pattern -- see examples_apps/demo-advanced's header/main/footer
 * borders) still each transmit and retain their own independent Kitty
 * image rather than overwriting each other's.
 *
 * Recompute + retransmit only happen when self->w/h have changed since the
 * last call for this exact widget (an internal, pointer-keyed cache, not a
 * caller-visible flag) -- a caller with a periodic redraw tick would
 * otherwise pay the full per-pixel cost and retransmit an identical image
 * every single frame, which also reads as a visible flicker each time the
 * terminal recomposites it. This means mutating a shared CTUI_BORDER's
 * rounded/dither/stroke/accent fields at runtime (nothing currently does
 * this -- every caller sets them once, before the first render) won't take
 * effect until the next actual resize; there's no cache-invalidation hook
 * for that today. */
void ctui_border_kitty_gfx_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* removes the Kitty placement ctui_border_kitty_gfx_render() transmitted for
 * border_widget, and forgets its cached geometry so a later re-show
 * retransmits instead of assuming nothing changed -- same "must call
 * explicitly, no implicit cleanup" contract as ctui_gfx_kitty_delete()
 * itself (see gfx.h), just scoped to one border widget's own image id
 * instead of a caller-supplied fixed one. Call this whenever a border
 * widget stops being reached by ctui_widget_dispatch_render() while it
 * might still have a live Kitty placement on screen (a toggled-off pane, a
 * closed panel, ...). No-op if this widget never actually rendered through
 * the Kitty tier. */
void ctui_border_kitty_cleanup(CTUI_WIDGET *border_widget);

#endif
