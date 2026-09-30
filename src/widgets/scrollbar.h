#ifndef CTUI_WIDGETS_SCROLLBAR_H
#define CTUI_WIDGETS_SCROLLBAR_H

#include "../ctui.h"
#include "style.h"

/* A vertical scrollbar: one column beside a view showing span of total
 * rows from first on. Not a widget of its own -- the table, the textview
 * (and an app's own view) give it their last column when they hold more
 * than they show and the style has a control hook, which is asked to draw
 * it (CTUI_CONTROL_SCROLLBAR); declined, it is a line with a heavy
 * stretch. A style without a hook shows none.
 *
 * Mouse: a drag on the thumb scrolls with it, a click above or below it
 * moves a page less one. One thumb is held at a time (the pointer's):
 * ctui_scrollbar_held() tells an app routing reports by where they are
 * that they belong to the view holding it. */
typedef struct {
  int dragging; /* the thumb is held */
  int grab;     /* where, in cells from its top */
} CTUI_SCROLLBAR;

/* whether m belongs to a held thumb (any press lets go of it): an app
 * sends it to the view it went to, wherever the pointer is now */
int ctui_scrollbar_held(const CTUI_MOUSE_EVENT_DATA *m);

/* whether a view of span rows holding total shows one in st */
int ctui_scrollbar_wanted(const CTUI_STYLE *st, int total, int span);

/* the thumb over rows cells: its first cell and how many (at least one) */
void ctui_scrollbar_thumb(int rows, int total, int span, int first, int *at,
                          int *len);

/* draws it down rows cells of self from (row, col) */
void ctui_scrollbar_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp,
                           const CTUI_STYLE *st, int row, int col, int rows,
                           int total, int span, int first);

/* a mouse report for the bar drawn there: the first row it asks for (the
 * one it has for a press on the thumb or the release ending a drag), or
 * -1 when the report isn't the bar's. A drag goes on wherever the pointer
 * is, so ask before any hit test of the view's own. */
int ctui_scrollbar_mouse(CTUI_SCROLLBAR *sb, const CTUI_WIDGET *self, int row,
                         int col, int rows, int total, int span, int first,
                         const CTUI_MOUSE_EVENT_DATA *m);

#endif
