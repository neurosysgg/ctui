#ifndef CTUI_WIDGETS_FORM_H
#define CTUI_WIDGETS_FORM_H

#include "../ctui.h"
#include "entry.h"
#include "style.h"

/* A settings page: rows of a label and a control, one focused.
 *
 *   Keyboard
 *   › Layout          de
 *     Repeat rate     ━━━━━━━●──────── 40
 *     Natural scroll  [x]
 *     Accel profile   ‹ flat ›
 *                     [ Apply ]
 *
 * Keys: up/down, Tab / shift+Tab move the focus (headings and disabled
 * rows are skipped). A toggle flips on space / Enter; a choice steps on
 * left / right (Enter / space: the next); a slider moves a step on left /
 * right, home / end go to its ends; a button presses on Enter / space; an
 * entry edits (entry.h) and presses on Enter. Mouse: a click focuses a row
 * and works its control (a toggle flips, a choice's ‹ › step, a slider
 * jumps to the click and follows a drag, a button presses, an entry
 * places its cursor); the wheel moves the focus. */
typedef enum {
  CTUI_FORM_HEADING,
  CTUI_FORM_ENTRY,
  CTUI_FORM_TOGGLE,
  CTUI_FORM_CHOICE,
  CTUI_FORM_SLIDER,
  CTUI_FORM_BUTTON, /* the label is the button's */
} CTUI_FORM_KIND;

typedef struct {
  CTUI_FORM_KIND kind;
  const char *label;
  const char *hint;  /* dim, after the control; may be NULL */
  CTUI_ENTRY *entry; /* ENTRY */
  int value;         /* TOGGLE 0/1, CHOICE the option, SLIDER the value */
  const char *const *options; /* CHOICE */
  int option_count;
  int min, max, step; /* SLIDER (step 0 = 1) */
  int disabled;
} CTUI_FORM_ROW;

typedef struct {
  CTUI_FORM_ROW *rows;
  int count;
  int focus;
  int scroll;
  int label_width; /* 0: the widest label */
  const CTUI_STYLE *style;
  /* the last render: rows shown, where the controls start, the dragged
   * slider (-1 = none) */
  int page, control_col, drag;
} CTUI_FORM;

enum {
  CTUI_FORM_NONE = 0,
  CTUI_FORM_MOVED,   /* the focus moved, or an entry's cursor */
  CTUI_FORM_CHANGED, /* rows[focus]'s value / text changed */
  CTUI_FORM_PRESSED, /* rows[focus]: a button, or Enter in an entry */
};

/* focuses the first row that takes it at or after row (for a fresh page) */
void ctui_form_focus(CTUI_FORM *f, int row);

int ctui_form_key(CTUI_FORM *f, const CTUI_KEYPRESS_EVENT_DATA *kp);
int ctui_form_mouse(CTUI_FORM *f, const CTUI_WIDGET *self,
                    const CTUI_MOUSE_EVENT_DATA *m);

void ctui_form_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* ("input", CTUI_KEYPRESS_EVENT / CTUI_MOUSE_EVENT): CHANGED and PRESSED
 * emit a CTUI_VALUE_CHANGED_EVENT (source "form", origin self), value =
 * "changed" | "pressed", enabled = the row's index */
int ctui_form_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);
int ctui_form_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
