#include "group.h"

#include "log.h"

CTUI_GROUP ctui_group_make(char *group_id, CTUI_WIDGET *members, size_t size) {
  ctui_logf(E_INF,
            "[CTUI:GROUP] - creating group \"%s\" @ tick %d (%zu members)\n",
            group_id, ctui_tick_advance(), size);
  return (CTUI_GROUP){.size = size, .group_id = group_id, .members = members};
}

void ctui_group_init(CTUI_GROUP *group, CTUI_COMPOSITOR *comp) {
  if (group->size == 0) {
    ctui_logf(E_WRN,
              "[CTUI:GROUP] - group \"%s\" has no members @ tick %d, nothing "
              "to bind\n",
              group->group_id, ctui_tick_advance());
    return;
  }

  ctui_widget_init(&group->members[0], comp);
  CTUI_CELL *shared_buf = group->members[0].buf;
  group->members[0].parent = group->parent;

  for (size_t i = 1; i < group->size; i++) {
    group->members[i].buf = shared_buf;
    group->members[i].parent = group->parent;
  }

  ctui_logf(E_INF,
            "[CTUI:GROUP] - group \"%s\" bound to shared compositor slice @ "
            "tick %d (%zu members)\n",
            group->group_id, ctui_tick_advance(), group->size);
}

void ctui_group_render(CTUI_GROUP *group, CTUI_COMPOSITOR *comp) {
  ctui_logf(E_INF,
            "[CTUI:GROUP] - render pass @ tick %d (group \"%s\", %zu "
            "members)\n",
            ctui_tick_advance(), group->group_id, group->size);
  for (size_t i = 0; i < group->size; i++) {
    CTUI_WIDGET *w = &group->members[i];

    ctui_widget_tick_advance(w);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) pre-render @ "
              "widget-tick %d\n",
              (void *)w, w->x, w->y, w->ticks);

    /* routes through the shared render-vs-gfx_render decision point
     * (core/widget.c) instead of calling w->render() directly -- see
     * ctui_split_render()'s identical comment for why */
    ctui_widget_dispatch_render(w, comp);

    ctui_widget_tick_advance(w);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) post-render @ "
              "widget-tick %d\n",
              (void *)w, w->x, w->y, w->ticks);
  }
}
