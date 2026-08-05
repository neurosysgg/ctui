#ifndef CTUI_SPLIT_HDR
#define CTUI_SPLIT_HDR

#include "compositor.h"
#include "widget.h"

typedef enum {
  CTUI_SPLIT_V, /* stack children top-to-bottom, dividing height */
  CTUI_SPLIT_H, /* arrange children left-to-right, dividing width */
} CTUI_SPLIT_MODE;

typedef struct {
  CTUI_SPLIT_MODE mode;
  CTUI_WIDGET **children; /* count-length array of pointers; caller-owned,
                           * not copied. Entries at/after count are simply
                           * never positioned, bound, or rendered -- grow
                           * count later (e.g. from an event handler) to
                           * reveal more panes without reconstructing
                           * anything. children[] itself may be longer than
                           * count, to hold panes not shown yet. */
  int count;              /* how many of children[] are currently active;
                           * split evenly across self's w (CTUI_SPLIT_H) or
                           * h (CTUI_SPLIT_V), remainder absorbed by the
                           * last active child */
  CTUI_COMPOSITOR *comp;  /* cached by ctui_split_layout() on every call, so
                           * code that changes count on the fly (e.g. an
                           * event handler, outside the normal
                           * ctui_widget_init() path) can re-partition
                           * immediately via
                           * ctui_split_layout(self, split->comp) instead of
                           * waiting for the next resize */
} CTUI_SPLIT;

/* sub-compositor utility: divides self's CURRENT x/y/w/h evenly among
 * widget_data's (a CTUI_SPLIT*) first `count` children and binds each one
 * via ctui_widget_init() -- so children address the SAME real compositor
 * self does, at their own (now-computed) absolute (x,y), rather than any
 * separate/virtual buffer. Does not touch self's own x/y/w/h: assign this
 * directly as a widget's layout() for a split with fixed position/size
 * (still works: children are still (re-)computed and rebound every
 * ctui_widget_init() call, including on resize), or call it at the end of
 * your own layout() if the split's position/size is itself dynamic -- see
 * the ctui demo's main_split_layout() for the latter pattern. */
void ctui_split_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* renders widget_data's (a CTUI_SPLIT*) first `count` children, in order --
 * the split itself draws nothing of its own */
void ctui_split_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

#endif
