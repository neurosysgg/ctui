#ifndef CTUI_TERM_H
#define CTUI_TERM_H

#include "gfx.h"

/* terminal lifecycle; verbosity is a bitmask of E_DBG/E_WRN/E_INF/E_ERR
 * (see logger.h) controlling which ctui_log/ctui_logf calls actually get
 * written. *mode is in/out: set it to the CTUI_GFX_MODE tier the app wants
 * (CTUI_GFX_ANSI16 to just want the mandatory floor) before calling.
 * ctui_init() detects the terminal's actual capabilities; if the
 * requested tier isn't there, it negotiates down -- overwriting *mode
 * with the highest tier the terminal actually supports -- rather than
 * failing, so the caller can decide how to degrade (e.g. skip a
 * 256-color-only widget if *mode reads back CTUI_GFX_ANSI16). Only the
 * mandatory CTUI_GFX_ANSI16 floor itself is a hard failure (-1, logged) --
 * see GFX_DESIGN.md. */
int ctui_init(int verbosity, CTUI_GFX_MODE *mode);
void ctui_shutdown(void);
void ctui_get_termsize(int *rows, int *cols);

#endif
