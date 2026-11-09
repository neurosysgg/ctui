#define _POSIX_C_SOURCE 200809L
#include "wl.h"

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

/* input events waiting for ctui_wl_next(), a ring */
#define QUEUE 256u

typedef struct {
  CTUI_EVENT ev;
  union {
    CTUI_KEYPRESS_EVENT_DATA key;
    CTUI_MOUSE_EVENT_DATA mouse;
  } d;
} QEV;

struct CTUI_WL {
  struct wl_display *dpy;
  struct wl_registry *reg;
  struct wl_compositor *comp;
  struct xdg_wm_base *wm;
  struct wl_seat *seat;
  struct wl_keyboard *kb;
  struct wl_pointer *ptr;
  struct wp_viewporter *vpr;
  struct wp_fractional_scale_manager_v1 *fsm;
  struct wp_cursor_shape_manager_v1 *csm;
  struct wp_cursor_shape_device_v1 *cursor;
  struct wl_surface *surf;
  struct xdg_surface *xs;
  struct xdg_toplevel *top;
  struct wp_viewport *vp;
  struct wp_fractional_scale_v1 *fs;
  struct wp_presentation *pres;
  CTUI_WL_TIMING timing;
  CTUI_WL_RAW_KEY raw;
  void *raw_ctx;
  int w, h;      /* logical, the compositor's or ours */
  int scale120;  /* the scale in 120ths (fractional) */
  int scale_int; /* or whole (wl_surface's preferred buffer scale) */
  int pw, ph;    /* the buffer's last size handed out */
  int configured, closed;
  int cw, ch; /* a cell in buffer pixels */

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *state;
  struct xkb_compose_state *compose;

  /* the key repeat: the compositor's rate and delay, the key held */
  int repeat_fd;
  int rate, delay;
  uint32_t repeat_key;
  QEV repeat_ev;

  /* the pointer: where (buffer pixels, cells), the buttons held, the
   * scroll not yet a whole notch */
  double px, py;
  int row, col;
  unsigned held;
  double scroll;

  QEV queue[QUEUE];
  unsigned head, count;
  QEV cur;
};

static unsigned mods_now(const CTUI_WL *wl) {
  if (!wl->state) {
    return 0;
  }
  unsigned m = 0;
  if (xkb_state_mod_name_is_active(wl->state, XKB_MOD_NAME_SHIFT,
                                   XKB_STATE_MODS_EFFECTIVE) > 0) {
    m |= CTUI_MOD_SHIFT;
  }
  if (xkb_state_mod_name_is_active(wl->state, XKB_MOD_NAME_CTRL,
                                   XKB_STATE_MODS_EFFECTIVE) > 0) {
    m |= CTUI_MOD_CTRL;
  }
  if (xkb_state_mod_name_is_active(wl->state, XKB_MOD_NAME_ALT,
                                   XKB_STATE_MODS_EFFECTIVE) > 0) {
    m |= CTUI_MOD_ALT;
  }
  return m;
}

static void push(CTUI_WL *wl, const QEV *q) {
  if (wl->count == QUEUE) {
    ctui_logf(E_WRN, "[CTUI:WL] - input queue full, an event dropped\n");
    return;
  }
  wl->queue[(wl->head + wl->count++) % QUEUE] = *q;
}

int ctui_wl_next(CTUI_WL *wl, CTUI_EVENT *ev) {
  if (!wl->count) {
    return 0;
  }
  wl->cur = wl->queue[wl->head];
  wl->head = (wl->head + 1) % QUEUE;
  wl->count--;
  *ev = wl->cur.ev;
  ev->event_data = &wl->cur.d;
  return 1;
}

int ctui_wl_queued(const CTUI_WL *wl) { return wl->count > 0; }

/* --- keys --- */

static void repeat_arm(CTUI_WL *wl, int on) {
  struct itimerspec it = {0};
  if (on && wl->rate > 0) {
    long iv = 1000000000L / wl->rate;
    it.it_value.tv_sec = wl->delay / 1000;
    it.it_value.tv_nsec = (long)(wl->delay % 1000) * 1000000L;
    it.it_interval.tv_nsec = iv % 1000000000L;
    it.it_interval.tv_sec = iv / 1000000000L;
    if (!it.it_value.tv_sec && !it.it_value.tv_nsec) {
      it.it_value.tv_nsec = 1;
    }
  }
  if (!on) {
    wl->repeat_key = 0;
  }
  timerfd_settime(wl->repeat_fd, 0, &it, NULL);
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

static int key_event(const CTUI_WL *wl, xkb_keysym_t sym, uint32_t cp, QEV *q) {
  memset(q, 0, sizeof *q);
  if (!ctui_wl_key_event(sym, cp, mods_now(wl), &q->d.key)) {
    return 0;
  }
  q->ev.type = CTUI_KEYPRESS_EVENT;
  q->ev.scope = CTUI_EVENT_SCOPE_GLOBAL;
  q->ev.ev_source = "input";
  return 1;
}

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                      int32_t fd, uint32_t size) {
  (void)kb;
  CTUI_WL *wl = data;
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
      wl->xkb, map, strnlen(map, size), XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map, size);
  if (!km) {
    ctui_logf(E_WRN, "[CTUI:WL] - the compositor's keymap didn't compile\n");
    return;
  }
  xkb_state_unref(wl->state);
  xkb_keymap_unref(wl->keymap);
  wl->keymap = km;
  wl->state = xkb_state_new(km);
  ctui_logf(E_INF, "[CTUI:WL] - keymap loaded @ tick %d\n",
            ctui_tick_advance());
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *s, struct wl_array *keys) {
  (void)data, (void)kb, (void)serial, (void)s, (void)keys;
}

/* focus gone: nothing stays held */
static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *s) {
  (void)kb, (void)serial, (void)s;
  CTUI_WL *wl = data;
  repeat_arm(wl, 0);
  if (wl->raw) {
    wl->raw(wl->raw_ctx, 0, 0);
  }
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
  (void)kb, (void)serial, (void)time;
  CTUI_WL *wl = data;
  xkb_keycode_t code = key + 8;
  if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    if (wl->repeat_key == code) {
      repeat_arm(wl, 0);
    }
    if (wl->raw) {
      wl->raw(wl->raw_ctx, key, 0);
    }
    return;
  }
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED ||
      (wl->raw && wl->raw(wl->raw_ctx, key, 1)) || !wl->state) {
    return;
  }
  xkb_keysym_t sym = xkb_state_key_get_one_sym(wl->state, code);
  uint32_t cp = xkb_state_key_get_utf32(wl->state, code);
  if (wl->compose &&
      xkb_compose_state_feed(wl->compose, sym) == XKB_COMPOSE_FEED_ACCEPTED) {
    switch (xkb_compose_state_get_status(wl->compose)) {
    case XKB_COMPOSE_COMPOSING:
      return;
    case XKB_COMPOSE_CANCELLED:
      xkb_compose_state_reset(wl->compose);
      return;
    case XKB_COMPOSE_COMPOSED: {
      char u[16];
      sym = xkb_compose_state_get_one_sym(wl->compose);
      xkb_compose_state_get_utf8(wl->compose, u, sizeof u);
      xkb_compose_state_reset(wl->compose);
      cp = 0;
      ctui_utf8_decode(u, &cp);
      break;
    }
    case XKB_COMPOSE_NOTHING:
      break;
    }
  }
  QEV q;
  if (!key_event(wl, sym, cp, &q)) {
    return;
  }
  push(wl, &q);
  if (xkb_keymap_key_repeats(wl->keymap, code)) {
    wl->repeat_ev = q;
    repeat_arm(wl, 1);
    wl->repeat_key = code;
  }
}

static void kb_mods(void *data, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
  (void)kb, (void)serial;
  CTUI_WL *wl = data;
  if (wl->state) {
    xkb_state_update_mask(wl->state, dep, lat, lock, 0, 0, grp);
  }
}

static void kb_repeat(void *data, struct wl_keyboard *kb, int32_t rate,
                      int32_t delay) {
  (void)kb;
  CTUI_WL *wl = data;
  wl->rate = rate;
  wl->delay = delay;
}

static const struct wl_keyboard_listener k_kb = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_mods,
    .repeat_info = kb_repeat,
};

void ctui_wl_repeat(CTUI_WL *wl) {
  uint64_t n = 0;
  if (read(wl->repeat_fd, &n, sizeof n) != (ssize_t)sizeof n ||
      !wl->repeat_key) {
    return;
  }
  /* a host that fell behind doesn't get a burst */
  for (uint64_t i = 0; i < n && i < 4; i++) {
    push(wl, &wl->repeat_ev);
  }
}

/* --- the pointer --- */

static int scale120(const CTUI_WL *wl) {
  return wl->vp && wl->scale120 ? wl->scale120 : wl->scale_int * 120;
}

static void mouse(CTUI_WL *wl, CTUI_MOUSE_ACTION action, int button) {
  QEV q;
  memset(&q, 0, sizeof q);
  q.ev.type = CTUI_MOUSE_EVENT;
  q.ev.scope = CTUI_EVENT_SCOPE_GLOBAL;
  q.ev.ev_source = "input";
  q.d.mouse.action = action;
  q.d.mouse.button = button;
  q.d.mouse.row = wl->row;
  q.d.mouse.col = wl->col;
  q.d.mouse.px = (int)wl->px;
  q.d.mouse.py = (int)wl->py;
  q.d.mouse.mods = mods_now(wl);
  push(wl, &q);
}

/* the lowest button held, as a terminal reports a drag; -1 for none */
static int held_button(const CTUI_WL *wl) {
  for (int b = 0; b < 8; b++) {
    if (wl->held & (1u << b)) {
      return b;
    }
  }
  return -1;
}

/* where the pointer is; 1 when that's another cell */
static int moved(CTUI_WL *wl, wl_fixed_t x, wl_fixed_t y) {
  double s = scale120(wl) / 120.0;
  wl->px = wl_fixed_to_double(x) * s;
  wl->py = wl_fixed_to_double(y) * s;
  int col = wl->cw > 0 ? (int)wl->px / wl->cw : 0;
  int row = wl->ch > 0 ? (int)wl->py / wl->ch : 0;
  int changed = col != wl->col || row != wl->row;
  wl->col = col;
  wl->row = row;
  return changed;
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
  (void)p, (void)s;
  CTUI_WL *wl = data;
  if (wl->cursor) {
    wp_cursor_shape_device_v1_set_shape(
        wl->cursor, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
  }
  moved(wl, x, y);
  mouse(wl, CTUI_MOUSE_MOTION, held_button(wl));
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {
  (void)p, (void)serial, (void)s;
  ((CTUI_WL *)data)->held = 0;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t x, wl_fixed_t y) {
  (void)p, (void)time;
  CTUI_WL *wl = data;
  if (moved(wl, x, y)) {
    mouse(wl, CTUI_MOUSE_MOTION, held_button(wl));
  }
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
  (void)p, (void)serial, (void)time;
  CTUI_WL *wl = data;
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
  wl->held = down ? wl->held | 1u << b : wl->held & ~(1u << b);
  mouse(wl, down ? CTUI_MOUSE_PRESS : CTUI_MOUSE_RELEASE, b);
}

/* a wheel's notch is 10 (wl_pointer's axis, before value120) */
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {
  (void)p, (void)time;
  CTUI_WL *wl = data;
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }
  wl->scroll += wl_fixed_to_double(value);
  for (; wl->scroll >= 10.0; wl->scroll -= 10.0) {
    mouse(wl, CTUI_MOUSE_SCROLL_DOWN, -1);
  }
  for (; wl->scroll <= -10.0; wl->scroll += 10.0) {
    mouse(wl, CTUI_MOUSE_SCROLL_UP, -1);
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
    ((CTUI_WL *)data)->scroll = 0;
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
  ((CTUI_WL *)data)->timing.clock_ok = clk == CLOCK_MONOTONIC;
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
  FEEDBACK *fb = wl->pres ? malloc(sizeof *fb) : NULL;
  if (!fb) {
    return;
  }
  fb->wl = wl;
  fb->tag = tag_ns;
  struct wp_presentation_feedback *f =
      wp_presentation_feedback(wl->pres, wl->surf);
  wp_presentation_feedback_add_listener(f, &k_fb, fb);
  wl->timing.asked++;
}

const CTUI_WL_TIMING *ctui_wl_timing(const CTUI_WL *wl) { return &wl->timing; }

void ctui_wl_set_raw_key(CTUI_WL *wl, CTUI_WL_RAW_KEY fn, void *ctx) {
  wl->raw = fn;
  wl->raw_ctx = ctx;
}

/* --- the seat, the registry, the window --- */

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
  CTUI_WL *wl = data;
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->kb) {
    wl->kb = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(wl->kb, &k_kb, wl);
  }
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->ptr) {
    wl->ptr = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(wl->ptr, &k_ptr, wl);
    if (wl->csm) {
      wl->cursor = wp_cursor_shape_manager_v1_get_pointer(wl->csm, wl->ptr);
    }
  }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
  (void)data, (void)seat, (void)name;
}
static const struct wl_seat_listener k_seat = {.capabilities = seat_caps,
                                               .name = seat_name};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener k_wm = {.ping = wm_ping};

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static void global(void *data, struct wl_registry *reg, uint32_t name,
                   const char *iface, uint32_t version) {
  CTUI_WL *wl = data;
  if (!strcmp(iface, wl_compositor_interface.name)) {
    wl->comp =
        wl_registry_bind(reg, name, &wl_compositor_interface, MIN(version, 6u));
  } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
    wl->wm =
        wl_registry_bind(reg, name, &xdg_wm_base_interface, MIN(version, 5u));
    xdg_wm_base_add_listener(wl->wm, &k_wm, wl);
  } else if (!strcmp(iface, wl_seat_interface.name) && !wl->seat) {
    /* 5: the pointer's frames and axis stops, before value120 */
    wl->seat =
        wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5u));
    wl_seat_add_listener(wl->seat, &k_seat, wl);
  } else if (!strcmp(iface, wp_viewporter_interface.name)) {
    wl->vpr = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
  } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
    wl->fsm = wl_registry_bind(reg, name,
                               &wp_fractional_scale_manager_v1_interface, 1);
  } else if (!strcmp(iface, wp_presentation_interface.name)) {
    wl->pres = wl_registry_bind(reg, name, &wp_presentation_interface,
                                MIN(version, 2u));
    wp_presentation_add_listener(wl->pres, &k_pres, wl);
  } else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) {
    wl->csm =
        wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
  }
}
static void global_remove(void *data, struct wl_registry *reg, uint32_t name) {
  (void)data, (void)reg, (void)name;
}
static const struct wl_registry_listener k_reg = {
    .global = global, .global_remove = global_remove};

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

CTUI_WL *ctui_wl_open(const char *title, const char *app_id, int w, int h,
                      char *err, unsigned n) {
  CTUI_WL *wl = calloc(1, sizeof *wl);
  if (!wl) {
    snprintf(err, n, "out of memory");
    return NULL;
  }
  wl->w = w;
  wl->h = h;
  wl->scale_int = 1;
  wl->repeat_fd = -1;
  wl->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  wl->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (!wl->xkb || wl->repeat_fd < 0) {
    snprintf(err, n, "no xkbcommon context or timerfd");
    ctui_wl_close(wl);
    return NULL;
  }
  /* compose and dead keys by the user's locale, as a terminal's are */
  const char *loc = setlocale(LC_CTYPE, NULL);
  const char *env = getenv("LC_ALL");
  env = env && *env ? env : getenv("LC_CTYPE");
  env = env && *env ? env : getenv("LANG");
  struct xkb_compose_table *ct =
      xkb_compose_table_new_from_locale(wl->xkb,
                                        env && *env ? env
                                        : loc       ? loc
                                                    : "C",
                                        XKB_COMPOSE_COMPILE_NO_FLAGS);
  if (ct) {
    wl->compose = xkb_compose_state_new(ct, XKB_COMPOSE_STATE_NO_FLAGS);
    xkb_compose_table_unref(ct);
  }
  wl->dpy = wl_display_connect(NULL);
  if (!wl->dpy) {
    snprintf(err, n, "no Wayland compositor (WAYLAND_DISPLAY)");
    ctui_wl_close(wl);
    return NULL;
  }
  wl->reg = wl_display_get_registry(wl->dpy);
  wl_registry_add_listener(wl->reg, &k_reg, wl);
  wl_display_roundtrip(wl->dpy);
  if (!wl->comp || !wl->wm) {
    snprintf(err, n, "the compositor has no %s",
             wl->comp ? "xdg_wm_base" : "wl_compositor");
    ctui_wl_close(wl);
    return NULL;
  }
  wl->surf = wl_compositor_create_surface(wl->comp);
  wl_surface_add_listener(wl->surf, &k_surf, wl);
  if (wl->vpr && wl->fsm) {
    wl->vp = wp_viewporter_get_viewport(wl->vpr, wl->surf);
    wl->fs =
        wp_fractional_scale_manager_v1_get_fractional_scale(wl->fsm, wl->surf);
    wp_fractional_scale_v1_add_listener(wl->fs, &k_fs, wl);
  }
  wl->xs = xdg_wm_base_get_xdg_surface(wl->wm, wl->surf);
  xdg_surface_add_listener(wl->xs, &k_xs, wl);
  wl->top = xdg_surface_get_toplevel(wl->xs);
  xdg_toplevel_add_listener(wl->top, &k_top, wl);
  xdg_toplevel_set_title(wl->top, title);
  xdg_toplevel_set_app_id(wl->top, app_id);
  wl_surface_commit(wl->surf);
  while (!wl->configured && wl_display_dispatch(wl->dpy) >= 0) {
  }
  /* the seat's devices, the keymap and the scale in their own round trip */
  wl_display_roundtrip(wl->dpy);
  wl_display_roundtrip(wl->dpy);
  ctui_logf(E_INF,
            "[CTUI:WL] - window %dx%d (scale %d/120, %s, cursor shapes %s, "
            "compose %s, timing %s) @ tick %d\n",
            wl->w, wl->h, scale120(wl), wl->vp ? "fractional" : "whole",
            wl->csm ? "on" : "off", wl->compose ? "on" : "off",
            wl->pres ? "on" : "off", ctui_tick_advance());
  return wl;
}

void ctui_wl_close(CTUI_WL *wl) {
  if (!wl) {
    return;
  }
  if (wl->cursor) {
    wp_cursor_shape_device_v1_destroy(wl->cursor);
  }
  if (wl->ptr) {
    wl_pointer_destroy(wl->ptr);
  }
  if (wl->kb) {
    wl_keyboard_destroy(wl->kb);
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
  if (wl->csm) {
    wp_cursor_shape_manager_v1_destroy(wl->csm);
  }
  if (wl->pres) {
    wp_presentation_destroy(wl->pres);
  }
  if (wl->fsm) {
    wp_fractional_scale_manager_v1_destroy(wl->fsm);
  }
  if (wl->vpr) {
    wp_viewporter_destroy(wl->vpr);
  }
  if (wl->seat) {
    wl_seat_destroy(wl->seat);
  }
  if (wl->wm) {
    xdg_wm_base_destroy(wl->wm);
  }
  if (wl->comp) {
    wl_compositor_destroy(wl->comp);
  }
  if (wl->reg) {
    wl_registry_destroy(wl->reg);
  }
  if (wl->dpy) {
    wl_display_disconnect(wl->dpy);
  }
  xkb_compose_state_unref(wl->compose);
  xkb_state_unref(wl->state);
  xkb_keymap_unref(wl->keymap);
  xkb_context_unref(wl->xkb);
  if (wl->repeat_fd >= 0) {
    close(wl->repeat_fd);
  }
  free(wl);
}

struct wl_display *ctui_wl_display(CTUI_WL *wl) { return wl->dpy; }
struct wl_surface *ctui_wl_surface(CTUI_WL *wl) { return wl->surf; }
int ctui_wl_fd(CTUI_WL *wl) { return wl_display_get_fd(wl->dpy); }
int ctui_wl_repeat_fd(CTUI_WL *wl) { return wl->repeat_fd; }

int ctui_wl_dispatch(CTUI_WL *wl) {
  if (wl_display_prepare_read(wl->dpy) == 0 &&
      wl_display_read_events(wl->dpy) < 0) {
    return -1;
  }
  return wl_display_dispatch_pending(wl->dpy) < 0 ? -1 : 0;
}

void ctui_wl_flush(CTUI_WL *wl) {
  wl_display_dispatch_pending(wl->dpy);
  wl_display_flush(wl->dpy);
}

int ctui_wl_size(CTUI_WL *wl, int *pw, int *ph) {
  int s = scale120(wl);
  int w = (wl->w * s + 60) / 120, h = (wl->h * s + 60) / 120;
  int changed = w != wl->pw || h != wl->ph;
  wl->pw = *pw = w;
  wl->ph = *ph = h;
  return changed;
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

int ctui_wl_closed(const CTUI_WL *wl) { return wl->closed; }

void ctui_wl_title(CTUI_WL *wl, const char *title) {
  xdg_toplevel_set_title(wl->top, title);
}
