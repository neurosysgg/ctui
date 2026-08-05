#include "event.h"

#include "ctui_internal.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

const char *ctui_keytype_name(CTUI_KEYTYPE type) {
  switch (type) {
  case CTUI_KEY_NONE:
    return "NONE";
  case CTUI_KEY_UP:
    return "UP";
  case CTUI_KEY_DOWN:
    return "DOWN";
  case CTUI_KEY_LEFT:
    return "LEFT";
  case CTUI_KEY_RIGHT:
    return "RIGHT";
  case CTUI_KEY_ENTER:
    return "ENTER";
  case CTUI_KEY_ESC:
    return "ESC";
  case CTUI_KEY_TAB:
    return "TAB";
  case CTUI_KEY_CHAR:
    return "CHAR";
  }
  return "UNKNOWN";
}

const char *ctui_eventtype_name(CTUI_EVENTTYPE type) {
  switch (type) {
  case CTUI_KEYPRESS_EVENT:
    return "KEYPRESS";
  case CTUI_FOCUS_EVENT:
    return "FOCUS";
  case CTUI_WIDGET_REDRAW:
    return "WIDGET_REDRAW";
  case CTUI_RESIZE_EVENT:
    return "RESIZE";
  case CTUI_TICK_EVENT:
    return "TICK";
  case CTUI_VALUE_CHANGED_EVENT:
    return "VALUE_CHANGED";
  case CTUI_DUMMY_EVENT:
    return "DUMMY";
  }
  return "UNKNOWN";
}

struct CTUI_EVENT_HANDLER {
  const char *source;
  CTUI_EVENTTYPE type;
  CTUI_WIDGET *widget;
  int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev);
};

void ctui_event_register(const char *source, CTUI_EVENTTYPE type,
                         CTUI_WIDGET *widget,
                         int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev)) {
  if (!g_app) {
    ctui_log(E_WRN,
             "[CTUI:EVENT] - no app registered, dropping registration\n");
    return;
  }

  if (g_app->handler_count == g_app->handler_cap) {
    int new_cap = g_app->handler_cap == 0 ? 4 : g_app->handler_cap * 2;
    g_app->handlers = realloc(g_app->handlers,
                              (size_t)new_cap * sizeof(CTUI_EVENT_HANDLER));
    g_app->handler_cap = new_cap;
  }

  g_app->handlers[g_app->handler_count++] = (CTUI_EVENT_HANDLER){
      .source = source, .type = type, .widget = widget, .handler = handler};

  ctui_logf(E_INF,
            "[CTUI:EVENT] - registered handler @ tick %d (source=\"%s\", "
            "type=%s, widget=%p)\n",
            ctui_tick_advance(), source, ctui_eventtype_name(type),
            (void *)widget);
}

int ctui_handle_event(CTUI_EVENT *ev) {
  int changed = 0;
  ctui_logf(E_INF,
            "[CTUI:EVENT] - dispatching %s event @ tick %d (source=\"%s\")\n",
            ctui_eventtype_name(ev->type), ctui_tick_advance(),
            ev->ev_source ? ev->ev_source : "(null)");
  if (!g_app) {
    ctui_log(E_WRN, "[CTUI:EVENT] - no app registered, dropping event\n");
    return 0;
  }
  for (int i = 0; i < g_app->handler_count; i++) {
    CTUI_EVENT_HANDLER *h = &g_app->handlers[i];
    if (h->type != ev->type)
      continue;
    if (h->source == NULL || ev->ev_source == NULL ||
        strcmp(h->source, ev->ev_source) != 0)
      continue;
    if (h->handler(h->widget, ev))
      changed = 1;
  }
  ctui_logf(E_INF, "[CTUI:EVENT] - event dispatched @ tick %d (changed=%d)\n",
            ctui_tick_advance(), changed);
  return changed;
}
