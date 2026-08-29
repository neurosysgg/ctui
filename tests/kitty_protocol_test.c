/* Exercises the Kitty graphics protocol support headlessly: the generic
 * base64 encoder core/gfx.c's wire emission depends on, and Phase 4's
 * per-widget renderer declaration (widget.h's supported_gfx_modes /
 * ctui_widget_set_gfx_renderer(), core/app.c's ctui_app_init() hard-fail
 * check). The actual APC escape bytes ctui_gfx_kitty_display() writes to a
 * real terminal aren't covered here -- see docs/protocol.md's testing
 * checklist for how that was verified (a throwaway pty check, not
 * something that belongs in a headless suite). */
#include "ctui.h"

#include "ctui_test.h"

#include <string.h>

/* g_gfx_mode is intentionally private (core/ctui_internal.h, never
 * included by ctui.h -- see that header's own comment). Redeclaring the
 * same extern here is the same gray-box trick ctui_test.h itself relies
 * on (reading screen->cells directly): there's no ctui_init() in a
 * headless test to set it for real, so this is the only way to simulate
 * "CTUI_GFX_KITTY was negotiated" for ctui_app_init()'s validation path. */
extern unsigned int g_gfx_mode;

/* Phase 6: ctui_gfx_kitty_reply_is_ok() is a non-static helper in
 * core/gfx.c, deliberately not declared in gfx.h -- it's not something
 * an app/widget author should ever call, only factored out of
 * ctui_gfx_kitty_probe_shm() so its APC-reply parsing is unit-testable
 * without a real terminal (the probe itself needs a live pty replying
 * to a real escape sequence, well outside what this headless suite can
 * exercise -- see docs/protocol.md's testing checklist for why that's a
 * pty_harness.py/real-terminal concern instead). Same forward-declare-an-
 * intentionally-private-symbol trick as g_gfx_mode above. */
extern int ctui_gfx_kitty_reply_is_ok(const char *buf, size_t len,
                                      unsigned int image_id);

static void noop_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  (void)comp;
}

static void test_base64(void) {
  char out[64];

  size_t n = ctui_util_base64_encode((const unsigned char *)"", 0, out,
                                     sizeof out);
  CTUI_TEST_ASSERT(n == 0 && strcmp(out, "") == 0,
                   "base64_encode(\"\") is empty");

  n = ctui_util_base64_encode((const unsigned char *)"f", 1, out, sizeof out);
  CTUI_TEST_ASSERT(n == 4 && strcmp(out, "Zg==") == 0,
                   "base64_encode(\"f\") == \"Zg==\"");

  n = ctui_util_base64_encode((const unsigned char *)"fo", 2, out,
                              sizeof out);
  CTUI_TEST_ASSERT(n == 4 && strcmp(out, "Zm8=") == 0,
                   "base64_encode(\"fo\") == \"Zm8=\"");

  n = ctui_util_base64_encode((const unsigned char *)"foo", 3, out,
                              sizeof out);
  CTUI_TEST_ASSERT(n == 4 && strcmp(out, "Zm9v") == 0,
                   "base64_encode(\"foo\") == \"Zm9v\"");

  n = ctui_util_base64_encode((const unsigned char *)"foobar", 6, out,
                              sizeof out);
  CTUI_TEST_ASSERT(n == 8 && strcmp(out, "Zm9vYmFy") == 0,
                   "base64_encode(\"foobar\") == \"Zm9vYmFy\"");

  char tiny[3];
  n = ctui_util_base64_encode((const unsigned char *)"foobar", 6, tiny,
                              sizeof tiny);
  CTUI_TEST_ASSERT(n == 0, "base64_encode rejects a too-small dst_cap");
}

static void test_widget_defaults(void) {
  CTUI_WIDGET w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
  CTUI_TEST_ASSERT(
      w.supported_gfx_modes ==
          (CTUI_GFX_ANSI16 | CTUI_GFX_ANSI256 | CTUI_GFX_TRUECOLOR),
      "ctui_widget_make() defaults supported_gfx_modes to all three text "
      "tiers");
  CTUI_TEST_ASSERT(w.gfx_render_mode == 0 && w.gfx_render == NULL,
                   "ctui_widget_make() registers no gfx renderer by "
                   "default");

  ctui_widget_set_gfx_renderer(&w, CTUI_GFX_KITTY, noop_render);
  CTUI_TEST_ASSERT(w.supported_gfx_modes == CTUI_GFX_KITTY,
                   "ctui_widget_set_gfx_renderer() narrows "
                   "supported_gfx_modes to exactly the given mode");
  CTUI_TEST_ASSERT(w.gfx_render_mode == CTUI_GFX_KITTY &&
                       w.gfx_render == noop_render,
                   "ctui_widget_set_gfx_renderer() records the mode/render "
                   "pair for ctui_app_render()'s dispatch");
}

static void test_app_init_validation(void) {
  int rows = 24, cols = 80;

  /* ordinary text widget: passes regardless of g_gfx_mode, since text
   * rendering doesn't depend on what got negotiated */
  {
    g_gfx_mode = 0;
    CTUI_WIDGET w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
    CTUI_WIDGET *widgets[] = {&w};
    CTUI_APP app;
    CTUI_TEST_ASSERT(ctui_app_init(&app, widgets, 1, rows, cols) == 0,
                     "ctui_app_init() accepts a plain text widget under "
                     "g_gfx_mode=0");
    ctui_app_free(&app);
  }

  /* a widget requiring CTUI_GFX_KITTY when it wasn't negotiated: hard
   * fails, no text fallback to degrade to */
  {
    g_gfx_mode = CTUI_GFX_TRUECOLOR;
    CTUI_WIDGET w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
    ctui_widget_set_gfx_renderer(&w, CTUI_GFX_KITTY, noop_render);
    CTUI_WIDGET *widgets[] = {&w};
    CTUI_APP app;
    CTUI_TEST_ASSERT(ctui_app_init(&app, widgets, 1, rows, cols) == -1,
                     "ctui_app_init() hard-fails a CTUI_GFX_KITTY widget "
                     "when TRUECOLOR (not KITTY) was negotiated");
  }

  /* same widget, but CTUI_GFX_KITTY actually was negotiated: passes */
  {
    g_gfx_mode = CTUI_GFX_KITTY;
    CTUI_WIDGET w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
    ctui_widget_set_gfx_renderer(&w, CTUI_GFX_KITTY, noop_render);
    CTUI_WIDGET *widgets[] = {&w};
    CTUI_APP app;
    CTUI_TEST_ASSERT(ctui_app_init(&app, widgets, 1, rows, cols) == 0,
                     "ctui_app_init() accepts a CTUI_GFX_KITTY widget once "
                     "CTUI_GFX_KITTY was actually negotiated");
    ctui_app_free(&app);
  }

  g_gfx_mode = 0;
}

static void test_kitty_shm_reply_parsing(void) {
  const char ok[] = "\x1b_Gi=1;OK\x1b\\";
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_reply_is_ok(ok, sizeof ok - 1, 1) == 1,
      "reply_is_ok() accepts a plain \"i=1;OK\" reply keyed to id 1");

  const char ok_extra_keys[] = "\x1b_Gi=1,More=stuff;OK\x1b\\";
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_reply_is_ok(ok_extra_keys, sizeof ok_extra_keys - 1,
                                 1) == 1,
      "reply_is_ok() still finds \";OK\" past extra keys before it");

  const char wrong_id[] = "\x1b_Gi=2;OK\x1b\\";
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_reply_is_ok(wrong_id, sizeof wrong_id - 1, 1) == 0,
      "reply_is_ok() rejects an OK reply keyed to a different image id");

  const char error[] = "\x1b_Gi=1;EINVAL:bad t=s request\x1b\\";
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_reply_is_ok(error, sizeof error - 1, 1) == 0,
      "reply_is_ok() rejects a real error reply for the right id");

  const char no_semicolon[] = "\x1b_Gi=1OK\x1b\\";
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_reply_is_ok(no_semicolon, sizeof no_semicolon - 1,
                                 1) == 0,
      "reply_is_ok() rejects a reply with no ';' separator at all");

  const char truncated[] = "\x1b_Gi=1;O";
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_reply_is_ok(truncated, sizeof truncated - 1, 1) == 0,
      "reply_is_ok() rejects a reply truncated mid-\"OK\"");

  CTUI_TEST_ASSERT(ctui_gfx_kitty_reply_is_ok(NULL, 0, 1) == 0,
                   "reply_is_ok() rejects a NULL/empty buffer (no reply "
                   "arrived within the probe timeout)");
}

int main(void) {
  ctui_log_init(E_ALL);

  test_base64();
  test_widget_defaults();
  test_app_init_validation();
  test_kitty_shm_reply_parsing();

  return ctui_test_summary();
}
