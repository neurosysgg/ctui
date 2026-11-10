#ifndef CTUI_HOST_H
#define CTUI_HOST_H

/* A ctui app in a window of its own (VK_DESIGN.md layer 3): a Wayland
 * window (src/wl/wl.h), a device presenting to it and src/vk/ painting
 * the frames (present.h: one CTUI_VK_GPU, one CTUI_VK_WIN). ctui_app_run() runs
 * as on a terminal: the window is its input source (keys, the mouse, resizes,
 * the close button ending the loop), and each frame drawn is presented once the
 * loop is about to wait -- only when something changed, so an idle app presents
 * nothing.
 *
 * There's no terminal: ctui_init() isn't called (the app sets the tier:
 * CTUI_GFX_KITTY, kitty's image calls going to the GPU through the graphics
 * sink the host sets, ctui_vk_gfx_sink()), and
 * stdout points at /dev/null while the host is open, so nothing an app
 * or ctui writes for a terminal (mouse modes, a window title) reaches the
 * shell it was started from.
 *
 * Promoted from ctui-mus, its first user, when ctui-wm's panel host
 * wanted the same present path for many surfaces (present.h). */

#include "ctui.h"

#include <stddef.h>
#include <stdint.h>

typedef struct CTUI_HOST CTUI_HOST;

/* the window w x h logical pixels, the device, the painter: *rows / *cols
 * the grid, for ctui_screen_create() and ctui_app_init(). NULL when any
 * of it can't be had (why in err). Logs through ctui's log, which must be
 * open (ctui_log_init()) */
CTUI_HOST *ctui_host_open(const char *title, const char *app_id, int w, int h,
                          int *rows, int *cols, char *err, size_t n);

/* screen's frames to the window and the window's input to
 * ctui_input_loop(), before ctui_app_run() */
void ctui_host_attach(CTUI_HOST *host, CTUI_SCREEN *screen);

/* CTUI_COLOR_DEFAULT's colours (0xRRGGBB), a terminal's own otherwise */
void ctui_host_set_default(CTUI_HOST *host, uint32_t fg, uint32_t bg);

void ctui_host_title(CTUI_HOST *host, const char *title);

/* after ctui_app_run(): the input and sinks given back, the device and
 * the window closed, stdout as it was */
void ctui_host_close(CTUI_HOST *host);

#endif
