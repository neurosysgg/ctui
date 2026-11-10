#ifndef CTUI_LINK_H
#define CTUI_LINK_H

/* A ctui app drawn by another process (ctui-wm's notes/kittyless.md: a
 * desktop's panels painted by one host on one device): what a terminal
 * would carry, minus the terminal. The app's end (CTUI_LINK) is its
 * screen sink, its graphics sink and its input source; the host's end
 * (CTUI_LINK_HOST) hands the same calls to sinks of its own -- src/vk/'s,
 * a test's -- and sends the app its input, its grid and messages of the
 * app's own.
 *
 * One socket (SOCK_SEQPACKET, a socketpair's end each): the cells go
 * through a grid both map (a memfd the host makes at each size), a frame
 * a list of the runs that changed, one frame in flight -- the app's sink
 * always takes a frame, keeping what changed until the host took the one
 * before, so a slow host makes the app skip frames, never queue them.
 * Image calls go as they are, their pixels in a memfd each. The host
 * never blocks on an app: a full socket drops the input it would have
 * sent (logged).
 *
 * Linux only (memfd, SCM_RIGHTS); libc and ctui's core. */

#include "ctui.h"

#include <stddef.h>
#include <stdint.h>

/* --- the app's end --- */

typedef struct CTUI_LINK CTUI_LINK;

/* the link on fd (blocking, owned from here): HELLO sent, and the host's
 * first grid waited for -- *rows / *cols for ctui_screen_create(), the
 * cell's pixels told to ctui_cell_px_set(). The graphics sink is set to
 * the link (before any image is made) and the tier to mode
 * (CTUI_GFX_TRUECOLOR, | CTUI_GFX_KITTY for images): no ctui_init().
 * NULL, or why in err */
CTUI_LINK *ctui_link_open(int fd, unsigned mode, int *rows, int *cols,
                          char *err, size_t n);
/* screen's frames to the host and the host's input to ctui_input_loop(),
 * before ctui_app_run(): a new grid comes as a CTUI_RESIZE_EVENT, the
 * host closing its end ends the loop */
void ctui_link_attach(CTUI_LINK *l, CTUI_SCREEN *screen);
/* after ctui_app_run(): the source and sinks given back, the socket
 * closed */
void ctui_link_close(CTUI_LINK *l);

/* CTUI_COLOR_DEFAULT's colours (0xRRGGBB), as a terminal's own are */
void ctui_link_set_default(CTUI_LINK *l, uint32_t fg, uint32_t bg);

/* a message of the app's own to the host (what it means is theirs: a
 * panel asking for its size, say): 0, or -1 */
int ctui_link_app(CTUI_LINK *l, const void *msg, size_t n);
/* the host's messages of the app's own, as they come (fn NULL: dropped) */
typedef void (*CTUI_LINK_APP_FN)(void *ctx, const void *msg, size_t n);
void ctui_link_on_app(CTUI_LINK *l, CTUI_LINK_APP_FN fn, void *ctx);

/* --- the host's end --- */

typedef struct CTUI_LINK_HOST CTUI_LINK_HOST;

typedef struct {
  /* where the app's frames and images go (src/vk/'s sinks, with that
   * surface's CTUI_VK as both contexts) */
  const CTUI_SCREEN_SINK *screen;
  void *screen_ctx;
  const CTUI_GFX_SINK *gfx;
  void *gfx_ctx;
  /* the app's default colours (NULL: ignored) */
  void (*set_default)(void *ctx, uint32_t fg, uint32_t bg);
  /* the app's own messages (NULL: dropped) */
  CTUI_LINK_APP_FN app;
  void *ctx; /* set_default's and app's */
} CTUI_LINK_HOST_SINKS;

/* the host's end on fd (made nonblocking, owned from here); the app's
 * HELLO is checked as it comes. NULL when out of memory */
CTUI_LINK_HOST *ctui_link_host_new(int fd, const CTUI_LINK_HOST_SINKS *sinks);
void ctui_link_host_free(CTUI_LINK_HOST *h);
int ctui_link_host_fd(const CTUI_LINK_HOST *h);

/* the app's grid at rows x cols, a cell cw x ch pixels: a new shared
 * grid sent, the host's own screen (its sink's) resized and redrawn from
 * the app's next frame. Before the app's first frame and at every size.
 * 0, or -1 */
int ctui_link_host_grid(CTUI_LINK_HOST *h, int rows, int cols, int cw, int ch);

/* the socket readable: every message waiting applied -- frames into the
 * screen sink (and acked), image calls into the graphics sink. 0, or -1
 * once the app is gone (or broke the protocol: logged) */
int ctui_link_host_read(CTUI_LINK_HOST *h);

/* input for the app (keys, key releases, the mouse, focus): 0, or -1
 * (another type, or its socket full: dropped) */
int ctui_link_host_event(CTUI_LINK_HOST *h, const CTUI_EVENT *ev);
/* a message of the app's own to it: 0, or -1 */
int ctui_link_host_app(CTUI_LINK_HOST *h, const void *msg, size_t n);

#endif
