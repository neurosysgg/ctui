#include "border.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

CTUI_BORDER ctui_border_make(unsigned char block_color) {
  CTUI_CELL block = {.ch = ' ', .fg = CTUI_COLOR_DEFAULT, .bg = block_color};
  unsigned char r, g, b;
  ctui_gfx_ansi16_rgb(block_color, &r, &g, &b);
  ctui_logf(E_INF,
            "[CTUI:BORDER] - creating border @ tick %d (block_color=%d)\n",
            ctui_tick_advance(), block_color);
  return (CTUI_BORDER){.edge = block,
                       .corner = block,
                       .rounded = 0,
                       .stroke_r = r,
                       .stroke_g = g,
                       .stroke_b = b,
                       .accent_r = r,
                       .accent_g = g,
                       .accent_b = b,
                       .dither = 0};
}

/* shared by ctui_border_render()'s edge/side/corner writes below -- picks
 * the ctui_widget_putc*() variant matching cell's own color_mode instead of
 * hardcoding the basic-ANSI one, so a caller that builds edge/corner with
 * richer fields (ctui_widget_putc_256()/putc_rgb()'s own fg256/rgb shape)
 * gets that tier drawn without ctui_border_render() needing to know
 * anything more about it -- same "widget says the color, the encoding
 * follows from color_mode" split the rest of ctui already uses. */
static void border_put_cell(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                            int col, CTUI_CELL cell) {
  switch (cell.color_mode) {
  case CTUI_COLOR_MODE_256:
    ctui_widget_putc_256(self, comp, row, col, cell.ch, cell.fg, cell.bg);
    break;
  case CTUI_COLOR_MODE_RGB:
    ctui_widget_putc_rgb(self, comp, row, col, cell.ch, cell.fg_r, cell.fg_g,
                         cell.fg_b, cell.bg_r, cell.bg_g, cell.bg_b);
    break;
  default:
    ctui_widget_putc(self, comp, row, col, cell.ch, cell.fg, cell.bg);
    break;
  }
}

void ctui_border_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_BORDER *border = self->widget_data;
  ctui_logf(E_INF, "[CTUI:BORDER] - rendering @ tick %d (%dx%d)\n",
            ctui_tick_advance(), self->w, self->h);

  for (int col = 1; col < self->w - 1; col++) {
    border_put_cell(self, comp, 0, col, border->edge);
    border_put_cell(self, comp, self->h - 1, col, border->edge);
  }
  for (int row = 1; row < self->h - 1; row++) {
    border_put_cell(self, comp, row, 0, border->edge);
    border_put_cell(self, comp, row, self->w - 1, border->edge);
  }

  border_put_cell(self, comp, 0, 0, border->corner);
  border_put_cell(self, comp, 0, self->w - 1, border->corner);
  border_put_cell(self, comp, self->h - 1, 0, border->corner);
  border_put_cell(self, comp, self->h - 1, self->w - 1, border->corner);
}

/* assumed pixel size of one terminal character cell -- same stand-in value
 * (and same "no cell-pixel-size query yet" caveat) as ctui-mus's own
 * meter.c uses for its Kitty-tier renderer; kept as a local #define rather
 * than a shared constant since nothing else in core needs it yet. */
#define CTUI_BORDER_PX_PER_COL 8
#define CTUI_BORDER_PX_PER_ROW 16
/* stroke width in pixels, centered on the rounded-rect boundary (half
 * inside, half outside) -- a few pixels reads as a crisp line at this
 * assumed cell size without looking like a filled band. Bumped from the
 * original 3.0: once every tile got its own border too (see ctui-mus's
 * gui/widgets/tile.c), a 3px stroke on an inner tile border read as too
 * thin against the same-weight outer frame right next to it; 5px still
 * reads as a line, not a filled band, at the ~8x16px assumed cell size
 * below. */
#define CTUI_BORDER_STROKE_PX 5.0

/* 4x4 ordered-dither (Bayer) matrix, values 0-15 -- used by the dithered
 * stroke path below to decide stroke-vs-accent per pixel instead of a
 * smooth blend, the classic cheap way to make a two-color transition read
 * as a textured pattern rather than a banding gradient. */
static const int ctui_border_bayer4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static double clamp01(double v) {
  return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* signed distance from (x,y) to a w x h rounded rect of corner radius r,
 * centered at (cx,cy) -- negative inside, positive outside, 0 on the
 * boundary. Standard formula (Inigo Quilez's roundedBox SDF); r=0
 * degenerates to a plain (square-cornered) box SDF, so this is used
 * unconditionally rather than branching rounded vs. not. */
static double rounded_box_sdf(double x, double y, double cx, double cy,
                              double halfw, double halfh, double r) {
  double qx = fabs(x - cx) - (halfw - r);
  double qy = fabs(y - cy) - (halfh - r);
  double outside_x = qx > 0.0 ? qx : 0.0;
  double outside_y = qy > 0.0 ? qy : 0.0;
  double inside = qx > qy ? qx : qy;
  if (inside > 0.0) {
    inside = 0.0;
  }
  return sqrt(outside_x * outside_x + outside_y * outside_y) + inside - r;
}

/* per-widget cache of the last-transmitted geometry, keyed by the widget's
 * own pointer identity -- same reasoning as image_id's derivation below:
 * CTUI_BORDER is commonly shared across several border widgets at once
 * (see border.h's doc comment), so per-instance "did anything actually
 * change" state can't live on it. Flat, realloc-grown, linear-scan array,
 * same shape/reasoning as core/widget.c's own gfx_pending queue -- plenty
 * fast at the scale this ever runs at (a handful of border widgets in one
 * app). Skips ctui_border_kitty_gfx_render()'s whole per-pixel SDF
 * computation *and* the Kitty retransmission when self->w/h haven't
 * changed since the last call -- a caller with a periodic redraw tick
 * (e.g. ctui-mus's 20ms tick) would otherwise recompute and retransmit
 * this widget's full pixel buffer every single frame even though its
 * geometry, and therefore its content, never changes without a resize --
 * wasted CPU, and a visible flicker each time the terminal recomposites
 * an identical image. */
typedef struct {
  const CTUI_WIDGET *widget;
  int w, h;
} ctui_border_kitty_cache_entry;

static ctui_border_kitty_cache_entry *ctui_border_kitty_cache = NULL;
static int ctui_border_kitty_cache_count = 0;
static int ctui_border_kitty_cache_cap = 0;

/* true (and updates the cache) the first time widget is seen or whenever
 * its w/h differ from the last call; false if unchanged, so the caller can
 * skip redrawing entirely. */
static int ctui_border_kitty_geometry_changed(const CTUI_WIDGET *widget) {
  for (int i = 0; i < ctui_border_kitty_cache_count; i++) {
    if (ctui_border_kitty_cache[i].widget == widget) {
      if (ctui_border_kitty_cache[i].w == widget->w &&
          ctui_border_kitty_cache[i].h == widget->h) {
        return 0;
      }
      ctui_border_kitty_cache[i].w = widget->w;
      ctui_border_kitty_cache[i].h = widget->h;
      return 1;
    }
  }
  if (ctui_border_kitty_cache_count == ctui_border_kitty_cache_cap) {
    ctui_border_kitty_cache_cap =
        ctui_border_kitty_cache_cap ? ctui_border_kitty_cache_cap * 2 : 4;
    ctui_border_kitty_cache =
        realloc(ctui_border_kitty_cache,
               sizeof(*ctui_border_kitty_cache) *
                   (size_t)ctui_border_kitty_cache_cap);
  }
  ctui_border_kitty_cache[ctui_border_kitty_cache_count++] =
      (ctui_border_kitty_cache_entry){
          .widget = widget, .w = widget->w, .h = widget->h};
  return 1;
}

/* same derivation ctui_border_kitty_gfx_render() uses below -- pulled out so
 * ctui_border_kitty_cleanup() can compute the exact same id to delete
 * without duplicating the bit-packing by hand. */
static unsigned int ctui_border_kitty_image_id(const CTUI_WIDGET *widget) {
  return 0x80000000u | (unsigned int)(((uintptr_t)widget >> 4) & 0x7fffffffu);
}

void ctui_border_kitty_gfx_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)comp;
  /* w/h collapsed to 0 is the established "not currently visible"
   * convention (docs/protocol.md's "Integrating a non-degradable
   * protocol") -- ctui-mus's border widgets never actually hide today, but
   * every other CTUI_GFX_KITTY widget in the codebase checks defensively. */
  if (self->w <= 0 || self->h <= 0) {
    return;
  }
  if (!ctui_border_kitty_geometry_changed(self)) {
    ctui_logf(E_DBG,
             "[CTUI:BORDER] - kitty gfx_render skipped @ tick %d, geometry "
             "unchanged (%dx%d)\n",
             ctui_tick_advance(), self->w, self->h);
    return;
  }
  CTUI_BORDER *border = self->widget_data;

  int px_w = self->w * CTUI_BORDER_PX_PER_COL;
  int px_h = self->h * CTUI_BORDER_PX_PER_ROW;
  unsigned char *rgba = calloc((size_t)px_w * (size_t)px_h, 4);
  if (!rgba) {
    ctui_log(E_WRN, "[CTUI:BORDER] - kitty gfx_render calloc failed\n");
    return;
  }

  double halfw = (px_w - 1) / 2.0, halfh = (px_h - 1) / 2.0;
  double cx = halfw, cy = halfh;
  double radius = 0.0;
  if (border->rounded) {
    double cell_min = CTUI_BORDER_PX_PER_COL < CTUI_BORDER_PX_PER_ROW
                          ? CTUI_BORDER_PX_PER_COL
                          : CTUI_BORDER_PX_PER_ROW;
    radius = cell_min * 1.5;
    double max_radius = (halfw < halfh ? halfw : halfh) - 1.0;
    if (radius > max_radius) {
      radius = max_radius > 0.0 ? max_radius : 0.0;
    }
  }

  for (int y = 0; y < px_h; y++) {
    for (int x = 0; x < px_w; x++) {
      double dist = rounded_box_sdf(x, y, cx, cy, halfw, halfh, radius);
      /* distance to the stroke band's own boundary (0 = centerline of the
       * stroke); <0 inside the stroke, faded to 0 alpha over ~1px so the
       * outline antialiases instead of aliasing into jagged pixel steps */
      double stroke_d = fabs(dist) - CTUI_BORDER_STROKE_PX * 0.5;
      double alpha = clamp01(1.0 - (stroke_d + 0.5));
      if (alpha <= 0.0) {
        continue; /* rgba already zeroed by calloc -- fully transparent */
      }

      unsigned char r = border->stroke_r, g = border->stroke_g,
                   b = border->stroke_b;
      if (border->dither) {
        double phase = fmod((x + y) / 40.0, 1.0);
        double threshold = (ctui_border_bayer4[x & 3][y & 3] + 0.5) / 16.0;
        if (phase > threshold) {
          r = border->accent_r;
          g = border->accent_g;
          b = border->accent_b;
        }
      }

      unsigned char *p = rgba + ((size_t)y * (size_t)px_w + (size_t)x) * 4;
      p[0] = r;
      p[1] = g;
      p[2] = b;
      p[3] = (unsigned char)(alpha * 255.0);
    }
  }

  /* derived from the widget's own pointer identity rather than stored on
   * CTUI_BORDER -- see border.h's doc comment for why (CTUI_BORDER is
   * commonly shared by several border widgets at once). High bit set to
   * stay clear of the small fixed ids other Kitty widgets use (e.g.
   * meter's/kitty_image's id=1). */
  unsigned int image_id = ctui_border_kitty_image_id(self);

  ctui_logf(E_DBG,
           "[CTUI:BORDER] - kitty gfx_render @ tick %d (row=%d, col=%d, "
           "cells=%dx%d, rounded=%d, dither=%d, id=%u)\n",
           ctui_tick_advance(), self->y + 1, self->x + 1, self->w, self->h,
           border->rounded, border->dither, image_id);
  ctui_gfx_kitty_display(self->y + 1, self->x + 1, self->w, self->h, rgba,
                        px_w, px_h, image_id, 0);
  free(rgba);
}

/* deletes the Kitty placement ctui_border_kitty_gfx_render() last
 * transmitted for this exact widget, same reasoning/convention as every
 * other Kitty-tier widget's own *_kitty_cleanup() (e.g.
 * ctui_oscilloscope_kitty_cleanup()): unlike a colored character cell, the
 * border's outline is a raster overlay independent of the text grid, so it
 * stays on screen even after this widget stops being reached by
 * ctui_widget_dispatch_render() (a tile whose pane got toggled off, a
 * closed panel, ...) -- a caller whose border can become not-currently-
 * visible must call this explicitly when that happens. Also evicts this
 * widget's geometry-cache entry (swap-with-last): leaving a stale entry
 * behind would make ctui_border_kitty_geometry_changed() report "unchanged"
 * the next time this same widget is shown again at the same w/h, skipping
 * retransmission entirely and leaving nothing displayed at an id the
 * terminal was just told to delete. No-op if this widget was never
 * rendered through the Kitty tier (not found in the cache) -- cheap enough
 * to call unconditionally rather than making every caller track whether
 * ctui_border_kitty_gfx_render() ever actually ran for it. */
void ctui_border_kitty_cleanup(CTUI_WIDGET *border_widget) {
  ctui_gfx_kitty_delete(ctui_border_kitty_image_id(border_widget));
  for (int i = 0; i < ctui_border_kitty_cache_count; i++) {
    if (ctui_border_kitty_cache[i].widget == border_widget) {
      ctui_border_kitty_cache[i] =
          ctui_border_kitty_cache[--ctui_border_kitty_cache_count];
      break;
    }
  }
}
