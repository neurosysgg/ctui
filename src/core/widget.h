#ifndef CTUI_WIDGET_H
#define CTUI_WIDGET_H

#include "compositor.h"

#include <stddef.h>

typedef struct CTUI_WIDGET CTUI_WIDGET;

struct CTUI_WIDGET {
  int x, y; /* top left position on screen in cells */
  int w, h; /* fixed size in cols/rows */
  void *widget_data;

  /* per-widget tick counter, separate from the global tick counter in
   * core/log.c; advanced via ctui_widget_tick_advance() */
  int ticks;

  /* pointer into the compositor's backing buffer at this widget's (x,y)
   * origin; set by ctui_widget_init(). NULL until then -- ctui_widget_putc()/
   * ctui_widget_puts() reject writes while it's NULL. Strided by the
   * compositor's cols, not this widget's w, since it's a slice of the larger
   * row-major buffer. */
  CTUI_CELL *buf;

  /* optional: recomputes self->x/y/w/h from comp's current rows/cols (e.g.
   * "cols/2"-style dynamic layout). Called by ctui_widget_init() before it
   * binds buf, so simply re-running ctui_widget_init() -- which
   * ctui_app_resize() does for every widget on a terminal resize -- reflows
   * dynamic widgets against the new size. NULL means fixed geometry: the
   * x/y/w/h passed to ctui_widget_make() are left alone. */
  void (*layout)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

  /* render callback, called unconditionally per frame -- must never be
   * NULL. Draws into comp via ctui_widget_putc()/ctui_widget_puts(), using
   * coordinates local to this widget (0,0 = top left), not the
   * compositor's absolute coordinates. */
  void (*render)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);
};

/* Trips if a field is appended to CTUI_WIDGET without updating
 * ctui_widget_make() (core/widget.c) to initialize it; a designated
 * initializer silently zero-fills an omitted field instead of erroring, so
 * this struct should always be built via ctui_widget_make() rather than a
 * literal. Does NOT catch a field inserted before `render` -- review
 * ctui_widget_make() by hand if you do that. */
_Static_assert(offsetof(CTUI_WIDGET, render) +
                       sizeof(((CTUI_WIDGET *)0)->render) ==
                   sizeof(CTUI_WIDGET),
               "CTUI_WIDGET changed shape; update ctui_widget_make()");

CTUI_WIDGET ctui_widget_make(int x, int y, int w, int h, void *widget_data,
                             void (*render)(CTUI_WIDGET *self,
                                            CTUI_COMPOSITOR *comp),
                             void (*layout)(CTUI_WIDGET *self,
                                           CTUI_COMPOSITOR *comp));

/* advances widget's own tick counter by one; queried (and logged) by
 * ctui_app_render() immediately before and after that widget's render()
 * call, for later per-widget debugging/perf tuning */
void ctui_widget_tick_advance(CTUI_WIDGET *widget);

/* if widget->layout is set, calls it first to recompute x/y/w/h against
 * comp's current rows/cols. Then binds widget->buf to its (x,y) slice of
 * comp's backing buffer. Called for every widget by ctui_app_init() and
 * again, per widget, by ctui_app_resize(). Logs and leaves widget->buf NULL
 * if the widget's (post-layout) origin falls outside comp. */
void ctui_widget_init(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp);

/* writes into widget's slice of comp, at (row, col) local to the widget (0,0
 * = widget's top left). Rejects (logs + no-op) writes outside the widget's
 * declared w/h, writes past comp's bounds, or widgets never bound via
 * ctui_widget_init(). */
void ctui_widget_putc(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, char ch, unsigned char fg, unsigned char bg);
void ctui_widget_puts(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, const char *str, unsigned char fg,
                      unsigned char bg);

#endif
