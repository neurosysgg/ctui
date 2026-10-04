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

  /* 1 (ctui_app_init()'s default) keeps the original behavior: ESC ends
   * ctui_app_run() before any handler sees it. Set to 0 after
   * ctui_app_init() for an app where ESC is an ordinary key (a shell, a
   * launcher) -- it's then dispatched like any other keypress, and the
   * app ends its run loop with ctui_app_quit() instead. */
  int quit_on_esc;
  int quit_requested; /* set by ctui_app_quit(), read by ctui_app_run() */

  /* optional (NULL: none), set after ctui_app_init(): called as
   * ctui_app_render() starts a frame, after each widget's render() -- every
   * widget ctui_widget_dispatch_render() draws, nested ones too, innermost
   * first -- and once the frame is in the compositor. For an app keeping
   * state across a frame's widgets: images placed under text, say, which
   * must go when a later widget draws over their cells. */
  void (*render_begin)(void *arg);
  void (*rendered)(CTUI_WIDGET *w, CTUI_COMPOSITOR *comp, void *arg);
  void (*render_end)(CTUI_COMPOSITOR *comp, void *arg);
  void *render_arg;
} CTUI_APP;

/* app / event loop */
/* allocates a rows x cols compositor and binds every widget to its slice of
 * it (see ctui_widget_init()). Also validates every widget's declared
 * supported_gfx_modes (widget.h) against the graphics mode ctui_init()
 * negotiated: a widget that opted into a non-degradable protocol (e.g.
 * CTUI_GFX_KITTY, via ctui_widget_set_gfx_renderer()) that wasn't actually
 * granted has nothing sensible to draw, so this is a hard fail (-1,
 * logged E_ERR) rather than a silent degrade -- the only failure case,
 * same 0/-1 convention as ctui_init(); a failed init leaves nothing to
 * free (app is unusable). Ordinary text widgets (the
 * ctui_widget_make() default) always pass regardless of what was
 * negotiated, since text rendering never depends on ctui_g_gfx_mode -- see
 * GFX_DESIGN.md's Phase 4. */
int ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count, int rows,
                  int cols);
void ctui_app_free(CTUI_APP *app); /* frees app->comp and app->handlers */
void ctui_app_render(CTUI_APP *app, CTUI_SCREEN *screen);
/* blocks until ESC (see quit_on_esc) or ctui_app_quit(); tick_ms is
 * passed straight through to
 * ctui_input_loop() -- <= 0 means "block on input only" (unchanged
 * behavior), > 0 also wakes every tick_ms with no input to dispatch a
 * CTUI_TICK_EVENT through the registry, same as any other event */
void ctui_app_run(CTUI_APP *app, CTUI_SCREEN *screen, int tick_ms);

/* the run loop again, from inside a handler (a modal dialog: the caller
 * opened one and wants its answer before it returns), until *done is set
 * -- by a handler, a timer or an fd watch -- or the app quits: 1 if done,
 * 0 if it quit (or input ended), and then the ctui_app_run() around it
 * returns too once the handler does. Events reach the same handlers as in
 * ctui_app_run(): the app routes keys to its dialog while it's open. The
 * current frame is drawn first. */
int ctui_app_run_until(CTUI_APP *app, CTUI_SCREEN *screen, int tick_ms,
                       const volatile int *done);

/* asks the running app's ctui_app_run() to return once the event currently
 * being handled finishes (same frame, no further input read). Callable from
 * any handler -- event, timer, or fd watch. */
void ctui_app_quit(void);

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
