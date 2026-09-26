#ifndef CTUI_WIDGETS_TABLE_H
#define CTUI_WIDGETS_TABLE_H

#include "../ctui.h"
#include "style.h"

/* A table: columns with a header, one row per item, a cursor row, marks.
 * The rows stay the app's: the table asks cell() for the text of the rows
 * on screen only, so a directory of 100 000 files costs a screenful per
 * frame. Sorting is the app's too (it knows a size from a name): the
 * table shows sort_col/sort_desc in the header and reports a header
 * click, the app re-orders its rows.
 *
 * Keys: up/down, pgup/pgdn, home/end move the cursor; Enter activates the
 * cursor row; space or insert toggles its mark and moves down (with
 * marks). Mouse: a click selects a row, a click on the selected row
 * activates it, a right click selects it and asks for its menu, the wheel
 * moves three rows, a header click sorts by that column (again: the
 * other way). */
typedef struct {
  const char *title;
  int width;       /* > 0: that many columns; 0: an even share of the rest */
  int align_right; /* numbers */
} CTUI_TABLE_COLUMN;

typedef struct {
  const CTUI_TABLE_COLUMN *columns;
  int column_count;
  int count;    /* rows */
  int selected; /* the cursor row */
  int scroll;   /* the first row shown */
  int header;   /* 1: the first line is the column titles */
  int sort_col; /* the column the header marks as sorted, -1 = none */
  int sort_desc;
  unsigned char *marks; /* count flags, the app's; NULL = no marking */
  /* row's text in column col: a string of the app's, or scratch (cap
   * bytes) filled and returned */
  const char *(*cell)(void *ctx, int row, int col, char *scratch,
                      size_t cap);
  /* optional: row's fg, 0 (CTUI_COLOR_DEFAULT) for the style's */
  unsigned char (*row_fg)(void *ctx, int row);
  void *ctx;
  const CTUI_STYLE *style;
  int page; /* rows shown by the last render (pgup/pgdn) */
} CTUI_TABLE;

/* what a key or a click did */
enum {
  CTUI_TABLE_NONE = 0,
  CTUI_TABLE_MOVED,    /* the cursor moved */
  CTUI_TABLE_ACTIVATE, /* Enter / a click on the cursor row */
  CTUI_TABLE_MARK,     /* a mark toggled (the cursor may have moved too) */
  CTUI_TABLE_SORT,     /* sort_col / sort_desc changed: re-order the rows */
  CTUI_TABLE_MENU,     /* a right click: the cursor row's menu */
};

/* moves the cursor to row (clamped), scrolling it into view; call after
 * the rows change too */
void ctui_table_select(CTUI_TABLE *t, int row);

int ctui_table_key(CTUI_TABLE *t, const CTUI_KEYPRESS_EVENT_DATA *kp);

/* a mouse report at self's absolute cells (self = the widget it was drawn
 * in, full size) */
int ctui_table_mouse(CTUI_TABLE *t, const CTUI_WIDGET *self,
                     const CTUI_MOUSE_EVENT_DATA *m);

void ctui_table_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* ("input", CTUI_KEYPRESS_EVENT) and ("input", CTUI_MOUSE_EVENT): each
 * result but NONE emits a CTUI_VALUE_CHANGED_EVENT (source "table", origin
 * self), value = "moved" | "activate" | "mark" | "sort" | "menu", enabled =
 * the cursor row (the column for "sort") */
int ctui_table_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);
int ctui_table_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
