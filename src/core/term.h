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

/* opts into SGR mouse reporting (CTUI_MOUSE_EVENT, core/event.h): presses,
 * releases and wheel always; pointer motion too when track_motion is
 * nonzero (including with no button held -- chatty, only ask for it if
 * something hovers). Call after ctui_init(). Turns off the terminal's own
 * click-to-select while active (kitty: hold shift to select anyway);
 * ctui_shutdown() turns it back off. */
void ctui_mouse_enable(int track_motion);

/* opts into focus reporting (CTUI_FOCUS_EVENT, core/event.h): the terminal
 * says when its window gains or loses keyboard focus -- e.g. a popup that
 * closes when the user clicks elsewhere. Call after ctui_init();
 * ctui_shutdown() turns it back off. */
void ctui_focus_enable(void);

/* opts into the kitty keyboard protocol's "disambiguate" level: keys the
 * legacy encoding can't tell apart (shift/ctrl+Enter, ctrl+Tab,
 * ctrl/shift+Backspace) arrive with their CTUI_MOD_* bits, and ESC is
 * never confused with the start of a sequence. Everything else decodes to
 * the same events as before (ctrl+letter is still its control byte).
 * Terminals without the protocol ignore it. Call after ctui_init();
 * ctui_shutdown() restores the previous mode. */
void ctui_kitty_keys_enable(void);

#endif
