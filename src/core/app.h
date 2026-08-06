#ifndef CTUI_APP_H
#define CTUI_APP_H

#include "compositor.h"
#include "event.h"
#include "screen.h"
#include "widget.h"

typedef struct {
  CTUI_WIDGET **widgets;
  int count;
  CTUI_COMPOSITOR *comp;

  /* dynamic array (realloc-grown) of registrations built by
   * ctui_event_register(); walked by ctui_handle_event() to find handlers
   * matching an incoming event's (ev_source, type) */
  CTUI_EVENT_HANDLER *handlers;
  int handler_count;
  int handler_cap;
} CTUI_APP;

/* app / event loop */
/* allocates a rows x cols compositor and binds every widget to its slice of
 * it (see ctui_widget_init()). Also validates every widget's declared
 * supported_gfx_modes (widget.h) against the graphics mode ctui_init()
 * negotiated: a widget that opted into a non-degradable protocol (e.g.
 * CTUI_GFX_KITTY, via ctui_widget_set_gfx_renderer()) that wasn't actually
 * granted has nothing sensible to draw, so this is a hard fail (-1,
 * logged E_ERR) rather than a silent degrade -- the only failure case,
 * same 0/-1 convention as ctui_init(). Ordinary text widgets (the
 * ctui_widget_make() default) always pass regardless of what was
 * negotiated, since text rendering never depends on g_gfx_mode -- see
 * GFX_DESIGN.md's Phase 4. */
int ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count, int rows,
                  int cols);
void ctui_app_free(CTUI_APP *app); /* frees app->comp and app->handlers */
void ctui_app_render(CTUI_APP *app, CTUI_SCREEN *screen);
/* blocks until ESC; tick_ms is passed straight through to
 * ctui_input_loop() -- <= 0 means "block on input only" (unchanged
 * behavior), > 0 also wakes every tick_ms with no input to dispatch a
 * CTUI_TICK_EVENT through the registry, same as any other event */
void ctui_app_run(CTUI_APP *app, CTUI_SCREEN *screen, int tick_ms);

/* handles a terminal resize: reallocates app->comp and screen to rows x
 * cols (ctui_compositor_resize()/ctui_screen_resize()), re-runs
 * ctui_widget_init() for every widget (re-running each widget's optional
 * layout() against the new size, then rebinding buf), then dispatches a
 * CTUI_RESIZE_EVENT through ctui_handle_event() (source "terminal") so
 * registered handlers can react beyond pure geometry. Does not
 * render/flush -- callers still do that afterward. ctui_app_run() calls
 * this automatically on SIGWINCH. */
void ctui_app_resize(CTUI_APP *app, CTUI_SCREEN *screen, int rows, int cols);

#endif
