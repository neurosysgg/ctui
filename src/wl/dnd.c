/* Drag and drop on a connection's surfaces (wl.h's ctui_wl_dnd_*()):
 * wl_data_device, bound on demand from the globals wl.c leaves unbound,
 * so a build without this file links as before. One drag over us at a
 * time (the offer under the pointer) and one of ours. */
#define _GNU_SOURCE /* pipe2 */

#include "wl_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

/* an offer's MIME types at most, space-separated */
#define DND_MIMES 4096

typedef struct {
  struct wl_data_offer *o;
  char mimes[DND_MIMES];
  size_t len;
  unsigned actions; /* the source's (source_actions) */
  unsigned action;  /* the compositor's choice (action) */
} OFFER;

typedef struct {
  struct wl_data_device_manager *mgr;
  struct wl_data_device *dev;
  uint32_t version;
  const CTUI_WL_DND_OPS *ops;
  void *ctx;
  /* the drag over us: its offer, the enter's serial, the surface, whether
   * it was dropped (then kept past the leave until the finish), whether a
   * type was taken */
  OFFER *cur;
  uint32_t serial;
  CTUI_WL *on;
  int dropped, accepted;
  /* our drag, and the surface it started on */
  struct wl_data_source *src;
  CTUI_WL *src_on;
} DND;

static void tell(DND *k, int kind, CTUI_WL *on, const char *mimes,
                 unsigned actions, CTUI_WL_DPY *d) {
  if (!k->ops || !k->ops->event) {
    return;
  }
  CTUI_WL_DND_EVENT e = {.kind = kind,
                         .on = on,
                         .px = d ? d->px : 0,
                         .py = d ? d->py : 0,
                         .row = d ? d->row : 0,
                         .col = d ? d->col : 0,
                         .actions = actions,
                         .mimes = mimes ? mimes : ""};
  k->ops->event(k->ctx, &e);
}

/* --- an offer --- */

static void offer_mime(void *data, struct wl_data_offer *o, const char *mime) {
  (void)o;
  OFFER *f = data;
  size_t n = strlen(mime);
  /* a type with a space in it would split: left out, as one too long */
  if (strchr(mime, ' ') || f->len + n + 2 > sizeof f->mimes) {
    return;
  }
  if (f->len) {
    f->mimes[f->len++] = ' ';
  }
  memcpy(f->mimes + f->len, mime, n + 1);
  f->len += n;
}

static void offer_source_actions(void *data, struct wl_data_offer *o,
                                 uint32_t actions) {
  (void)o;
  ((OFFER *)data)->actions = actions;
}

static void offer_action(void *data, struct wl_data_offer *o, uint32_t a) {
  (void)o;
  ((OFFER *)data)->action = a;
}

static const struct wl_data_offer_listener k_offer = {.offer = offer_mime,
                                                      .source_actions =
                                                          offer_source_actions,
                                                      .action = offer_action};

static void offer_free(OFFER *f) {
  if (f) {
    wl_data_offer_destroy(f->o);
    free(f);
  }
}

/* --- the device: drags over us --- */

/* where the drag is on k->on, as wl.c's pointer counts it (d's px, py,
 * row, col: the pointer isn't on the surface while a drag is) */
static void at(CTUI_WL_DPY *d, const CTUI_WL *wl, wl_fixed_t x, wl_fixed_t y) {
  double s = wl ? ctui_wl_scale120((CTUI_WL *)wl) / 120.0 : 1.0;
  double fx = wl_fixed_to_double(x) * s - (wl ? wl->cx0 : 0);
  double fy = wl_fixed_to_double(y) * s - (wl ? wl->cy0 : 0);
  d->px = fx > 0 ? fx : 0;
  d->py = fy > 0 ? fy : 0;
  d->col = wl && wl->cw > 0 ? (int)d->px / wl->cw : 0;
  d->row = wl && wl->ch > 0 ? (int)d->py / wl->ch : 0;
}

/* the drag over us forgotten (its offer destroyed) */
static void drop_clear(DND *k) {
  offer_free(k->cur);
  k->cur = NULL;
  k->on = NULL;
  k->dropped = k->accepted = 0;
}

static void dev_data_offer(void *data, struct wl_data_device *dev,
                           struct wl_data_offer *o) {
  (void)data, (void)dev;
  OFFER *f = calloc(1, sizeof *f);
  if (!f) {
    wl_data_offer_destroy(o);
    return;
  }
  f->o = o;
  wl_data_offer_add_listener(o, &k_offer, f);
}

static void dev_enter(void *data, struct wl_data_device *dev, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
                      struct wl_data_offer *o) {
  (void)dev;
  CTUI_WL_DPY *d = data;
  DND *k = d->dnd;
  /* a new drag (the last drop's maybe still being read: that one goes) */
  drop_clear(k);
  k->serial = serial;
  k->on = s ? wl_surface_get_user_data(s) : NULL;
  k->cur = o ? wl_data_offer_get_user_data(o) : NULL;
  if (!k->cur && o) {
    wl_data_offer_destroy(o);
  }
  at(d, k->on, x, y);
  if (k->on && k->cur) {
    tell(k, CTUI_WL_DND_ENTER, k->on, k->cur->mimes, k->cur->actions, d);
  }
}

static void dev_leave(void *data, struct wl_data_device *dev) {
  (void)dev;
  CTUI_WL_DPY *d = data;
  DND *k = d->dnd;
  if (k->dropped) {
    return; /* the leave after a drop: the offer stays until the finish */
  }
  CTUI_WL *on = k->on;
  int told = on && k->cur;
  drop_clear(k);
  if (told) {
    tell(k, CTUI_WL_DND_LEAVE, on, NULL, 0, NULL);
  }
}

static void dev_motion(void *data, struct wl_data_device *dev, uint32_t time,
                       wl_fixed_t x, wl_fixed_t y) {
  (void)dev, (void)time;
  CTUI_WL_DPY *d = data;
  DND *k = d->dnd;
  if (!k->on || !k->cur || k->dropped) {
    return;
  }
  at(d, k->on, x, y);
  tell(k, CTUI_WL_DND_MOTION, k->on, NULL, k->cur->actions, d);
}

static void dev_drop(void *data, struct wl_data_device *dev) {
  (void)dev;
  CTUI_WL_DPY *d = data;
  DND *k = d->dnd;
  if (!k->on || !k->cur) {
    return;
  }
  k->dropped = 1;
  tell(k, CTUI_WL_DND_DROP, k->on, k->cur->mimes, k->cur->actions, d);
}

static void dev_selection(void *data, struct wl_data_device *dev,
                          struct wl_data_offer *o) {
  (void)data, (void)dev;
  /* the clipboard's offer: not ours to use here (data-control's is) */
  if (o) {
    offer_free(wl_data_offer_get_user_data(o));
  }
}

static const struct wl_data_device_listener k_dev = {
    .data_offer = dev_data_offer,
    .enter = dev_enter,
    .leave = dev_leave,
    .motion = dev_motion,
    .drop = dev_drop,
    .selection = dev_selection};

void ctui_wl_dnd_accept(CTUI_WL_DPY *d, const char *mime, unsigned action) {
  DND *k = d ? d->dnd : NULL;
  if (!k || !k->cur || k->dropped) {
    return;
  }
  int take = mime && *mime && action;
  wl_data_offer_accept(k->cur->o, k->serial, take ? mime : NULL);
  if (k->version >= 3) {
    action &= CTUI_WL_DND_COPY | CTUI_WL_DND_MOVE | CTUI_WL_DND_ASK;
    wl_data_offer_set_actions(
        k->cur->o, take ? action : 0,
        take ? (action & CTUI_WL_DND_COPY   ? CTUI_WL_DND_COPY
                : action & CTUI_WL_DND_MOVE ? CTUI_WL_DND_MOVE
                                            : CTUI_WL_DND_ASK)
             : 0);
  }
  k->accepted = take;
  wl_display_flush(d->dpy);
}

int ctui_wl_dnd_receive(CTUI_WL_DPY *d, const char *mime) {
  DND *k = d ? d->dnd : NULL;
  int p[2];
  if (!k || !k->cur || !k->dropped || !mime || !*mime ||
      pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0) {
    return -1;
  }
  wl_data_offer_receive(k->cur->o, mime, p[1]);
  wl_display_flush(d->dpy);
  close(p[1]);
  return p[0];
}

void ctui_wl_dnd_finish(CTUI_WL_DPY *d, unsigned action) {
  DND *k = d ? d->dnd : NULL;
  if (!k || !k->cur) {
    return;
  }
  /* finish is a protocol error without a type taken and an action the
   * compositor chose (v3): then the offer only goes */
  if (k->dropped && action && k->accepted && k->version >= 3 &&
      k->cur->action) {
    wl_data_offer_finish(k->cur->o);
  }
  drop_clear(k);
  wl_display_flush(d->dpy);
}

/* --- our drag --- */

static void src_target(void *data, struct wl_data_source *s, const char *mime) {
  (void)s;
  DND *k = data;
  tell(k, CTUI_WL_DND_S_TARGET, k->src_on, mime, 0, NULL);
}

static void src_send(void *data, struct wl_data_source *s, const char *mime,
                     int32_t fd) {
  (void)s;
  DND *k = data;
  int fl = fcntl(fd, F_GETFL);
  if (fl >= 0) {
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (k->ops && k->ops->send) {
    k->ops->send(k->ctx, k->src_on, mime, fd);
  } else {
    close(fd);
  }
}

static void src_end(DND *k, int kind) {
  CTUI_WL *on = k->src_on;
  if (k->src) {
    wl_data_source_destroy(k->src);
  }
  k->src = NULL;
  k->src_on = NULL;
  tell(k, kind, on, NULL, 0, NULL);
}

static void src_cancelled(void *data, struct wl_data_source *s) {
  (void)s;
  src_end(data, CTUI_WL_DND_S_CANCELLED);
}

static void src_dropped(void *data, struct wl_data_source *s) {
  (void)s;
  DND *k = data;
  tell(k, CTUI_WL_DND_S_DROPPED, k->src_on, NULL, 0, NULL);
}

static void src_finished(void *data, struct wl_data_source *s) {
  (void)s;
  src_end(data, CTUI_WL_DND_S_FINISHED);
}

static void src_action(void *data, struct wl_data_source *s, uint32_t a) {
  (void)s;
  DND *k = data;
  tell(k, CTUI_WL_DND_S_ACTION, k->src_on, NULL, a, NULL);
}

static const struct wl_data_source_listener k_src = {
    .target = src_target,
    .send = src_send,
    .cancelled = src_cancelled,
    .dnd_drop_performed = src_dropped,
    .dnd_finished = src_finished,
    .action = src_action};

void ctui_wl_dnd_cancel(CTUI_WL_DPY *d) {
  DND *k = d ? d->dnd : NULL;
  if (k && k->src) {
    wl_data_source_destroy(k->src);
    k->src = NULL;
    k->src_on = NULL;
    wl_display_flush(d->dpy);
  }
}

int ctui_wl_dnd_start(CTUI_WL *wl, const char *mimes, unsigned actions) {
  CTUI_WL_DPY *d = wl ? wl->d : NULL;
  DND *k = d ? d->dnd : NULL;
  if (!k || !k->dev || !d->press_serial || d->ptr_on != wl || !mimes ||
      !*mimes) {
    return -1;
  }
  if (k->src) {
    src_end(k, CTUI_WL_DND_S_CANCELLED);
  }
  k->src = wl_data_device_manager_create_data_source(k->mgr);
  if (!k->src) {
    return -1;
  }
  wl_data_source_add_listener(k->src, &k_src, k);
  char one[256];
  for (const char *p = mimes; *p;) {
    p += strspn(p, " ");
    size_t n = strcspn(p, " ");
    if (n && n < sizeof one) {
      memcpy(one, p, n);
      one[n] = '\0';
      wl_data_source_offer(k->src, one);
    }
    p += n;
  }
  if (k->version >= 3) {
    actions &= CTUI_WL_DND_COPY | CTUI_WL_DND_MOVE | CTUI_WL_DND_ASK;
    wl_data_source_set_actions(k->src, actions ? actions : CTUI_WL_DND_COPY);
  }
  k->src_on = wl;
  wl_data_device_start_drag(k->dev, k->src, wl->surf, NULL, d->press_serial);
  wl_display_flush(d->dpy);
  return 0;
}

/* --- the device's life --- */

static void dnd_surface_gone(CTUI_WL_DPY *d, CTUI_WL *wl) {
  DND *k = d->dnd;
  if (k->on == wl) {
    k->on = NULL; /* its offer stays: the leave or the finish ends it */
  }
  if (k->src_on == wl) {
    k->src_on = NULL;
  }
}

static void dnd_free(CTUI_WL_DPY *d) {
  DND *k = d->dnd;
  ctui_wl_dnd_cancel(d);
  offer_free(k->cur);
  if (k->dev) {
    if (k->version >= 2) {
      wl_data_device_release(k->dev);
    } else {
      wl_data_device_destroy(k->dev);
    }
  }
  if (k->mgr) {
    wl_data_device_manager_destroy(k->mgr); /* the proxy: it has no request */
  }
  free(k);
  d->dnd = NULL;
  d->dnd_free = NULL;
  d->dnd_surface_gone = NULL;
}

int ctui_wl_dnd_init(CTUI_WL_DPY *d, const CTUI_WL_DND_OPS *ops, void *ctx) {
  if (!d) {
    return -1;
  }
  if (d->dnd) {
    DND *k = d->dnd;
    k->ops = ops;
    k->ctx = ctx;
    return 0;
  }
  const CTUI_WL_GLOBAL *g =
      ctui_wl_global_find(d, wl_data_device_manager_interface.name);
  if (!g || !d->seat) {
    return -1;
  }
  DND *k = calloc(1, sizeof *k);
  if (!k) {
    return -1;
  }
  k->version = g->version < 3 ? g->version : 3;
  k->mgr = wl_registry_bind(d->reg, g->global,
                            &wl_data_device_manager_interface, k->version);
  k->dev =
      k->mgr ? wl_data_device_manager_get_data_device(k->mgr, d->seat) : NULL;
  k->ops = ops;
  k->ctx = ctx;
  d->dnd = k;
  d->dnd_free = dnd_free;
  d->dnd_surface_gone = dnd_surface_gone;
  if (!k->dev) {
    dnd_free(d);
    return -1;
  }
  wl_data_device_add_listener(k->dev, &k_dev, d);
  wl_display_flush(d->dpy);
  return 0;
}
