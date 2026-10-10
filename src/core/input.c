#define _POSIX_C_SOURCE 200809L

#include "input.h"

#include "ctui_internal.h"
#include "io.h"
#include "log.h"
#include "term.h"
#include "timer.h"
#include "utf8.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/* --- pushback: bytes read off STDIN by something other than this file,
 * handed back so the input loop still sees them ---
 *
 * ctui_gfx_kitty_probe_shm() (core/gfx.c) has to read STDIN directly to
 * catch the terminal's one-shot APC reply, and anything else that lands
 * in that ~250ms window -- a keystroke typed during startup, a response
 * to some other query -- comes along with it. Before this existed, the
 * probe simply discarded whatever it had read, so those bytes were gone.
 *
 * Deliberately a plain fixed buffer rather than a growable one: the only
 * producer is a single bounded probe read (256 bytes), and there is no
 * sensible recovery from "the pushback buffer is full" anyway -- an
 * overflow would mean dropping input either way, so it's better to drop
 * it at a documented cap and log, than to allocate on a path that runs
 * during ctui_init(). */
#define CTUI_INPUT_PUSHBACK_MAX 256
static char g_pushback[CTUI_INPUT_PUSHBACK_MAX];
static size_t g_pushback_len = 0;
static size_t g_pushback_pos = 0;

static int pushback_pending(void) { return g_pushback_pos < g_pushback_len; }

void ctui_input_pushback(const char *bytes, size_t len) {
  if (bytes == NULL || len == 0) {
    return;
  }
  /* compact whatever is left before appending, so repeated pushbacks
   * don't crawl up the buffer */
  if (g_pushback_pos > 0) {
    size_t left = g_pushback_len - g_pushback_pos;
    memmove(g_pushback, g_pushback + g_pushback_pos, left);
    g_pushback_len = left;
    g_pushback_pos = 0;
  }
  size_t room = sizeof g_pushback - g_pushback_len;
  if (len > room) {
    ctui_logf(E_WRN,
              "[CTUI:INPUT] - pushback overflow @ tick %d, dropping %zu of "
              "%zu byte(s)\n",
              ctui_tick_advance(), len - room, len);
    len = room;
  }
  memcpy(g_pushback + g_pushback_len, bytes, len);
  g_pushback_len += len;
  ctui_logf(E_INF, "[CTUI:INPUT] - pushed back %zu byte(s) @ tick %d\n", len,
            ctui_tick_advance());
}

/* every read in this file goes through here, so pushed-back bytes are
 * indistinguishable from freshly-typed ones to everything downstream */
static ssize_t input_read(char *c) {
  if (pushback_pending()) {
    *c = g_pushback[g_pushback_pos++];
    return 1;
  }
  return read(STDIN_FILENO, c, 1);
}

static int read_byte_timeout(unsigned char *c, int timeout_ms) {
  if (pushback_pending()) {
    *c = (unsigned char)g_pushback[g_pushback_pos++];
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout got pushback byte 0x%02x @ "
              "tick %d\n",
              *c, ctui_tick_advance());
    return 1;
  }

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  struct timeval tv = {.tv_sec = 0, .tv_usec = timeout_ms * 1000};
  int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
  if (r <= 0) {
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout(%dms) timed out @ tick %d\n",
              timeout_ms, ctui_tick_advance());
    return 0;
  }
  char byte;
  if (input_read(&byte) != 1) {
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout read failed @ tick %d\n",
              ctui_tick_advance());
    return 0;
  }
  *c = (unsigned char)byte;
  ctui_logf(E_DBG,
            "[CTUI:INPUT] - read_byte_timeout got byte 0x%02x @ tick %d\n",
            *c, ctui_tick_advance());
  return 1;
}

/* how long to wait for the rest of an escape sequence / UTF-8 sequence
 * once its first byte arrived -- a terminal sends the whole thing in one
 * write, so anything slower than this is a lone ESC keypress */
#define CTUI_INPUT_SEQ_TIMEOUT_MS 50

static long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int resolve_key(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp,
                       CTUI_KEYTYPE type, uint32_t ch, unsigned int mods) {
  ev->type = CTUI_KEYPRESS_EVENT;
  ev->ev_source = "input";
  ev->event_data = kp;
  kp->type = type;
  kp->ch = ch;
  kp->mods = mods;
  if (type == CTUI_KEY_CHAR) {
    ctui_logf(E_INF,
              "[CTUI:INPUT] - resolved key %s (U+%04X, mods=0x%x) @ tick %d\n",
              ctui_keytype_name(type), ch, mods, ctui_tick_advance());
  } else {
    ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s (mods=0x%x) @ tick %d\n",
              ctui_keytype_name(type), mods, ctui_tick_advance());
  }
  return 1;
}

/* "\x1b[<b;x;yM" / "...m" -- params is everything between '<' and the
 * final byte. See CTUI_MOUSE_EVENT_DATA for what each field means. */
static int resolve_mouse(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp,
                         CTUI_MOUSE_EVENT_DATA *md, const char *params,
                         unsigned char final) {
  char *end;
  long b = strtol(params, &end, 10);
  long x = *end == ';' ? strtol(end + 1, &end, 10) : 0;
  long y = *end == ';' ? strtol(end + 1, &end, 10) : 0;
  if (!ctui_g_mouse_pixels && (x < 1 || y < 1)) {
    ctui_logf(E_WRN, "[CTUI:INPUT] - malformed SGR mouse report @ tick %d\n",
              ctui_tick_advance());
    return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
  }

  /* 128-131: buttons 8-11 (back, forward, ...), as 3-6 -- before this
   * they read as their low bits: back a left click */
  int btn = (int)(b & 3) + ((b & 128) ? CTUI_MOUSE_BUTTON_BACK : 0);
  md->mods = ((b & 4) ? CTUI_MOD_SHIFT : 0) | ((b & 8) ? CTUI_MOD_ALT : 0) |
             ((b & 16) ? CTUI_MOD_CTRL : 0);
  md->row = (int)y - 1;
  md->col = (int)x - 1;
  md->px = md->py = -1;
  int cw, ch;
  if (ctui_g_mouse_pixels && ctui_cell_px(&cw, &ch) == 0) {
    /* kitty: 0-based, relative to the cell area, past it while a drag
     * leaves the window */
    md->px = (int)x;
    md->py = (int)y;
    md->col = md->px >= 0 ? md->px / cw : -1 - (-1 - md->px) / cw;
    md->row = md->py >= 0 ? md->py / ch : -1 - (-1 - md->py) / ch;
  }
  if (b & 256) {
    /* kitty's leave report (pixel mode only): nothing to act on */
    return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
  }
  if (b & 64) {
    /* 66/67 are horizontal wheel -- no action for those yet */
    if (btn != 0 && btn != 1) {
      return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
    }
    md->action = btn == 0 ? CTUI_MOUSE_SCROLL_UP : CTUI_MOUSE_SCROLL_DOWN;
    md->button = -1;
  } else if (b & 32) {
    md->action = CTUI_MOUSE_MOTION;
    md->button = btn == 3 && !(b & 128) ? -1 : btn;
  } else {
    md->action = final == 'M' ? CTUI_MOUSE_PRESS : CTUI_MOUSE_RELEASE;
    md->button = btn;
  }

  ev->type = CTUI_MOUSE_EVENT;
  ev->ev_source = "input";
  ev->event_data = md;
  ctui_logf(E_INF,
            "[CTUI:INPUT] - resolved mouse action=%d button=%d @ %d,%d "
            "(mods=0x%x) @ tick %d\n",
            md->action, md->button, md->col, md->row, md->mods,
            ctui_tick_advance());
  return 1;
}

/* a kitty keyboard protocol report (ctui_kitty_keys_enable()): "CSI code;
 * mods u", code a Unicode codepoint. Decoded to the event the legacy bytes
 * for the same key would give, so a handler can't tell the difference
 * unless it looks at mods -- which now also say shift/ctrl on Enter, Tab
 * and Backspace, the point of turning the protocol on. */
static int resolve_csi_u(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp,
                         long code, unsigned int mods) {
  switch (code) {
  case 13:
    return resolve_key(ev, kp, CTUI_KEY_ENTER, 0, mods);
  case 9:
    return mods & CTUI_MOD_SHIFT
               ? resolve_key(ev, kp, CTUI_KEY_BACKTAB, 0,
                             mods & ~(unsigned int)CTUI_MOD_SHIFT)
               : resolve_key(ev, kp, CTUI_KEY_TAB, 0, mods);
  case 27:
    return resolve_key(ev, kp, CTUI_KEY_ESC, 0, mods);
  default:
    break;
  }
  /* 57344-63743 are kitty's own codes for keys without a codepoint
   * (keypad, media keys, lone modifiers) */
  if ((code < 0x20 && code != 8) || code > 0x10FFFF ||
      (code >= 57344 && code <= 63743)) {
    return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
  }
  uint32_t ch = (uint32_t)code;
  /* ctrl+letter as its control byte, without the CTRL bit, like legacy */
  if ((mods & CTUI_MOD_CTRL) && ((ch >= 'a' && ch <= 'z') ||
                                 (ch >= '@' && ch <= '_'))) {
    ch &= 0x1f;
    mods &= ~(unsigned int)CTUI_MOD_CTRL;
  }
  return resolve_key(ev, kp, CTUI_KEY_CHAR, ch, mods);
}

/* everything after "\x1b[": parameter/intermediate bytes, then one final
 * byte in 0x40-0x7e. Unrecognised sequences resolve to CTUI_KEY_NONE --
 * consumed whole, so their tail never leaks through as stray CHAR events
 * (and never as CTUI_KEY_ESC, which would quit a default app). */
static int resolve_csi(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp,
                       CTUI_MOUSE_EVENT_DATA *md) {
  char params[32];
  size_t n = 0;
  unsigned char c;
  for (;;) {
    if (!read_byte_timeout(&c, CTUI_INPUT_SEQ_TIMEOUT_MS)) {
      ctui_logf(E_WRN, "[CTUI:INPUT] - truncated CSI sequence @ tick %d\n",
                ctui_tick_advance());
      return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
    }
    if (c >= 0x40 && c <= 0x7e) {
      break;
    }
    if (n < sizeof params - 1) {
      params[n++] = (char)c;
    }
  }
  params[n] = '\0';

  if (params[0] == '<' && (c == 'M' || c == 'm')) {
    return resolve_mouse(ev, kp, md, params + 1, c);
  }
  /* focus reports (ctui_focus_enable()): a bare CSI I / CSI O */
  if (!params[0] && (c == 'I' || c == 'O')) {
    static CTUI_FOCUS_EVENT_DATA focus_data;
    focus_data.focused = c == 'I';
    ev->type = CTUI_FOCUS_EVENT;
    ev->ev_source = "input";
    ev->event_data = &focus_data;
    ctui_logf(E_INF, "[CTUI:INPUT] - resolved focus %s @ tick %d\n",
              c == 'I' ? "in" : "out", ctui_tick_advance());
    return 1;
  }

  /* "p1;p2": p1 selects the key for '~' finals, p2 is 1 + modifier bits */
  char *end;
  long p1 = strtol(params, &end, 10);
  long p2 = *end == ';' ? strtol(end + 1, NULL, 10) : 1;
  unsigned int mods = p2 > 1 ? (unsigned int)(p2 - 1) & 7u : 0;

  switch (c) {
  case 'u':
    return resolve_csi_u(ev, kp, p1, mods);
  case 'A':
    return resolve_key(ev, kp, CTUI_KEY_UP, 0, mods);
  case 'B':
    return resolve_key(ev, kp, CTUI_KEY_DOWN, 0, mods);
  case 'C':
    return resolve_key(ev, kp, CTUI_KEY_RIGHT, 0, mods);
  case 'D':
    return resolve_key(ev, kp, CTUI_KEY_LEFT, 0, mods);
  case 'H':
    return resolve_key(ev, kp, CTUI_KEY_HOME, 0, mods);
  case 'F':
    return resolve_key(ev, kp, CTUI_KEY_END, 0, mods);
  case 'Z':
    return resolve_key(ev, kp, CTUI_KEY_BACKTAB, 0, mods);
  case 'P':
  case 'Q':
  case 'R':
  case 'S':
    /* F1-F4 with modifiers: "CSI 1;5P" (bare, they come as SS3) */
    if (p1 <= 1) {
      return resolve_key(ev, kp, (CTUI_KEYTYPE)(CTUI_KEY_F1 + (c - 'P')), 0,
                         mods);
    }
    break;
  case '~':
    /* F5-F12 skip 16 and 22 (the VT220's gaps); 11-14 are F1-F4 on some
     * terminals */
    if ((p1 >= 11 && p1 <= 15) || (p1 >= 17 && p1 <= 21) || p1 == 23 ||
        p1 == 24) {
      int n = p1 <= 15 ? (int)p1 - 11 : p1 <= 21 ? (int)p1 - 12 : (int)p1 - 13;
      return resolve_key(ev, kp, (CTUI_KEYTYPE)(CTUI_KEY_F1 + n), 0, mods);
    }
    switch (p1) {
    case 1:
    case 7:
      return resolve_key(ev, kp, CTUI_KEY_HOME, 0, mods);
    case 4:
    case 8:
      return resolve_key(ev, kp, CTUI_KEY_END, 0, mods);
    case 2:
      return resolve_key(ev, kp, CTUI_KEY_INSERT, 0, mods);
    case 3:
      return resolve_key(ev, kp, CTUI_KEY_DELETE, 0, mods);
    case 5:
      return resolve_key(ev, kp, CTUI_KEY_PGUP, 0, mods);
    case 6:
      return resolve_key(ev, kp, CTUI_KEY_PGDN, 0, mods);
    default:
      break;
    }
    break;
  default:
    break;
  }
  ctui_logf(E_DBG,
            "[CTUI:INPUT] - ignoring unhandled CSI \"%s%c\" @ tick %d\n",
            params, c, ctui_tick_advance());
  return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
}

/* a UTF-8 lead byte arrived: pull its continuation bytes (sent in the same
 * write as the lead) and decode the codepoint */
static uint32_t read_utf8_rest(unsigned char lead) {
  int len = (lead & 0xE0) == 0xC0   ? 2
            : (lead & 0xF0) == 0xE0 ? 3
            : (lead & 0xF8) == 0xF0 ? 4
                                    : 1;
  char buf[5] = {(char)lead};
  for (int i = 1; i < len; i++) {
    unsigned char c;
    if (!read_byte_timeout(&c, CTUI_INPUT_SEQ_TIMEOUT_MS)) {
      break;
    }
    buf[i] = (char)c;
  }
  uint32_t cp;
  ctui_utf8_decode(buf, &cp);
  return cp;
}

/* one non-ESC byte (or the byte after an ESC, for alt+key) -> a key */
static int resolve_byte(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp,
                        unsigned char c, unsigned int mods) {
  if (c == '\r' || c == '\n') {
    return resolve_key(ev, kp, CTUI_KEY_ENTER, 0, mods);
  }
  if (c == '\t') {
    return resolve_key(ev, kp, CTUI_KEY_TAB, 0, mods);
  }
  if (c >= 0x80) {
    return resolve_key(ev, kp, CTUI_KEY_CHAR, read_utf8_rest(c), mods);
  }
  return resolve_key(ev, kp, CTUI_KEY_CHAR, c, mods);
}

/* everything after "\x1b]" up to BEL or ESC \ -> CTUI_OSC_EVENT. A body
 * that stops arriving mid-way (no terminator within the sequence timeout)
 * is dropped as CTUI_KEY_NONE, like an unfinished CSI. */
static int resolve_osc(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp) {
  static char body[CTUI_OSC_MAX + 1];
  static CTUI_OSC_EVENT_DATA osc;
  size_t n = 0;
  int truncated = 0;
  unsigned char c;
  for (;;) {
    if (!read_byte_timeout(&c, CTUI_INPUT_SEQ_TIMEOUT_MS)) {
      ctui_logf(E_WRN, "[CTUI:INPUT] - unfinished OSC (%zu bytes) @ tick %d\n",
                n, ctui_tick_advance());
      return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
    }
    if (c == 0x07) {
      break;
    }
    if (c == 0x1b) {
      /* ST is ESC \; anything else after an ESC ends it too */
      read_byte_timeout(&c, CTUI_INPUT_SEQ_TIMEOUT_MS);
      break;
    }
    if (n < CTUI_OSC_MAX) {
      body[n++] = (char)c;
    } else {
      truncated = 1;
    }
  }
  body[n] = '\0';
  size_t digits = 0;
  while (digits < n && body[digits] >= '0' && body[digits] <= '9') {
    digits++;
  }
  osc.code = -1;
  osc.text = body;
  if (digits && digits < 10 && (digits == n || body[digits] == ';')) {
    osc.code = atoi(body);
    osc.text = body + digits + (digits < n);
  }
  osc.len = strlen(osc.text);
  osc.truncated = truncated;
  ev->type = CTUI_OSC_EVENT;
  ev->ev_source = "input";
  ev->event_data = &osc;
  ctui_logf(E_INF, "[CTUI:INPUT] - resolved OSC %d (%zu bytes%s) @ tick %d\n",
            osc.code, osc.len, truncated ? ", truncated" : "",
            ctui_tick_advance());
  return 1;
}

/* an APC (ESC _ ... ST): the terminal's reply to a kitty graphics
 * command (G...), handed to gfx.c's log; never a key. Anything else in
 * APC form is dropped. */
static int resolve_apc(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp) {
  char body[512];
  size_t n = 0;
  unsigned char c;
  for (;;) {
    if (!read_byte_timeout(&c, CTUI_INPUT_SEQ_TIMEOUT_MS)) {
      ctui_logf(E_WRN, "[CTUI:INPUT] - unfinished APC (%zu bytes) @ tick %d\n",
                n, ctui_tick_advance());
      return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
    }
    if (c == 0x07) {
      break;
    }
    if (c == 0x1b) {
      read_byte_timeout(&c, CTUI_INPUT_SEQ_TIMEOUT_MS); /* ST's \ */
      break;
    }
    if (n < sizeof body - 1) {
      body[n++] = (char)c;
    }
  }
  body[n] = '\0';
  if (body[0] == 'G') {
    ctui_gfx_kitty_reply(body + 1);
  } else {
    ctui_logf(E_DBG, "[CTUI:INPUT] - dropped an APC (%zu bytes) @ tick %d\n", n,
              ctui_tick_advance());
  }
  return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
}

static int resolve_escape(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp,
                          CTUI_MOUSE_EVENT_DATA *md) {
  unsigned char c;
  if (!read_byte_timeout(&c, CTUI_INPUT_SEQ_TIMEOUT_MS)) {
    return resolve_key(ev, kp, CTUI_KEY_ESC, 0, 0);
  }
  if (c == '[') {
    return resolve_csi(ev, kp, md);
  }
  if (c == ']') {
    return resolve_osc(ev, kp);
  }
  if (c == '_') {
    return resolve_apc(ev, kp);
  }
  if (c == 'O') {
    /* SS3: arrows/home/end in application cursor mode, F1-F4 */
    unsigned char f;
    if (read_byte_timeout(&f, CTUI_INPUT_SEQ_TIMEOUT_MS)) {
      switch (f) {
      case 'A':
        return resolve_key(ev, kp, CTUI_KEY_UP, 0, 0);
      case 'B':
        return resolve_key(ev, kp, CTUI_KEY_DOWN, 0, 0);
      case 'C':
        return resolve_key(ev, kp, CTUI_KEY_RIGHT, 0, 0);
      case 'D':
        return resolve_key(ev, kp, CTUI_KEY_LEFT, 0, 0);
      case 'H':
        return resolve_key(ev, kp, CTUI_KEY_HOME, 0, 0);
      case 'F':
        return resolve_key(ev, kp, CTUI_KEY_END, 0, 0);
      case 'P':
      case 'Q':
      case 'R':
      case 'S':
        return resolve_key(ev, kp, (CTUI_KEYTYPE)(CTUI_KEY_F1 + (f - 'P')), 0,
                           0);
      default:
        break;
      }
    }
    return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
  }
  if (c == 0x1b) {
    return resolve_key(ev, kp, CTUI_KEY_ESC, 0, CTUI_MOD_ALT);
  }
  return resolve_byte(ev, kp, c, CTUI_MOD_ALT);
}

/* absolute deadline for the next CTUI_TICK_EVENT, or 0 when none is armed.
 * Armed on the first wait after an input event and only reset by input,
 * not by timer wakes or fd activity -- so "tick after tick_ms without
 * input" holds no matter how often other sources wake the loop. */
static long g_tick_due = 0;

static const CTUI_INPUT_SOURCE *g_source = NULL;

void ctui_input_set_source(const CTUI_INPUT_SOURCE *src) {
  ctui_logf(E_INF, "[CTUI:INPUT] - events come from %s @ tick %d\n",
            src ? "a source" : "the terminal", ctui_tick_advance());
  g_source = src;
}

/* 1: *ev is the source's event, 0: none, -1: the source ended */
static int source_next(CTUI_EVENT *ev, int readable) {
  int r = g_source->next(g_source->ctx, ev, readable);
  if (r > 0) {
    g_tick_due = 0;
  } else if (r < 0) {
    ctui_logf(E_INF, "[CTUI:INPUT] - the source ended @ tick %d\n",
              ctui_tick_advance());
  }
  return r;
}

int ctui_input_loop(CTUI_EVENT *ev, int tick_ms) {
  /* owns its own event_data storage rather than relying on the caller to
   * pre-populate ev->event_data, since which struct shape is needed depends
   * on which event type this call ends up producing */
  static CTUI_KEYPRESS_EVENT_DATA kp_data;
  static CTUI_RESIZE_EVENT_DATA resize_data;
  static CTUI_MOUSE_EVENT_DATA mouse_data;
  static CTUI_IO_EVENT_DATA io_data;
  char c;

  for (;;) {
    if (ctui_g_resize_pending) {
      ctui_g_resize_pending = 0;
      ctui_get_termsize(&resize_data.rows, &resize_data.cols);
      ev->type = CTUI_RESIZE_EVENT;
      ev->ev_source = "terminal";
      ev->event_data = &resize_data;
      ctui_logf(E_INF, "[CTUI:INPUT] - resize detected @ tick %d (%dx%d)\n",
                ctui_tick_advance(), resize_data.cols, resize_data.rows);
      return 1;
    }

    if (g_source) {
      int r = source_next(ev, 0);
      if (r != 0) {
        return r > 0;
      }
    }

    /* pushed-back bytes are already "readable"; select() would not see
     * them and would sit out the whole timeout before we ever looked */
    if (g_source || !pushback_pending()) {
      int in_fd = g_source ? g_source->fd : STDIN_FILENO;
      fd_set rfds, wfds;
      FD_ZERO(&rfds);
      FD_ZERO(&wfds);
      int maxfd = -1;
      if (in_fd >= 0) {
        FD_SET(in_fd, &rfds);
        maxfd = in_fd;
      }
      ctui_io_fill(&rfds, &wfds, &maxfd);

      long now = mono_ms();
      long timeout = -1;
      if (tick_ms > 0) {
        if (g_tick_due == 0) {
          g_tick_due = now + tick_ms;
        }
        timeout = g_tick_due > now ? g_tick_due - now : 0;
      }
      long timer_due = ctui_timer_ms_until_due();
      if (timer_due >= 0 && (timeout < 0 || timer_due < timeout)) {
        timeout = timer_due;
      }
      struct timeval tv = {.tv_sec = timeout / 1000,
                           .tv_usec = (timeout % 1000) * 1000};

      ctui_logf(E_DBG,
                "[CTUI:INPUT] - waiting @ tick %d (timeout=%ldms, maxfd=%d)\n",
                ctui_tick_advance(), timeout, maxfd);
      int r = select(maxfd + 1, &rfds, &wfds, NULL, timeout >= 0 ? &tv : NULL);
      if (r < 0) {
        if (errno == EINTR) {
          /* almost certainly SIGWINCH; loop back to the pending-resize
           * check */
          continue;
        }
        ctui_logf(E_WRN, "[CTUI:INPUT] - select failed @ tick %d\n",
                  ctui_tick_advance());
        return 0;
      }
      if (r == 0) {
        ev->event_data = NULL;
        ev->ev_source = "timer";
        if (tick_ms > 0 && mono_ms() >= g_tick_due) {
          g_tick_due = 0;
          ev->type = CTUI_TICK_EVENT;
          ctui_logf(E_DBG, "[CTUI:INPUT] - tick @ tick %d\n",
                    ctui_tick_advance());
        } else {
          /* woke for a timer deadline: ctui_app_run() runs
           * ctui_timer_tick() after every event, which fires it */
          ev->type = CTUI_TIMER_EVENT;
          ctui_logf(E_DBG, "[CTUI:INPUT] - timer wake @ tick %d\n",
                    ctui_tick_advance());
        }
        return 1;
      }
      if (in_fd < 0 || !FD_ISSET(in_fd, &rfds)) {
        if (ctui_io_ready(&rfds, &wfds, &io_data)) {
          ev->type = CTUI_IO_EVENT;
          ev->ev_source = "io";
          ev->event_data = &io_data;
          return 1;
        }
        continue;
      }
      if (g_source) {
        int sr = source_next(ev, 1);
        if (sr != 0) {
          return sr > 0;
        }
        continue;
      }
    }

    ssize_t n = input_read(&c);
    if (n == 1) {
      break;
    }
    /* only a failed read sets errno: at EOF (a hung-up tty's read() too)
     * it's whatever an earlier call left, an EINTR as like as not */
    if (n < 0 && errno == EINTR) {
      /* almost certainly SIGWINCH; loop back to the pending-resize check */
      continue;
    }
    ctui_logf(E_WRN, "[CTUI:INPUT] - read failed/EOF @ tick %d\n",
              ctui_tick_advance());
    return 0;
  }

  g_tick_due = 0;
  if (c == '\x1b') {
    return resolve_escape(ev, &kp_data, &mouse_data);
  }
  return resolve_byte(ev, &kp_data, (unsigned char)c, 0);
}
