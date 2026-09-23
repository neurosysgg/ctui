#ifndef CTUI_TIMER_H
#define CTUI_TIMER_H

#include "event.h"
#include "widget.h"

/* opaque: a single (widget, handler) timer registration. Full definition
 * lives in core/timer.c -- callers only ever touch one through the pointer
 * ctui_timer_register()/ctui_timer_register_synchronized() hands back. */
typedef struct CTUI_TIMER CTUI_TIMER;

/* Registers an independent timer: fires handler(widget, ev) roughly every
 * duration_ms, off a countdown owned entirely by this one registration --
 * it never joins (or drifts relative to) any other timer, synchronized or
 * not, even one registered with the same duration_ms. See
 * ctui_timer_register_synchronized() for the grouped alternative. Requires
 * ctui_app_init() to have run first (same precondition as
 * ctui_event_register()); logs and returns NULL otherwise. */
CTUI_TIMER *ctui_timer_register(int duration_ms, CTUI_WIDGET *widget,
                                int (*handler)(CTUI_WIDGET *self,
                                               CTUI_EVENT *ev));

/* Registers a synchronized timer: every timer registered with the same
 * duration_ms is bucketed into one shared group and fires together off a
 * single deadline for the whole group -- so e.g. three widgets all
 * registered at 300ms update in lockstep, frame after frame, instead of
 * drifting apart the way three independently-scheduled 300ms timers
 * eventually would. A different duration_ms gets its own, separate group. */
CTUI_TIMER *ctui_timer_register_synchronized(
    int duration_ms, CTUI_WIDGET *widget,
    int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev));

/* Stops timer from ever firing again and frees it (removing it from its
 * synchronized group, if any; an emptied group is freed too). Safe from
 * inside any timer handler, including timer's own -- it's skipped from
 * that moment on and freed once the current ctui_timer_tick() returns. */
void ctui_timer_cancel(CTUI_TIMER *timer);

/* milliseconds until the earliest pending timer/group deadline (0 if one
 * is already overdue), or -1 if nothing is registered. Lets
 * ctui_input_loop() sleep in select() exactly until the next timer is
 * due, instead of only noticing timers whenever some other event (or
 * tick_ms) happens to wake the loop. */
long ctui_timer_ms_until_due(void);

/* Fires (dispatches a CTUI_TIMER_EVENT, source "timer", directly to) every
 * timer/group whose deadline has elapsed since the last call, then
 * reschedules it duration_ms out from now. Called once per
 * ctui_app_run() loop iteration; ctui_input_loop() wakes the loop at the
 * next deadline (ctui_timer_ms_until_due()), so timers fire on time even
 * with tick_ms <= 0. Returns 1 if any fired
 * handler reported a visible change, 0 otherwise. */
int ctui_timer_tick(void);

/* Frees every registered timer/group and empties the registry. Called by
 * ctui_app_init()/ctui_app_free() -- apps never need to call this
 * themselves. */
void ctui_timer_reset(void);

#endif
