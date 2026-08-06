#include "split.h"

#include "log.h"

/* smallest cols such that cols*cols >= count -- avoids pulling in <math.h>
 * for a single ceil(sqrt()) call; count is always small (number of
 * simultaneously-visible panes), so the linear scan is negligible */
static int ctui_split_grid_cols(int count) {
  int cols = 1;
  while (cols * cols < count) {
    cols++;
  }
  return cols;
}

void ctui_split_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_SPLIT *split = self->widget_data;
  split->comp = comp;

  if (split->count <= 0) {
    ctui_logf(E_WRN,
              "[CTUI:SPLIT] - widget %p has no active children @ tick %d, "
              "nothing to lay out\n",
              (void *)self, ctui_tick_advance());
    return;
  }

  if (split->mode == CTUI_SPLIT_V) {
    int base = self->h / split->count;
    int remainder = self->h - base * split->count;
    int y = self->y;
    for (int i = 0; i < split->count; i++) {
      CTUI_WIDGET *child = split->children[i];
      child->x = self->x;
      child->w = self->w;
      child->y = y;
      child->h = base + (i == split->count - 1 ? remainder : 0);
      y += child->h;
      ctui_widget_init(child, comp);
    }
  } else if (split->mode == CTUI_SPLIT_H) {
    int base = self->w / split->count;
    int remainder = self->w - base * split->count;
    int x = self->x;
    for (int i = 0; i < split->count; i++) {
      CTUI_WIDGET *child = split->children[i];
      child->y = self->y;
      child->h = self->h;
      child->x = x;
      child->w = base + (i == split->count - 1 ? remainder : 0);
      x += child->w;
      ctui_widget_init(child, comp);
    }
  } else {
    int cols = ctui_split_grid_cols(split->count);
    int rows = (split->count + cols - 1) / cols;
    int base_w = self->w / cols, rem_w = self->w - base_w * cols;
    int base_h = self->h / rows, rem_h = self->h - base_h * rows;
    for (int i = 0; i < split->count; i++) {
      CTUI_WIDGET *child = split->children[i];
      int r = i / cols, c = i % cols;
      child->x = self->x + c * base_w;
      child->w = base_w + (c == cols - 1 ? rem_w : 0);
      child->y = self->y + r * base_h;
      child->h = base_h + (r == rows - 1 ? rem_h : 0);
      ctui_widget_init(child, comp);
    }
  }

  ctui_logf(E_INF,
            "[CTUI:SPLIT] - widget %p laid out @ tick %d (%s, %d active "
            "children, self=%dx%d @ %d,%d)\n",
            (void *)self, ctui_tick_advance(),
            split->mode == CTUI_SPLIT_V   ? "V"
            : split->mode == CTUI_SPLIT_H ? "H"
                                          : "GRID",
            split->count, self->w, self->h, self->x, self->y);
}

void ctui_split_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_SPLIT *split = self->widget_data;
  ctui_logf(E_INF,
            "[CTUI:SPLIT] - render pass @ tick %d (widget %p, %d active "
            "children)\n",
            ctui_tick_advance(), (void *)self, split->count);
  for (int i = 0; i < split->count; i++) {
    CTUI_WIDGET *child = split->children[i];

    ctui_widget_tick_advance(child);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) pre-render @ "
              "widget-tick %d\n",
              (void *)child, child->x, child->y, child->ticks);

    /* routes through the shared render-vs-gfx_render decision point
     * (core/widget.c) instead of calling child->render() directly -- so
     * a Phase 4 (non-degradable-protocol) child behaves exactly like a
     * top-level one, deferred to ctui_widget_flush_gfx() after this
     * frame's flush rather than rendered here */
    ctui_widget_dispatch_render(child, comp);

    ctui_widget_tick_advance(child);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) post-render @ "
              "widget-tick %d\n",
              (void *)child, child->x, child->y, child->ticks);
  }
}
