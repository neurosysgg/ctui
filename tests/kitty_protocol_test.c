/* Exercises the Kitty graphics protocol support headlessly: the generic
 * base64 encoder core/gfx.c's wire emission depends on, and Phase 4's
 * per-widget renderer declaration (widget.h's supported_gfx_modes /
 * ctui_widget_set_gfx_renderer(), core/app.c's ctui_app_init() hard-fail
 * check). The actual APC escape bytes ctui_gfx_kitty_display() writes to a
 * real terminal aren't covered here -- see docs/protocol.md's testing
 * checklist for how that was verified (a throwaway pty check, not
 * something that belongs in a headless suite). */
/* posix_openpt()/grantpt()/ptsname() for the probe's pty test */
#define _XOPEN_SOURCE 700

#include "ctui.h"

#include "ctui_test.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ctui_g_gfx_mode is intentionally private (core/ctui_internal.h, never
 * included by ctui.h -- see that header's own comment). Redeclaring the
 * same extern here is the same gray-box trick ctui_test.h itself relies
 * on (reading screen->cells directly): there's no ctui_init() in a
 * headless test to set it for real, so this is the only way to simulate
 * "CTUI_GFX_KITTY was negotiated" for ctui_app_init()'s validation path. */
extern unsigned int ctui_g_gfx_mode;

/* Phase 6: ctui_gfx_kitty_reply_is_ok() is a non-static helper in
 * core/gfx.c, deliberately not declared in gfx.h -- it's not something
 * an app/widget author should ever call, only factored out of
 * ctui_gfx_kitty_probe_shm() so its APC-reply parsing is unit-testable
 * without a real terminal (the probe itself needs a live pty replying
 * to a real escape sequence, well outside what this headless suite can
 * exercise -- see docs/protocol.md's testing checklist for why that's a
 * pty_harness.py/real-terminal concern instead). Same forward-declare-an-
 * intentionally-private-symbol trick as ctui_g_gfx_mode above. */
/* Phase 6 follow-up: same gray-box arrangement as reply_is_ok() below --
 * ctui_gfx_kitty_apc_span() is a non-static core/gfx.c helper kept out
 * of gfx.h, factored out of the probe so "which part of this read was
 * the terminal's reply, and which part was the user typing?" can be
 * tested without a live pty. */
extern int ctui_gfx_kitty_apc_span(const char *buf, size_t len,
                                   size_t *start, size_t *end);

extern int ctui_gfx_kitty_apc_complete(const char *buf, size_t len);

extern int ctui_gfx_kitty_reply_is_ok(const char *buf, size_t len,
                                      unsigned int image_id);

/* same gray-box arrangement: ctui_gfx_kitty_place_file()'s escape
 * formatter, so its wire format is checkable without a terminal */
extern size_t ctui_gfx_kitty_place_file_escape(char *out, size_t cap,
                                               unsigned int image_id,
                                               const char *path, int cols,
                                               int rows);

/* same again: CTUI_GFX_KITTY_IMAGE's t=s escape (with its frame name
 * linked), and switches no terminal can flip for a headless test */
extern size_t ctui_gfx_kitty_image_commit_escape(CTUI_GFX_KITTY_IMAGE *img,
                                                 int row, int col,
                                                 int cell_cols, int cell_rows,
                                                 int z, char *out, size_t cap);
extern void ctui_gfx_kitty_shm_set_supported(int on);
extern void ctui_gfx_kitty_image_set_stale_ms(int ms);

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

  /* ordinary text widget: passes regardless of ctui_g_gfx_mode, since text
   * rendering doesn't depend on what got negotiated */
  {
    ctui_g_gfx_mode = 0;
    CTUI_WIDGET w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
    CTUI_WIDGET *widgets[] = {&w};
    CTUI_APP app;
    CTUI_TEST_ASSERT(ctui_app_init(&app, widgets, 1, rows, cols) == 0,
                     "ctui_app_init() accepts a plain text widget under "
                     "ctui_g_gfx_mode=0");
    ctui_app_free(&app);
  }

  /* a widget requiring CTUI_GFX_KITTY when it wasn't negotiated: hard
   * fails, no text fallback to degrade to */
  {
    ctui_g_gfx_mode = CTUI_GFX_TRUECOLOR;
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
    ctui_g_gfx_mode = CTUI_GFX_KITTY;
    CTUI_WIDGET w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
    ctui_widget_set_gfx_renderer(&w, CTUI_GFX_KITTY, noop_render);
    CTUI_WIDGET *widgets[] = {&w};
    CTUI_APP app;
    CTUI_TEST_ASSERT(ctui_app_init(&app, widgets, 1, rows, cols) == 0,
                     "ctui_app_init() accepts a CTUI_GFX_KITTY widget once "
                     "CTUI_GFX_KITTY was actually negotiated");
    ctui_app_free(&app);
  }

  ctui_g_gfx_mode = 0;
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

  /* the id has to end where the needle does -- a plain prefix match
   * would read "i=10" as a hit for id 1 and accept another image's reply
   * as evidence about this one. */
  const char longer_id[] = "\x1b_Gi=10;OK\x1b\\";
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_reply_is_ok(longer_id, sizeof longer_id - 1, 1) == 0,
      "reply_is_ok() rejects id 10's reply when asked about id 1 (no "
      "prefix match)");
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_reply_is_ok(longer_id, sizeof longer_id - 1, 10) == 1,
      "reply_is_ok() still accepts that same reply when asked about id 10");

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

/* The probe reads STDIN raw, so whatever else landed in its ~250ms
 * window arrives glued to the reply. Everything outside the APC span is
 * real input that must survive (it used to be silently discarded, eating
 * any keystroke typed during startup), so these cases are really about
 * where the boundaries fall. */
static void test_kitty_apc_span(void) {
  size_t start = 99, end = 99;

  const char reply_only[] = "\x1b_Gi=1;OK\x1b\\";
  CTUI_TEST_ASSERT(ctui_gfx_kitty_apc_span(reply_only, sizeof reply_only - 1,
                                           &start, &end) == 1 &&
                       start == 0 && end == sizeof reply_only - 1,
                   "apc_span() spans exactly a lone reply, leaving nothing "
                   "to push back");

  const char trailing[] = "\x1b_Gi=1;OK\x1b\\q";
  CTUI_TEST_ASSERT(ctui_gfx_kitty_apc_span(trailing, sizeof trailing - 1,
                                           &start, &end) == 1 &&
                       start == 0 && end == sizeof trailing - 2,
                   "apc_span() leaves a keystroke typed *after* the reply "
                   "outside the span");

  const char leading[] = "q\x1b_Gi=1;OK\x1b\\";
  CTUI_TEST_ASSERT(ctui_gfx_kitty_apc_span(leading, sizeof leading - 1,
                                           &start, &end) == 1 &&
                       start == 1 && end == sizeof leading - 1,
                   "apc_span() leaves a keystroke typed *before* the reply "
                   "outside the span");

  const char both[] = "ab\x1b_Gi=1;OK\x1b\\cd";
  CTUI_TEST_ASSERT(ctui_gfx_kitty_apc_span(both, sizeof both - 1, &start,
                                           &end) == 1 &&
                       start == 2 && end == sizeof both - 3,
                   "apc_span() isolates the reply with input on both sides");

  /* the ordinary non-Kitty case: the terminal never answers, so every
   * byte read in that window belongs to the user. */
  const char no_apc[] = "hello";
  CTUI_TEST_ASSERT(ctui_gfx_kitty_apc_span(no_apc, sizeof no_apc - 1, &start,
                                           &end) == 0,
                   "apc_span() reports no span when there is no APC "
                   "introducer, so the whole buffer is foreign input");

  /* a half-arrived reply is still ours -- replaying part of an escape
   * sequence as keystrokes would be worse than dropping it. */
  const char truncated[] = "x\x1b_Gi=1;O";
  CTUI_TEST_ASSERT(ctui_gfx_kitty_apc_span(truncated, sizeof truncated - 1,
                                           &start, &end) == 1 &&
                       start == 1 && end == sizeof truncated - 1,
                   "apc_span() runs a terminator-less reply to end of "
                   "buffer rather than replaying a partial escape");

  CTUI_TEST_ASSERT(ctui_gfx_kitty_apc_span(NULL, 0, &start, &end) == 0,
                   "apc_span() rejects a NULL/empty buffer");
}

static void test_kitty_apc_complete(void) {
  static const char whole[] = "\x1b_Gi=1;OK\x1b\\";
  static const char typed[] = "a\x1b_Gi=1;OK\x1b\\b";
  CTUI_TEST_ASSERT(ctui_gfx_kitty_apc_complete(whole, sizeof whole - 1) &&
                       ctui_gfx_kitty_apc_complete(typed, sizeof typed - 1),
                   "apc_complete(): a terminated reply, with or without "
                   "input around it");
  CTUI_TEST_ASSERT(!ctui_gfx_kitty_apc_complete(whole, sizeof whole - 2) &&
                       !ctui_gfx_kitty_apc_complete(whole, 5) &&
                       !ctui_gfx_kitty_apc_complete("abc", 3) &&
                       !ctui_gfx_kitty_apc_complete(NULL, 0),
                   "apc_complete(): not yet -- half a terminator, half a "
                   "reply, no reply");
}

/* the probe against a pty: the parent plays the terminal and answers
 * (reply: NULL = never, else in two writes gap_ms apart); the child runs
 * ctui_gfx_kitty_probe_shm() on the pty and exits with how long it took,
 * in 10 ms units */
static int probe_ms(const char *reply, int gap_ms) {
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    int slave = open(ptsname(master), O_RDWR);
    struct termios t;
    tcgetattr(slave, &t);
    t.c_lflag &= ~(unsigned)(ECHO | ICANON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(slave, TCSANOW, &t);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    ctui_gfx_kitty_probe_shm();
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    _exit((int)(ms / 10 > 250 ? 250 : ms / 10));
  }
  /* the query first, up to its ESC \ */
  char buf[256];
  size_t got = 0;
  while (got < sizeof buf) {
    ssize_t r = read(master, buf + got, sizeof buf - got);
    if (r <= 0) {
      break;
    }
    got += (size_t)r;
    if (got >= 2 && buf[got - 2] == '\x1b' && buf[got - 1] == '\\') {
      break;
    }
  }
  if (reply) {
    size_t half = strlen(reply) / 2;
    ssize_t w = write(master, reply, half);
    struct timespec gap = {0, gap_ms * 1000000L};
    nanosleep(&gap, NULL);
    w = write(master, reply + half, strlen(reply) - half);
    (void)w;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  close(master);
  return WIFEXITED(status) ? WEXITSTATUS(status) * 10 : -1;
}

static void test_kitty_probe_timing(void) {
  int ms = probe_ms("\x1b_Gi=1;OK\x1b\\", 0);
  CTUI_TEST_ASSERT(ms >= 0 && ms < 100,
                   "the probe returns once kitty's reply is in, not after "
                   "its 250 ms timeout (%d ms)",
                   ms);
  ms = probe_ms("\x1b_Gi=1;OK\x1b\\", 30);
  CTUI_TEST_ASSERT(ms >= 20 && ms < 150,
                   "a reply in two pieces: it waits for the second (%d ms)",
                   ms);
  ms = probe_ms(NULL, 0);
  CTUI_TEST_ASSERT(ms >= 200,
                   "no reply (not kitty): it still waits the timeout out "
                   "(%d ms)",
                   ms);
}

static void test_place_file_escape(void) {
  char out[256];
  size_t n = ctui_gfx_kitty_place_file_escape(out, sizeof out, 7, "/i.png", 2,
                                              1);
  const char *want = "\x1b_Ga=T,t=f,f=100,i=7,q=2,c=2,r=1,U=1;L2kucG5n\x1b\\";
  CTUI_TEST_ASSERT(n == strlen(want) && memcmp(out, want, n) == 0,
                   "place_file: transmit + virtual placement (U=1) of a PNG "
                   "by path (f=100,t=f), the path base64'd");
  CTUI_TEST_ASSERT(
      ctui_gfx_kitty_place_file_escape(out, sizeof out, 0, "/i.png", 2, 1) ==
              0 &&
          ctui_gfx_kitty_place_file_escape(out, sizeof out, 0x1000000,
                                           "/i.png", 2, 1) == 0 &&
          ctui_gfx_kitty_place_file_escape(out, sizeof out, 7, "", 2, 1) ==
              0 &&
          ctui_gfx_kitty_place_file_escape(out, sizeof out, 7, "/i.png", 0,
                                           1) == 0 &&
          ctui_gfx_kitty_place_file_escape(out, 20, 7, "/i.png", 2, 1) == 0,
      "place_file refuses id 0, an id past 24 bits (placeholders carry it "
      "in an RGB fg), an empty path, no cells, a buffer too small");
}

/* the shm name an image escape carries (its base64 payload) */
static int escape_name(const char *esc, size_t n, char *name, size_t cap) {
  const char *keys = memchr(esc, '_', n);
  const char *semi = keys ? memchr(keys, ';', n - (size_t)(keys - esc)) : NULL;
  if (semi == NULL || n < 2) {
    return 0;
  }
  const char *b64 = semi + 1;
  size_t len = (size_t)(esc + n - 2 - b64);
  static const char tab[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  unsigned acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len && b64[i] != '='; i++) {
    const char *c = strchr(tab, b64[i]);
    if (c == NULL) {
      return 0;
    }
    acc = (acc << 6) | (unsigned)(c - tab);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o + 1 >= cap) {
        return 0;
      }
      name[o++] = (char)((acc >> bits) & 0xFF);
    }
  }
  name[o] = '\0';
  return 1;
}

/* what kitty does with a t=s name: read it all, then shm_unlink it; the
 * first byte read, or -1 */
static int terminal_reads(const char *name, size_t len) {
  int fd = shm_open(name, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }
  unsigned char *buf = malloc(len);
  ssize_t got = buf ? pread(fd, buf, len, 0) : -1;
  int first = got == (ssize_t)len ? buf[0] : -1;
  free(buf);
  close(fd);
  shm_unlink(name);
  return first;
}

static void test_kitty_image(void) {
  ctui_gfx_kitty_shm_set_supported(1);
  CTUI_GFX_KITTY_IMAGE *img = ctui_gfx_kitty_image_new(9);
  const int w = 4, h = 2;
  const size_t len = (size_t)w * h * 4;
  char esc[256], name[3][64];

  unsigned char *a = ctui_gfx_kitty_image_begin(img, w, h, 0);
  CTUI_TEST_ASSERT(a != NULL && ((uintptr_t)a & 63) == 0,
                   "image: begin gives a buffer, aligned");
  memset(a, 0x11, len);
  size_t n = ctui_gfx_kitty_image_commit_escape(img, 3, 5, 2, 1, 0, esc,
                                                sizeof esc);
  CTUI_TEST_ASSERT(n > 0 && memcmp(esc,
                                   "\x1b[3;5H\x1b_Ga=T,t=s,f=32,i=9,q=2,"
                                   "v=2,s=4,S=32,c=2,r=1,C=1;",
                                   45) == 0,
                   "image: commit escape = CUP + raw t=s (no o=z), the size "
                   "in S");
  CTUI_TEST_ASSERT(escape_name(esc, n, name[0], sizeof name[0]) &&
                       name[0][0] == '/',
                   "image: the escape carries a shm name");

  /* the terminal hasn't read frame 0: the next begin is another buffer,
   * holding frame 0 (keep) */
  unsigned char *b = ctui_gfx_kitty_image_begin(img, w, h, 1);
  CTUI_TEST_ASSERT(b != NULL && b != a && b[0] == 0x11 && b[len - 1] == 0x11,
                   "image: an unread buffer isn't handed out; keep copies "
                   "the last frame into the next one");
  memset(b, 0x22, len);
  n = ctui_gfx_kitty_image_commit_escape(img, 1, 1, 2, 1, 0, esc, sizeof esc);
  escape_name(esc, n, name[1], sizeof name[1]);
  unsigned char *c = ctui_gfx_kitty_image_begin(img, w, h, 0);
  CTUI_TEST_ASSERT(c != NULL && c != a && c != b, "image: a third buffer");
  memset(c, 0x33, len);
  n = ctui_gfx_kitty_image_commit_escape(img, 1, 1, 2, 1, 0, esc, sizeof esc);
  escape_name(esc, n, name[2], sizeof name[2]);
  CTUI_TEST_ASSERT(ctui_gfx_kitty_image_begin(img, w, h, 0) == NULL,
                   "image: every buffer unread -> NULL (skip the frame)");

  CTUI_TEST_ASSERT(terminal_reads(name[0], len) == 0x11 &&
                       terminal_reads(name[1], len) == 0x22,
                   "image: the terminal reads each frame's pixels by name");
  unsigned char *d = ctui_gfx_kitty_image_begin(img, w, h, 1);
  CTUI_TEST_ASSERT((d == a || d == b) && d[0] == 0x33,
                   "image: a buffer read (name unlinked) is free again; keep "
                   "copies the last frame (still unread) into it");
  ctui_gfx_kitty_image_commit_escape(img, 1, 1, 2, 1, 0, esc, sizeof esc);
  CTUI_TEST_ASSERT(terminal_reads(name[2], len) == 0x33,
                   "image: an unread frame's pixels stay as committed");

  /* the last frame's own buffer, once read, comes back without a copy */
  char last[64];
  escape_name(esc, strlen(esc), last, sizeof last);
  terminal_reads(last, len);
  unsigned char *e = ctui_gfx_kitty_image_begin(img, w, h, 1);
  CTUI_TEST_ASSERT(e == d, "image: keep reuses the last frame's own buffer "
                           "once the terminal has read it");

  /* a terminal that never reads: the buffers come back after the stale
   * time, their frame names removed */
  ctui_gfx_kitty_image_commit_escape(img, 1, 1, 2, 1, 0, esc, sizeof esc);
  char lost[64];
  escape_name(esc, strlen(esc), lost, sizeof lost);
  ctui_gfx_kitty_image_set_stale_ms(0);
  unsigned char *f = ctui_gfx_kitty_image_begin(img, w, h, 0);
  int gone = shm_open(lost, O_RDONLY, 0);
  CTUI_TEST_ASSERT(f == e && gone < 0,
                   "image: an unread buffer past the stale time is taken "
                   "back and its frame name unlinked");
  if (gone >= 0) {
    close(gone);
  }
  ctui_gfx_kitty_image_set_stale_ms(CTUI_GFX_KITTY_IMAGE_STALE_MS);

  /* a size change: a bigger mapping */
  unsigned char *g = ctui_gfx_kitty_image_begin(img, 64, 64, 1);
  CTUI_TEST_ASSERT(g != NULL, "image: a bigger size maps a bigger buffer");
  if (g) {
    memset(g, 0x44, 64 * 64 * 4);
  }
  ctui_gfx_kitty_image_free(img);

  /* no t=s: one heap buffer, never busy */
  ctui_gfx_kitty_shm_set_supported(0);
  img = ctui_gfx_kitty_image_new(10);
  unsigned char *p = ctui_gfx_kitty_image_begin(img, w, h, 0);
  CTUI_TEST_ASSERT(p != NULL && ((uintptr_t)p & 63) == 0 &&
                       ctui_gfx_kitty_image_begin(img, w, h, 1) == p,
                   "image: without t=s one heap buffer, handed out again");
  ctui_gfx_kitty_image_free(img);
  ctui_gfx_kitty_shm_set_supported(-1);
}

int main(void) {
  ctui_log_init(E_ALL);

  test_base64();
  test_widget_defaults();
  test_app_init_validation();
  test_kitty_shm_reply_parsing();
  test_kitty_apc_span();
  test_kitty_apc_complete();
  test_kitty_probe_timing();
  test_place_file_escape();
  test_kitty_image();

  return ctui_test_summary();
}
