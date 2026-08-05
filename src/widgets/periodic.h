#ifndef CTUI_WIDGETS_PERIODIC_H
#define CTUI_WIDGETS_PERIODIC_H

#include "../ctui.h"

/* timer glue, nothing else: registers widget against the core timer
 * subsystem so a widget that wants to update itself on a schedule doesn't
 * need its own call site to reach into core/timer.h or choose between
 * ctui_timer_register()/ctui_timer_register_synchronized() itself.
 * synchronized picks ctui_timer_register_synchronized() -- widgets
 * registered with the same duration_ms fire together, off one shared
 * deadline -- vs. ctui_timer_register() for a private countdown that
 * never joins a group. Carries no widget_data or render of its own; what
 * handler actually does on each fire (and what widget_data it reads/
 * writes to do it) is entirely up to the caller -- see
 * src/widgets/flicker.c for a concrete periodic widget built on top of
 * this. */
CTUI_TIMER *ctui_periodic_register(CTUI_WIDGET *widget, int duration_ms,
                                   int synchronized,
                                   int (*handler)(CTUI_WIDGET *self,
                                                  CTUI_EVENT *ev));

#endif
