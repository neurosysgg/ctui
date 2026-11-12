#ifndef CTUI_KEYNAME_H
#define CTUI_KEYNAME_H

#include "event.h"

#include <stddef.h>

/* A key as text ("ctrl+f", "shift+tab", "F5", "é", "alt+enter"), parsed
 * into the CTUI_KEYPRESS_EVENT_DATA the terminal's bytes for that key give
 * (core/input.c), so a handler can't tell the difference:
 *
 *   MODS+KEY     mods: ctrl, alt, shift, super, joined by '+'
 *                ("ctrl+alt+x")
 *   KEY          a name, as ctui_keytype_name() says it (up, down, left,
 *                right, enter, esc, tab, backtab, home, end, pgup, pgdn,
 *                insert, delete, f1-f12), backspace, space; or one
 *                character, taken literally ("A", "+", "ctrl++")
 *
 * Names and modifiers in any case; a character as written. As the
 * terminal sends them: ctrl+letter (and ctrl+@ .. ctrl+_) is its control
 * byte without the CTRL bit (ctrl+f: ch 0x06), shift+letter its capital
 * without the SHIFT bit ("shift+g": 'G'), shift+tab is BACKTAB,
 * backspace is CHAR 0x7f, space CHAR ' '. */

/* 0 with *out filled (repeat, base 0), -1 for text that isn't a key */
int ctui_key_parse(const char *s, size_t n, CTUI_KEYPRESS_EVENT_DATA *out);

#endif
