#ifndef CTUI_EVENT_H
#define CTUI_EVENT_H

#include "widget.h"

#include <stdint.h>

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
  CTUI_KEY_HOME,
  CTUI_KEY_END,
  CTUI_KEY_PGUP,
  CTUI_KEY_PGDN,
  CTUI_KEY_INSERT,
  CTUI_KEY_DELETE,
  CTUI_KEY_BACKTAB, /* shift+tab */
} CTUI_KEYTYPE;

/* modifier bits on CTUI_KEYPRESS_EVENT_DATA.mods/CTUI_MOUSE_EVENT_DATA.mods,
 * as far as the terminal reports them: CSI modifier params (ctrl+up =
 * "\x1b[1;5A"), ESC-prefixed bytes (alt+x), and SGR mouse button bits.
 * Plain ctrl+letter still arrives as CTUI_KEY_CHAR with the raw control
 * byte (0x01-0x1a) in ch and no CTRL bit, same as it always has. */
enum {
  CTUI_MOD_SHIFT = 1 << 0,
  CTUI_MOD_ALT = 1 << 1,
  CTUI_MOD_CTRL = 1 << 2,
};

typedef struct {
  CTUI_KEYTYPE type;
  uint32_t ch; /* the typed Unicode codepoint, for CTUI_KEY_CHAR (decoded
                * from however many UTF-8 bytes the terminal sent); plain
                * char comparisons work as-is for ASCII */
  unsigned int mods; /* CTUI_MOD_* */
} CTUI_KEYPRESS_EVENT_DATA;

typedef enum {
  CTUI_MOUSE_PRESS,
  CTUI_MOUSE_RELEASE,
  CTUI_MOUSE_MOTION, /* only reported after ctui_mouse_enable(1) */
  CTUI_MOUSE_SCROLL_UP,
  CTUI_MOUSE_SCROLL_DOWN,
} CTUI_MOUSE_ACTION;

typedef struct {
  CTUI_MOUSE_ACTION action;
  int button; /* 0 left, 1 middle, 2 right; -1 for motion with nothing
               * held and for scroll */
  int row, col; /* 0-based absolute screen cell -- test against a widget
                 * with ctui_widget_contains() */
  unsigned int mods; /* CTUI_MOD_* */
} CTUI_MOUSE_EVENT_DATA;

typedef struct {
  int focused; /* 1 = the terminal window gained keyboard focus, 0 = lost it */
} CTUI_FOCUS_EVENT_DATA;

/* readiness bits for ctui_io_watch() (core/io.h) */
enum {
  CTUI_IO_READ = 1 << 0,
  CTUI_IO_WRITE = 1 << 1,
};

typedef struct {
  int fd;             /* the watched fd that's ready */
  unsigned int ready; /* CTUI_IO_READ/CTUI_IO_WRITE bits that are ready */
} CTUI_IO_EVENT_DATA;

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
  CTUI_FOCUS_EVENT, /* the terminal window gained/lost keyboard focus
                     * (CSI I / CSI O), ev_source "input" -- see
                     * CTUI_FOCUS_EVENT_DATA. Never emitted until the app
                     * opts in with ctui_focus_enable(). Dispatched through
                     * the registry to every listener. */
  CTUI_WIDGET_REDRAW,
  CTUI_RESIZE_EVENT,
  CTUI_TICK_EVENT, /* periodic timer tick; emitted by ctui_app_run() (via
                    * ctui_input_loop()) when no input arrives within its
                    * tick_ms interval. No payload -- event_data is NULL.
                    * ev_source is "timer". See ctui_app_run(). */
  CTUI_TIMER_EVENT, /* fired by ctui_timer_tick() when a registered
                     * CTUI_TIMER (or synchronized group) reaches its
                     * deadline. No payload -- event_data is NULL.
                     * ev_source is "timer", same string CTUI_TICK_EVENT
                     * uses, but dispatched directly to each timer's own
                     * (widget, handler) pair by core/timer.c, not through
                     * ctui_handle_event()'s registry -- see
                     * ctui_timer_register()/
                     * ctui_timer_register_synchronized() in
                     * core/timer.h. */
  CTUI_VALUE_CHANGED_EVENT, /* a widget's value changed; see
                            * CTUI_VALUE_CHANGED_EVENT_DATA. Any widget can
                            * emit one by calling ctui_handle_event() from
                            * within its own handler -- see
                            * ctui_menu_handle_keypress() for the pattern. */
  CTUI_MOUSE_EVENT, /* SGR mouse report, ev_source "input" -- see
                     * CTUI_MOUSE_EVENT_DATA. Never emitted until the app
                     * opts in with ctui_mouse_enable(). Dispatched through
                     * the registry like a keypress, to every listener:
                     * each checks ctui_widget_contains() itself. */
  CTUI_IO_EVENT,    /* a watched fd became ready -- see ctui_io_watch() in
                     * core/io.h. ev_source "io", event_data is a
                     * CTUI_IO_EVENT_DATA. Like CTUI_TIMER_EVENT, dispatched
                     * directly to the watch's own (widget, handler), not
                     * through the registry. */
  CTUI_DUMMY_EVENT,
} CTUI_EVENTTYPE;

typedef enum {
  CTUI_EVENT_SCOPE_GLOBAL, /* every matching (source, type) handler runs,
                           * regardless of origin -- today's behavior,
                           * unchanged. origin is ignored even if set. */
  CTUI_EVENT_SCOPE_BUBBLE, /* only handlers registered on ev->origin
                           * itself, or on one of its ancestors via
                           * CTUI_WIDGET.parent, run -- and only as far up
                           * the chain as every CTUI_SPLIT/CTUI_GROUP step
                           * currently has the child on its active path
                           * (CTUI_WIDGET.is_active_child). Requires
                           * ev->origin to be set; see EVENT_DESIGN.md. */
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
  /* the widget that produced this event, or NULL. Ignored under
   * CTUI_EVENT_SCOPE_GLOBAL. Required under CTUI_EVENT_SCOPE_BUBBLE --
   * an emitter that wants bubble semantics sets this to `self` (see
   * ctui_list_handle_keypress() and the other three CTUI_VALUE_CHANGED_
   * EVENT emit sites for the pattern). See EVENT_DESIGN.md. */
  CTUI_WIDGET *origin;
} CTUI_EVENT;

const char *ctui_keytype_name(CTUI_KEYTYPE type);
const char *ctui_eventtype_name(CTUI_EVENTTYPE type);

/* opaque: one (source, type) -> (widget, handler) registration. Full
 * definition lives in core/event.c -- callers only ever touch entries
 * through ctui_event_register()/ctui_handle_event(), never construct or read
 * one directly. */
typedef struct CTUI_EVENT_HANDLER CTUI_EVENT_HANDLER;

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

/* removes every registration made against widget -- the counterpart for
 * a widget that goes away at runtime (a dismissed notification, a closed
 * popup). Safe to call from inside a handler, including the widget's own:
 * removed entries stop firing immediately, and the registry is compacted
 * once the outermost ctui_handle_event() returns. */
void ctui_event_unregister(CTUI_WIDGET *widget);

/* walks the registry built by ctui_event_register(). Under
 * CTUI_EVENT_SCOPE_GLOBAL, calls handler(widget, ev) for every
 * registration whose (source, type) matches (ev->ev_source, ev->type).
 * Under CTUI_EVENT_SCOPE_BUBBLE (requires ev->origin), instead walks
 * ev->origin, then ev->origin->parent, etc. up to NULL -- pruned early if
 * a step's parent has a non-NULL is_active_child that reports the child
 * isn't currently active -- and at each widget in that (possibly
 * truncated) chain calls handler(widget, ev) for every registration whose
 * (source, type, widget) all match. ev->origin's own handlers always run,
 * regardless of pruning; see EVENT_DESIGN.md for the full design. Returns
 * 1 if any handler returned 1 (a visible change occurred, so the caller
 * should re-render), 0 otherwise. Widgets emit their own events by
 * calling this from within a handler -- see ctui_menu_handle_keypress()
 * for the pattern. */
int ctui_handle_event(CTUI_EVENT *ev);

#endif
