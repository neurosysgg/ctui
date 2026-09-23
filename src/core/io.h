#ifndef CTUI_IO_H
#define CTUI_IO_H

#include "event.h"
#include "widget.h"

#include <sys/select.h>

/* fd watches: lets a widget hook an arbitrary file descriptor (a DBus
 * connection, a unix socket, a child's pipe, an inotify fd) into
 * ctui_app_run()'s one select() loop, alongside stdin and timers -- no
 * threads, no second loop. Same registry shape and ownership rules as
 * core/timer.c: the handler runs on the loop thread, gets a CTUI_IO_EVENT
 * whose event_data is a CTUI_IO_EVENT_DATA, and returns 1 if it changed
 * something visible. Level-triggered: a handler that leaves data unread
 * gets called again on the next loop iteration, so drain what you can. */

/* opaque: one (fd, events) -> (widget, handler) registration */
typedef struct CTUI_IO_WATCH CTUI_IO_WATCH;

/* starts watching fd for events (CTUI_IO_READ and/or CTUI_IO_WRITE, from
 * core/event.h). Requires ctui_app_init() to have run first (same
 * precondition as ctui_timer_register()), since ctui_app_init()/
 * ctui_app_free() reset this registry. The caller keeps owning fd --
 * nothing here ever closes it. Returns NULL (logged) on a negative fd,
 * one past FD_SETSIZE, or an empty events mask. */
CTUI_IO_WATCH *ctui_io_watch(int fd, unsigned int events, CTUI_WIDGET *widget,
                             int (*handler)(CTUI_WIDGET *self,
                                            CTUI_EVENT *ev));

/* replaces watch's events mask, e.g. adding CTUI_IO_WRITE only while an
 * outgoing buffer is non-empty. 0 pauses the watch without removing it. */
void ctui_io_set_events(CTUI_IO_WATCH *watch, unsigned int events);

/* stops and frees watch. Safe from inside any handler, including watch's
 * own: it stops firing immediately and is freed once the current
 * dispatch returns. Close the fd yourself afterwards, not before --
 * select() on a closed fd is an error. */
void ctui_io_unwatch(CTUI_IO_WATCH *watch);

/* core-internal plumbing for ctui_input_loop()/ctui_app_run() -- apps
 * never call these directly */

/* adds every active watch to rfds/wfds, raising *maxfd as needed */
void ctui_io_fill(fd_set *rfds, fd_set *wfds, int *maxfd);
/* first active watch that select() reported ready, filled into *data;
 * returns 0 if none */
int ctui_io_ready(fd_set *rfds, fd_set *wfds, CTUI_IO_EVENT_DATA *data);
/* runs every active watch on data->fd whose events intersect data->ready
 * (the CTUI_IO_EVENT ctui_input_loop() produced); returns 1 if any handler
 * reported a visible change */
int ctui_io_dispatch(CTUI_EVENT *ev);
/* frees every watch; called by ctui_app_init()/ctui_app_free() */
void ctui_io_reset(void);

#endif
