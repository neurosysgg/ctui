#include "app.h"

#include "ctui_internal.h"
#include "input.h"
#include "log.h"

#include <stdlib.h>

CTUI_APP *g_app = NULL;

void ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count, int rows,
                   int cols) {
  app->widgets = widgets;
  app->count = count;
  app->comp = ctui_compositor_create(rows, cols);
  app->handlers = NULL;
  app->handler_count = 0;
  app->handler_cap = 0;
  g_app = app;
  ctui_logf(E_INF,
            "[CTUI:APP] - app initialised @ tick %d (%d widgets, %dx%d "
            "compositor)\n",
            ctui_tick_advance(), count, cols, rows);

  for (int i = 0; i < count; i++) {
    ctui_widget_init(widgets[i], app->comp);
  }
}

void ctui_app_free(CTUI_APP *app) {
  ctui_logf(E_INF, "[CTUI:APP] - freeing app @ tick %d\n",
            ctui_tick_advance());
  free(app->handlers);
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

    w->render(w, app->comp);

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

  CTUI_EVENT ev;
  ev.scope = CTUI_EVENT_SCOPE_GLOBAL;

  while (ctui_input_loop(&ev, tick_ms)) {
    if (ev.type == CTUI_RESIZE_EVENT) {
      CTUI_RESIZE_EVENT_DATA *resize_data = ev.event_data;
      ctui_app_resize(app, screen, resize_data->rows, resize_data->cols);
      ctui_app_render(app, screen);
      ctui_screen_flush(screen);
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

    if (ctui_handle_event(&ev)) {
      ctui_app_render(app, screen);
      ctui_screen_flush(screen);
    }
  }
  ctui_logf(E_INF, "[CTUI:APP] - run loop exited @ tick %d\n",
            ctui_tick_advance());
}
