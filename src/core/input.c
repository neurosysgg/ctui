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
  if (x < 1 || y < 1) {
    ctui_logf(E_WRN, "[CTUI:INPUT] - malformed SGR mouse report @ tick %d\n",
              ctui_tick_advance());
    return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
  }

  int btn = (int)(b & 3);
  md->mods = ((b & 4) ? CTUI_MOD_SHIFT : 0) | ((b & 8) ? CTUI_MOD_ALT : 0) |
             ((b & 16) ? CTUI_MOD_CTRL : 0);
  md->row = (int)y - 1;
  md->col = (int)x - 1;
  if (b & 64) {
    /* 66/67 are horizontal wheel -- no action for those yet */
    if (btn > 1) {
      return resolve_key(ev, kp, CTUI_KEY_NONE, 0, 0);
    }
    md->action = btn == 0 ? CTUI_MOUSE_SCROLL_UP : CTUI_MOUSE_SCROLL_DOWN;
    md->button = -1;
  } else if (b & 32) {
    md->action = CTUI_MOUSE_MOTION;
    md->button = btn == 3 ? -1 : btn;
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

  /* "p1;p2": p1 selects the key for '~' finals, p2 is 1 + modifier bits */
  char *end;
  long p1 = strtol(params, &end, 10);
  long p2 = *end == ';' ? strtol(end + 1, NULL, 10) : 1;
  unsigned int mods = p2 > 1 ? (unsigned int)(p2 - 1) & 7u : 0;

  switch (c) {
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
  case '~':
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

static int resolve_escape(CTUI_EVENT *ev, CTUI_KEYPRESS_EVENT_DATA *kp,
                          CTUI_MOUSE_EVENT_DATA *md) {
  unsigned char c;
  if (!read_byte_timeout(&c, CTUI_INPUT_SEQ_TIMEOUT_MS)) {
    return resolve_key(ev, kp, CTUI_KEY_ESC, 0, 0);
  }
  if (c == '[') {
    return resolve_csi(ev, kp, md);
  }
  if (c == 'O') {
    /* SS3: arrows/home/end in application cursor mode */
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
    if (g_resize_pending) {
      g_resize_pending = 0;
      ctui_get_termsize(&resize_data.rows, &resize_data.cols);
      ev->type = CTUI_RESIZE_EVENT;
      ev->ev_source = "terminal";
      ev->event_data = &resize_data;
      ctui_logf(E_INF, "[CTUI:INPUT] - resize detected @ tick %d (%dx%d)\n",
                ctui_tick_advance(), resize_data.cols, resize_data.rows);
      return 1;
    }

    /* pushed-back bytes are already "readable"; select() would not see
     * them and would sit out the whole timeout before we ever looked */
    if (!pushback_pending()) {
      fd_set rfds, wfds;
      FD_ZERO(&rfds);
      FD_ZERO(&wfds);
      FD_SET(STDIN_FILENO, &rfds);
      int maxfd = STDIN_FILENO;
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
      if (!FD_ISSET(STDIN_FILENO, &rfds)) {
        if (ctui_io_ready(&rfds, &wfds, &io_data)) {
          ev->type = CTUI_IO_EVENT;
          ev->ev_source = "io";
          ev->event_data = &io_data;
          return 1;
        }
        continue;
      }
    }

    if (input_read(&c) == 1) {
      break;
    }
    if (errno == EINTR) {
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
