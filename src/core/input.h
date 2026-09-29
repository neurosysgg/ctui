#ifndef CTUI_INPUT_H
#define CTUI_INPUT_H

#include "event.h"

/* blocking; tick_ms <= 0 blocks indefinitely (as before), > 0 emits a
 * CTUI_TICK_EVENT (source "timer") if no input arrives within tick_ms.
 * Returns 0 on EOF/error. */
int ctui_input_loop(CTUI_EVENT *ev, int tick_ms);

/* where ctui_input_loop() gets its events instead of the terminal: a
 * socket of decoded events, a device's reports, or nothing at all (fd
 * -1: only timers, fd watches and ticks wake the loop). next() must not
 * block: it's called with readable 0 before every wait (hand out an event
 * already buffered, don't touch the fd) and with 1 once fd is readable.
 * It fills *ev (event_data pointing at the source's own storage) and
 * returns 1, 0 for nothing (yet), -1 at the end (the loop returns 0). */
typedef struct {
  int fd;
  int (*next)(void *ctx, CTUI_EVENT *ev, int readable);
  void *ctx;
} CTUI_INPUT_SOURCE;

/* NULL: the terminal again. The source is borrowed, not copied. */
void ctui_input_set_source(const CTUI_INPUT_SOURCE *src);

#endif
