#ifndef CTUI_WIDGETS_STYLE_H
#define CTUI_WIDGETS_STYLE_H

#include "../ctui.h"

/* A control an app widget is about to draw as glyphs (a form's toggle and
 * slider, a dialog's progress bar, a view's scrollbar), offered to the
 * style's `control` hook first: an app that draws its controls itself --
 * as pixels, say -- does it there.
 *
 * The kinds from BUTTON on carry text (a label, a value, what's typed, a
 * heading) and are drawn *under* it: a hook that takes one has put
 * something beneath the cells (kitty can place an image below the text),
 * and the widget then writes its text plain -- without the glyphs that
 * stood for the control's edges (`[ ]`, `‹ ›`, box drawing) and on st->bg
 * also where it would have used sel_bg (the control shows the focus). */
typedef enum {
  CTUI_CONTROL_TOGGLE,    /* value 0/1 of max 1 */
  CTUI_CONTROL_SLIDER,    /* value of max: where its thumb is */
  CTUI_CONTROL_PROGRESS,  /* value of max: how far it is filled */
  CTUI_CONTROL_SCROLLBAR, /* value = the first shown of max, span of them
                           * showing; one column, rows cells down */
  CTUI_CONTROL_BUTTON,    /* a push button, its label over it */
  CTUI_CONTROL_CHOICE,    /* one of options, its value over it (a
                           * drop-down's look) */
  CTUI_CONTROL_FIELD,     /* a text entry, its text over it */
  CTUI_CONTROL_GROUP,     /* a heading and the rows under it, rows down;
                           * the heading over its top edge at label */
  CTUI_CONTROL_FRAME,     /* a dialog's box, rows down; its title over
                           * its top edge at label */
} CTUI_CONTROL_KIND;

typedef struct {
  CTUI_CONTROL_KIND kind;
  int value, max; /* 0 <= value <= max */
  int focused, disabled;
  int span; /* a scrollbar's: how many of max show */
  int rows; /* the cells it takes downwards; 0 = the one row */
  /* GROUP's, FRAME's: the columns of its top row the heading / title
   * takes (from col: its edge left open there); label_cols 0 = none */
  int label, label_cols;
} CTUI_CONTROL;

/* Draws c over the cols cells from (row, col) of self (widget
 * coordinates, as ctui_widget_putc() takes them; c->rows rows of them
 * when set) on bg -- or, for the kinds that carry text, under them.
 * Returns non-zero if it drew them all; 0 leaves the control to the
 * widget, which then draws its glyphs (so a hook may decline per call:
 * no room, no graphics).
 * The cells are the ones the glyphs would take: clicks and drags don't
 * change. */
typedef int (*CTUI_CONTROL_DRAW)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp,
                                 int row, int col, int cols,
                                 const CTUI_CONTROL *c, unsigned char bg,
                                 void *arg);

/* The colours the app widgets (entry, table, dialog, textview, form,
 * tabs) share, so an app themes all of them in one place: each takes a
 * `const CTUI_STYLE *style`, NULL meaning ctui_style_default. Basic
 * CTUI_COLOR_* values. `control` (NULL: none) is asked before a control
 * is drawn as glyphs; with one set, a table or a textview longer than its
 * pane also shows a scrollbar in its last column (widgets/scrollbar.h). */
typedef struct {
  unsigned char fg, bg;
  unsigned char dim_fg;         /* hints, headers, secondary text */
  unsigned char title_fg;       /* titles, headings */
  unsigned char sel_fg, sel_bg; /* the cursor row, the focused control */
  unsigned char mark_fg;        /* marked rows, a toggle that's on */
  unsigned char error_fg;
  CTUI_CONTROL_DRAW control;
  void *control_arg;
} CTUI_STYLE;

extern const CTUI_STYLE ctui_style_default;

/* style, or the default for NULL */
static inline const CTUI_STYLE *ctui_style_of(const CTUI_STYLE *style) {
  return style ? style : &ctui_style_default;
}

/* asks st's hook to draw c (above); 0 without one, or if it declined */
static inline int ctui_style_control(const CTUI_STYLE *st, CTUI_WIDGET *self,
                                     CTUI_COMPOSITOR *comp, int row, int col,
                                     int cols, const CTUI_CONTROL *c,
                                     unsigned char bg) {
  return st->control && cols > 0 &&
         st->control(self, comp, row, col, cols, c, bg, st->control_arg);
}

#endif
