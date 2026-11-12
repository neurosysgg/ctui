/* `make test` (where wayland-client, its scanner and protocols, and
 * xkbcommon are): the window's keys (src/wl/wl.h) as a terminal's bytes
 * would give them, from keysyms and from a US keymap's keys the way the
 * compositor's would turn them; and that no compositor is a NULL with a
 * reason, not a crash. Never opens a window: WAYLAND_DISPLAY names a
 * socket that isn't there (unset, libwayland would try wayland-0) */
#define _POSIX_C_SOURCE 200809L

#include "ctui.h"
#include "ctui_test.h"
#include "wl/wl.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>

static CTUI_KEYPRESS_EVENT_DATA key(uint32_t sym, uint32_t cp, unsigned mods) {
  CTUI_KEYPRESS_EVENT_DATA k;
  if (!ctui_wl_key_event(sym, cp, mods, &k)) {
    k.type = CTUI_KEY_NONE;
  }
  return k;
}

static void test_keysyms(void) {
  CTUI_KEYPRESS_EVENT_DATA k = key(XKB_KEY_Up, 0, 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_UP && k.mods == 0, "Up");
  k = key(XKB_KEY_KP_Enter, '\r', 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_ENTER, "keypad Enter is Enter");
  k = key(XKB_KEY_Tab, '\t', CTUI_MOD_SHIFT);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_BACKTAB && k.mods == 0,
                   "shift+Tab is BACKTAB, the shift in it");
  k = key(XKB_KEY_ISO_Left_Tab, 0, CTUI_MOD_SHIFT);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_BACKTAB, "ISO_Left_Tab is BACKTAB");
  k = key(XKB_KEY_BackSpace, '\b', 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_CHAR && k.ch == 0x7f,
                   "Backspace is 0x7f, as a terminal sends it");
  k = key(XKB_KEY_A, 'A', CTUI_MOD_SHIFT);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_CHAR && k.ch == 'A' && k.mods == 0,
                   "shift+a: 'A', the shift in the char");
  k = key(XKB_KEY_a, 0x01, CTUI_MOD_CTRL);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_CHAR && k.ch == 0x01 && k.mods == 0,
                   "ctrl+a: its control byte, the ctrl in it");
  k = key(XKB_KEY_C, 0x03, CTUI_MOD_CTRL | CTUI_MOD_SHIFT);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_CHAR && k.ch == 0x03 &&
                       k.mods == CTUI_MOD_SHIFT,
                   "ctrl+shift+c: its control byte, shift kept (not "
                   "ctrl+c), as kitty's CSI u decodes");
  k = key(XKB_KEY_x, 'x', CTUI_MOD_ALT);
  CTUI_TEST_ASSERT(k.ch == 'x' && k.mods == CTUI_MOD_ALT, "alt+x keeps alt");
  k = key(XKB_KEY_Right, 0, CTUI_MOD_CTRL);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_RIGHT && k.mods == CTUI_MOD_CTRL,
                   "ctrl+Right keeps ctrl");
  k = key(XKB_KEY_F5, 0, 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_F5, "F5");
  k = key(XKB_KEY_Shift_L, 0, CTUI_MOD_SHIFT);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_NONE, "a modifier alone: no event");
  k = key(XKB_KEY_eacute, 0xe9, 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_CHAR && k.ch == 0xe9, "é");
}

/* evdev code through a US keymap with mods held, as kb_key() does it */
static CTUI_KEYPRESS_EVENT_DATA typed(struct xkb_keymap *km, unsigned evdev,
                                      int shift, int ctrl) {
  struct xkb_state *st = xkb_state_new(km);
  xkb_mod_mask_t m = 0;
  if (shift) {
    m |= 1u << xkb_keymap_mod_get_index(km, XKB_MOD_NAME_SHIFT);
  }
  if (ctrl) {
    m |= 1u << xkb_keymap_mod_get_index(km, XKB_MOD_NAME_CTRL);
  }
  xkb_state_update_mask(st, m, 0, 0, 0, 0, 0);
  xkb_keycode_t code = evdev + 8;
  unsigned mods = (shift ? CTUI_MOD_SHIFT : 0) | (ctrl ? CTUI_MOD_CTRL : 0);
  CTUI_KEYPRESS_EVENT_DATA k = key(xkb_state_key_get_one_sym(st, code),
                                   xkb_state_key_get_utf32(st, code), mods);
  xkb_state_unref(st);
  return k;
}

static void test_keymap(void) {
  struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_rule_names us = {.layout = "us"};
  struct xkb_keymap *km =
      ctx ? xkb_keymap_new_from_names(ctx, &us, XKB_KEYMAP_COMPILE_NO_FLAGS)
          : NULL;
  if (!km) {
    printf("  skipped: no US keymap (xkeyboard-config)\n");
    xkb_context_unref(ctx);
    return;
  }
  CTUI_KEYPRESS_EVENT_DATA k = typed(km, KEY_Q, 0, 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_CHAR && k.ch == 'q', "Q's key: 'q'");
  k = typed(km, KEY_Q, 1, 0);
  CTUI_TEST_ASSERT(k.ch == 'Q' && k.mods == 0, "shift: 'Q'");
  k = typed(km, KEY_C, 0, 1);
  CTUI_TEST_ASSERT(k.ch == 0x03 && k.mods == 0, "ctrl+c: 0x03");
  k = typed(km, KEY_C, 1, 1);
  CTUI_TEST_ASSERT(k.ch == 0x03 && k.mods == CTUI_MOD_SHIFT,
                   "ctrl+shift+c through the keymap: 0x03 with shift");
  k = typed(km, KEY_BACKSPACE, 0, 0);
  CTUI_TEST_ASSERT(k.ch == 0x7f, "Backspace's key: 0x7f");
  k = typed(km, KEY_TAB, 1, 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_BACKTAB, "shift+Tab's key: BACKTAB");
  k = typed(km, KEY_ESC, 0, 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_ESC, "Esc's key: ESC");
  k = typed(km, KEY_LEFTSHIFT, 1, 0);
  CTUI_TEST_ASSERT(k.type == CTUI_KEY_NONE, "Shift's key alone: nothing");
  xkb_keymap_unref(km);
  xkb_context_unref(ctx);
}

static void test_no_compositor(void) {
  setenv("WAYLAND_DISPLAY", "ctui-wl-test-no-such-socket", 1);
  unsetenv("WAYLAND_SOCKET");
  char err[160] = "";
  CTUI_WL *wl = ctui_wl_open("test", "ctui-test", 100, 100, err, sizeof err);
  CTUI_TEST_ASSERT(!wl && err[0], "no compositor: NULL, why (%s)", err);
  ctui_wl_close(wl);
}

int main(void) {
  ctui_log_init(E_ALL);
  test_keysyms();
  test_keymap();
  test_no_compositor();
  return ctui_test_summary();
}
