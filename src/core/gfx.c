/* must precede every #include, same reasoning as term.c's own copy of
 * this comment: shm_open()/poll()/clock_gettime() are POSIX, not C11,
 * and -std=c11 sets __STRICT_ANSI__ which suppresses glibc's exposure of
 * them without this. */
#define _POSIX_C_SOURCE 200809L

#include "gfx.h"

#include "cell.h"
#include "ctui_internal.h"
#include "deflate.h"
#include "log.h"
#include "profile.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

unsigned int ctui_gfx_detect_caps(void) {
  unsigned int caps = CTUI_GFX_ANSI16;

  const char *term = getenv("TERM");
  const char *colorterm = getenv("COLORTERM");
  const char *kitty_window = getenv("KITTY_WINDOW_ID");

  if (term && strstr(term, "256color")) {
    caps |= CTUI_GFX_ANSI256;
  }

  if (colorterm &&
      (strcmp(colorterm, "truecolor") == 0 || strcmp(colorterm, "24bit") == 0)) {
    /* a truecolor terminal is a 256-color terminal too */
    caps |= CTUI_GFX_TRUECOLOR | CTUI_GFX_ANSI256;
  }

  if (kitty_window || (term && strcmp(term, "xterm-kitty") == 0)) {
    /* kitty implies both text tiers below it */
    caps |= CTUI_GFX_KITTY | CTUI_GFX_TRUECOLOR | CTUI_GFX_ANSI256;
  }

  ctui_logf(E_INF,
            "[CTUI:GFX] - detected caps 0x%x @ tick %d (TERM=%s, "
            "COLORTERM=%s, KITTY_WINDOW_ID=%s)\n",
            caps, ctui_tick_advance(), term ? term : "(unset)",
            colorterm ? colorterm : "(unset)",
            kitty_window ? kitty_window : "(unset)");
  return caps;
}

/* xterm's standard 16-color palette, normal-intensity half (indices 0-7,
 * matching CTUI_COLOR_BLACK..WHITE) -- see the doc comment on
 * ctui_gfx_ansi16_rgb() in gfx.h for what this table is/isn't claiming. */
static const unsigned char ctui_ansi16_rgb_table[9][3] = {
    [CTUI_COLOR_DEFAULT] = {0, 0, 0},     [CTUI_COLOR_BLACK] = {0, 0, 0},
    [CTUI_COLOR_RED] = {205, 0, 0},       [CTUI_COLOR_GREEN] = {0, 205, 0},
    [CTUI_COLOR_YELLOW] = {205, 205, 0},  [CTUI_COLOR_BLUE] = {0, 0, 238},
    [CTUI_COLOR_MAGENTA] = {205, 0, 205}, [CTUI_COLOR_CYAN] = {0, 205, 205},
    [CTUI_COLOR_WHITE] = {229, 229, 229},
};

void ctui_gfx_ansi16_rgb(unsigned char color, unsigned char *r,
                         unsigned char *g, unsigned char *b) {
  const unsigned char *rgb =
      ctui_ansi16_rgb_table[color <= CTUI_COLOR_WHITE ? color : 0];
  *r = rgb[0];
  *g = rgb[1];
  *b = rgb[2];
}

/* kitty splits the *encoded* payload into chunks, not the raw pixel data
 * -- concatenation happens terminal-side after base64-decoding every
 * chunk in sequence, so any byte offset into the encoded string is a
 * valid split point. 4096 matches the protocol spec's own chunk-size
 * guidance. */
#define CTUI_KITTY_CHUNK 4096

/* Phase 5a: every escape fragment ctui_gfx_kitty_display()/
 * ctui_gfx_kitty_delete() used to write() individually (the CUP escape,
 * every chunk's key-list prefix, every chunk's base64 body, every chunk's
 * terminator) now appends here instead -- one real write() per frame,
 * issued by ctui_gfx_kitty_flush(), instead of dozens. Same growable,
 * high-water-mark, never-shrinks shape as core/widget.c's gfx_pending
 * array: capacity only ever grows, len resets to 0 once flushed. */
static char *g_kitty_batch = NULL;
static size_t g_kitty_batch_len = 0, g_kitty_batch_cap = 0;

static void kitty_batch_append(const void *data, size_t len) {
  if (g_kitty_batch_len + len > g_kitty_batch_cap) {
    g_kitty_batch_cap = g_kitty_batch_cap ? g_kitty_batch_cap * 2 : 8192;
    while (g_kitty_batch_len + len > g_kitty_batch_cap) {
      g_kitty_batch_cap *= 2;
    }
    g_kitty_batch = realloc(g_kitty_batch, g_kitty_batch_cap);
  }
  memcpy(g_kitty_batch + g_kitty_batch_len, data, len);
  g_kitty_batch_len += len;
}

void ctui_gfx_kitty_flush(void) {
  CTUI_PROFILE_SPAN sp = ctui_profile_begin();
  if (g_kitty_batch_len > 0) {
    write(STDOUT_FILENO, g_kitty_batch, g_kitty_batch_len);
  }
  ctui_profile_end(sp, "gfx.kitty_batch_flush");
  g_kitty_batch_len = 0;
}

/* Phase 5b: our own encoder's correctness is the actual risk here (no
 * env var signals a real Kitty terminal's o=z support is broken, and o=z
 * is spec-mandated), not terminal compatibility -- so this is a plain
 * runtime toggle, not a CTUI_GFX_MODE bit. Default on. */
static int g_kitty_compression_enabled = 1;

void ctui_gfx_kitty_set_compression(int enabled) {
  g_kitty_compression_enabled = enabled ? 1 : 0;
  ctui_logf(E_INF,
            "[CTUI:GFX] - kitty compression %s @ tick %d\n",
            g_kitty_compression_enabled ? "enabled" : "disabled",
            ctui_tick_advance());
}

/* Phase 6: -1 = not yet probed, 0 = probed and unsupported, 1 = probed
 * and supported. Cached once per process by ctui_gfx_kitty_probe_shm()
 * (called from ctui_init(), term.c) so every later
 * ctui_gfx_kitty_display() call just branches on this instead of
 * re-probing per frame. A value of -1 reaching ctui_gfx_kitty_display()
 * (e.g. a headless caller that never went through ctui_init()) is
 * treated the same as 0 -- fall back to t=d -- by the plain `== 1` check
 * at the call site below, not a separate branch. */
static int g_kitty_shm_supported = -1;

/* monotonic per-process counter: every shm segment this file ever opens
 * (probe or real frame) gets a unique name, never reused across calls.
 * Reusing a name across frames would race a slow-to-shm_unlink() prior
 * transmission (the terminal is responsible for unlinking, on its own
 * schedule -- see GFX_DESIGN.md's Phase 6) against this frame's
 * shm_open(O_EXCL); a fresh name every time sidesteps that entirely
 * instead of trying to detect/recover from the collision. */
static unsigned long g_kitty_shm_seq = 0;

static void kitty_shm_name(char *buf, size_t cap, unsigned long seq) {
  snprintf(buf, cap, "/ctui-k-%d-%lu", (int)getpid(), seq);
}

/* snprintf() returns the length it *would* have written, which is >= cap
 * on truncation -- handing that straight to write()/kitty_batch_append()
 * as a length reads past the buffer. Every control string built below is
 * comfortably inside its buffer today; this is what keeps that true if a
 * future key or a longer shm name ever changes the arithmetic. */
static size_t clamp_snprintf_len(int n, size_t cap) {
  if (n < 0) {
    return 0;
  }
  return (size_t)n >= cap ? cap - 1 : (size_t)n;
}

/* Phase 6 shm reaping. Per the Kitty spec the *terminal* unlinks a t=s
 * segment once it has read it, which is why kitty_display_shm() doesn't
 * -- unlinking from this side would race a terminal that hasn't opened
 * the name yet. But that leaves nothing responsible for a segment the
 * terminal never reads: an image rejected under q=2 (errors suppressed,
 * so we never hear about it), a terminal that crashes or detaches, or
 * this process exiting between the write and the terminal's read. Each
 * orphan is a whole frame's payload sitting in /dev/shm, i.e. in RAM, and
 * at 50fps across several widgets that accumulates fast.
 *
 * So: remember the last CTUI_KITTY_SHM_TRACKED names and unlink each one
 * as its slot gets reused, plus everything still outstanding at
 * ctui_shutdown(). By the time a slot comes back around the terminal has
 * long since read and unlinked that segment, so the unlink almost always
 * fails with ENOENT -- which is exactly the intended outcome, and why the
 * return value is ignored. The one case it succeeds is the case this
 * exists for. Bounds the worst case at CTUI_KITTY_SHM_TRACKED orphans
 * rather than one per frame forever. */
#define CTUI_KITTY_SHM_TRACKED 64
static char g_kitty_shm_live[CTUI_KITTY_SHM_TRACKED][64];
static int g_kitty_shm_live_next = 0;

static void kitty_shm_track(const char *name) {
  char *slot = g_kitty_shm_live[g_kitty_shm_live_next];
  if (slot[0] != '\0') {
    shm_unlink(slot); /* ENOENT expected -- see the doc comment above */
  }
  snprintf(slot, sizeof(g_kitty_shm_live[0]), "%s", name);
  g_kitty_shm_live_next = (g_kitty_shm_live_next + 1) % CTUI_KITTY_SHM_TRACKED;
}

void ctui_gfx_kitty_shm_reap(void) {
  for (int i = 0; i < CTUI_KITTY_SHM_TRACKED; i++) {
    if (g_kitty_shm_live[i][0] != '\0') {
      shm_unlink(g_kitty_shm_live[i]);
      g_kitty_shm_live[i][0] = '\0';
    }
  }
}

/* true if buf (len bytes, a raw APC reply captured off stdin) contains a
 * success reply keyed to image_id, e.g. "\x1b_Gi=1;OK\x1b\\" -- factored
 * out of ctui_gfx_kitty_probe_shm() purely so the parsing itself is
 * unit-testable without a real terminal (tests/kitty_protocol_test.c
 * forward-declares this the same way it already gray-box-declares
 * g_gfx_mode; not part of the public gfx.h surface, same reasoning).
 * Deliberately loose -- scans for "i=<id>" then requires "OK" right
 * after the next ';', rather than a strict grammar parse, since a real
 * terminal's reply is one short trusted line, not adversarial input. */
int ctui_gfx_kitty_reply_is_ok(const char *buf, size_t len,
                               unsigned int image_id) {
  if (buf == NULL || len == 0) {
    return 0;
  }
  char needle[32];
  int needle_len = snprintf(needle, sizeof needle, "i=%u", image_id);
  for (size_t i = 0; i + (size_t)needle_len <= len; i++) {
    if (memcmp(buf + i, needle, (size_t)needle_len) != 0) {
      continue;
    }
    /* "i=1" must not match the "i=1" prefix of "i=10" -- the id has to
     * end where the needle does. Only reachable today via the probe,
     * which always uses image_id 1, but a reply keyed to a different
     * image is not evidence about this one. */
    size_t after_id = i + (size_t)needle_len;
    if (after_id < len && buf[after_id] >= '0' && buf[after_id] <= '9') {
      continue;
    }
    for (size_t j = after_id; j < len; j++) {
      if (buf[j] != ';') {
        continue;
      }
      return j + 2 < len && buf[j + 1] == 'O' && buf[j + 2] == 'K';
    }
  }
  return 0;
}

/* Locates the Kitty APC reply inside a raw probe read, so everything
 * that *isn't* the reply can be handed back to core/input.c rather than
 * discarded. A Kitty reply is "ESC _ G <payload> ESC \" -- anything
 * before or after that span is not ours: it is real terminal input that
 * happened to arrive during the probe's window, most often a keystroke
 * typed while the app was still starting up.
 *
 * On success writes the span to *start and *end (end is one past the
 * final byte of the terminator) and returns 1. Returns 0 when there is
 * no APC
 * introducer at all -- the common non-Kitty case, where the entire
 * buffer is foreign input.
 *
 * An introducer with no terminator is reported as a span running to the
 * end of the buffer: a truncated reply is still ours, and replaying half
 * an escape sequence as keystrokes would be worse than dropping it.
 *
 * Non-static and absent from gfx.h for the same reason
 * ctui_gfx_kitty_reply_is_ok() above is: not an app-facing call, just
 * factored out so the parsing is unit-testable without a live terminal
 * (tests/kitty_protocol_test.c forward-declares it). */
int ctui_gfx_kitty_apc_span(const char *buf, size_t len, size_t *start,
                            size_t *end) {
  if (buf == NULL || len < 3 || start == NULL || end == NULL) {
    return 0;
  }
  for (size_t i = 0; i + 2 < len; i++) {
    if (buf[i] != '\x1b' || buf[i + 1] != '_' || buf[i + 2] != 'G') {
      continue;
    }
    *start = i;
    for (size_t j = i + 3; j + 1 < len; j++) {
      if (buf[j] == '\x1b' && buf[j + 1] == '\\') {
        *end = j + 2;
        return 1;
      }
    }
    *end = len; /* introducer but no terminator -- see above */
    return 1;
  }
  return 0;
}

/* how long ctui_gfx_kitty_probe_shm() waits for a reply before assuming
 * the terminal either doesn't support t=s or isn't a real Kitty terminal
 * at all -- generous enough for a real terminal's near-instant APC reply
 * to arrive over a local pty, short enough that a terminal which will
 * never reply (the common case: anything that isn't Kitty) doesn't stall
 * ctui_init() noticeably. */
#define CTUI_KITTY_PROBE_TIMEOUT_MS 250

/* Phase 6: one-shot startup probe for Kitty's t=s (shared-memory)
 * transmission medium, called by ctui_init() (term.c) only once
 * CTUI_GFX_KITTY has actually been negotiated. ctui's Kitty path
 * otherwise runs entirely with q=2 (all APC replies suppressed) --
 * there's no other signal available for whether t=s specifically works
 * on this terminal, so this temporarily asks for replies (q=0) on one
 * disposable a=q (query -- doesn't display or persist anything) probe
 * image, parses whatever comes back within CTUI_KITTY_PROBE_TIMEOUT_MS,
 * and caches the result in g_kitty_shm_supported for the rest of the
 * session. No-op if already probed (idempotent) or if stdout isn't a
 * real terminal (nothing to probe). Every failure mode -- shm_open()
 * failing locally, no reply at all, a reply that isn't OK -- falls back
 * to g_kitty_shm_supported = 0 (today's t=d path), never blocks
 * ctui_init() from completing. */
void ctui_gfx_kitty_probe_shm(void) {
  if (g_kitty_shm_supported != -1) {
    return;
  }
  if (!isatty(STDOUT_FILENO)) {
    g_kitty_shm_supported = 0;
    return;
  }

  char name[64];
  kitty_shm_name(name, sizeof name, g_kitty_shm_seq++);

  int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
  if (fd < 0) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty shm probe: shm_open(%s) failed (%s) @ "
              "tick %d, falling back to t=d\n",
              name, strerror(errno), ctui_tick_advance());
    g_kitty_shm_supported = 0;
    return;
  }
  unsigned char pixel[4] = {0, 0, 0, 0};
  void *map = NULL;
  if (ftruncate(fd, (off_t)sizeof pixel) == -1 ||
      (map = mmap(NULL, sizeof pixel, PROT_WRITE, MAP_SHARED, fd, 0)) ==
          MAP_FAILED) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty shm probe: ftruncate/mmap(%s) failed (%s) "
              "@ tick %d, falling back to t=d\n",
              name, strerror(errno), ctui_tick_advance());
    close(fd);
    shm_unlink(name);
    g_kitty_shm_supported = 0;
    return;
  }
  memcpy(map, pixel, sizeof pixel);
  munmap(map, sizeof pixel);
  close(fd);

  char b64[64];
  size_t b64_len =
      ctui_util_base64_encode((const unsigned char *)name, strlen(name), b64,
                              sizeof b64);

  char out[160];
  int n = snprintf(out, sizeof out,
                   "\x1b_Ga=q,i=1,s=1,v=1,f=32,t=s,q=0;%.*s\x1b\\",
                   (int)b64_len, b64);
  ssize_t written =
      write(STDOUT_FILENO, out, clamp_snprintf_len(n, sizeof out));
  (void)written; /* best-effort: a failed write here just means the probe
                   * times out below and falls back to t=d, same as any
                   * other silent-terminal outcome */

  char reply[256];
  size_t reply_len = 0;
  struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_nsec += (long)CTUI_KITTY_PROBE_TIMEOUT_MS * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }
  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                        (deadline.tv_nsec - now.tv_nsec) / 1000000;
    if (remaining_ms <= 0 || reply_len >= sizeof reply - 1) {
      break;
    }
    int pr = poll(&pfd, 1, (int)remaining_ms);
    if (pr <= 0) {
      break; /* timeout, or poll() error -- no reply either way */
    }
    ssize_t r = read(STDIN_FILENO, reply + reply_len,
                     sizeof reply - 1 - reply_len);
    if (r <= 0) {
      break;
    }
    reply_len += (size_t)r;
  }

  /* a=q means the terminal was never asked to keep this segment open
   * past replying (unlike a real a=T transmission, which today's
   * kitty_display_shm() leaves for the terminal itself to shm_unlink())
   * -- clean up our own probe segment regardless of outcome, since
   * nothing else ever will if the terminal doesn't understand t=s/a=q at
   * all. */
  shm_unlink(name);

  /* Whatever in this read wasn't the terminal's APC reply is real input
   * -- a keystroke typed during startup, or another query's response --
   * and used to be dropped on the floor here, eating up to
   * CTUI_KITTY_PROBE_TIMEOUT_MS of the user's typing. Hand it back to
   * core/input.c in arrival order instead: the bytes before the reply
   * first, then the bytes after it. On a terminal that never replies
   * (anything that isn't Kitty) there is no span at all and the whole
   * buffer is foreign input. */
  if (reply_len > 0) {
    size_t apc_start = 0, apc_end = 0;
    if (ctui_gfx_kitty_apc_span(reply, reply_len, &apc_start, &apc_end)) {
      ctui_input_pushback(reply, apc_start);
      if (apc_end < reply_len) {
        ctui_input_pushback(reply + apc_end, reply_len - apc_end);
      }
    } else {
      ctui_input_pushback(reply, reply_len);
    }
  }

  g_kitty_shm_supported =
      ctui_gfx_kitty_reply_is_ok(reply, reply_len, 1) ? 1 : 0;
  ctui_logf(E_INF,
            "[CTUI:GFX] - kitty shm probe result: %s (%zu reply bytes) @ "
            "tick %d\n",
            g_kitty_shm_supported ? "supported (t=s)"
                                   : "unsupported, using t=d",
            reply_len, ctui_tick_advance());
}

/* t=d (direct): base64-encode payload and write it in CTUI_KITTY_CHUNK
 * pieces -- the only transport that existed before Phase 6, unchanged in
 * behavior from before this split, just factored out so
 * kitty_display_shm() below has somewhere to fall back to on any local
 * shm failure without duplicating the chunking loop. */
static void kitty_display_td(int row, int col, int cell_cols, int cell_rows,
                             const unsigned char *payload, size_t payload_len,
                             int width, int height, unsigned int image_id,
                             int z, int use_compression) {
  size_t b64_cap = ctui_util_base64_len(payload_len) + 1;
  char *b64 = malloc(b64_cap);
  if (b64 == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_display rejected @ tick %d, malloc(%zu) "
              "failed\n",
              ctui_tick_advance(), b64_cap);
    return;
  }
  /* split so a profile run can tell CPU-bound base64 encoding (scales with
   * payload_len) apart from the batch-append loop below (scales with
   * b64_len but also reflects memcpy cost into the batch buffer, not a
   * blocking syscall anymore -- see gfx.kitty_batch_flush for the span
   * that now maps to what gfx.kitty_write used to measure). */
  CTUI_PROFILE_SPAN encode_span = ctui_profile_begin();
  size_t b64_len = ctui_util_base64_encode(payload, payload_len, b64, b64_cap);
  ctui_profile_end(encode_span, "gfx.kitty_encode");

  char out[128];
  int n = snprintf(out, sizeof out, "\x1b[%d;%dH", row, col);
  kitty_batch_append(out, (size_t)n);

  CTUI_PROFILE_SPAN append_span = ctui_profile_begin();
  size_t sent = 0;
  while (sent < b64_len) {
    size_t chunk = b64_len - sent;
    if (chunk > CTUI_KITTY_CHUNK) {
      chunk = CTUI_KITTY_CHUNK;
    }
    int more = (sent + chunk) < b64_len;
    if (sent == 0) {
      /* a=T: transmit + display immediately. f=32: raw RGBA pixels (as
       * opposed to f=100, PNG-encoded). o=z: payload is zlib-wrapped
       * DEFLATE instead of raw bytes (only when use_compression actually
       * won the size comparison above) -- orthogonal to f=32, which still
       * describes the pixel format the far end reconstructs after
       * decompressing. s/v: pixel dimensions, required for raw formats
       * since there's no container header to read them from. c/r: scale
       * the image to cover this many character cells. q=2: suppress both
       * success and error responses -- ctui has no code path that reads
       * stdin for an APC reply. C=1: don't move the cursor after
       * displaying -- the protocol default (C=0) moves it to just past
       * the image, as if it had been printed as text, which for an image
       * tall/low enough to reach the terminal's last row forces the
       * terminal to scroll the whole screen to keep the cursor visible.
       * ctui always repositions the cursor explicitly (the CUP escape
       * right above, and again before every ctui_screen_flush() write)
       * rather than relying on wherever a previous write left it, so a
       * side-effect cursor move here only ever fights that -- this
       * surfaced as the whole screen drifting upward on every redraw once
       * a Kitty image (ctui-mus's footer border) first reached the last
       * row. */
      if (use_compression) {
        n = snprintf(
            out, sizeof out,
            "\x1b_Ga=T,f=32,o=z,s=%d,v=%d,c=%d,r=%d,i=%u,z=%d,q=2,C=1,m=%d;",
            width, height, cell_cols, cell_rows, image_id, z, more);
      } else {
        n = snprintf(
            out, sizeof out,
            "\x1b_Ga=T,f=32,s=%d,v=%d,c=%d,r=%d,i=%u,z=%d,q=2,C=1,m=%d;",
            width, height, cell_cols, cell_rows, image_id, z, more);
      }
    } else {
      /* continuation chunks repeat only m -- every other key was already
       * established by the first chunk */
      n = snprintf(out, sizeof out, "\x1b_Gm=%d;", more);
    }
    kitty_batch_append(out, (size_t)n);
    kitty_batch_append(b64 + sent, chunk);
    kitty_batch_append("\x1b\\", 2);
    sent += chunk;
  }
  ctui_profile_end(append_span, "gfx.kitty_batch_append");

  free(b64);
  ctui_logf(E_INF,
            "[CTUI:GFX] - kitty_display (t=d) @ tick %d (%dx%d px @ row=%d, "
            "col=%d, id=%u, z=%d, %zu b64 bytes, payload=%zu, "
            "compressed=%s)\n",
            ctui_tick_advance(), width, height, row, col, image_id, z,
            b64_len, payload_len, use_compression ? "yes" : "no");
}

/* t=s (shared memory): writes payload into a POSIX shm object and sends
 * only its base64-encoded *name* -- no base64 of the pixel payload
 * itself, no CTUI_KITTY_CHUNK chunking loop, since the control string is
 * always small regardless of image size (see GFX_DESIGN.md's Phase 6).
 * Falls back to kitty_display_td() for this one call on any local
 * failure (shm_open/ftruncate/mmap) rather than flipping
 * g_kitty_shm_supported off process-wide -- a transient local resource
 * failure (e.g. /dev/shm momentarily full) isn't evidence the terminal
 * doesn't support t=s, so it shouldn't permanently disable the fast
 * path over one bad frame. */
static void kitty_display_shm(int row, int col, int cell_cols, int cell_rows,
                              const unsigned char *payload, size_t payload_len,
                              int width, int height, unsigned int image_id,
                              int z, int use_compression) {
  char name[64];
  kitty_shm_name(name, sizeof name, g_kitty_shm_seq++);

  int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
  if (fd < 0) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty shm_open(%s) failed (%s) @ tick %d, "
              "falling back to t=d for this frame\n",
              name, strerror(errno), ctui_tick_advance());
    kitty_display_td(row, col, cell_cols, cell_rows, payload, payload_len,
                     width, height, image_id, z, use_compression);
    return;
  }
  void *map = NULL;
  if (ftruncate(fd, (off_t)payload_len) == -1 ||
      (map = mmap(NULL, payload_len, PROT_WRITE, MAP_SHARED, fd, 0)) ==
          MAP_FAILED) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty shm ftruncate/mmap(%s, %zu) failed (%s) @ "
              "tick %d, falling back to t=d for this frame\n",
              name, payload_len, strerror(errno), ctui_tick_advance());
    close(fd);
    shm_unlink(name);
    kitty_display_td(row, col, cell_cols, cell_rows, payload, payload_len,
                     width, height, image_id, z, use_compression);
    return;
  }
  memcpy(map, payload, payload_len);
  munmap(map, payload_len);
  close(fd);
  /* no shm_unlink() here -- per spec, the terminal itself unlinks a t=s
   * segment once it's read it (see GFX_DESIGN.md's Phase 6); unlinking
   * from this side too would race whenever the terminal hasn't opened it
   * by name yet. kitty_shm_track() is the backstop for the case where the
   * terminal never reads it at all -- see its own doc comment. */
  kitty_shm_track(name);

  char b64[96];
  size_t b64_len = ctui_util_base64_encode((const unsigned char *)name,
                                           strlen(name), b64, sizeof b64);

  char out[256];
  int n;
  if (use_compression) {
    n = snprintf(out, sizeof out,
                 "\x1b[%d;%dH\x1b_Ga=T,f=32,o=z,t=s,s=%d,v=%d,c=%d,r=%d,i=%u,"
                 "z=%d,q=2,C=1;%.*s\x1b\\",
                 row, col, width, height, cell_cols, cell_rows, image_id, z,
                 (int)b64_len, b64);
  } else {
    n = snprintf(out, sizeof out,
                 "\x1b[%d;%dH\x1b_Ga=T,f=32,t=s,s=%d,v=%d,c=%d,r=%d,i=%u,z=%d,"
                 "q=2,C=1;%.*s\x1b\\",
                 row, col, width, height, cell_cols, cell_rows, image_id, z,
                 (int)b64_len, b64);
  }
  kitty_batch_append(out, clamp_snprintf_len(n, sizeof out));

  ctui_logf(E_INF,
            "[CTUI:GFX] - kitty_display (t=s) @ tick %d (%dx%d px @ row=%d, "
            "col=%d, id=%u, z=%d, shm=%s, payload=%zu, compressed=%s)\n",
            ctui_tick_advance(), width, height, row, col, image_id, z, name,
            payload_len, use_compression ? "yes" : "no");
}

void ctui_gfx_kitty_display(int row, int col, int cell_cols, int cell_rows,
                            const unsigned char *rgba, int width, int height,
                            unsigned int image_id, int z) {
  if (!isatty(STDOUT_FILENO)) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_display rejected @ tick %d, stdout isn't "
              "a real terminal\n",
              ctui_tick_advance());
    return;
  }
  if (width <= 0 || height <= 0 || rgba == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_display rejected @ tick %d, degenerate "
              "image (%dx%d, rgba=%p)\n",
              ctui_tick_advance(), width, height, (const void *)rgba);
    return;
  }

  size_t raw_len = (size_t)width * (size_t)height * 4;

  /* Phase 5b: always attempt compression and compare -- never worse than
   * today, since a losing attempt (compressed_len >= raw_len, plausible
   * for small images like loudness_meter.c's baked-in text row given
   * DEFLATE's own block overhead) just falls through to the raw path
   * below unchanged. Phase 6: this same payload/use_compression choice
   * feeds either transport below -- o=z is a legal, orthogonal key on a
   * t=s control string too (see GFX_DESIGN.md's Phase 6), so there's no
   * separate compression decision to make per transport. */
  unsigned char *compressed = NULL;
  const unsigned char *payload = rgba;
  size_t payload_len = raw_len;
  int use_compression = 0;
  if (g_kitty_compression_enabled) {
    CTUI_PROFILE_SPAN compress_span = ctui_profile_begin();
    size_t compressed_len = 0;
    compressed = ctui_deflate_compress(rgba, raw_len, &compressed_len);
    ctui_profile_end(compress_span, "gfx.kitty_compress");
    if (compressed != NULL && compressed_len < raw_len) {
      payload = compressed;
      payload_len = compressed_len;
      use_compression = 1;
    }
  }

  if (g_kitty_shm_supported == 1) {
    kitty_display_shm(row, col, cell_cols, cell_rows, payload, payload_len,
                      width, height, image_id, z, use_compression);
  } else {
    kitty_display_td(row, col, cell_cols, cell_rows, payload, payload_len,
                     width, height, image_id, z, use_compression);
  }
  free(compressed);
}

void ctui_gfx_kitty_delete(unsigned int image_id) {
  if (!isatty(STDOUT_FILENO)) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_delete rejected @ tick %d, stdout isn't a "
              "real terminal\n",
              ctui_tick_advance());
    return;
  }

  char out[32];
  int n = snprintf(out, sizeof out, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", image_id);
  kitty_batch_append(out, (size_t)n);

  ctui_logf(E_INF, "[CTUI:GFX] - kitty_delete @ tick %d (id=%u)\n",
            ctui_tick_advance(), image_id);
}
