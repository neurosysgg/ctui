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
  CTUI_WIDGET *parent; /* the widget this group is logically nested inside,
                        * or NULL for a standalone/top-level group. Unlike
                        * CTUI_SPLIT, a group is never itself wrapped in a
                        * CTUI_WIDGET in current usage, so there's no
                        * natural "self" for ctui_group_init() to derive
                        * this from -- set it directly (like split->count/
                        * children[] are already poked directly by app
                        * code) before calling ctui_group_init() if this
                        * group needs to participate in event bubbling.
                        * Defaults NULL via ctui_group_make()'s zero-fill.
                        * See EVENT_DESIGN.md's Resolved open question 1. */
} CTUI_GROUP;

CTUI_GROUP ctui_group_make(char *group_id, CTUI_WIDGET *members, size_t size);

/* binds every member's buf to the SAME compositor slice, derived from
 * members[0]'s (x,y) via ctui_widget_init() -- members after the first do
 * not get a slice computed from their own (x,y), they just inherit
 * members[0]'s. Members are therefore expected to share (x,y,w,h); drawing
 * them in order (see ctui_group_render()) then naturally layers, since each
 * member's render() writes into the same cells the previous one did. Also
 * sets every member's ->parent to group->parent (whatever it was when this
 * was called) -- unlike ctui_split_layout(), never sets is_active_child on
 * any member, since a group has no active/inactive subset for one to
 * check. */
void ctui_group_init(CTUI_GROUP *group, CTUI_COMPOSITOR *comp);

/* walks group->members in order, calling each member's render() into the
 * buffer bound by ctui_group_init(). Does not blit -- callers still reach
 * the screen via ctui_app_render() or ctui_compositor_blit(). */
void ctui_group_render(CTUI_GROUP *group, CTUI_COMPOSITOR *comp);

#endif
