/* `make test` (where src/vk/ and src/wl/ build; skipped without sway or
 * lavapipe): the presenters (src/vk/present.h) on one device for two
 * layer surfaces of one connection, in a headless sway of the suite's own
 * (tests/wl/headless.h) on lavapipe -- frames presented only when
 * something changed, a surface resized by the compositor, the scale it
 * asks for, the validation layer clean when it's there; and
 * the one-window host (host.h) opened and closed on top of it. The
 * pixels themselves are vk_test's. */
#define _GNU_SOURCE

#include "../wl/headless.h"
#include "ctui.h"
#include "ctui_test.h"
#include "vk/host.h"
#include "vk/present.h"
#include "wl/layer.h"
#include "wl/wl.h"

#include <wayland-client.h>

#define LVP "/usr/share/vulkan/icd.d/lvp_icd.json"

static const CTUI_WL_LAYER k_bar = {
    .output = "HEADLESS-1",
    .ns = "bar",
    .layer = CTUI_WL_LAYER_TOP,
    .anchor = CTUI_WL_EDGE_TOP | CTUI_WL_EDGE_LEFT | CTUI_WL_EDGE_RIGHT,
    .h = 30,
    .exclusive = 30,
};

static const CTUI_WL_LAYER k_box = {
    .output = "HEADLESS-1",
    .ns = "popup",
    .layer = CTUI_WL_LAYER_OVERLAY,
    .w = 240,
    .h = 120,
};

/* what the compositor said since: a few round trips */
static void settle(CTUI_WL_DPY *d) {
  for (int i = 0; i < 3; i++) {
    wl_display_roundtrip(ctui_wl_dpy_display(d));
  }
}

static int present(CTUI_VK_WIN *w) { return ctui_vk_win_present(w, 0, NULL); }

static void test_presenters(void) {
  char err[200] = "";
  CTUI_WL_DPY *d = ctui_wl_connect(err, sizeof err);
  CTUI_TEST_ASSERT(d != NULL, "connected (%s)", err);
  if (!d) {
    return;
  }
  CTUI_VK_GPU *g = ctui_vk_gpu_open(d, err, sizeof err);
  CTUI_TEST_ASSERT(g != NULL, "a device for the display (%s)", err);
  CTUI_WL *bar = ctui_wl_layer(d, &k_bar, err, sizeof err);
  CTUI_WL *box = ctui_wl_layer(d, &k_box, err, sizeof err);
  CTUI_TEST_ASSERT(bar && box, "two layer surfaces (%s)", err);
  if (!g || !bar || !box) {
    ctui_wl_close(bar);
    ctui_wl_close(box);
    ctui_vk_gpu_close(g);
    ctui_wl_disconnect(d);
    return;
  }

  int br = 0, bc = 0, xr = 0, xc = 0;
  CTUI_VK_WIN *wb = ctui_vk_win_new(g, bar, &br, &bc, err, sizeof err);
  CTUI_VK_WIN *wx = ctui_vk_win_new(g, box, &xr, &xc, err, sizeof err);
  CTUI_TEST_ASSERT(wb && wx, "a presenter each, one device (%s)", err);
  CTUI_TEST_ASSERT(br >= 1 && bc > 100 && xr > 1 && xc > 10,
                   "their grids: the bar %dx%d, the box %dx%d", br, bc, xr, xc);
  if (!wb || !wx) {
    ctui_vk_win_free(wb);
    ctui_vk_win_free(wx);
    ctui_wl_close(bar);
    ctui_wl_close(box);
    ctui_vk_gpu_close(g);
    ctui_wl_disconnect(d);
    return;
  }
  CTUI_TEST_ASSERT(ctui_vk_win_wl(wb) == bar && ctui_vk_win_vk(wb) &&
                       ctui_vk_win_vk(wb) != ctui_vk_win_vk(wx),
                   "a painter of its own each");

  CTUI_SCREEN *sb = ctui_screen_create(br, bc);
  CTUI_SCREEN *sx = ctui_screen_create(xr, xc);
  ctui_screen_set_sink(sb, ctui_vk_screen_sink(), ctui_vk_win_vk(wb));
  ctui_screen_set_sink(sx, ctui_vk_screen_sink(), ctui_vk_win_vk(wx));
  ctui_screen_puts(sb, 0, 0, "bar", 7, 0);
  ctui_screen_puts(sx, 0, 0, "box", 7, 0);
  ctui_screen_flush(sb);
  ctui_screen_flush(sx);
  CTUI_TEST_ASSERT(present(wb) == 1 && present(wx) == 1,
                   "a first frame each, presented");
  CTUI_TEST_ASSERT(present(wb) == 0 && present(wx) == 0,
                   "nothing changed: nothing presented");
  ctui_screen_puts(sb, 0, 4, "!", 7, 0);
  ctui_screen_flush(sb);
  CTUI_TEST_ASSERT(present(wb) == 1 && present(wx) == 0,
                   "one cell on the bar: the bar alone presents");

  /* sway says the scale before the first frame: the presenter was made
   * at 1.5 already, and mapping it changes nothing */
  settle(d);
  int pw, ph;
  int changed = ctui_wl_size(bar, &pw, &ph);
  CTUI_TEST_ASSERT(ctui_wl_scale120(bar) == 180 && !changed && pw == 1920 &&
                       ph == 45,
                   "at the output's 1.5 from the start: %d/120, %dx%d px",
                   ctui_wl_scale120(bar), pw, ph);

  CTUI_WL_LAYER taller = k_bar;
  taller.h = taller.exclusive = 90;
  ctui_wl_layer_set(bar, &taller);
  settle(d);
  CTUI_TEST_ASSERT(ctui_wl_size(bar, &pw, &ph) && ph == 135,
                   "the bar made taller: %dx%d px", pw, ph);
  int r0 = br;
  CTUI_TEST_ASSERT(ctui_vk_win_resize(wb, &br, &bc) == 0 && br > r0,
                   "more rows: %d -> %d", r0, br);
  ctui_screen_resize(sb, br, bc);
  ctui_screen_puts(sb, br - 1, 0, "last row", 7, 0);
  ctui_screen_flush(sb);
  CTUI_TEST_ASSERT(present(wb) == 1, "the taller bar presented");
  CTUI_TEST_ASSERT(ctui_vk_gpu_warnings(g) == 0,
                   "the validation layer: %u warnings",
                   ctui_vk_gpu_warnings(g));

  ctui_screen_free(sb);
  ctui_screen_free(sx);
  ctui_vk_win_free(wx);
  ctui_wl_close(box);
  ctui_vk_win_free(wb);
  ctui_wl_close(bar);
  ctui_vk_gpu_close(g);
  ctui_wl_disconnect(d);
}

/* the one-window host on top: open, attach, close */
static void test_host(void) {
  char err[200] = "";
  int rows = 0, cols = 0;
  CTUI_HOST *h =
      ctui_host_open("t", "ctui-test", 400, 300, &rows, &cols, err, sizeof err);
  CTUI_TEST_ASSERT(h && rows > 1 && cols > 1, "a host window, %dx%d (%s)", rows,
                   cols, err);
  if (h) {
    CTUI_SCREEN *s = ctui_screen_create(rows, cols);
    ctui_host_attach(h, s);
    ctui_host_set_default(h, 0xc0c0c0, 0x000080);
    ctui_screen_puts(s, 0, 0, "host", 7, 0);
    ctui_screen_flush(s);
    ctui_host_close(h);
    ctui_screen_free(s);
  }
  CTUI_TEST_ASSERT(1, "closed again, stdout back");
}

int main(void) {
  ctui_log_init(E_ALL);
  if (!headless_available() || access(LVP, R_OK) != 0) {
    printf("  -- no sway or no lavapipe: skipped\n");
    return ctui_test_summary();
  }
  /* lavapipe presents to a CPU compositor (wl_shm); a card's driver
   * wouldn't, and the user's choice of drivers is kept (no lavapipe
   * dropped) */
  setenv("VK_DRIVER_FILES", LVP, 1);
  setenv("CTUI_HOST_VALIDATION", "1", 1);
  int up = headless_start("output HEADLESS-1 resolution 1920x1080 scale 1.5\n");
  CTUI_TEST_ASSERT(up == 0, "a headless sway");
  if (up == 0) {
    test_presenters();
    test_host();
  }
  headless_stop();
  return ctui_test_summary();
}
