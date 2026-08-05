#ifndef CTUI_INTERNAL_H
#define CTUI_INTERNAL_H

/* private state shared between core translation units. Never included by
 * ctui.h -- nothing outside src/core/ should see this. */

#include "app.h"

#include <signal.h>

/* set (async-signal-safe: just a flag) by handle_sigwinch() in term.c;
 * polled by ctui_input_loop() in input.c */
extern volatile sig_atomic_t g_resize_pending;

/* the currently running app, set by ctui_app_init() (app.c); read by
 * ctui_event_register()/ctui_handle_event() (event.c) so registrations
 * don't need an app pointer threaded through every call */
extern CTUI_APP *g_app;

#endif
