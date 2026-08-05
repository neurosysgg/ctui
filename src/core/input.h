#ifndef CTUI_INPUT_H
#define CTUI_INPUT_H

#include "event.h"

/* blocking; tick_ms <= 0 blocks indefinitely (as before), > 0 emits a
 * CTUI_TICK_EVENT (source "timer") if no input arrives within tick_ms.
 * Returns 0 on EOF/error. */
int ctui_input_loop(CTUI_EVENT *ev, int tick_ms);

#endif
