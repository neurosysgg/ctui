#ifndef CTUI_H
#define CTUI_H

#include "logger.h" /* for E_DBG/E_WRN/E_INF/E_ERR/E_ALL verbosity flags */

#include <stddef.h>

typedef struct {
  char ch;
  unsigned char fg;
  unsigned char bg;
} CTUI_CELL;

typedef struct {
  int rows, cols;
  CTUI_CELL *cells;  /* frame being built */
  CTUI_CELL *buffer; /* buffer currently displayed on screen */
} CTUI_SCREEN;

typedef struct {
  int rows, cols;   /* full screen output dimensions */
  CTUI_CELL *cells; /* backing store for the whole screen output; each
                     * widget's CTUI_WIDGET.buf is a strided slice into this
                     * array, set up by ctui_widget_init() */
} CTUI_COMPOSITOR;

typedef enum {
  CTUI_KEY_NONE = 0,
  CTUI_KEY_UP,
  CTUI_KEY_DOWN,
  CTUI_KEY_LEFT,
  CTUI_KEY_RIGHT,
  CTUI_KEY_ENTER,
  CTUI_KEY_ESC,
  CTUI_KEY_TAB,
  CTUI_KEY_CHAR,
} CTUI_KEYTYPE;

typedef struct {
  CTUI_KEYTYPE type;
  char ch;
} CTUI_KEYPRESS_EVENT_DATA;

typedef struct {
  int rows, cols; /* new terminal size */
} CTUI_RESIZE_EVENT_DATA;

typedef struct {
  const char *value; /* the new value; lifetime is the emitting widget's
                      * responsibility -- listeners should copy it out, not
                      * retain the pointer past their handler call */
  int enabled;        /* generic on/off flag alongside value, e.g. a menu
                       * item's toggled state -- not every emitter
                       * populates this meaningfully, so treat it as
                       * informational unless you know the specific source
                       * sets it (see ctui_menu_handle_keypress()) */
} CTUI_VALUE_CHANGED_EVENT_DATA;

typedef enum {
  CTUI_KEYPRESS_EVENT,
  CTUI_FOCUS_EVENT,
  CTUI_WIDGET_REDRAW,
  CTUI_RESIZE_EVENT,
  CTUI_TICK_EVENT, /* periodic timer tick; emitted by ctui_app_run() (via
                    * ctui_input_loop()) when no input arrives within its
                    * tick_ms interval. No payload -- event_data is NULL.
                    * ev_source is "timer". See ctui_app_run(). */
  CTUI_VALUE_CHANGED_EVENT, /* a widget's value changed; see
                            * CTUI_VALUE_CHANGED_EVENT_DATA. Any widget can
                            * emit one by calling ctui_handle_event() from
                            * within its own handler -- see
                            * ctui_menu_handle_keypress() for the pattern. */
  CTUI_DUMMY_EVENT,
} CTUI_EVENTTYPE;

typedef enum {
  CTUI_EVENT_SCOPE_GLOBAL,
} CTUI_EVENT_SCOPE;

typedef struct {
  CTUI_EVENTTYPE type;
  CTUI_EVENT_SCOPE scope;
  /* identifies who emitted this event, e.g. "menu" or "input" -- a plain
   * string convention shared between emitter and listener, not derived
   * from any widget pointer/identity. Forms half of the registration key
   * (paired with type) that ctui_event_register()/ctui_handle_event() match
   * on; see those for the full contract. */
  const char *ev_source;
  void *event_data;
} CTUI_EVENT;

typedef struct CTUI_WIDGET CTUI_WIDGET;

struct CTUI_WIDGET {
  int x, y; /* top left position on screen in cells */
  int w, h; /* fixed size in cols/rows */
  void *widget_data;

  /* per-widget tick counter, separate from the global tick counter in
   * ctui.c; advanced via ctui_widget_tick_advance() */
  int ticks;

  /* pointer into the compositor's backing buffer at this widget's (x,y)
   * origin; set by ctui_widget_init(). NULL until then -- ctui_widget_putc()/
   * ctui_widget_puts() reject writes while it's NULL. Strided by the
   * compositor's cols, not this widget's w, since it's a slice of the larger
   * row-major buffer. */
  CTUI_CELL *buf;

  /* optional: recomputes self->x/y/w/h from comp's current rows/cols (e.g.
   * "cols/2"-style dynamic layout). Called by ctui_widget_init() before it
   * binds buf, so simply re-running ctui_widget_init() -- which
   * ctui_app_resize() does for every widget on a terminal resize -- reflows
   * dynamic widgets against the new size. NULL means fixed geometry: the
   * x/y/w/h passed to ctui_widget_make() are left alone. */
  void (*layout)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

  /* render callback, called unconditionally per frame -- must never be
   * NULL. Draws into comp via ctui_widget_putc()/ctui_widget_puts(), using
   * coordinates local to this widget (0,0 = top left), not the
   * compositor's absolute coordinates. */
  void (*render)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);
};

/* Trips if a field is appended to CTUI_WIDGET without updating
 * ctui_widget_make() (ctui.c) to initialize it; a designated initializer
 * silently zero-fills an omitted field instead of erroring, so this struct
 * should always be built via ctui_widget_make() rather than a literal. Does
 * NOT catch a field inserted before `render` -- review ctui_widget_make()
 * by hand if you do that. */
_Static_assert(offsetof(CTUI_WIDGET, render) +
                       sizeof(((CTUI_WIDGET *)0)->render) ==
                   sizeof(CTUI_WIDGET),
               "CTUI_WIDGET changed shape; update ctui_widget_make()");

CTUI_WIDGET ctui_widget_make(int x, int y, int w, int h, void *widget_data,
                             void (*render)(CTUI_WIDGET *self,
                                            CTUI_COMPOSITOR *comp),
                             void (*layout)(CTUI_WIDGET *self,
                                           CTUI_COMPOSITOR *comp));

/* advances widget's own tick counter by one; queried (and logged) by
 * ctui_app_render() immediately before and after that widget's render()
 * call, for later per-widget debugging/perf tuning */
void ctui_widget_tick_advance(CTUI_WIDGET *widget);

/* if widget->layout is set, calls it first to recompute x/y/w/h against
 * comp's current rows/cols. Then binds widget->buf to its (x,y) slice of
 * comp's backing buffer. Called for every widget by ctui_app_init() and
 * again, per widget, by ctui_app_resize(). Logs and leaves widget->buf NULL
 * if the widget's (post-layout) origin falls outside comp. */
void ctui_widget_init(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp);

/* writes into widget's slice of comp, at (row, col) local to the widget (0,0
 * = widget's top left). Rejects (logs + no-op) writes outside the widget's
 * declared w/h, writes past comp's bounds, or widgets never bound via
 * ctui_widget_init(). */
void ctui_widget_putc(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, char ch, unsigned char fg, unsigned char bg);
void ctui_widget_puts(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, const char *str, unsigned char fg,
                      unsigned char bg);

/* string-layout utilities for widget render() callbacks; operate on plain
 * char buffers rather than compositor cells -- callers still push the
 * result through ctui_widget_puts()/ctui_screen_puts() to get it on screen
 * with color. Both return 0 on success, -1 on invalid input (logged via
 * E_WRN). */

/* centers center_str within line in place, using line's current strlen() as
 * the target width (so line is expected to already be padded/allocated to
 * that width by the caller). Pads both sides with fill.ch; fill.fg/fill.bg
 * are accepted for symmetry with other CTUI_CELL-based APIs but unused
 * here, since line is a plain string, not a cell buffer. Fails if
 * center_str is longer than line -- truncate it first with
 * ctui_util_truncate_str() if it might not fit. */
int ctui_util_center_h(char *center_str, char *line, CTUI_CELL fill);

/* truncates str in place to desired chars total, replacing its tail with
 * trunc (e.g. "...", ">>") so the result is exactly desired chars long.
 * No-op if str already fits within desired. Fails if trunc itself is longer
 * than desired. */
int ctui_util_truncate_str(char *str, size_t desired, char *trunc);

typedef struct {
  size_t size;
  char *group_id;
  CTUI_WIDGET *members; /* size-length array of widgets (not pointers); all
                         * members share a single compositor slice -- see
                         * ctui_group_init() */
} CTUI_GROUP;

CTUI_GROUP ctui_group_make(char *group_id, CTUI_WIDGET *members, size_t size);

/* binds every member's buf to the SAME compositor slice, derived from
 * members[0]'s (x,y) via ctui_widget_init() -- members after the first do
 * not get a slice computed from their own (x,y), they just inherit
 * members[0]'s. Members are therefore expected to share (x,y,w,h); drawing
 * them in order (see ctui_group_render()) then naturally layers, since each
 * member's render() writes into the same cells the previous one did. */
void ctui_group_init(CTUI_GROUP *group, CTUI_COMPOSITOR *comp);

/* walks group->members in order, calling each member's render() into the
 * buffer bound by ctui_group_init(). Does not blit -- callers still reach
 * the screen via ctui_app_render() or ctui_compositor_blit(). */
void ctui_group_render(CTUI_GROUP *group, CTUI_COMPOSITOR *comp);

typedef enum {
  CTUI_SPLIT_V, /* stack children top-to-bottom, dividing height */
  CTUI_SPLIT_H, /* arrange children left-to-right, dividing width */
} CTUI_SPLIT_MODE;

typedef struct {
  CTUI_SPLIT_MODE mode;
  CTUI_WIDGET **children; /* count-length array of pointers; caller-owned,
                           * not copied. Entries at/after count are simply
                           * never positioned, bound, or rendered -- grow
                           * count later (e.g. from an event handler) to
                           * reveal more panes without reconstructing
                           * anything. children[] itself may be longer than
                           * count, to hold panes not shown yet. */
  int count;              /* how many of children[] are currently active;
                           * split evenly across self's w (CTUI_SPLIT_H) or
                           * h (CTUI_SPLIT_V), remainder absorbed by the
                           * last active child */
  CTUI_COMPOSITOR *comp;  /* cached by ctui_split_layout() on every call, so
                           * code that changes count on the fly (e.g. an
                           * event handler, outside the normal
                           * ctui_widget_init() path) can re-partition
                           * immediately via
                           * ctui_split_layout(self, split->comp) instead of
                           * waiting for the next resize */
} CTUI_SPLIT;

/* sub-compositor utility: divides self's CURRENT x/y/w/h evenly among
 * widget_data's (a CTUI_SPLIT*) first `count` children and binds each one
 * via ctui_widget_init() -- so children address the SAME real compositor
 * self does, at their own (now-computed) absolute (x,y), rather than any
 * separate/virtual buffer. Does not touch self's own x/y/w/h: assign this
 * directly as a widget's layout() for a split with fixed position/size
 * (still works: children are still (re-)computed and rebound every
 * ctui_widget_init() call, including on resize), or call it at the end of
 * your own layout() if the split's position/size is itself dynamic -- see
 * the ctui demo's main_split_layout() for the latter pattern. */
void ctui_split_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* renders widget_data's (a CTUI_SPLIT*) first `count` children, in order --
 * the split itself draws nothing of its own */
void ctui_split_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* opaque: one (source, type) -> (widget, handler) registration. Full
 * definition lives in ctui.c -- callers only ever touch entries through
 * ctui_event_register()/ctui_handle_event(), never construct or read one
 * directly. */
typedef struct CTUI_EVENT_HANDLER CTUI_EVENT_HANDLER;

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

/* basic ANSI colors */
enum {
  CTUI_COLOR_DEFAULT = 0,
  CTUI_COLOR_BLACK,
  CTUI_COLOR_RED,
  CTUI_COLOR_GREEN,
  CTUI_COLOR_YELLOW,
  CTUI_COLOR_BLUE,
  CTUI_COLOR_MAGENTA,
  CTUI_COLOR_CYAN,
  CTUI_COLOR_WHITE,
};

/* terminal lifecycle; verbosity is a bitmask of E_DBG/E_WRN/E_INF/E_ERR
 * (see logger.h) controlling which ctui_log/ctui_logf calls actually get
 * written */
int ctui_init(int verbosity);
void ctui_shutdown(void);
void ctui_get_termsize(int *rows, int *cols);

/* logging; negative return means the write failed, non-negative is the
 * number of bytes written (0 if filtered out by verbosity). level is
 * exactly one of E_DBG/E_WRN/E_INF/E_ERR (see logger.h) */
int ctui_log(int level, const char *log_str);
int ctui_logf(int level, const char *fmt, ...);
int ctui_tick_advance(void);
const char *ctui_keytype_name(CTUI_KEYTYPE type);
const char *ctui_eventtype_name(CTUI_EVENTTYPE type);

/* screen buffer */
CTUI_SCREEN *ctui_screen_create(int rows, int cols);
void ctui_screen_free(CTUI_SCREEN *s);
void ctui_screen_clear(CTUI_SCREEN *s);
void ctui_screen_putc(CTUI_SCREEN *s, int row, int col, char ch,
                      unsigned char fg, unsigned char bg);
void ctui_screen_puts(CTUI_SCREEN *s, int row, int col, const char *str,
                      unsigned char fg, unsigned char bg);
void ctui_screen_flush(
    CTUI_SCREEN *s); /* diff against prev frame, write only the changes */
/* reallocates s to rows x cols in place (same CTUI_SCREEN*, new backing
 * storage) and forces a full redraw on the next flush. Also clears the
 * real terminal outright, since a shrink could otherwise leave stale
 * content lingering outside the new (smaller) bounds. */
void ctui_screen_resize(CTUI_SCREEN *s, int rows, int cols);

/* compositor: single backing buffer for the whole screen output, sliced up
 * among widgets by ctui_widget_init() */
CTUI_COMPOSITOR *ctui_compositor_create(int rows, int cols);
void ctui_compositor_free(CTUI_COMPOSITOR *comp);
/* resets every cell to blank (space, default fg/bg). Called once per render
 * pass by ctui_app_render(), before any widget draws -- without this, a
 * cell a widget drew to last frame but doesn't draw to this frame (because
 * it moved, shrank, or a widget was hidden) would keep showing whatever was
 * drawn there previously, since nothing else ever touches it again. */
void ctui_compositor_clear(CTUI_COMPOSITOR *comp);
/* copies comp's cells onto screen's frame-being-built; called once per
 * render pass by ctui_app_render(), after every widget has drawn */
void ctui_compositor_blit(CTUI_COMPOSITOR *comp, CTUI_SCREEN *screen);
/* reallocates comp to rows x cols in place (same CTUI_COMPOSITOR*, new
 * backing storage); every widget's old buf is now dangling until rebound
 * via ctui_widget_init() -- see ctui_app_resize(), which does this for you */
void ctui_compositor_resize(CTUI_COMPOSITOR *comp, int rows, int cols);

/* app / event loop */
/* allocates a rows x cols compositor and binds every widget to its slice of
 * it (see ctui_widget_init()) */
void ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count, int rows,
                   int cols);
void ctui_app_free(CTUI_APP *app); /* frees app->comp and app->handlers */
void ctui_app_render(CTUI_APP *app, CTUI_SCREEN *screen);
/* blocking; tick_ms <= 0 blocks indefinitely (as before), > 0 emits a
 * CTUI_TICK_EVENT (source "timer") if no input arrives within tick_ms.
 * Returns 0 on EOF/error. */
int ctui_input_loop(CTUI_EVENT *ev, int tick_ms);
/* blocks until ESC; tick_ms is passed straight through to
 * ctui_input_loop() -- <= 0 means "block on input only" (unchanged
 * behavior), > 0 also wakes every tick_ms with no input to dispatch a
 * CTUI_TICK_EVENT through the registry, same as any other event */
void ctui_app_run(CTUI_APP *app, CTUI_SCREEN *screen, int tick_ms);

/* event handler registry -- addEventListener()-style: widgets no longer
 * implement a catch-all on_event() that checks whether it cares about each
 * incoming event; instead, register a handler for exactly the (source,
 * type) pair you want, and ctui_handle_event() only ever calls handlers
 * that match. Requires ctui_app_init() to have run first (registrations are
 * stored on the current app, tracked the same way g_app is). */

/* registers handler to run against widget whenever ctui_handle_event() sees
 * an event with ev->ev_source equal to source (compared with strcmp) and
 * ev->type equal to type. Multiple handlers can be registered for the same
 * (source, type) pair -- e.g. two different widgets both listening for
 * ("menu", CTUI_VALUE_CHANGED_EVENT) -- and ctui_handle_event() fires all of
 * them, in registration order. source is a plain string convention agreed
 * on between whoever emits events under that name and whoever listens for
 * them (see CTUI_EVENT.ev_source); it is not derived from any widget
 * pointer/identity, so two widget instances of the same kind currently
 * can't be told apart by source alone. */
void ctui_event_register(const char *source, CTUI_EVENTTYPE type,
                         CTUI_WIDGET *widget,
                         int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev));

/* walks the registry built by ctui_event_register(), calling handler(widget,
 * ev) for every registration whose (source, type) matches
 * (ev->ev_source, ev->type). Returns 1 if any handler returned 1 (a visible
 * change occurred, so the caller should re-render), 0 otherwise. Widgets
 * emit their own events by calling this from within a handler -- see
 * ctui_menu_handle_keypress() for the pattern. */
int ctui_handle_event(CTUI_EVENT *ev);

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
