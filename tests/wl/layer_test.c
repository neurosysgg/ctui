/* `make test` (src/wl/'s builds; skipped without sway): several surfaces
 * on one connection against a headless sway of the suite's own (a scratch
 * XDG_RUNTIME_DIR, the CPU renderer, no input devices, DISPLAY unset) --
 * the outputs by name, layer surfaces configured as asked (a bar along an
 * edge, a box in the middle), a window beside them, a bar moved, what a
 * layer can't change refused, an output that isn't there. Input routing
 * to a surface needs devices headless sway doesn't have: ctui-wm's
 * screens drive it (a virtual keyboard and pointer). */
#define _GNU_SOURCE

#include "ctui.h"
#include "ctui_test.h"
#include "headless.h"
#include "wl/layer.h"
#include "wl/wl.h"

#include <wayland-client.h>

/* 1920x1080 at 1.5: 1280x720 logical */
static const CTUI_WL_LAYER k_bar = {
    .output = "HEADLESS-1",
    .ns = "bar",
    .layer = CTUI_WL_LAYER_TOP,
    .anchor = CTUI_WL_EDGE_TOP | CTUI_WL_EDGE_LEFT | CTUI_WL_EDGE_RIGHT,
    .h = 30,
    .exclusive = 30,
};

static void test_surfaces(void) {
  char err[160] = "";
  CTUI_WL_DPY *d = ctui_wl_connect(err, sizeof err);
  CTUI_TEST_ASSERT(d != NULL, "connected (%s)", err);
  if (!d) {
    return;
  }
  const char *names[4];
  int n = ctui_wl_outputs(d, names, 4);
  CTUI_TEST_ASSERT(n == 1 && !strcmp(names[0], "HEADLESS-1"),
                   "one output, by its name (%d: %s)", n, n ? names[0] : "");

  CTUI_WL *bar = ctui_wl_layer(d, &k_bar, err, sizeof err);
  CTUI_TEST_ASSERT(bar != NULL, "a bar along the top (%s)", err);
  int w = 0, h = 0;
  if (bar) {
    ctui_wl_logical(bar, &w, &h);
  }
  CTUI_TEST_ASSERT(w == 1280 && h == 30,
                   "the bar is as wide as the output, as high as asked "
                   "(%dx%d)",
                   w, h);

  CTUI_WL_LAYER box = {.output = "HEADLESS-1",
                       .ns = "popup",
                       .layer = CTUI_WL_LAYER_OVERLAY,
                       .w = 200,
                       .h = 100,
                       .keyboard = CTUI_WL_KEYBOARD_EXCLUSIVE};
  CTUI_WL *pop = ctui_wl_layer(d, &box, err, sizeof err);
  w = h = 0;
  if (pop) {
    ctui_wl_logical(pop, &w, &h);
  }
  CTUI_TEST_ASSERT(pop && w == 200 && h == 100,
                   "a box in the middle, on the same connection (%dx%d, %s)", w,
                   h, err);

  CTUI_WL *win = ctui_wl_window(d, "t", "ctui-test", 300, 200, err, sizeof err);
  CTUI_TEST_ASSERT(win != NULL && ctui_wl_dpy(win) == d,
                   "a window beside them (%s)", err);
  ctui_wl_set_user(win, &box);
  CTUI_TEST_ASSERT(ctui_wl_user(win) == &box, "a surface keeps its user");

  if (bar) {
    CTUI_WL_LAYER taller = k_bar;
    taller.h = taller.exclusive = 50;
    CTUI_TEST_ASSERT(ctui_wl_layer_set(bar, &taller) == 0, "the bar resized");
    wl_display_roundtrip(ctui_wl_dpy_display(d));
    ctui_wl_logical(bar, &w, &h);
    CTUI_TEST_ASSERT(w == 1280 && h == 50, "configured again: %dx%d", w, h);
    CTUI_WL_LAYER moved = taller;
    moved.output = "HEADLESS-2";
    CTUI_TEST_ASSERT(ctui_wl_layer_set(bar, &moved) == -1,
                     "another output: refused (a new surface's)");
    moved = taller;
    moved.ns = "other";
    CTUI_TEST_ASSERT(ctui_wl_layer_set(bar, &moved) == -1,
                     "another namespace: refused");
  }
  if (win) {
    CTUI_TEST_ASSERT(ctui_wl_layer_set(win, &k_bar) == -1,
                     "a window isn't a layer");
  }

  CTUI_WL_LAYER gone = k_bar;
  gone.output = "HEADLESS-9";
  err[0] = '\0';
  CTUI_TEST_ASSERT(!ctui_wl_layer(d, &gone, err, sizeof err) &&
                       strstr(err, "HEADLESS-9"),
                   "an output that isn't there: NULL, named (%s)", err);

  CTUI_EVENT ev;
  CTUI_WL *on = NULL;
  CTUI_TEST_ASSERT(!ctui_wl_dpy_next(d, &ev, &on), "no input, no events");
  CTUI_TEST_ASSERT(!ctui_wl_closed(bar) && !ctui_wl_closed(pop),
                   "nothing closed by the compositor");

  ctui_wl_close(pop);
  ctui_wl_close(win);
  ctui_wl_close(bar);
  ctui_wl_disconnect(d);
}

/* the one-window shorthand still stands alone */
static void test_open(void) {
  char err[160] = "";
  CTUI_WL *wl = ctui_wl_open("t", "ctui-test", 320, 240, err, sizeof err);
  CTUI_TEST_ASSERT(wl != NULL, "ctui_wl_open (%s)", err);
  int pw = 0, ph = 0;
  if (wl) {
    ctui_wl_size(wl, &pw, &ph);
  }
  CTUI_TEST_ASSERT(pw > 0 && ph > 0, "its buffer has a size (%dx%d)", pw, ph);
  ctui_wl_close(wl);
}

int main(void) {
  ctui_log_init(E_ALL);
  if (!headless_available()) {
    printf("  -- no sway: skipped\n");
    return ctui_test_summary();
  }
  int up = headless_start("output HEADLESS-1 resolution 1920x1080 scale 1.5\n");
  CTUI_TEST_ASSERT(up == 0, "a headless sway (%s)",
                   up == 0 ? getenv("WAYLAND_DISPLAY") : "none");
  /* never a client on another compositor: only with its socket found */
  if (up == 0) {
    test_surfaces();
    test_open();
  }
  headless_stop();
  return ctui_test_summary();
}
