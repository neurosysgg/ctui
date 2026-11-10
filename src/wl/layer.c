#define _POSIX_C_SOURCE 200809L
#include "layer.h"
#include "wl_internal.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
  struct zwlr_layer_surface_v1 *ls;
  char *output, *ns; /* what it was made with: never changes */
} LAYER;

static void shell_free(void *shell) {
  struct zwlr_layer_shell_v1 *s = shell;
  if (zwlr_layer_shell_v1_get_version(s) >= 3) {
    zwlr_layer_shell_v1_destroy(s);
  } else {
    wl_proxy_destroy((struct wl_proxy *)s);
  }
}

/* the shell, bound on its first surface: 4 for on-demand keys */
static struct zwlr_layer_shell_v1 *shell(CTUI_WL_DPY *d) {
  if (d->layer_shell) {
    return d->layer_shell;
  }
  const CTUI_WL_GLOBAL *g =
      ctui_wl_global_find(d, zwlr_layer_shell_v1_interface.name);
  if (!g) {
    return NULL;
  }
  d->layer_shell = wl_registry_bind(
      d->reg, g->global, &zwlr_layer_shell_v1_interface, MIN(g->version, 4u));
  d->layer_shell_free = shell_free;
  return d->layer_shell;
}

static void ls_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                         uint32_t serial, uint32_t w, uint32_t h) {
  CTUI_WL *wl = data;
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  /* 0: the size it asked for stands */
  if (w > 0) {
    wl->w = (int)w;
  }
  if (h > 0) {
    wl->h = (int)h;
  }
  wl->configured = 1;
}

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
  (void)ls;
  CTUI_WL *wl = data;
  wl->closed = 1;
  ctui_logf(E_INF, "[CTUI:WL] - layer surface closed by the compositor\n");
}

static const struct zwlr_layer_surface_v1_listener k_ls = {
    .configure = ls_configure,
    .closed = ls_closed,
};

static void role_free(CTUI_WL *wl) {
  LAYER *l = wl->role;
  zwlr_layer_surface_v1_destroy(l->ls);
  free(l->output);
  free(l->ns);
  free(l);
}

static int same(const char *a, const char *b) {
  return a == b || (a && b && !strcmp(a, b));
}

/* the requests that make l's state pending, for the next commit */
static void apply(CTUI_WL *wl, const CTUI_WL_LAYER *l) {
  struct zwlr_layer_surface_v1 *ls = ((LAYER *)wl->role)->ls;
  unsigned v = zwlr_layer_surface_v1_get_version(ls);
  if (v >= 2) {
    zwlr_layer_surface_v1_set_layer(ls, (uint32_t)l->layer);
  }
  zwlr_layer_surface_v1_set_anchor(ls, l->anchor);
  zwlr_layer_surface_v1_set_size(ls, (uint32_t)(l->w > 0 ? l->w : 0),
                                 (uint32_t)(l->h > 0 ? l->h : 0));
  zwlr_layer_surface_v1_set_margin(ls, l->margin_top, l->margin_right,
                                   l->margin_bottom, l->margin_left);
  zwlr_layer_surface_v1_set_exclusive_zone(ls, l->exclusive);
  int kb = l->keyboard;
  if (kb == CTUI_WL_KEYBOARD_ON_DEMAND && v < 4) {
    ctui_logf(E_WRN,
              "[CTUI:WL] - layer shell %u has no on-demand keys: "
              "none\n",
              v);
    kb = CTUI_WL_KEYBOARD_NONE;
  }
  zwlr_layer_surface_v1_set_keyboard_interactivity(ls, (uint32_t)kb);
  if (l->w > 0) {
    wl->w = l->w;
  }
  if (l->h > 0) {
    wl->h = l->h;
  }
}

CTUI_WL *ctui_wl_layer(CTUI_WL_DPY *d, const CTUI_WL_LAYER *l, char *err,
                       unsigned n) {
  struct zwlr_layer_shell_v1 *sh = shell(d);
  if (!sh) {
    snprintf(err, n, "the compositor has no layer shell");
    return NULL;
  }
  struct wl_output *out = NULL;
  if (l->output && !(out = ctui_wl_output_find(d, l->output))) {
    snprintf(err, n, "no output %s", l->output);
    return NULL;
  }
  CTUI_WL *wl = ctui_wl_surface_new(d, l->w, l->h);
  LAYER *role = wl ? calloc(1, sizeof *role) : NULL;
  if (!role) {
    snprintf(err, n, "out of memory");
    ctui_wl_close(wl);
    return NULL;
  }
  role->output = l->output ? strdup(l->output) : NULL;
  role->ns = strdup(l->ns ? l->ns : "ctui");
  role->ls = zwlr_layer_shell_v1_get_layer_surface(
      sh, wl->surf, out, (uint32_t)l->layer, role->ns);
  zwlr_layer_surface_v1_add_listener(role->ls, &k_ls, wl);
  wl->role = role;
  wl->role_free = role_free;
  apply(wl, l);
  if (ctui_wl_surface_wait(wl) < 0 || wl->closed) {
    snprintf(err, n, "the compositor %s the layer surface at once",
             wl->closed ? "closed" : "went before configuring");
    ctui_wl_close(wl);
    return NULL;
  }
  ctui_logf(E_INF,
            "[CTUI:WL] - layer surface %s on %s, layer %d, %dx%d (scale "
            "%d/120) @ tick %d\n",
            role->ns, l->output ? l->output : "(any)", l->layer, wl->w, wl->h,
            ctui_wl_scale120(wl), ctui_tick_advance());
  return wl;
}

int ctui_wl_layer_set(CTUI_WL *wl, const CTUI_WL_LAYER *l) {
  LAYER *role = wl->role;
  if (wl->role_free != role_free || !same(role->output, l->output) ||
      !same(role->ns, l->ns ? l->ns : "ctui")) {
    return -1;
  }
  apply(wl, l);
  wl_surface_commit(wl->surf);
  return 0;
}
