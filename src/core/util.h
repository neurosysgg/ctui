#ifndef CTUI_UTIL_H
#define CTUI_UTIL_H

#include "cell.h"

#include <stddef.h>

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

#endif
