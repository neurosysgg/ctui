#ifndef CTUI_EVENT_H
#define CTUI_EVENT_H

#include "widget.h"

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
