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
 * it (see ctui_widget_init()) */
void ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count, int rows,
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
