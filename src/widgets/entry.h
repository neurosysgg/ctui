#ifndef CTUI_WIDGETS_ENTRY_H
#define CTUI_WIDGETS_ENTRY_H

#include "../ctui.h"
#include "style.h"

/* A one-line text input. The text lives in a caller-owned buffer; the
 * entry edits it in place like a shell's line: left/right (ctrl: a word),
 * home/end, ctrl+a/e, backspace, delete, ctrl+w or ctrl/alt+backspace (a
 * word before the cursor), ctrl+u / ctrl+k (everything before / after
 * it). Blanks and '/' separate words. Bytes an edit frees are zeroed, so a
 * secret doesn't linger past the NUL.
 *
 * Drawn one row high, scrolled so the cursor (a block over the glyph it
 * sits on) stays visible; a secret shows a dot per glyph. Other widgets
 * embed it (a form row, a dialog) through ctui_entry_key() and
 * ctui_entry_draw(); on its own it's a widget of its own via
 * ctui_entry_render() and ctui_entry_handle_keypress(). */
typedef struct {
  char *buf; /* NUL-terminated, cap bytes including the NUL */
  size_t cap;
  size_t cursor;           /* byte offset into buf */
  int scroll;              /* the first column shown, kept by drawing */
  int secret;              /* dots instead of the text */
  const char *placeholder; /* dim, while buf is empty; may be NULL */
  const CTUI_STYLE *style;
} CTUI_ENTRY;

/* replaces the text (cut to fit cap), the cursor at its end */
void ctui_entry_set(CTUI_ENTRY *e, const char *text);

/* empties the text, zeroing the whole buffer */
void ctui_entry_clear(CTUI_ENTRY *e);

/* one key: 1 if the text or the cursor changed. Enter, Esc, Tab, up and
 * down are left to the caller (0). */
int ctui_entry_key(CTUI_ENTRY *e, const CTUI_KEYPRESS_EVENT_DATA *kp);

/* a click col columns into where the entry was last drawn: moves the
 * cursor there (the text's end past it) */
void ctui_entry_click(CTUI_ENTRY *e, int col);

/* draws e into self at (row, col), width columns wide; the cursor only
 * when focused */
void ctui_entry_draw(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                     int col, int width, CTUI_ENTRY *e, int focused);

/* draws the entry (widget_data) on self's first row, focused */
void ctui_entry_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* ("input", CTUI_KEYPRESS_EVENT): edits; Enter emits a
 * CTUI_VALUE_CHANGED_EVENT (source "entry", value = the text, origin =
 * self) */
int ctui_entry_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
