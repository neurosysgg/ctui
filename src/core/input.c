#define _POSIX_C_SOURCE 200809L

#include "input.h"

#include "ctui_internal.h"
#include "log.h"
#include "term.h"

#include <errno.h>
#include <string.h>
#include <sys/select.h>
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

static int read_byte_timeout(char *c, int timeout_ms) {
  if (pushback_pending()) {
    *c = g_pushback[g_pushback_pos++];
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout got pushback byte 0x%02x @ "
              "tick %d\n",
              (unsigned char)*c, ctui_tick_advance());
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
  int ok = input_read(c) == 1;
  if (ok) {
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout got byte 0x%02x @ tick %d\n",
              (unsigned char)*c, ctui_tick_advance());
  } else {
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout read failed @ tick %d\n",
              ctui_tick_advance());
  }
  return ok;
}

int ctui_input_loop(CTUI_EVENT *ev, int tick_ms) {
  /* owns its own event_data storage rather than relying on the caller to
   * pre-populate ev->event_data, since which struct shape is needed depends
   * on which event type this call ends up producing */
  static CTUI_KEYPRESS_EVENT_DATA kp_data;
  static CTUI_RESIZE_EVENT_DATA resize_data;
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
     * them and would sit out the whole tick_ms before we ever looked. */
    if (tick_ms > 0 && !pushback_pending()) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(STDIN_FILENO, &fds);
      struct timeval tv = {.tv_sec = tick_ms / 1000,
                           .tv_usec = (tick_ms % 1000) * 1000};
      ctui_logf(E_DBG, "[CTUI:INPUT] - waiting for input @ tick %d (tick_ms=%d)\n",
                ctui_tick_advance(), tick_ms);
      int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
      if (r == 0) {
        ev->type = CTUI_TICK_EVENT;
        ev->ev_source = "timer";
        ev->event_data = NULL;
        ctui_logf(E_DBG, "[CTUI:INPUT] - tick @ tick %d\n",
                  ctui_tick_advance());
        return 1;
      }
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
      /* fds ready; fall through to the read() below */
    } else {
      ctui_logf(E_DBG, "[CTUI:INPUT] - waiting for input @ tick %d\n",
                ctui_tick_advance());
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

  ev->type = CTUI_KEYPRESS_EVENT;
  ev->ev_source = "input";
  ev->event_data = &kp_data;
  CTUI_KEYPRESS_EVENT_DATA *ev_data = &kp_data;

  if (c == '\x1b') {
    char seq0, seq1;
    /* single ESC produces no follow-up; arrow key does */
    if (!read_byte_timeout(&seq0, 50)) {
      ev_data->type = CTUI_KEY_ESC;
      ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                ctui_keytype_name(ev_data->type), ctui_tick_advance());
      return 1;
    }

    if (!read_byte_timeout(&seq1, 50)) {
      ev_data->type = CTUI_KEY_ESC;
      ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                ctui_keytype_name(ev_data->type), ctui_tick_advance());
      return 1;
    }

    if (seq0 == '[') {
      switch (seq1) {
      case 'A':
        ev_data->type = CTUI_KEY_UP;
        ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                  ctui_keytype_name(ev_data->type), ctui_tick_advance());
        return 1;
      case 'B':
        ev_data->type = CTUI_KEY_DOWN;
        ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                  ctui_keytype_name(ev_data->type), ctui_tick_advance());
        return 1;
      case 'C':
        ev_data->type = CTUI_KEY_RIGHT;
        ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                  ctui_keytype_name(ev_data->type), ctui_tick_advance());
        return 1;
      case 'D':
        ev_data->type = CTUI_KEY_LEFT;
        ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                  ctui_keytype_name(ev_data->type), ctui_tick_advance());
        return 1;
      default:
        break;
      }
    }

    ev_data->type = CTUI_KEY_ESC;
    ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
              ctui_keytype_name(ev_data->type), ctui_tick_advance());
    return 1;
  }

  if (c == '\r' || c == '\n') {
    ev_data->type = CTUI_KEY_ENTER;
    ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
              ctui_keytype_name(ev_data->type), ctui_tick_advance());
    return 1;
  }

  if (c == '\t') {
    ev_data->type = CTUI_KEY_TAB;
    ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
              ctui_keytype_name(ev_data->type), ctui_tick_advance());
    return 1;
  }

  ev_data->type = CTUI_KEY_CHAR;
  ev_data->ch = c;
  ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s ('%c') @ tick %d\n",
            ctui_keytype_name(ev_data->type), c, ctui_tick_advance());
  return 1;
}
