#ifndef CTUI_WIDGETS_DIALOG_H
#define CTUI_WIDGETS_DIALOG_H

#include "../ctui.h"
#include "entry.h"
#include "style.h"

#define CTUI_DIALOG_MAX_BUTTONS 6

/* A modal box centred over its widget (make it the app's last widget, full
 * size, so it paints over the rest): a title, wrapped text, an optional
 * line to type into (a rename), an optional progress bar, and buttons. It
 * draws nothing while closed. "Modal" is the app's part: while open, it
 * sends input to the dialog only (the registry has no stopping a key).
 *
 * Keys: Tab / shift+Tab (and left / right without an entry) move between
 * buttons, Enter picks the focused one, Esc cancels; with an entry the
 * other keys edit it. Mouse: a click on a button picks it. Picking or
 * cancelling closes the dialog. */
typedef struct {
  int open;
  const char *title;
  const char *text; /* wrapped to the box; may be NULL */
  const char *buttons[CTUI_DIALOG_MAX_BUTTONS];
  int button_count; /* 0: no buttons (a progress box); Esc still cancels */
  int focus;        /* the focused button, and the one picked */
  int progress;     /* 0-100: a bar; -1: none */
  CTUI_ENTRY *entry; /* a line to type into, or NULL */
  int width;         /* the box's inner width wanted, 0 = 48 */
  const CTUI_STYLE *style;
  /* the last render's button cells, for clicks */
  int button_row, button_from[CTUI_DIALOG_MAX_BUTTONS],
      button_to[CTUI_DIALOG_MAX_BUTTONS];
} CTUI_DIALOG;

enum {
  CTUI_DIALOG_NONE = 0,
  CTUI_DIALOG_REDRAW, /* focus moved or the entry changed */
  CTUI_DIALOG_CHOSEN, /* buttons[focus] picked; closed */
  CTUI_DIALOG_CANCEL, /* Esc; closed */
};

int ctui_dialog_key(CTUI_DIALOG *d, const CTUI_KEYPRESS_EVENT_DATA *kp);
int ctui_dialog_mouse(CTUI_DIALOG *d, const CTUI_MOUSE_EVENT_DATA *m);

void ctui_dialog_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* ("input", CTUI_KEYPRESS_EVENT / CTUI_MOUSE_EVENT), only while open: a
 * pick or a cancel emits a CTUI_VALUE_CHANGED_EVENT (source "dialog",
 * origin self), value = the button's label or NULL for a cancel, enabled =
 * its index or -1. Every key counts as handled while open. */
int ctui_dialog_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);
int ctui_dialog_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
