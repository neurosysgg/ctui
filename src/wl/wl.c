#define _POSIX_C_SOURCE 200809L
#include "wl_internal.h"

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <linux/input-event-codes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

static unsigned mods_now(const CTUI_WL_DPY *d) {
  if (!d->state) {
    return 0;
  }
  unsigned m = 0;
  if (xkb_state_mod_name_is_active(d->state, XKB_MOD_NAME_SHIFT,
                                   XKB_STATE_MODS_EFFECTIVE) > 0) {
    m |= CTUI_MOD_SHIFT;
  }
  if (xkb_state_mod_name_is_active(d->state, XKB_MOD_NAME_CTRL,
                                   XKB_STATE_MODS_EFFECTIVE) > 0) {
    m |= CTUI_MOD_CTRL;
  }
  if (xkb_state_mod_name_is_active(d->state, XKB_MOD_NAME_ALT,
                                   XKB_STATE_MODS_EFFECTIVE) > 0) {
    m |= CTUI_MOD_ALT;
  }
  return m;
}

static void push(CTUI_WL_DPY *d, const CTUI_WL_QEV *q) {
  if (d->count == CTUI_WL_QUEUE) {
    ctui_logf(E_WRN, "[CTUI:WL] - input queue full, an event dropped\n");
    return;
  }
  d->queue[(d->head + d->count++) % CTUI_WL_QUEUE] = *q;
}

int ctui_wl_dpy_next(CTUI_WL_DPY *d, CTUI_EVENT *ev, CTUI_WL **on) {
  while (d->count) {
    d->cur = d->queue[d->head];
    d->head = (d->head + 1) % CTUI_WL_QUEUE;
    d->count--;
    if (!d->cur.on) {
      continue; /* its surface closed while it waited */
    }
    *ev = d->cur.ev;
    ev->event_data = &d->cur.d;
    if (on) {
      *on = d->cur.on;
    }
    return 1;
  }
  return 0;
}

int ctui_wl_next(CTUI_WL *wl, CTUI_EVENT *ev) {
  return ctui_wl_dpy_next(wl->d, ev, NULL);
}

int ctui_wl_queued(const CTUI_WL *wl) { return wl->d->count > 0; }

/* --- keys --- */

static void repeat_arm(CTUI_WL_DPY *d, int on) {
  struct itimerspec it = {0};
  if (on && d->rate > 0) {
    long iv = 1000000000L / d->rate;
    it.it_value.tv_sec = d->delay / 1000;
    it.it_value.tv_nsec = (long)(d->delay % 1000) * 1000000L;
    it.it_interval.tv_nsec = iv % 1000000000L;
    it.it_interval.tv_sec = iv / 1000000000L;
    if (!it.it_value.tv_sec && !it.it_value.tv_nsec) {
      it.it_value.tv_nsec = 1;
    }
  }
  if (!on) {
    d->repeat_key = 0;
  }
  timerfd_settime(d->repeat_fd, 0, &it, NULL);
}

/* the event the legacy bytes for this key would give (core/input.c's
 * resolve_csi_u() does the same for kitty's reports); 0 for a key that
 * makes none (a modifier alone) */
int ctui_wl_key_event(uint32_t sym, uint32_t cp, unsigned mods,
                      CTUI_KEYPRESS_EVENT_DATA *out) {
  CTUI_KEYTYPE type = CTUI_KEY_CHAR;
  uint32_t ch = 0;
  switch (sym) {
  case XKB_KEY_Up:
  case XKB_KEY_KP_Up:
    type = CTUI_KEY_UP;
    break;
  case XKB_KEY_Down:
  case XKB_KEY_KP_Down:
    type = CTUI_KEY_DOWN;
    break;
  case XKB_KEY_Left:
  case XKB_KEY_KP_Left:
    type = CTUI_KEY_LEFT;
    break;
  case XKB_KEY_Right:
  case XKB_KEY_KP_Right:
    type = CTUI_KEY_RIGHT;
    break;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    type = CTUI_KEY_ENTER;
    break;
  case XKB_KEY_Escape:
    type = CTUI_KEY_ESC;
    break;
  case XKB_KEY_Tab:
  case XKB_KEY_ISO_Left_Tab:
    if (sym == XKB_KEY_ISO_Left_Tab || (mods & CTUI_MOD_SHIFT)) {
      type = CTUI_KEY_BACKTAB;
      mods &= ~(unsigned)CTUI_MOD_SHIFT;
    } else {
      type = CTUI_KEY_TAB;
    }
    break;
  case XKB_KEY_BackSpace:
    ch = 0x7f;
    break;
  case XKB_KEY_Home:
  case XKB_KEY_KP_Home:
    type = CTUI_KEY_HOME;
    break;
  case XKB_KEY_End:
  case XKB_KEY_KP_End:
    type = CTUI_KEY_END;
    break;
  case XKB_KEY_Prior:
  case XKB_KEY_KP_Prior:
    type = CTUI_KEY_PGUP;
    break;
  case XKB_KEY_Next:
  case XKB_KEY_KP_Next:
    type = CTUI_KEY_PGDN;
    break;
  case XKB_KEY_Insert:
  case XKB_KEY_KP_Insert:
    type = CTUI_KEY_INSERT;
    break;
  case XKB_KEY_Delete:
  case XKB_KEY_KP_Delete:
    type = CTUI_KEY_DELETE;
    break;
  default:
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) {
      type = (CTUI_KEYTYPE)(CTUI_KEY_F1 + (int)(sym - XKB_KEY_F1));
      break;
    }
    if (!cp) {
      return 0;
    }
    ch = cp;
    /* the char says shift already, as a terminal's byte does; a control
     * byte says ctrl (xkbcommon's Control transformation made it) */
    mods &= ~(unsigned)CTUI_MOD_SHIFT;
    if (ch < 0x20 || ch == 0x7f) {
      mods &= ~(unsigned)CTUI_MOD_CTRL;
    }
  }
  memset(out, 0, sizeof *out);
  out->type = type;
  out->ch = ch;
  out->mods = mods;
  return 1;
}

static int key_event(const CTUI_WL_DPY *d, xkb_keysym_t sym, uint32_t cp,
                     CTUI_WL_QEV *q) {
  memset(q, 0, sizeof *q);
  if (!ctui_wl_key_event(sym, cp, mods_now(d), &q->d.key)) {
    return 0;
  }
  q->ev.type = CTUI_KEYPRESS_EVENT;
  q->ev.scope = CTUI_EVENT_SCOPE_GLOBAL;
  q->ev.ev_source = "input";
  q->on = d->kb_on;
  return 1;
}

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                      int32_t fd, uint32_t size) {
  (void)kb;
  CTUI_WL_DPY *d = data;
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }
  char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED) {
    return;
  }
  struct xkb_keymap *km = xkb_keymap_new_from_buffer(
      d->xkb, map, strnlen(map, size), XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map, size);
  if (!km) {
    ctui_logf(E_WRN, "[CTUI:WL] - the compositor's keymap didn't compile\n");
    return;
  }
  xkb_state_unref(d->state);
  xkb_keymap_unref(d->keymap);
  d->keymap = km;
  d->state = xkb_state_new(km);
  ctui_logf(E_INF, "[CTUI:WL] - keymap loaded @ tick %d\n",
            ctui_tick_advance());
}

/* the surface an enter names: ours all (each wl_surface's user data is
 * its CTUI_WL); NULL when it was destroyed before the event came */
static CTUI_WL *surface_of(struct wl_surface *s) {
  return s ? wl_surface_get_user_data(s) : NULL;
}

static void focus_event(CTUI_WL_DPY *d, CTUI_WL *on, int focused) {
  if (!d->focus_events || !on) {
    return;
  }
  CTUI_WL_QEV q;
  memset(&q, 0, sizeof q);
  q.ev.type = CTUI_FOCUS_EVENT;
  q.ev.scope = CTUI_EVENT_SCOPE_GLOBAL;
  q.ev.ev_source = "input";
  q.d.focus.focused = focused;
  q.on = on;
  push(d, &q);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *s, struct wl_array *keys) {
  (void)kb, (void)serial, (void)keys;
  CTUI_WL_DPY *d = data;
  d->kb_on = surface_of(s);
  focus_event(d, d->kb_on, 1);
}

/* focus gone: nothing stays held */
static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *s) {
  (void)kb, (void)serial, (void)s;
  CTUI_WL_DPY *d = data;
  repeat_arm(d, 0);
  if (d->raw) {
    d->raw(d->raw_ctx, 0, 0);
  }
  focus_event(d, d->kb_on, 0);
  d->kb_on = NULL;
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
  (void)kb, (void)serial, (void)time;
  CTUI_WL_DPY *d = data;
  xkb_keycode_t code = key + 8;
  if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    if (d->repeat_key == code) {
      repeat_arm(d, 0);
    }
    if (d->raw) {
      d->raw(d->raw_ctx, key, 0);
    }
    return;
  }
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED ||
      (d->raw && d->raw(d->raw_ctx, key, 1)) || !d->state) {
    return;
  }
  xkb_keysym_t sym = xkb_state_key_get_one_sym(d->state, code);
  uint32_t cp = xkb_state_key_get_utf32(d->state, code);
  if (d->compose &&
      xkb_compose_state_feed(d->compose, sym) == XKB_COMPOSE_FEED_ACCEPTED) {
    switch (xkb_compose_state_get_status(d->compose)) {
    case XKB_COMPOSE_COMPOSING:
      return;
    case XKB_COMPOSE_CANCELLED:
      xkb_compose_state_reset(d->compose);
      return;
    case XKB_COMPOSE_COMPOSED: {
      char u[16];
      sym = xkb_compose_state_get_one_sym(d->compose);
      xkb_compose_state_get_utf8(d->compose, u, sizeof u);
      xkb_compose_state_reset(d->compose);
      cp = 0;
      ctui_utf8_decode(u, &cp);
      break;
    }
    case XKB_COMPOSE_NOTHING:
      break;
    }
  }
  CTUI_WL_QEV q;
  if (!key_event(d, sym, cp, &q)) {
    return;
  }
  push(d, &q);
  if (xkb_keymap_key_repeats(d->keymap, code)) {
    d->repeat_ev = q;
    repeat_arm(d, 1);
    d->repeat_key = code;
  }
}

static void kb_mods(void *data, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
  (void)kb, (void)serial;
  CTUI_WL_DPY *d = data;
  if (d->state) {
    xkb_state_update_mask(d->state, dep, lat, lock, 0, 0, grp);
  }
}

static void kb_repeat(void *data, struct wl_keyboard *kb, int32_t rate,
                      int32_t delay) {
  (void)kb;
  CTUI_WL_DPY *d = data;
  d->rate = rate;
  d->delay = delay;
}

static const struct wl_keyboard_listener k_kb = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_mods,
    .repeat_info = kb_repeat,
};

void ctui_wl_dpy_repeat(CTUI_WL_DPY *d) {
  uint64_t n = 0;
  if (read(d->repeat_fd, &n, sizeof n) != (ssize_t)sizeof n || !d->repeat_key) {
    return;
  }
  /* a host that fell behind doesn't get a burst */
  for (uint64_t i = 0; i < n && i < 4; i++) {
    push(d, &d->repeat_ev);
  }
}

void ctui_wl_repeat(CTUI_WL *wl) { ctui_wl_dpy_repeat(wl->d); }

/* --- the pointer --- */

static int scale120(const CTUI_WL *wl) {
  return wl->vp && wl->scale120 ? wl->scale120 : wl->scale_int * 120;
}

static void mouse(CTUI_WL_DPY *d, CTUI_MOUSE_ACTION action, int button) {
  if (!d->ptr_on) {
    return;
  }
  CTUI_WL_QEV q;
  memset(&q, 0, sizeof q);
  q.ev.type = CTUI_MOUSE_EVENT;
  q.ev.scope = CTUI_EVENT_SCOPE_GLOBAL;
  q.ev.ev_source = "input";
  q.d.mouse.action = action;
  q.d.mouse.button = button;
  q.d.mouse.row = d->row;
  q.d.mouse.col = d->col;
  q.d.mouse.px = (int)d->px;
  q.d.mouse.py = (int)d->py;
  q.d.mouse.mods = mods_now(d);
  q.on = d->ptr_on;
  push(d, &q);
}

/* the lowest button held, as a terminal reports a drag; -1 for none */
static int held_button(const CTUI_WL_DPY *d) {
  for (int b = 0; b < 8; b++) {
    if (d->held & (1u << b)) {
      return b;
    }
  }
  return -1;
}

/* where the pointer is on its surface; 1 when that's another cell */
static int moved(CTUI_WL_DPY *d, wl_fixed_t x, wl_fixed_t y) {
  const CTUI_WL *wl = d->ptr_on;
  if (!wl) {
    return 0;
  }
  double s = scale120(wl) / 120.0;
  /* from the cells' origin, as a terminal reports them past its padding;
   * in the inset, the first cell's edge */
  double fx = wl_fixed_to_double(x) * s - wl->cx0;
  double fy = wl_fixed_to_double(y) * s - wl->cy0;
  d->px = fx > 0 ? fx : 0;
  d->py = fy > 0 ? fy : 0;
  int col = wl->cw > 0 ? (int)d->px / wl->cw : 0;
  int row = wl->ch > 0 ? (int)d->py / wl->ch : 0;
  int changed = col != d->col || row != d->row;
  d->col = col;
  d->row = row;
  return changed;
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
  (void)p;
  CTUI_WL_DPY *d = data;
  if (d->cursor) {
    wp_cursor_shape_device_v1_set_shape(
        d->cursor, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
  }
  d->ptr_on = surface_of(s);
  moved(d, x, y);
  mouse(d, CTUI_MOUSE_MOTION, held_button(d));
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {
  (void)p, (void)serial, (void)s;
  CTUI_WL_DPY *d = data;
  d->held = 0;
  d->ptr_on = NULL;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t x, wl_fixed_t y) {
  (void)p, (void)time;
  CTUI_WL_DPY *d = data;
  if (moved(d, x, y)) {
    mouse(d, CTUI_MOUSE_MOTION, held_button(d));
  }
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
  (void)p, (void)serial, (void)time;
  CTUI_WL_DPY *d = data;
  int b;
  switch (button) {
  case BTN_LEFT:
    b = 0;
    break;
  case BTN_MIDDLE:
    b = 1;
    break;
  case BTN_RIGHT:
    b = 2;
    break;
  case BTN_SIDE:
    b = CTUI_MOUSE_BUTTON_BACK;
    break;
  case BTN_EXTRA:
    b = CTUI_MOUSE_BUTTON_FORWARD;
    break;
  default:
    return;
  }
  int down = state == WL_POINTER_BUTTON_STATE_PRESSED;
  d->held = down ? d->held | 1u << b : d->held & ~(1u << b);
  mouse(d, down ? CTUI_MOUSE_PRESS : CTUI_MOUSE_RELEASE, b);
}

/* a wheel's notch is 10 (wl_pointer's axis, before value120) */
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {
  (void)p, (void)time;
  CTUI_WL_DPY *d = data;
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }
  d->scroll += wl_fixed_to_double(value);
  for (; d->scroll >= 10.0; d->scroll -= 10.0) {
    mouse(d, CTUI_MOUSE_SCROLL_DOWN, -1);
  }
  for (; d->scroll <= -10.0; d->scroll += 10.0) {
    mouse(d, CTUI_MOUSE_SCROLL_UP, -1);
  }
}

static void ptr_frame(void *data, struct wl_pointer *p) { (void)data, (void)p; }
static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t src) {
  (void)data, (void)p, (void)src;
}
static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
                          uint32_t axis) {
  (void)p, (void)time;
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    ((CTUI_WL_DPY *)data)->scroll = 0;
  }
}
static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
                              int32_t discrete) {
  (void)data, (void)p, (void)axis, (void)discrete;
}

static const struct wl_pointer_listener k_ptr = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

/* --- the display's timing --- */

static void pres_clock(void *data, struct wp_presentation *p, uint32_t clk) {
  (void)p;
  ((CTUI_WL_DPY *)data)->clock_ok = clk == CLOCK_MONOTONIC;
}
static const struct wp_presentation_listener k_pres = {.clock_id = pres_clock};

/* a frame's feedback: its tag, until the compositor says */
typedef struct {
  CTUI_WL *wl;
  uint64_t tag;
} FEEDBACK;

static void fb_output(void *data, struct wp_presentation_feedback *f,
                      struct wl_output *o) {
  (void)data, (void)f, (void)o;
}
static void fb_presented(void *data, struct wp_presentation_feedback *f,
                         uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                         uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
                         uint32_t flags) {
  FEEDBACK *fb = data;
  CTUI_WL_TIMING *t = &fb->wl->timing;
  uint64_t at = (((uint64_t)sec_hi << 32 | sec_lo) * 1000000000ull) + nsec;
  double lat = (double)at - (double)fb->tag;
  t->presented++;
  t->lat_sum += lat;
  t->lat_min = t->presented == 1 || lat < t->lat_min ? lat : t->lat_min;
  t->lat_max = lat > t->lat_max ? lat : t->lat_max;
  t->refresh_ns = refresh;
  t->last_ns = at;
  t->last_seq = (uint64_t)seq_hi << 32 | seq_lo;
  t->vsync += !!(flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
  t->zero_copy += !!(flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY);
  wp_presentation_feedback_destroy(f);
  free(fb);
}
static void fb_discarded(void *data, struct wp_presentation_feedback *f) {
  FEEDBACK *fb = data;
  fb->wl->timing.discarded++;
  wp_presentation_feedback_destroy(f);
  free(fb);
}
static const struct wp_presentation_feedback_listener k_fb = {
    .sync_output = fb_output,
    .presented = fb_presented,
    .discarded = fb_discarded,
};

void ctui_wl_feedback(CTUI_WL *wl, uint64_t tag_ns) {
  FEEDBACK *fb = wl->d->pres ? malloc(sizeof *fb) : NULL;
  if (!fb) {
    return;
  }
  fb->wl = wl;
  fb->tag = tag_ns;
  struct wp_presentation_feedback *f =
      wp_presentation_feedback(wl->d->pres, wl->surf);
  wp_presentation_feedback_add_listener(f, &k_fb, fb);
  wl->timing.clock_ok = wl->d->clock_ok;
  wl->timing.asked++;
}

const CTUI_WL_TIMING *ctui_wl_timing(const CTUI_WL *wl) { return &wl->timing; }

void ctui_wl_set_raw_key(CTUI_WL *wl, CTUI_WL_RAW_KEY fn, void *ctx) {
  wl->d->raw = fn;
  wl->d->raw_ctx = ctx;
}

/* --- the seat, the outputs, the registry --- */

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
  CTUI_WL_DPY *d = data;
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->kb) {
    d->kb = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(d->kb, &k_kb, d);
  }
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->ptr) {
    d->ptr = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(d->ptr, &k_ptr, d);
    if (d->csm) {
      d->cursor = wp_cursor_shape_manager_v1_get_pointer(d->csm, d->ptr);
    }
  }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
  (void)data, (void)seat, (void)name;
}
static const struct wl_seat_listener k_seat = {.capabilities = seat_caps,
                                               .name = seat_name};

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t sub, const char *make,
                         const char *model, int32_t transform) {
  (void)data, (void)o, (void)x, (void)y, (void)pw, (void)ph, (void)sub;
  (void)make, (void)model, (void)transform;
}
static void out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
                     int32_t h, int32_t refresh) {
  (void)data, (void)o, (void)flags, (void)w, (void)h, (void)refresh;
}
static void out_done(void *data, struct wl_output *o) { (void)data, (void)o; }
static void out_scale(void *data, struct wl_output *o, int32_t factor) {
  (void)o;
  ((CTUI_WL_OUTPUT *)data)->scale = factor;
}
static void out_name(void *data, struct wl_output *o, const char *name) {
  (void)o;
  CTUI_WL_OUTPUT *out = data;
  snprintf(out->name, sizeof out->name, "%s", name);
}
static void out_desc(void *data, struct wl_output *o, const char *desc) {
  (void)data, (void)o, (void)desc;
}
static const struct wl_output_listener k_out = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_desc,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener k_wm = {.ping = wm_ping};

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* an output is a pointer its listener keeps: the slots never move while
 * bound (outputs[] is fixed, a removed one only marked) */
static CTUI_WL_OUTPUT *output_slot(CTUI_WL_DPY *d) {
  for (int i = 0; i < d->n_outputs; i++) {
    if (!d->outputs[i].o) {
      return &d->outputs[i];
    }
  }
  return d->n_outputs < CTUI_WL_OUTPUTS ? &d->outputs[d->n_outputs++] : NULL;
}

static void global(void *data, struct wl_registry *reg, uint32_t name,
                   const char *iface, uint32_t version) {
  CTUI_WL_DPY *d = data;
  CTUI_WL_OUTPUT *out;
  if (!strcmp(iface, wl_compositor_interface.name)) {
    d->comp =
        wl_registry_bind(reg, name, &wl_compositor_interface, MIN(version, 6u));
  } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
    d->wm =
        wl_registry_bind(reg, name, &xdg_wm_base_interface, MIN(version, 5u));
    xdg_wm_base_add_listener(d->wm, &k_wm, d);
  } else if (!strcmp(iface, wl_seat_interface.name) && !d->seat) {
    /* 5: the pointer's frames and axis stops, before value120 */
    d->seat = wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5u));
    wl_seat_add_listener(d->seat, &k_seat, d);
  } else if (!strcmp(iface, wl_output_interface.name) && version >= 4 &&
             (out = output_slot(d))) {
    /* 4: its name, what a layer surface is placed by */
    memset(out, 0, sizeof *out);
    out->global = name;
    out->scale = 1;
    out->o = wl_registry_bind(reg, name, &wl_output_interface, 4);
    wl_output_add_listener(out->o, &k_out, out);
  } else if (!strcmp(iface, wp_viewporter_interface.name)) {
    d->vpr = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
  } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
    d->fsm = wl_registry_bind(reg, name,
                              &wp_fractional_scale_manager_v1_interface, 1);
  } else if (!strcmp(iface, wp_presentation_interface.name)) {
    d->pres = wl_registry_bind(reg, name, &wp_presentation_interface,
                               MIN(version, 2u));
    wp_presentation_add_listener(d->pres, &k_pres, d);
  } else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) {
    d->csm =
        wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
  } else if (d->n_extra < CTUI_WL_EXTRA &&
             strlen(iface) < sizeof d->extra[0].iface) {
    CTUI_WL_GLOBAL *g = &d->extra[d->n_extra++];
    g->global = name;
    g->version = version;
    memcpy(g->iface, iface, strlen(iface) + 1);
  }
}

/* an output unplugged: its object goes (layer shell's rule: the
 * compositor closes the surfaces on it) */
static void global_remove(void *data, struct wl_registry *reg, uint32_t name) {
  (void)reg;
  CTUI_WL_DPY *d = data;
  for (int i = 0; i < d->n_outputs; i++) {
    if (d->outputs[i].o && d->outputs[i].global == name) {
      wl_output_release(d->outputs[i].o);
      memset(&d->outputs[i], 0, sizeof d->outputs[i]);
      return;
    }
  }
}
static const struct wl_registry_listener k_reg = {
    .global = global, .global_remove = global_remove};

struct wl_output *ctui_wl_output_find(CTUI_WL_DPY *d, const char *name) {
  for (int i = 0; i < d->n_outputs; i++) {
    if (d->outputs[i].o && !strcmp(d->outputs[i].name, name)) {
      return d->outputs[i].o;
    }
  }
  return NULL;
}

const CTUI_WL_GLOBAL *ctui_wl_global_find(CTUI_WL_DPY *d, const char *iface) {
  for (int i = 0; i < d->n_extra; i++) {
    if (!strcmp(d->extra[i].iface, iface)) {
      return &d->extra[i];
    }
  }
  return NULL;
}

int ctui_wl_outputs(const CTUI_WL_DPY *d, const char **names, int max) {
  int n = 0;
  for (int i = 0; i < d->n_outputs; i++) {
    if (d->outputs[i].o) {
      if (n < max) {
        names[n] = d->outputs[i].name;
      }
      n++;
    }
  }
  return n;
}

/* --- the connection --- */

CTUI_WL_DPY *ctui_wl_connect(char *err, unsigned n) {
  CTUI_WL_DPY *d = calloc(1, sizeof *d);
  if (!d) {
    snprintf(err, n, "out of memory");
    return NULL;
  }
  d->repeat_fd = -1;
  d->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  d->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (!d->xkb || d->repeat_fd < 0) {
    snprintf(err, n, "no xkbcommon context or timerfd");
    ctui_wl_disconnect(d);
    return NULL;
  }
  /* compose and dead keys by the user's locale, as a terminal's are */
  const char *loc = setlocale(LC_CTYPE, NULL);
  const char *env = getenv("LC_ALL");
  env = env && *env ? env : getenv("LC_CTYPE");
  env = env && *env ? env : getenv("LANG");
  struct xkb_compose_table *ct =
      xkb_compose_table_new_from_locale(d->xkb,
                                        env && *env ? env
                                        : loc       ? loc
                                                    : "C",
                                        XKB_COMPOSE_COMPILE_NO_FLAGS);
  if (ct) {
    d->compose = xkb_compose_state_new(ct, XKB_COMPOSE_STATE_NO_FLAGS);
    xkb_compose_table_unref(ct);
  }
  d->dpy = wl_display_connect(NULL);
  if (!d->dpy) {
    snprintf(err, n, "no Wayland compositor (WAYLAND_DISPLAY)");
    ctui_wl_disconnect(d);
    return NULL;
  }
  d->reg = wl_display_get_registry(d->dpy);
  wl_registry_add_listener(d->reg, &k_reg, d);
  wl_display_roundtrip(d->dpy);
  if (!d->comp) {
    snprintf(err, n, "the compositor has no wl_compositor");
    ctui_wl_disconnect(d);
    return NULL;
  }
  /* the seat's devices, the keymap and the outputs' names in their own
   * round trips */
  wl_display_roundtrip(d->dpy);
  wl_display_roundtrip(d->dpy);
  ctui_logf(E_INF,
            "[CTUI:WL] - connected (%d outputs, cursor shapes %s, compose %s, "
            "timing %s) @ tick %d\n",
            ctui_wl_outputs(d, NULL, 0), d->csm ? "on" : "off",
            d->compose ? "on" : "off", d->pres ? "on" : "off",
            ctui_tick_advance());
  return d;
}

void ctui_wl_disconnect(CTUI_WL_DPY *d) {
  if (!d) {
    return;
  }
  if (d->surfaces) {
    ctui_logf(E_WRN, "[CTUI:WL] - disconnected with %d surfaces open\n",
              d->surfaces);
  }
  if (d->layer_shell) {
    d->layer_shell_free(d->layer_shell);
  }
  if (d->cursor) {
    wp_cursor_shape_device_v1_destroy(d->cursor);
  }
  if (d->ptr) {
    wl_pointer_destroy(d->ptr);
  }
  if (d->kb) {
    wl_keyboard_destroy(d->kb);
  }
  for (int i = 0; i < d->n_outputs; i++) {
    if (d->outputs[i].o) {
      wl_output_release(d->outputs[i].o);
    }
  }
  if (d->csm) {
    wp_cursor_shape_manager_v1_destroy(d->csm);
  }
  if (d->pres) {
    wp_presentation_destroy(d->pres);
  }
  if (d->fsm) {
    wp_fractional_scale_manager_v1_destroy(d->fsm);
  }
  if (d->vpr) {
    wp_viewporter_destroy(d->vpr);
  }
  if (d->seat) {
    wl_seat_destroy(d->seat);
  }
  if (d->wm) {
    xdg_wm_base_destroy(d->wm);
  }
  if (d->comp) {
    wl_compositor_destroy(d->comp);
  }
  if (d->reg) {
    wl_registry_destroy(d->reg);
  }
  if (d->dpy) {
    wl_display_disconnect(d->dpy);
  }
  xkb_compose_state_unref(d->compose);
  xkb_state_unref(d->state);
  xkb_keymap_unref(d->keymap);
  xkb_context_unref(d->xkb);
  if (d->repeat_fd >= 0) {
    close(d->repeat_fd);
  }
  free(d);
}

void ctui_wl_dpy_focus_events(CTUI_WL_DPY *d, int on) { d->focus_events = on; }

struct wl_display *ctui_wl_dpy_display(CTUI_WL_DPY *d) { return d->dpy; }
int ctui_wl_dpy_fd(CTUI_WL_DPY *d) { return wl_display_get_fd(d->dpy); }
int ctui_wl_dpy_repeat_fd(CTUI_WL_DPY *d) { return d->repeat_fd; }

int ctui_wl_dpy_dispatch(CTUI_WL_DPY *d) {
  if (wl_display_prepare_read(d->dpy) == 0 &&
      wl_display_read_events(d->dpy) < 0) {
    return -1;
  }
  return wl_display_dispatch_pending(d->dpy) < 0 ? -1 : 0;
}

void ctui_wl_dpy_flush(CTUI_WL_DPY *d) {
  wl_display_dispatch_pending(d->dpy);
  wl_display_flush(d->dpy);
}

/* --- a surface --- */

static void fs_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                         uint32_t scale) {
  (void)fs;
  ((CTUI_WL *)data)->scale120 = (int)scale;
}
static const struct wp_fractional_scale_v1_listener k_fs = {.preferred_scale =
                                                                fs_preferred};

static void surf_enter(void *data, struct wl_surface *s, struct wl_output *o) {
  (void)data, (void)s, (void)o;
}
static void surf_leave(void *data, struct wl_surface *s, struct wl_output *o) {
  (void)data, (void)s, (void)o;
}
static void surf_scale(void *data, struct wl_surface *s, int32_t scale) {
  (void)s;
  ((CTUI_WL *)data)->scale_int = scale > 0 ? scale : 1;
}
static void surf_transform(void *data, struct wl_surface *s, uint32_t t) {
  (void)data, (void)s, (void)t;
}
static const struct wl_surface_listener k_surf = {
    .enter = surf_enter,
    .leave = surf_leave,
    .preferred_buffer_scale = surf_scale,
    .preferred_buffer_transform = surf_transform,
};

CTUI_WL *ctui_wl_surface_new(CTUI_WL_DPY *d, int w, int h) {
  CTUI_WL *wl = calloc(1, sizeof *wl);
  if (!wl) {
    return NULL;
  }
  wl->d = d;
  wl->w = w;
  wl->h = h;
  wl->scale_int = 1;
  wl->surf = wl_compositor_create_surface(d->comp);
  wl_surface_add_listener(wl->surf, &k_surf, wl);
  if (d->vpr && d->fsm) {
    wl->vp = wp_viewporter_get_viewport(d->vpr, wl->surf);
    wl->fs =
        wp_fractional_scale_manager_v1_get_fractional_scale(d->fsm, wl->surf);
    wp_fractional_scale_v1_add_listener(wl->fs, &k_fs, wl);
  }
  d->surfaces++;
  return wl;
}

int ctui_wl_surface_wait(CTUI_WL *wl) {
  wl_surface_commit(wl->surf);
  while (!wl->configured && !wl->closed) {
    if (wl_display_dispatch(wl->d->dpy) < 0) {
      return -1;
    }
  }
  /* the scale the compositor wants for it, in its own round trip */
  return wl_display_roundtrip(wl->d->dpy) < 0 ? -1 : 0;
}

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
  CTUI_WL *wl = data;
  xdg_surface_ack_configure(xs, serial);
  wl->configured = 1;
}
static const struct xdg_surface_listener k_xs = {.configure = xs_configure};

static void top_configure(void *data, struct xdg_toplevel *top, int32_t w,
                          int32_t h, struct wl_array *states) {
  (void)top, (void)states;
  CTUI_WL *wl = data;
  if (w > 0 && h > 0) {
    wl->w = w;
    wl->h = h;
  }
}
static void top_close(void *data, struct xdg_toplevel *top) {
  (void)top;
  ((CTUI_WL *)data)->closed = 1;
}
static void top_bounds(void *data, struct xdg_toplevel *top, int32_t w,
                       int32_t h) {
  (void)data, (void)top, (void)w, (void)h;
}
static void top_caps(void *data, struct xdg_toplevel *top,
                     struct wl_array *caps) {
  (void)data, (void)top, (void)caps;
}
static const struct xdg_toplevel_listener k_top = {
    .configure = top_configure,
    .close = top_close,
    .configure_bounds = top_bounds,
    .wm_capabilities = top_caps,
};

CTUI_WL *ctui_wl_window(CTUI_WL_DPY *d, const char *title, const char *app_id,
                        int w, int h, char *err, unsigned n) {
  if (!d->wm) {
    snprintf(err, n, "the compositor has no xdg_wm_base");
    return NULL;
  }
  CTUI_WL *wl = ctui_wl_surface_new(d, w, h);
  if (!wl) {
    snprintf(err, n, "out of memory");
    return NULL;
  }
  wl->xs = xdg_wm_base_get_xdg_surface(d->wm, wl->surf);
  xdg_surface_add_listener(wl->xs, &k_xs, wl);
  wl->top = xdg_surface_get_toplevel(wl->xs);
  xdg_toplevel_add_listener(wl->top, &k_top, wl);
  xdg_toplevel_set_title(wl->top, title);
  xdg_toplevel_set_app_id(wl->top, app_id);
  if (ctui_wl_surface_wait(wl) < 0) {
    snprintf(err, n, "the compositor went before the window was configured");
    ctui_wl_close(wl);
    return NULL;
  }
  ctui_logf(E_INF, "[CTUI:WL] - window %dx%d (scale %d/120, %s) @ tick %d\n",
            wl->w, wl->h, scale120(wl), wl->vp ? "fractional" : "whole",
            ctui_tick_advance());
  return wl;
}

CTUI_WL *ctui_wl_open(const char *title, const char *app_id, int w, int h,
                      char *err, unsigned n) {
  CTUI_WL_DPY *d = ctui_wl_connect(err, n);
  if (!d) {
    return NULL;
  }
  CTUI_WL *wl = ctui_wl_window(d, title, app_id, w, h, err, n);
  if (!wl) {
    ctui_wl_disconnect(d);
    return NULL;
  }
  wl->owns_dpy = 1;
  return wl;
}

void ctui_wl_close(CTUI_WL *wl) {
  if (!wl) {
    return;
  }
  CTUI_WL_DPY *d = wl->d;
  if (d->kb_on == wl) {
    repeat_arm(d, 0);
    d->kb_on = NULL;
  }
  if (d->ptr_on == wl) {
    d->ptr_on = NULL;
    d->held = 0;
  }
  for (unsigned i = 0; i < d->count; i++) {
    CTUI_WL_QEV *q = &d->queue[(d->head + i) % CTUI_WL_QUEUE];
    if (q->on == wl) {
      q->on = NULL;
    }
  }
  if (wl->role_free) {
    wl->role_free(wl);
  }
  if (wl->top) {
    xdg_toplevel_destroy(wl->top);
  }
  if (wl->xs) {
    xdg_surface_destroy(wl->xs);
  }
  if (wl->fs) {
    wp_fractional_scale_v1_destroy(wl->fs);
  }
  if (wl->vp) {
    wp_viewport_destroy(wl->vp);
  }
  if (wl->surf) {
    wl_surface_destroy(wl->surf);
  }
  d->surfaces--;
  int owns = wl->owns_dpy;
  free(wl);
  if (owns) {
    ctui_wl_disconnect(d);
  } else {
    wl_display_flush(d->dpy);
  }
}

CTUI_WL_DPY *ctui_wl_dpy(CTUI_WL *wl) { return wl->d; }
struct wl_display *ctui_wl_display(CTUI_WL *wl) { return wl->d->dpy; }
struct wl_surface *ctui_wl_surface(CTUI_WL *wl) { return wl->surf; }
int ctui_wl_fd(CTUI_WL *wl) { return ctui_wl_dpy_fd(wl->d); }
int ctui_wl_repeat_fd(CTUI_WL *wl) { return wl->d->repeat_fd; }
int ctui_wl_dispatch(CTUI_WL *wl) { return ctui_wl_dpy_dispatch(wl->d); }
void ctui_wl_flush(CTUI_WL *wl) { ctui_wl_dpy_flush(wl->d); }

void ctui_wl_set_user(CTUI_WL *wl, void *user) { wl->user = user; }
void *ctui_wl_user(const CTUI_WL *wl) { return wl->user; }

int ctui_wl_size(CTUI_WL *wl, int *pw, int *ph) {
  int s = scale120(wl);
  int w = (wl->w * s + 60) / 120, h = (wl->h * s + 60) / 120;
  int changed = w != wl->pw || h != wl->ph;
  wl->pw = *pw = w;
  wl->ph = *ph = h;
  return changed;
}

void ctui_wl_logical(const CTUI_WL *wl, int *w, int *h) {
  *w = wl->w;
  *h = wl->h;
}

void ctui_wl_viewport(CTUI_WL *wl) {
  if (wl->vp && wl->scale120) {
    wl_surface_set_buffer_scale(wl->surf, 1);
    wp_viewport_set_destination(wl->vp, wl->w, wl->h);
  } else {
    wl_surface_set_buffer_scale(wl->surf, wl->scale_int);
  }
}

int ctui_wl_scale120(CTUI_WL *wl) { return scale120(wl); }

void ctui_wl_cell(CTUI_WL *wl, int cw, int ch) {
  wl->cw = cw;
  wl->ch = ch;
}

void ctui_wl_cell_origin(CTUI_WL *wl, int x, int y) {
  wl->cx0 = x;
  wl->cy0 = y;
}

int ctui_wl_closed(const CTUI_WL *wl) { return wl->closed; }

void ctui_wl_title(CTUI_WL *wl, const char *title) {
  if (wl->top) {
    xdg_toplevel_set_title(wl->top, title);
  }
}
