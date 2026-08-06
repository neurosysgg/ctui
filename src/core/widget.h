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

  /* render callback -- must never be NULL. Draws into comp via
   * ctui_widget_putc()/ctui_widget_puts(), using coordinates local to
   * this widget (0,0 = top left), not the compositor's absolute
   * coordinates. Called once per frame *if* this widget's gfx_render_mode
   * doesn't match the negotiated mode (the common case: every ordinary
   * widget, always) -- see ctui_widget_dispatch_render() below for the
   * one place that decision actually gets made. Never call this
   * directly when walking a collection of widgets (ctui_app_render(),
   * ctui_split_render(), ctui_group_render() all route through
   * ctui_widget_dispatch_render() instead) or a Phase 4 widget's
   * gfx_render never fires. */
  void (*render)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

  /* bitmask of CTUI_GFX_MODE tiers (core/gfx.h) this widget can render
   * under. Set by ctui_widget_make() to every text tier (ANSI16 |
   * ANSI256 | TRUECOLOR) -- every ordinary widget qualifies under any
   * negotiated text tier with zero code changes, since a cell's own
   * color_mode (not this bitmask) is what actually drives
   * ctui_screen_flush()'s degrade-to-text-color rendering. Narrowed by
   * ctui_widget_set_gfx_renderer() for a widget built around a
   * non-degradable protocol (e.g. Kitty pixel graphics) that has nothing
   * sensible to fall back to as plain text. Checked by ctui_app_init()
   * for *top-level* widgets only -- see that function's own comment for
   * why a widget nested in a CTUI_SPLIT/CTUI_GROUP isn't validated the
   * same way and instead just degrades to render() if unmatched. */
  unsigned int supported_gfx_modes;

  /* the single non-text CTUI_GFX_MODE tier gfx_render below is registered
   * for, or 0 if none -- set only by ctui_widget_set_gfx_renderer(). */
  unsigned int gfx_render_mode;

  /* alternate render callback, used by ctui_widget_dispatch_render()
   * instead of render above when gfx_render_mode matches the negotiated
   * g_gfx_mode. NULL (the ctui_widget_make() default) means "no
   * alternate renderer" -- every widget just uses render. */
  void (*gfx_render)(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

  /* the widget this one is nested inside via CTUI_SPLIT/CTUI_GROUP, or
   * NULL for any top-level widget (ctui_app_init() never sets this) --
   * see EVENT_DESIGN.md. Set by ctui_split_layout() (per active child,
   * every layout pass) and ctui_group_init() (from CTUI_GROUP.parent,
   * every member); nothing else touches it. This is the one piece of
   * tree structure event bubbling needs that nothing else tracks --
   * parents already know their children (split->children, group-
   * >members), nothing walked the other direction before this. */
  CTUI_WIDGET *parent;

  /* set on a PARENT widget (self) by ctui_split_layout() to a checker
   * that reports whether child is currently one of self's active
   * children (split->children[0..count)); NULL means "always active,"
   * which is correct both for an ordinary widget (nothing ever walks up
   * INTO one, since only split/group ever set a child's parent) and for
   * a CTUI_GROUP parent (ctui_group_init() never sets this -- a group
   * has no active/inactive subset, every member is always active). Read
   * only by ctui_handle_event()'s CTUI_EVENT_SCOPE_BUBBLE walk, on the
   * parent of whichever widget the walk is currently at, to decide
   * whether the walk may continue past it. See EVENT_DESIGN.md's Phase
   * 3 and Resolved open question 2. */
  int (*is_active_child)(CTUI_WIDGET *parent, CTUI_WIDGET *child);
};

/* Trips if a field is appended to CTUI_WIDGET without updating
 * ctui_widget_make() (core/widget.c) to initialize it; a designated
 * initializer silently zero-fills an omitted field instead of erroring, so
 * this struct should always be built via ctui_widget_make() rather than a
 * literal. Does NOT catch a field inserted before `is_active_child` --
 * review ctui_widget_make() by hand if you do that. */
_Static_assert(offsetof(CTUI_WIDGET, is_active_child) +
                       sizeof(((CTUI_WIDGET *)0)->is_active_child) ==
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

/* same as ctui_widget_putc()/puts() above, except fg/bg are read as a
 * 0-255 ANSI 256-color index (CTUI_COLOR_MODE_256) instead of a basic
 * CTUI_COLOR_* index -- opt-in for a widget that explicitly wants the
 * richer palette. Requires the app to have negotiated at least
 * CTUI_GFX_ANSI256 via ctui_init(); nothing at this layer enforces that
 * yet (see GFX_DESIGN.md's Phase 4, deferred). */
void ctui_widget_putc_256(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, char ch, unsigned char fg256,
                          unsigned char bg256);
void ctui_widget_puts_256(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, const char *str, unsigned char fg256,
                          unsigned char bg256);

/* same as ctui_widget_putc()/puts() above, except fg/bg are full 24-bit
 * (CTUI_COLOR_MODE_RGB) instead of a palette index -- opt-in for a widget
 * that explicitly wants truecolor. Requires the app to have negotiated at
 * least CTUI_GFX_TRUECOLOR via ctui_init(); nothing at this layer enforces
 * that yet (see GFX_DESIGN.md's Phase 4, deferred). */
void ctui_widget_putc_rgb(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, char ch, unsigned char fg_r,
                          unsigned char fg_g, unsigned char fg_b,
                          unsigned char bg_r, unsigned char bg_g,
                          unsigned char bg_b);
void ctui_widget_puts_rgb(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                          int col, const char *str, unsigned char fg_r,
                          unsigned char fg_g, unsigned char fg_b,
                          unsigned char bg_r, unsigned char bg_g,
                          unsigned char bg_b);

/* opts widget into a non-degradable graphics protocol (currently just
 * CTUI_GFX_KITTY, core/gfx.h) instead of the three text tiers every
 * widget gets from ctui_widget_make() by default. mode must be exactly
 * one CTUI_GFX_MODE bit -- pass it as CTUI_GFX_KITTY, not a combination.
 * Sets widget->supported_gfx_modes = mode (replacing the default text
 * bitmask entirely: a pixel-graphics widget has no sensible plain-text
 * content, so there's nothing left to degrade to at the *top* level --
 * see ctui_app_init()), records mode/render for
 * ctui_widget_dispatch_render()'s dispatch. Call once per widget, after
 * ctui_widget_make(). */
void ctui_widget_set_gfx_renderer(CTUI_WIDGET *widget, unsigned int mode,
                                  void (*render)(CTUI_WIDGET *self,
                                                 CTUI_COMPOSITOR *comp));

/* the single place that decides whether widget draws into comp via its
 * plain render() (text -- the common case, and everything that isn't an
 * exact gfx_render_mode match) or defers to its gfx_render() instead
 * (queued here, not called yet -- see ctui_widget_flush_gfx()). Every
 * caller that walks a collection of widgets and would otherwise call
 * ->render() directly routes through this: ctui_app_render() for
 * top-level app widgets, ctui_split_render()/ctui_group_render() for
 * CTUI_SPLIT/CTUI_GROUP children. That's what makes a Phase 4 widget
 * (ctui_widget_set_gfx_renderer()) behave identically whether it's a
 * top-level widget or nested arbitrarily deep in splits/groups -- this
 * function doesn't know or care which case it's in. */
void ctui_widget_dispatch_render(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp);

/* fires gfx_render() on every widget ctui_widget_dispatch_render() queued
 * this frame -- anywhere in the tree, not just top-level -- then clears
 * the queue. Must run after ctui_screen_flush(), never before: a
 * non-degradable protocol's transmit function (ctui_gfx_kitty_display()
 * for Kitty) writes straight to the terminal, bypassing CTUI_CELL/the
 * compositor entirely, so running this first would let that frame's
 * flush paint blank text right back over pixels that were just written
 * (the widget's own compositor cells never change, since it never calls
 * any ctui_widget_putc*() variant, so flush's shadow-buffer diff treats
 * them as stale on at least the first render). ctui_app_run() is the
 * only caller; every ctui_screen_flush() call site calls this
 * immediately afterward. */
void ctui_widget_flush_gfx(CTUI_COMPOSITOR *comp);

/* frees the gfx dispatch queue ctui_widget_dispatch_render() grows and
 * empties the registry (mirrors ctui_timer_reset() in core/timer.h).
 * Called by ctui_app_init()/ctui_app_free() -- apps never need to call
 * this themselves. */
void ctui_widget_gfx_reset(void);

#endif
