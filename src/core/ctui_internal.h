#ifndef CTUI_INTERNAL_H
#define CTUI_INTERNAL_H

/* private state shared between core translation units. Never included by
 * ctui.h -- nothing outside src/core/ should see this. */

#include "app.h"

#include <signal.h>
#include <stddef.h>

/* set (async-signal-safe: just a flag) by handle_sigwinch() in term.c;
 * polled by ctui_input_loop() in input.c */
extern volatile sig_atomic_t g_resize_pending;

/* the currently running app, set by ctui_app_init() (app.c); read by
 * ctui_event_register()/ctui_handle_event() (event.c) so registrations
 * don't need an app pointer threaded through every call */
extern CTUI_APP *g_app;

/* the graphics mode negotiated by ctui_init() (term.c) -- one CTUI_GFX_MODE
 * value, not a bitmask. Not read by ctui_screen_flush() (per-cell
 * color_mode drives emission there, see GFX_DESIGN.md); reserved for
 * Phase 4's future per-widget support validation. */
extern unsigned int g_gfx_mode;

/* hand bytes back to ctui_input_loop() (input.c) after some other core
 * TU has already read them off STDIN. Today's only caller is
 * ctui_gfx_kitty_probe_shm() (gfx.c), which must read STDIN directly to
 * catch the terminal's APC reply and would otherwise swallow anything
 * else that arrived in the same window -- notably a keystroke typed
 * during ctui_init(). Bytes are replayed in order, ahead of anything
 * still unread on the fd. */
void ctui_input_pushback(const char *bytes, size_t len);

#endif
