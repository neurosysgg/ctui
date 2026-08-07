#ifndef CTUI_WIDGETS_KITTY_IMAGE_H
#define CTUI_WIDGETS_KITTY_IMAGE_H

#include "ctui.h"

typedef struct {
  int px_w, px_h;         /* pixel dimensions of rgba below */
  unsigned char *rgba;    /* px_w*px_h*4 bytes, owned -- see
                           * ctui_kitty_image_free() */
  unsigned int image_id;  /* passed to ctui_gfx_kitty_display() so every
                           * redraw replaces the same image in place
                           * instead of layering a new one on top */
} CTUI_KITTY_IMAGE;

/* generates a px_w x px_h RGB diagonal gradient (no external image
 * dependency -- this is a proving-ground widget for the Kitty graphics
 * protocol, not a real image viewer) as an owned, opaque (alpha=255)
 * RGBA buffer. image_id is a caller-chosen nonzero id, stable across this
 * widget's lifetime. */
CTUI_KITTY_IMAGE ctui_kitty_image_make(int px_w, int px_h,
                                       unsigned int image_id);
void ctui_kitty_image_free(CTUI_KITTY_IMAGE *img);

/* text-tier render(), required by ctui_widget_make() but never actually
 * invoked in practice -- ctui_app_init() only lets this widget's app
 * start at all once CTUI_GFX_KITTY was negotiated (see
 * ctui_widget_set_gfx_renderer() in main.c), at which point
 * ctui_app_render()'s Phase 4 dispatch always picks gfx_render below
 * instead. Draws a plain placeholder so the widget still degrades
 * sanely if that invariant is ever loosened. */
void ctui_kitty_image_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* the actual content: transmits/displays widget_data's rgba buffer via
 * ctui_gfx_kitty_display(), scaled to cover self's w x h cells at self's
 * (x,y) compositor origin. Registered via ctui_widget_set_gfx_renderer(),
 * called directly by ctui_app_run() after each frame's screen flush --
 * see core/app.c's render_gfx_widgets(). Skips the transmission entirely
 * (no-op, not even a resend) when this exact widget's rgba pointer, pixel
 * dimensions, image_id, and cell geometry all match the last call -- same
 * "don't retransmit an identical frame every periodic tick" fix as
 * border.c's own cache (PROCESS.md's "Addendum 2"), just keyed on content
 * identity instead of geometry alone, since this widget's pixels (unlike
 * CTUI_BORDER's) aren't derived from geometry at all. A caller that wants
 * a genuinely animated image (changing pixels behind the same pointer)
 * must allocate a fresh rgba buffer per frame rather than mutating one in
 * place, or this cache will wrongly treat it as unchanged. */
void ctui_kitty_image_gfx_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
