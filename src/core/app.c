#include "app.h"

#include "ctui_internal.h"
#include "gfx.h"
#include "input.h"
#include "log.h"
#include "timer.h"

#include <stdlib.h>

CTUI_APP *g_app = NULL;

/* the three tiers every widget supports by default (ctui_widget_make()) --
 * always satisfiable regardless of g_gfx_mode, since ctui_screen_flush()
 * degrades a cell's own color_mode to whatever ANSI codes fit, independent
 * of what got negotiated (see GFX_DESIGN.md's "Resolved open questions").
 * Masking these out of a widget's declared supported_gfx_modes before
 * checking against g_gfx_mode is what makes the ctui_app_init() gfx check
 * below a no-op for ordinary text widgets -- it only ever actually fires
 * for a widget that opted into a specific non-degradable protocol (e.g.
 * CTUI_GFX_KITTY) via ctui_widget_set_gfx_renderer(). */
#define CTUI_GFX_TEXT_TIERS \
  (CTUI_GFX_ANSI16 | CTUI_GFX_ANSI256 | CTUI_GFX_TRUECOLOR)

int ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count, int rows,
                  int cols) {
  app->widgets = widgets;
  app->count = count;
  app->comp = ctui_compositor_create(rows, cols);
  app->handlers = NULL;
  app->handler_count = 0;
  app->handler_cap = 0;
  g_app = app;
  ctui_timer_reset();
  ctui_logf(E_INF,
            "[CTUI:APP] - app initialised @ tick %d (%d widgets, %dx%d "
            "compositor)\n",
            ctui_tick_advance(), count, cols, rows);

  /* only the top-level widgets[] passed in here get validated -- a
   * widget nested inside a CTUI_SPLIT/CTUI_GROUP isn't reachable from
   * this array at all (it lives in the split/group's own children/
   * members, discovered only once ctui_split_render()/
   * ctui_group_render() walk it at render time), so there's no hard-fail
   * check to make for it. That's fine: ctui_widget_dispatch_render()
   * (core/widget.c) -- the single place render-vs-gfx_render actually
   * gets decided, for top-level AND nested widgets alike -- falls back
   * to a mismatched widget's plain render() rather than drawing nothing,
   * so a nested gfx-mode widget degrades gracefully with no validation
   * needed. A *top-level* widget still gets the harder guarantee (a
   * startup failure, not a silent fallback) since it's central to the
   * app working at all, not an optional nested extra. */
  for (int i = 0; i < count; i++) {
    ctui_widget_init(widgets[i], app->comp);

    unsigned int required =
        widgets[i]->supported_gfx_modes & ~CTUI_GFX_TEXT_TIERS;
    if (required && !(required & g_gfx_mode)) {
      ctui_logf(E_ERR,
                "[CTUI:APP] - widget %p requires gfx mode 0x%x, but 0x%x "
                "was negotiated @ tick %d; no text fallback to degrade to, "
                "aborting init\n",
                (void *)widgets[i], required, g_gfx_mode,
                ctui_tick_advance());
      return -1;
    }
  }
  return 0;
}

void ctui_app_free(CTUI_APP *app) {
  ctui_logf(E_INF, "[CTUI:APP] - freeing app @ tick %d\n",
            ctui_tick_advance());
  free(app->handlers);
  ctui_timer_reset();
  ctui_compositor_free(app->comp);
}

void ctui_app_render(CTUI_APP *app, CTUI_SCREEN *screen) {
  ctui_logf(E_INF, "[CTUI:APP] - render pass @ tick %d (%d widgets)\n",
            ctui_tick_advance(), app->count);
  ctui_compositor_clear(app->comp);
  for (int i = 0; i < app->count; i++) {
    CTUI_WIDGET *w = app->widgets[i];

    ctui_widget_tick_advance(w);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) pre-render @ "
              "widget-tick %d\n",
              (void *)w, w->x, w->y, w->ticks);

    /* routes through the one shared render-vs-gfx_render decision point
     * (core/widget.c) instead of calling w->render() directly -- a
     * gfx-mode match gets queued there for ctui_widget_flush_gfx() to
     * fire after this frame's flush, rather than drawn into the
     * compositor here (see that function's own docs for why the
     * ordering matters) */
    ctui_widget_dispatch_render(w, app->comp);

    ctui_widget_tick_advance(w);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) post-render @ "
              "widget-tick %d\n",
              (void *)w, w->x, w->y, w->ticks);
  }
  ctui_compositor_blit(app->comp, screen);
}

void ctui_app_resize(CTUI_APP *app, CTUI_SCREEN *screen, int rows, int cols) {
  ctui_logf(E_INF, "[CTUI:APP] - resize @ tick %d (%dx%d -> %dx%d)\n",
            ctui_tick_advance(), app->comp->cols, app->comp->rows, cols,
            rows);

  ctui_compositor_resize(app->comp, rows, cols);
  ctui_screen_resize(screen, rows, cols);

  /* rebind every widget (and re-run each one's optional layout() first, so
   * dynamic geometry reflows against the new size) before anything else
   * touches app->comp -- widgets with stale buf pointers from the freed
   * compositor would otherwise write into freed memory */
  for (int i = 0; i < app->count; i++) {
    ctui_widget_init(app->widgets[i], app->comp);
  }

  CTUI_RESIZE_EVENT_DATA resize_data = {.rows = rows, .cols = cols};
  CTUI_EVENT ev = {.type = CTUI_RESIZE_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "terminal",
                   .event_data = &resize_data};
  ctui_handle_event(&ev);
}

void ctui_app_run(CTUI_APP *app, CTUI_SCREEN *screen, int tick_ms) {
  ctui_logf(E_INF, "[CTUI:APP] - run loop starting @ tick %d (tick_ms=%d)\n",
            ctui_tick_advance(), tick_ms);
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);
  ctui_widget_flush_gfx(app->comp);

  CTUI_EVENT ev;
  ev.scope = CTUI_EVENT_SCOPE_GLOBAL;

  while (ctui_input_loop(&ev, tick_ms)) {
    if (ev.type == CTUI_RESIZE_EVENT) {
      CTUI_RESIZE_EVENT_DATA *resize_data = ev.event_data;
      ctui_app_resize(app, screen, resize_data->rows, resize_data->cols);
      ctui_app_render(app, screen);
      ctui_screen_flush(screen);
      ctui_widget_flush_gfx(app->comp);
      continue;
    }

    if (ev.type == CTUI_KEYPRESS_EVENT) {
      CTUI_KEYPRESS_EVENT_DATA *kp_data = ev.event_data;
      if (kp_data->type == CTUI_KEY_ESC) {
        ctui_logf(E_INF,
                  "[CTUI:APP] - ESC received @ tick %d, breaking run loop\n",
                  ctui_tick_advance());
        break;
      }
    }

    int changed = ctui_handle_event(&ev);
    if (ctui_timer_tick()) {
      changed = 1;
    }
    if (changed) {
      ctui_app_render(app, screen);
      ctui_screen_flush(screen);
      ctui_widget_flush_gfx(app->comp);
    }
  }
  ctui_logf(E_INF, "[CTUI:APP] - run loop exited @ tick %d\n",
            ctui_tick_advance());
}
