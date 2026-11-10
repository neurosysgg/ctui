#ifndef CTUI_WL_LAYER_H
#define CTUI_WL_LAYER_H

/* A surface as a layer of the desktop (wlr-layer-shell: a panel, a bar,
 * a popup over everything) on wl.h's connection, beside its windows.
 * Optional on top of src/wl/: layer.c and the glue of ctui's
 * protocols/wlr-layer-shell-unstable-v1.xml (not in wayland-protocols)
 * are built only by who needs them, so a window's app needs neither. */

#include "wl.h"

enum {
  CTUI_WL_LAYER_BACKGROUND,
  CTUI_WL_LAYER_BOTTOM,
  CTUI_WL_LAYER_TOP,
  CTUI_WL_LAYER_OVERLAY,
};

/* anchor bits, layer shell's own values */
enum {
  CTUI_WL_EDGE_TOP = 1,
  CTUI_WL_EDGE_BOTTOM = 2,
  CTUI_WL_EDGE_LEFT = 4,
  CTUI_WL_EDGE_RIGHT = 8,
};

/* who gets the keyboard: never, only it (while it's mapped), or it once
 * clicked (layer shell 4; older compositors: never) */
enum {
  CTUI_WL_KEYBOARD_NONE,
  CTUI_WL_KEYBOARD_EXCLUSIVE,
  CTUI_WL_KEYBOARD_ON_DEMAND,
};

typedef struct {
  const char *output; /* by name (ctui_wl_outputs()); NULL: the
                       * compositor's pick */
  const char *ns;     /* its namespace ("panel", "notification", ...) */
  int layer;          /* CTUI_WL_LAYER_* */
  unsigned anchor;    /* CTUI_WL_EDGE_* bits; 0: centred */
  int w, h;           /* logical pixels; 0 along an axis anchored at both
                       * ends: as long as the output */
  int margin_top, margin_right, margin_bottom, margin_left;
  int exclusive; /* the edge it reserves (>0), none (0), or -1: put it
                  * over what others reserved too */
  int keyboard;  /* CTUI_WL_KEYBOARD_* */
} CTUI_WL_LAYER;

/* the layer surface l says on d, configured once; NULL when the
 * compositor has no layer shell, the output isn't there, or it closed it
 * at once (why in err) */
CTUI_WL *ctui_wl_layer(CTUI_WL_DPY *d, const CTUI_WL_LAYER *l, char *err,
                       unsigned n);
/* the same surface moved, resized, raised or given the keyboard: what l
 * says now, from its next commit (the compositor configures it again).
 * -1 for what a layer surface can't change (its output, its namespace):
 * close it and make another */
int ctui_wl_layer_set(CTUI_WL *wl, const CTUI_WL_LAYER *l);

#endif
