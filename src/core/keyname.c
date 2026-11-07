#include "keyname.h"

#include "utf8.h"

#include <string.h>
#include <strings.h>

static int is(const char *s, size_t n, const char *name) {
  return strlen(name) == n && strncasecmp(s, name, n) == 0;
}

static int named(const char *s, size_t n, CTUI_KEYPRESS_EVENT_DATA *out) {
  static const struct {
    const char *name;
    CTUI_KEYTYPE type;
    uint32_t ch;
  } keys[] = {
      {"up", CTUI_KEY_UP, 0},
      {"down", CTUI_KEY_DOWN, 0},
      {"left", CTUI_KEY_LEFT, 0},
      {"right", CTUI_KEY_RIGHT, 0},
      {"enter", CTUI_KEY_ENTER, 0},
      {"esc", CTUI_KEY_ESC, 0},
      {"tab", CTUI_KEY_TAB, 0},
      {"backtab", CTUI_KEY_BACKTAB, 0},
      {"home", CTUI_KEY_HOME, 0},
      {"end", CTUI_KEY_END, 0},
      {"pgup", CTUI_KEY_PGUP, 0},
      {"pgdn", CTUI_KEY_PGDN, 0},
      {"insert", CTUI_KEY_INSERT, 0},
      {"delete", CTUI_KEY_DELETE, 0},
      {"backspace", CTUI_KEY_CHAR, 0x7f},
      {"space", CTUI_KEY_CHAR, ' '},
  };
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    if (is(s, n, keys[i].name)) {
      out->type = keys[i].type;
      out->ch = keys[i].ch;
      return 0;
    }
  }
  if ((n == 2 || n == 3) && (s[0] == 'f' || s[0] == 'F') && s[1] >= '1' &&
      s[1] <= '9') {
    int f = s[1] - '0';
    if (n == 3) {
      if (s[2] < '0' || s[2] > '9') {
        return -1;
      }
      f = f * 10 + (s[2] - '0');
    }
    if (f > 12) {
      return -1;
    }
    out->type = (CTUI_KEYTYPE)(CTUI_KEY_F1 + f - 1);
    out->ch = 0;
    return 0;
  }
  return -1;
}

int ctui_key_parse(const char *s, size_t n, CTUI_KEYPRESS_EVENT_DATA *out) {
  CTUI_KEYPRESS_EVENT_DATA k = {0};
  /* a '+' that isn't the key's first byte ends a modifier: "ctrl++" is
   * ctrl and '+' */
  for (;;) {
    const char *plus = n > 1 ? memchr(s + 1, '+', n - 1) : NULL;
    if (!plus) {
      break;
    }
    size_t len = (size_t)(plus - s);
    if (is(s, len, "ctrl")) {
      k.mods |= CTUI_MOD_CTRL;
    } else if (is(s, len, "alt")) {
      k.mods |= CTUI_MOD_ALT;
    } else if (is(s, len, "shift")) {
      k.mods |= CTUI_MOD_SHIFT;
    } else {
      return -1;
    }
    s += len + 1;
    n -= len + 1;
  }
  if (n == 0) {
    return -1;
  }
  if (named(s, n, &k) != 0) {
    /* one codepoint, the whole of the text: a character (decoded from a
     * copy: s needn't be NUL-terminated) */
    char buf[5] = {0};
    uint32_t cp;
    if (n > 4) {
      return -1;
    }
    memcpy(buf, s, n);
    if (ctui_utf8_decode(buf, &cp) != (int)n || cp < 0x20 || cp == 0x7f ||
        (cp == 0xFFFD && n == 1)) {
      return -1;
    }
    k.type = CTUI_KEY_CHAR;
    k.ch = cp;
  }
  if (k.type == CTUI_KEY_TAB && (k.mods & CTUI_MOD_SHIFT)) {
    k.type = CTUI_KEY_BACKTAB;
    k.mods &= ~(unsigned)CTUI_MOD_SHIFT;
  }
  if (k.type == CTUI_KEY_CHAR && (k.mods & CTUI_MOD_CTRL) &&
      ((k.ch >= 'a' && k.ch <= 'z') || (k.ch >= '@' && k.ch <= '_'))) {
    k.ch &= 0x1f;
    k.mods &= ~(unsigned)CTUI_MOD_CTRL;
  }
  *out = k;
  return 0;
}
