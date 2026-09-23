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
  case CTUI_KEY_HOME:
    return "HOME";
  case CTUI_KEY_END:
    return "END";
  case CTUI_KEY_PGUP:
    return "PGUP";
  case CTUI_KEY_PGDN:
    return "PGDN";
  case CTUI_KEY_INSERT:
    return "INSERT";
  case CTUI_KEY_DELETE:
    return "DELETE";
  case CTUI_KEY_BACKTAB:
    return "BACKTAB";
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
  case CTUI_TIMER_EVENT:
    return "TIMER";
  case CTUI_VALUE_CHANGED_EVENT:
    return "VALUE_CHANGED";
  case CTUI_MOUSE_EVENT:
    return "MOUSE";
  case CTUI_IO_EVENT:
    return "IO";
  case CTUI_DUMMY_EVENT:
    return "DUMMY";
  }
  return "UNKNOWN";
}

struct CTUI_EVENT_HANDLER {
  const char *source;
  CTUI_EVENTTYPE type;
  CTUI_WIDGET *widget;
  int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev); /* NULL = unregistered,
                                                     * awaiting compaction */
};

/* how many ctui_handle_event() calls are on the stack (handlers emit
 * events of their own, so this nests). Unregistration only tombstones
 * while this is nonzero -- compacting mid-dispatch would shift entries
 * under an index some outer loop is still walking. */
static int g_dispatch_depth = 0;
static int g_tombstones = 0;

static void compact_handlers(void) {
  int kept = 0;
  for (int i = 0; i < g_app->handler_count; i++) {
    if (g_app->handlers[i].handler) {
      g_app->handlers[kept++] = g_app->handlers[i];
    }
  }
  ctui_logf(E_INF,
            "[CTUI:EVENT] - compacted registry @ tick %d (%d -> %d "
            "handlers)\n",
            ctui_tick_advance(), g_app->handler_count, kept);
  g_app->handler_count = kept;
  g_tombstones = 0;
}

void ctui_event_unregister(CTUI_WIDGET *widget) {
  if (!g_app) {
    ctui_log(E_WRN, "[CTUI:EVENT] - no app registered, nothing to "
                    "unregister\n");
    return;
  }
  int removed = 0;
  for (int i = 0; i < g_app->handler_count; i++) {
    CTUI_EVENT_HANDLER *h = &g_app->handlers[i];
    if (h->handler && h->widget == widget) {
      h->handler = NULL;
      removed++;
    }
  }
  g_tombstones += removed;
  ctui_logf(E_INF,
            "[CTUI:EVENT] - unregistered %d handler(s) for widget %p @ tick "
            "%d\n",
            removed, (void *)widget, ctui_tick_advance());
  if (g_dispatch_depth == 0 && g_tombstones) {
    compact_handlers();
  }
}

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

/* runs every handler registered for (ev->ev_source, ev->type, widget) --
 * the same (source, type) match ctui_handle_event()'s CTUI_EVENT_SCOPE_
 * GLOBAL path already does, narrowed to one specific widget. Shared by
 * both the GLOBAL loop (widget == h->widget is implicit, every handler
 * matches) and the BUBBLE walk below (one call per widget in the
 * ancestor chain). */
static int ctui_event_dispatch_to_widget(CTUI_EVENT *ev,
                                         CTUI_WIDGET *widget) {
  int changed = 0;
  for (int i = 0; i < g_app->handler_count; i++) {
    CTUI_EVENT_HANDLER *h = &g_app->handlers[i];
    if (!h->handler || h->type != ev->type || h->widget != widget)
      continue;
    if (h->source == NULL || ev->ev_source == NULL ||
        strcmp(h->source, ev->ev_source) != 0)
      continue;
    if (h->handler(h->widget, ev))
      changed = 1;
  }
  return changed;
}

/* CTUI_EVENT_SCOPE_BUBBLE dispatch: walks ev->origin, then ev->origin->
 * parent, etc. up to NULL, running each widget's own (source, type,
 * widget) handlers as it goes -- see EVENT_DESIGN.md's Phase 2/3.
 * ev->origin's own handlers always run; the walk only stops moving
 * further UP once a step's parent has an is_active_child that reports
 * the child it just ran isn't currently active (Resolved open question
 * 3: the check gates continuing the walk, never the widget it's
 * currently on). */
static int ctui_event_dispatch_bubble(CTUI_EVENT *ev) {
  if (!ev->origin) {
    ctui_log(E_WRN,
             "[CTUI:EVENT] - CTUI_EVENT_SCOPE_BUBBLE event with no origin, "
             "dropping\n");
    return 0;
  }

  int changed = 0;
  CTUI_WIDGET *w = ev->origin;
  while (w) {
    if (ctui_event_dispatch_to_widget(ev, w))
      changed = 1;

    CTUI_WIDGET *parent = w->parent;
    if (parent && parent->is_active_child &&
       !parent->is_active_child(parent, w)) {
      ctui_logf(E_DBG,
                "[CTUI:EVENT] - bubble walk stopped @ tick %d, widget %p "
                "not active in parent %p\n",
                ctui_tick_advance(), (void *)w, (void *)parent);
      break;
    }
    w = parent;
  }
  return changed;
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
  g_dispatch_depth++;
  if (ev->scope == CTUI_EVENT_SCOPE_BUBBLE) {
    changed = ctui_event_dispatch_bubble(ev);
  } else {
    for (int i = 0; i < g_app->handler_count; i++) {
      CTUI_EVENT_HANDLER *h = &g_app->handlers[i];
      if (!h->handler || h->type != ev->type)
        continue;
      if (h->source == NULL || ev->ev_source == NULL ||
          strcmp(h->source, ev->ev_source) != 0)
        continue;
      if (h->handler(h->widget, ev))
        changed = 1;
    }
  }
  if (--g_dispatch_depth == 0 && g_tombstones) {
    compact_handlers();
  }
  ctui_logf(E_INF, "[CTUI:EVENT] - event dispatched @ tick %d (changed=%d)\n",
            ctui_tick_advance(), changed);
  return changed;
}
