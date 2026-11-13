#ifndef CTUI_WL_INTERNAL_H
#define CTUI_WL_INTERNAL_H

/* src/wl/'s own: the connection and a surface as wl.c and layer.c share
 * them. Never included outside src/wl/. */

#include "wl.h"

#include <stdint.h>

struct wl_compositor;
struct wl_output;
struct wl_keyboard;
struct wl_pointer;
struct wl_registry;
struct wl_seat;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;
struct wp_presentation;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;
struct xkb_compose_state;

/* input events waiting for ctui_wl_next(), a ring */
#define CTUI_WL_QUEUE 256u
/* outputs, and globals wl.c doesn't bind itself (layer.c's) */
#define CTUI_WL_OUTPUTS 16
#define CTUI_WL_EXTRA 64

typedef struct {
  CTUI_EVENT ev;
  union {
    CTUI_KEYPRESS_EVENT_DATA key;
    CTUI_MOUSE_EVENT_DATA mouse;
    CTUI_FOCUS_EVENT_DATA focus;
  } d;
  CTUI_WL *on; /* the surface it's for; NULL once that one closed */
} CTUI_WL_QEV;

typedef struct {
  struct wl_output *o;
  uint32_t global;
  int scale;
  char name[32];
} CTUI_WL_OUTPUT;

typedef struct {
  uint32_t global, version;
  char iface[48];
} CTUI_WL_GLOBAL;

struct CTUI_WL_DPY {
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
  struct wp_presentation *pres;
  int clock_ok; /* wp_presentation's clock is MONOTONIC */

  CTUI_WL_OUTPUT outputs[CTUI_WL_OUTPUTS];
  int n_outputs;
  CTUI_WL_GLOBAL extra[CTUI_WL_EXTRA];
  int n_extra;
  /* layer.c's zwlr_layer_shell_v1, bound on its first surface */
  void *layer_shell;
  void (*layer_shell_free)(void *shell);
  /* dnd.c's drag and drop, made by ctui_wl_dnd_init() */
  void *dnd;
  void (*dnd_free)(CTUI_WL_DPY *d);
  void (*dnd_surface_gone)(CTUI_WL_DPY *d, CTUI_WL *wl);

  CTUI_WL_RAW_KEY raw;
  void *raw_ctx;
  int focus_events; /* CTUI_FOCUS_EVENTs on the keyboard's enter / leave */
  int surfaces;     /* open on this connection */
  int frames_done;  /* frame callbacks come in since the last flush */

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *state;
  struct xkb_compose_state *compose;

  /* the key repeat: the compositor's rate and delay, the key held */
  int repeat_fd;
  int rate, delay;
  uint32_t repeat_key;
  CTUI_WL_QEV repeat_ev;

  /* which surface has the keyboard, which the pointer */
  CTUI_WL *kb_on, *ptr_on;
  uint32_t ptr_serial; /* the pointer's last enter: a shape needs it */
  /* the left button's press while it's held (0: up): a drag starts on it */
  uint32_t press_serial;
  /* the pointer: where (buffer pixels of ptr_on, its cells), the buttons
   * held, the scroll not yet a whole notch */
  double px, py;
  int row, col;
  unsigned held;
  double scroll;

  CTUI_WL_QEV queue[CTUI_WL_QUEUE];
  unsigned head, count;
  CTUI_WL_QEV cur;
};

struct CTUI_WL {
  CTUI_WL_DPY *d;
  int owns_dpy; /* ctui_wl_open()'s: closing it disconnects */
  struct wl_surface *surf;
  struct wp_viewport *vp;
  struct wp_fractional_scale_v1 *fs;
  /* a window's role */
  struct xdg_surface *xs;
  struct xdg_toplevel *top;
  /* another role's (layer.c): its object, and how it goes */
  void *role;
  void (*role_free)(CTUI_WL *wl);
  void *user;
  CTUI_WL_TIMING timing;
  int w, h;      /* logical, the compositor's or ours */
  int scale120;  /* the scale in 120ths (fractional) */
  int scale_int; /* or whole (wl_surface's preferred buffer scale) */
  int pw, ph;    /* the buffer's last size handed out */
  int configured, closed;
  int cw, ch; /* a cell in buffer pixels */
  int cx0, cy0; /* the cells' origin in buffer pixels (a host's inset) */
  struct wl_callback *frame; /* asked for, not come yet (ctui_wl_frame) */
  uint32_t shape; /* the pointer's over it (cursor-shape-v1's; 0: default) */
};

/* a bare surface on d (scale, viewport), no role yet; NULL when out of
 * memory */
CTUI_WL *ctui_wl_surface_new(CTUI_WL_DPY *d, int w, int h);
/* the role made: commit, wait for the first configure; -1 when the
 * connection went first */
int ctui_wl_surface_wait(CTUI_WL *wl);
/* an output by name; NULL for none */
struct wl_output *ctui_wl_output_find(CTUI_WL_DPY *d, const char *name);
/* a global wl.c left unbound, by interface name; NULL for none */
const CTUI_WL_GLOBAL *ctui_wl_global_find(CTUI_WL_DPY *d, const char *iface);

#endif
