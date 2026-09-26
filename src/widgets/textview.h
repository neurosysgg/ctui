#ifndef CTUI_WIDGETS_TEXTVIEW_H
#define CTUI_WIDGETS_TEXTVIEW_H

#include "../ctui.h"
#include "style.h"

/* A read-only text pane (a file's preview, a log, a man page): the app's
 * text, indexed by line once, shown either wrapped at the width (long
 * lines take several rows, like less) or unwrapped with a sideways
 * scroll. Tabs go to the next multiple of 8, other control bytes show as
 * '?', a '\r' before a line's end is dropped.
 *
 * Keys: up/down, pgup/pgdn (space too), home/end; left/right scroll 8
 * columns sideways when unwrapped. Mouse: the wheel, three rows. */
typedef struct {
  const char *text; /* the app's, kept as long as it's shown */
  size_t len;
  size_t *lines; /* each line's start, line_count + 1 of them (the last one
                  * is len + 1, one past the end) */
  int line_count;
  int wrap;
  int top;     /* the first line shown */
  int top_row; /* wrapped: which of top's rows is the first shown */
  int hscroll; /* unwrapped: the first column shown */
  int page, width; /* the last render's rows and columns */
  const CTUI_STYLE *style;
} CTUI_TEXTVIEW;

/* shows text (len bytes, the app's), from the top: 0, or -1 without
 * memory for the index (then nothing is shown) */
int ctui_textview_set(CTUI_TEXTVIEW *tv, const char *text, size_t len);
void ctui_textview_free(CTUI_TEXTVIEW *tv);

/* 1 if the view moved */
int ctui_textview_key(CTUI_TEXTVIEW *tv, const CTUI_KEYPRESS_EVENT_DATA *kp);
int ctui_textview_mouse(CTUI_TEXTVIEW *tv, const CTUI_WIDGET *self,
                        const CTUI_MOUSE_EVENT_DATA *m);

void ctui_textview_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* ("input", CTUI_KEYPRESS_EVENT / CTUI_MOUSE_EVENT) */
int ctui_textview_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);
int ctui_textview_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
