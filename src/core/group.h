#ifndef CTUI_GROUP_H
#define CTUI_GROUP_H

#include "compositor.h"
#include "widget.h"

#include <stddef.h>

typedef struct {
  size_t size;
  char *group_id;
  CTUI_WIDGET *members; /* size-length array of widgets (not pointers); all
                         * members share a single compositor slice -- see
                         * ctui_group_init() */
} CTUI_GROUP;

CTUI_GROUP ctui_group_make(char *group_id, CTUI_WIDGET *members, size_t size);

/* binds every member's buf to the SAME compositor slice, derived from
 * members[0]'s (x,y) via ctui_widget_init() -- members after the first do
 * not get a slice computed from their own (x,y), they just inherit
 * members[0]'s. Members are therefore expected to share (x,y,w,h); drawing
 * them in order (see ctui_group_render()) then naturally layers, since each
 * member's render() writes into the same cells the previous one did. */
void ctui_group_init(CTUI_GROUP *group, CTUI_COMPOSITOR *comp);

/* walks group->members in order, calling each member's render() into the
 * buffer bound by ctui_group_init(). Does not blit -- callers still reach
 * the screen via ctui_app_render() or ctui_compositor_blit(). */
void ctui_group_render(CTUI_GROUP *group, CTUI_COMPOSITOR *comp);

#endif
