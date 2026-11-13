/* The link (src/link/link.h): an app's end and a host's end in one
 * process over a socketpair, the host's sinks a test's -- the first grid,
 * frames as the runs that changed (one in flight: a change made before
 * the ack goes with the next), input both ways round, a new grid as a
 * resize, a frame of an old grid dropped and acked, clusters by their
 * codepoints, image calls with their pixels, default colours, the app's
 * own messages, a broken message refused, the host going ending the
 * app's loop. */
#define _GNU_SOURCE

#include "ctui.h"
#include "ctui_test.h"
#include "link/link.h"
#include "link/wire.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- the host's sinks: what reached them --- */

static CTUI_CELL got[64 * 16];
static int got_rows, got_cols, frames;

static int t_frame(void *ctx, const CTUI_SCREEN *s) {
  (void)ctx;
  got_rows = s->rows;
  got_cols = s->cols;
  memcpy(got, s->cells, (size_t)s->rows * s->cols * sizeof *got);
  frames++;
  return 0;
}
static const CTUI_SCREEN_SINK k_tsink = {.frame = t_frame};

static unsigned rgba_id, rgba_w, rgba_h, rgba_first, deleted, flushes;
static unsigned char committed[16];
static void t_display(void *ctx, int row, int col, int cc, int cr,
                      const unsigned char *rgba, int w, int h, unsigned id,
                      int z) {
  (void)ctx, (void)row, (void)col, (void)cc, (void)cr, (void)rgba, (void)w;
  (void)h, (void)id, (void)z;
}
static unsigned char t_img[16];
static unsigned char *t_begin(void *ctx, unsigned id, int w, int h, int keep) {
  (void)ctx, (void)id, (void)keep;
  return w * h * 4 <= (int)sizeof t_img ? t_img : NULL;
}
static void t_commit(void *ctx, unsigned id, int row, int col, int cc, int cr,
                     int z) {
  (void)ctx, (void)id, (void)row, (void)col, (void)cc, (void)cr, (void)z;
  memcpy(committed, t_img, sizeof committed);
}
static void t_free(void *ctx, unsigned id) { (void)ctx, (void)id; }
static void t_delete(void *ctx, unsigned id) {
  (void)ctx;
  deleted = id;
}
static unsigned files, file_id, n_puts, put_id, put_pid, unputs;
static void t_file(void *ctx, unsigned id, const char *p, int c, int r) {
  (void)ctx, (void)p, (void)c, (void)r;
  files++;
  file_id = id;
}
static void t_rgba(void *ctx, unsigned id, const unsigned char *rgba, int w,
                   int h, int c, int r) {
  (void)ctx, (void)c, (void)r;
  rgba_id = id;
  rgba_w = (unsigned)w;
  rgba_h = (unsigned)h;
  memcpy(&rgba_first, rgba, 4);
}
static void t_px(void *ctx, int row, int col, int cc, int cr, int x, int y,
                 int w, int h, const unsigned char *rgba, int width, int height,
                 unsigned id, int z) {
  (void)ctx, (void)row, (void)col, (void)cc, (void)cr, (void)x, (void)y;
  (void)w, (void)h, (void)rgba, (void)width, (void)height, (void)id, (void)z;
}
static void t_wipe(void *ctx, unsigned id, unsigned l, unsigned m, double x,
                   double v, double e) {
  (void)ctx, (void)id, (void)l, (void)m, (void)x, (void)v, (void)e;
}
static void t_put(void *ctx, unsigned id, unsigned pid, int row, int col, int c,
                  int r, int z) {
  (void)ctx, (void)row, (void)col, (void)c, (void)r, (void)z;
  n_puts++;
  put_id = id;
  put_pid = pid;
}
static void t_unput(void *ctx, unsigned id, unsigned pid) {
  (void)ctx, (void)id, (void)pid;
  unputs++;
}
static void t_flush(void *ctx) {
  (void)ctx;
  flushes++;
}
static const CTUI_GFX_SINK k_tgfx = {
    .display = t_display,
    .image_begin = t_begin,
    .image_commit = t_commit,
    .image_free = t_free,
    .delete = t_delete,
    .place_file = t_file,
    .place_rgba = t_rgba,
    .display_px = t_px,
    .wipe = t_wipe,
    .put = t_put,
    .unput = t_unput,
    .flush = t_flush,
};

static uint32_t def_fg, def_bg;
static char app_msg[64];
static void t_default(void *ctx, uint32_t fg, uint32_t bg) {
  (void)ctx;
  def_fg = fg;
  def_bg = bg;
}
static void t_app(void *ctx, const void *msg, size_t n) {
  (void)ctx;
  snprintf(app_msg, sizeof app_msg, "%.*s", (int)n, (const char *)msg);
}

static char shape[32];
static int shapes;
static void t_pointer(void *ctx, const char *name) {
  (void)ctx;
  snprintf(shape, sizeof shape, "%s", name);
  shapes++;
}

/* drag and drop: what each end last heard */
static CTUI_LINK_DND h_dnd, a_dnd;
static char h_mimes[128], a_mimes[128], a_data[32];
static int h_dnds, a_dnds;
static void t_dnd(void *ctx, const CTUI_LINK_DND *m, const char *mimes) {
  (void)ctx;
  h_dnd = *m;
  snprintf(h_mimes, sizeof h_mimes, "%s", mimes);
  h_dnds++;
}
static void a_dnd_fn(void *ctx, const CTUI_LINK_DND *m, const char *mimes,
                     int fd) {
  (void)ctx;
  a_dnd = *m;
  snprintf(a_mimes, sizeof a_mimes, "%s", mimes);
  a_dnds++;
  a_data[0] = '\0';
  if (fd >= 0) {
    ssize_t n = read(fd, a_data, sizeof a_data - 1);
    a_data[n > 0 ? n : 0] = '\0';
    close(fd);
  }
}

static char app_got[64];
static void a_app(void *ctx, const void *msg, size_t n) {
  (void)ctx;
  snprintf(app_got, sizeof app_got, "%.*s", (int)n, (const char *)msg);
}

static int got_text(int row, const char *s) {
  for (int i = 0; s[i]; i++) {
    if (got[row * got_cols + i].ch != (uint32_t)(unsigned char)s[i]) {
      return 0;
    }
  }
  return 1;
}

/* the app's loop once: what the host sent taken, the next event (a tick
 * when none) */
static CTUI_EVENTTYPE app_step(CTUI_EVENT *ev) {
  memset(ev, 0, sizeof *ev);
  if (!ctui_input_loop(ev, 20)) {
    return CTUI_DUMMY_EVENT; /* the loop ended */
  }
  return ev->type;
}

int main(void) {
  ctui_log_init(E_ALL);
  int sv[2];
  CTUI_TEST_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) ==
                       0,
                   "a socketpair");
  CTUI_LINK_HOST_SINKS sinks = {.screen = &k_tsink,
                                .gfx = &k_tgfx,
                                .set_default = t_default,
                                .app = t_app,
                                .pointer = t_pointer,
                                .dnd = t_dnd};
  CTUI_LINK_HOST *h = ctui_link_host_new(sv[1], &sinks);
  CTUI_TEST_ASSERT(h && ctui_link_host_fd(h) == sv[1], "the host's end");
  CTUI_TEST_ASSERT(ctui_link_host_grid(h, 4, 20, 9, 18) == 0,
                   "a first grid sent, before the app is there");

  char err[128] = "";
  int rows = 0, cols = 0;
  CTUI_LINK *l = ctui_link_open(sv[0], CTUI_GFX_TRUECOLOR | CTUI_GFX_KITTY,
                                &rows, &cols, err, sizeof err);
  CTUI_TEST_ASSERT(l && rows == 4 && cols == 20,
                   "the app's end: the host's grid, %dx%d (%s)", cols, rows,
                   err);
  int cw = 0, ch = 0;
  ctui_cell_px(&cw, &ch);
  CTUI_TEST_ASSERT(cw == 9 && ch == 18, "its cells' pixels told: %dx%d", cw,
                   ch);
  ctui_link_on_app(l, a_app, NULL);
  CTUI_SCREEN *s = ctui_screen_create(rows, cols);
  ctui_link_attach(l, s);

  ctui_screen_puts(s, 0, 0, "hello", CTUI_COLOR_WHITE, CTUI_COLOR_BLUE);
  ctui_screen_flush(s);
  CTUI_TEST_ASSERT(ctui_link_host_read(h) == 0 && frames == 1 &&
                       got_text(0, "hello") && got[0].bg == CTUI_COLOR_BLUE,
                   "a frame: the host's screen has the app's cells");

  /* before the ack is read: the change waits, then goes as one frame */
  ctui_screen_puts(s, 1, 0, "one", 7, 0);
  ctui_screen_flush(s);
  ctui_screen_puts(s, 2, 0, "two", 7, 0);
  ctui_screen_flush(s);
  CTUI_TEST_ASSERT(ctui_link_host_read(h) == 0 && frames == 1,
                   "one frame in flight: nothing more until the ack");
  CTUI_EVENT ev;
  CTUI_EVENTTYPE t = app_step(&ev);
  CTUI_TEST_ASSERT(t == CTUI_TICK_EVENT, "the ack taken, no event for it");
  CTUI_TEST_ASSERT(ctui_link_host_read(h) == 0 && frames == 2 &&
                       got_text(1, "one") && got_text(2, "two") &&
                       got_text(0, "hello"),
                   "both changes in the next frame, the rest kept");

  CTUI_KEYPRESS_EVENT_DATA kd = {.type = CTUI_KEY_CHAR, .ch = 'q'};
  CTUI_EVENT kev = {.type = CTUI_KEYPRESS_EVENT, .event_data = &kd};
  CTUI_TEST_ASSERT(ctui_link_host_event(h, &kev) == 0, "a key sent");
  t = app_step(&ev);
  CTUI_TEST_ASSERT(t == CTUI_KEYPRESS_EVENT &&
                       ((CTUI_KEYPRESS_EVENT_DATA *)ev.event_data)->ch == 'q' &&
                       !strcmp(ev.ev_source, "input"),
                   "the app's loop hands it out as its input");
  CTUI_MOUSE_EVENT_DATA md = {.action = CTUI_MOUSE_PRESS,
                              .button = 0,
                              .row = 1,
                              .col = 3,
                              .px = 30,
                              .py = 20};
  CTUI_EVENT mev = {.type = CTUI_MOUSE_EVENT, .event_data = &md};
  ctui_link_host_event(h, &mev);
  t = app_step(&ev);
  CTUI_MOUSE_EVENT_DATA *mg = ev.event_data;
  CTUI_TEST_ASSERT(t == CTUI_MOUSE_EVENT && mg->col == 3 && mg->px == 30,
                   "a click, with its pixels");
  CTUI_EVENT bad = {.type = CTUI_TICK_EVENT};
  CTUI_TEST_ASSERT(ctui_link_host_event(h, &bad) == -1,
                   "a tick isn't input: refused");

  /* a new grid: a resize for the app, its next frame all of it */
  CTUI_TEST_ASSERT(ctui_link_host_grid(h, 6, 30, 9, 18) == 0, "a new grid");
  t = app_step(&ev);
  CTUI_RESIZE_EVENT_DATA *rd = ev.event_data;
  CTUI_TEST_ASSERT(t == CTUI_RESIZE_EVENT && rd->rows == 6 && rd->cols == 30,
                   "the app gets it as a resize: %dx%d", rd->cols, rd->rows);
  ctui_screen_resize(s, 6, 30);
  ctui_screen_puts(s, 5, 0, "bottom", 7, 0);
  ctui_screen_flush(s);
  ctui_link_host_read(h);
  CTUI_TEST_ASSERT(got_rows == 6 && got_cols == 30 && got_text(5, "bottom"),
                   "the host's screen at the new size: %dx%d", got_cols,
                   got_rows);
  app_step(&ev); /* the ack */

  /* clusters go by their codepoints */
  ctui_screen_puts(s, 3, 0, "e\xcc\x81!", 7, 0);
  ctui_screen_flush(s);
  ctui_link_host_read(h);
  int nc = 0;
  const uint32_t *cps = ctui_cell_cluster(got[3 * 30].ch, &nc);
  CTUI_TEST_ASSERT(cps && nc == 2 && cps[0] == 'e' && cps[1] == 0x301 &&
                       got[3 * 30 + 1].ch == '!',
                   "a cluster's cell arrives as that cluster");
  app_step(&ev);

  /* images, colours, messages */
  unsigned char px[2 * 1 * 4] = {10, 20, 30, 255, 40, 50, 60, 255};
  CTUI_TEST_ASSERT(ctui_gfx_place_rgba(77, px, 2, 1, 3, 1) == 0,
                   "an image placed through the app's graphics sink");
  ctui_gfx_kitty_delete(78);
  CTUI_GFX_KITTY_IMAGE *img = ctui_gfx_kitty_image_new(79);
  unsigned char *paint = img ? ctui_gfx_kitty_image_begin(img, 2, 2, 0) : NULL;
  if (paint) {
    for (int i = 0; i < 16; i++) {
      paint[i] = (unsigned char)(i + 1);
    }
    ctui_gfx_kitty_image_commit(img, 0, 0, 1, 1, 0);
  }
  ctui_link_set_default(l, 0xc0c0c0, 0x000080);
  ctui_link_app(l, "size 5", 6);
  /* no terminal: the shapes go over the link, each change once */
  ctui_pointer_shape("pointer");
  ctui_pointer_shape("pointer");
  ctui_pointer_shape("ew-resize");
  ctui_link_host_read(h);
  CTUI_TEST_ASSERT(shapes == 2 && !strcmp(shape, "ew-resize"),
                   "the pointer's shapes to the host, a change each (%d, %s)",
                   shapes, shape);
  CTUI_TEST_ASSERT(rgba_id == 77 && rgba_w == 2 && rgba_h == 1 &&
                       !memcmp(&rgba_first, px, 4),
                   "the host's graphics sink has it, its pixels too");
  CTUI_TEST_ASSERT(deleted == 78, "a delete goes as it is");
  CTUI_TEST_ASSERT(paint && committed[0] == 1 && committed[15] == 16,
                   "an image painted in the app, committed with its pixels");
  CTUI_TEST_ASSERT(def_fg == 0xc0c0c0 && def_bg == 0x000080,
                   "the default colours");
  CTUI_TEST_ASSERT(!strcmp(app_msg, "size 5"), "the app's message: %s",
                   app_msg);
  ctui_gfx_kitty_image_free(img);
  ctui_link_host_app(h, "close", 5);
  app_step(&ev);
  CTUI_TEST_ASSERT(!strcmp(app_got, "close"), "the host's message: %s",
                   app_got);

  /* drag and drop: nothing to a host that hasn't said it does it */
  ctui_link_on_dnd(l, a_dnd_fn, NULL);
  CTUI_LINK_DND acc = {.kind = CTUI_LINK_DND_ACCEPT, .action = 1};
  CTUI_TEST_ASSERT(!ctui_link_dnd_can(l) &&
                       ctui_link_dnd(l, &acc, "text/uri-list") == -1,
                   "no DND_CAN yet: the app's drag and drop not sent");
  CTUI_LINK_DND can = {.kind = CTUI_LINK_DND_CAN};
  ctui_link_host_dnd(h, &can, NULL, -1);
  CTUI_LINK_DND enter = {.kind = CTUI_LINK_DND_ENTER,
                         .actions = 3,
                         .row = 2,
                         .col = 5,
                         .px = 47,
                         .py = 40};
  ctui_link_host_dnd(h, &enter, "text/uri-list text/plain", -1);
  app_step(&ev);
  CTUI_TEST_ASSERT(ctui_link_dnd_can(l) && a_dnds == 2 &&
                       a_dnd.kind == CTUI_LINK_DND_ENTER && a_dnd.col == 5 &&
                       a_dnd.px == 47 &&
                       !strcmp(a_mimes, "text/uri-list text/plain"),
                   "the host can: a drag's enter with its types (%s)", a_mimes);
  CTUI_TEST_ASSERT(ctui_link_dnd(l, &acc, "text/uri-list") == 0 &&
                       ctui_link_host_read(h) == 0 && h_dnds == 1 &&
                       h_dnd.kind == CTUI_LINK_DND_ACCEPT &&
                       h_dnd.action == 1 && !strcmp(h_mimes, "text/uri-list"),
                   "the app's accept to the host's sink");
  int pp[2];
  CTUI_TEST_ASSERT(pipe(pp) == 0 && write(pp[1], "file:///a\r\n", 11) == 11,
                   "a pipe for the data");
  close(pp[1]);
  CTUI_LINK_DND data = {.kind = CTUI_LINK_DND_DATA, .tag = 7};
  ctui_link_host_dnd(h, &data, "text/uri-list", pp[0]);
  close(pp[0]);
  app_step(&ev);
  CTUI_TEST_ASSERT(a_dnd.kind == CTUI_LINK_DND_DATA && a_dnd.tag == 7 &&
                       !strcmp(a_data, "file:///a\r\n"),
                   "the dropped data's fd handed to the app (%s)", a_data);
  char longer[CTUI_LINK_DND_MIMES + 8];
  memset(longer, 'x', sizeof longer - 1);
  longer[sizeof longer - 1] = '\0';
  CTUI_TEST_ASSERT(ctui_link_dnd(l, &acc, longer) == -1,
                   "types past the limit: not sent");
  /* a broken one (its types unterminated) breaks the link; a type the host
   * doesn't know is skipped */
  socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pp);
  CTUI_LINK_HOST *h3 = ctui_link_host_new(pp[1], &sinks);
  CTUI_LINK_M_HELLO hi = {CTUI_LINK_MAGIC, CTUI_LINK_VERSION,
                          sizeof(CTUI_CELL)};
  ctui_link_send(pp[0], CTUI_LINK_HELLO, &hi, sizeof hi, NULL, 0, -1);
  ctui_link_send(pp[0], 63, "?", 1, NULL, 0, -1);
  int skipped = ctui_link_host_read(h3) == 0;
  ctui_link_send(pp[0], CTUI_LINK_DND_ASK, &acc, sizeof acc, "ab", 2, -1);
  CTUI_TEST_ASSERT(skipped && ctui_link_host_read(h3) == -1,
                   "an unknown type skipped, a DND without its NUL refused");
  ctui_link_host_free(h3);
  close(pp[0]);

  /* a frame of an older grid: dropped, acked */
  int fr = frames;
  CTUI_LINK_M_FRAME old = {.gen = 1, .runs = 0};
  int raw[2];
  socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, raw);
  CTUI_LINK_HOST *h2 = ctui_link_host_new(raw[1], &sinks);
  ctui_link_host_grid(h2, 2, 2, 8, 16);
  ctui_link_host_grid(h2, 3, 3, 8, 16);
  CTUI_LINK_M_HELLO hello = {CTUI_LINK_MAGIC, CTUI_LINK_VERSION,
                             sizeof(CTUI_CELL)};
  ctui_link_send(raw[0], CTUI_LINK_HELLO, &hello, sizeof hello, NULL, 0, -1);
  ctui_link_send(raw[0], CTUI_LINK_FRAME, &old, sizeof old, NULL, 0, -1);
  CTUI_TEST_ASSERT(ctui_link_host_read(h2) == 0 && frames == fr,
                   "an old grid's frame: not applied");
  char buf[CTUI_LINK_MAX];
  int fd = -1, acked = 0;
  for (long n; (n = ctui_link_recv(raw[0], buf, &fd)) > 0;) {
    if (fd >= 0) {
      close(fd);
    }
    acked |= ((CTUI_LINK_HDR *)buf)->type == CTUI_LINK_ACK;
  }
  CTUI_TEST_ASSERT(acked, "acked all the same (the app's one in flight)");
  CTUI_LINK_RUN wide = {.row = 0, .col = 1, .n = 5};
  CTUI_LINK_M_FRAME f = {.gen = 2, .runs = 1};
  ctui_link_send(raw[0], CTUI_LINK_FRAME, &f, sizeof f, &wide, sizeof wide, -1);
  CTUI_TEST_ASSERT(ctui_link_host_read(h2) == -1,
                   "a run past the grid's edge: refused");
  ctui_link_host_free(h2);
  close(raw[0]);

  socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, raw);
  h2 = ctui_link_host_new(raw[1], &sinks);
  CTUI_LINK_M_HELLO other = {CTUI_LINK_MAGIC, CTUI_LINK_VERSION, 3};
  ctui_link_send(raw[0], CTUI_LINK_HELLO, &other, sizeof other, NULL, 0, -1);
  CTUI_TEST_ASSERT(ctui_link_host_read(h2) == -1,
                   "an app of another build (its cells' size): refused");
  ctui_link_host_free(h2);
  close(raw[0]);

  /* a pointer shape that isn't a CSS name: refused */
  static const char *const bad_shape[] = {"Pointer", "", "a;b",
                                          "pointer-pointer-pointer-pointer-"
                                          "x"};
  int refused = 0;
  for (int i = 0; i < 4; i++) {
    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, raw);
    h2 = ctui_link_host_new(raw[1], &sinks);
    ctui_link_send(raw[0], CTUI_LINK_HELLO, &hello, sizeof hello, NULL, 0, -1);
    ctui_link_send(raw[0], CTUI_LINK_POINTER, bad_shape[i],
                   strlen(bad_shape[i]) + 1, NULL, 0, -1);
    refused += ctui_link_host_read(h2) == -1;
    ctui_link_host_free(h2);
    close(raw[0]);
  }
  socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, raw);
  h2 = ctui_link_host_new(raw[1], &sinks);
  ctui_link_send(raw[0], CTUI_LINK_HELLO, &hello, sizeof hello, NULL, 0, -1);
  ctui_link_send(raw[0], CTUI_LINK_POINTER, "grab", 4, NULL, 0, -1);
  refused += ctui_link_host_read(h2) == -1; /* no NUL */
  ctui_link_host_free(h2);
  close(raw[0]);
  CTUI_TEST_ASSERT(refused == 5,
                   "shapes not named as CSS names, unterminated or too long: "
                   "refused (%d of 5)",
                   refused);

  /* a host that lost it all (re-exec'd, its socket handed on): the app
   * sends it all again, its next frame the whole screen */
  ctui_gfx_kitty_place_file(90, "/tmp/x.png", 2, 1);
  ctui_gfx_kitty_place_file(91, "/tmp/gone.png", 2, 1);
  ctui_gfx_kitty_delete(91);
  ctui_gfx_kitty_put(77, 5, 0, 0, 1, 1, -1);
  ctui_gfx_kitty_put(77, 6, 0, 0, 1, 1, -1);
  ctui_gfx_kitty_unput(77, 6);
  ctui_pointer_shape("grab");
  ctui_link_host_read(h);
  /* no sinks for now: frames acked into nothing */
  ctui_link_host_set_sinks(h, NULL);
  fr = frames;
  ctui_screen_puts(s, 4, 0, "away", 7, 0);
  ctui_screen_flush(s);
  app_step(&ev);
  CTUI_TEST_ASSERT(ctui_link_host_read(h) == 0 && frames == fr,
                   "without sinks: a frame taken, nothing drawn");
  app_step(&ev); /* its ack */
  int kept = dup(ctui_link_host_fd(h));
  ctui_link_host_free(h);
  rgba_id = files = n_puts = unputs = 0;
  def_fg = def_bg = 0;
  shape[0] = '\0';
  h = ctui_link_host_adopt(kept, &sinks);
  CTUI_TEST_ASSERT(h && ctui_link_host_reset(h) == 0 &&
                       ctui_link_host_grid(h, 6, 30, 9, 18) == 0,
                   "a new host on the same socket: reset, a grid");
  t = app_step(&ev); /* the reset taken on the way */
  CTUI_TEST_ASSERT(t == CTUI_RESIZE_EVENT, "the grid: a resize");
  CTUI_TEST_ASSERT(!ctui_link_dnd_can(l),
                   "after a reset the new host says again if it does dnd");
  memset(got, 0, sizeof got);
  ctui_screen_flush(s); /* nothing changed in the app */
  CTUI_TEST_ASSERT(ctui_link_host_read(h) == 0 && got_rows == 6 &&
                       got_text(5, "bottom") && got_text(4, "away"),
                   "the whole screen in its next frame, what changed while "
                   "away too");
  cps = ctui_cell_cluster(got[3 * 30].ch, &nc);
  CTUI_TEST_ASSERT(cps && nc == 2 && cps[1] == 0x301,
                   "its clusters told again");
  CTUI_TEST_ASSERT(rgba_id == 77 && !memcmp(&rgba_first, px, 4) && files == 1 &&
                       file_id == 90 && n_puts == 1 && put_id == 77 &&
                       put_pid == 5 && unputs == 0,
                   "the live images again (rgba %u, files %u, puts %u): "
                   "not the deleted, not the unput",
                   rgba_id, files, n_puts);
  CTUI_TEST_ASSERT(def_fg == 0xc0c0c0 && def_bg == 0x000080 &&
                       !strcmp(shape, "grab"),
                   "the default colours and the pointer again (%s)", shape);
  app_step(&ev);
  ctui_screen_puts(s, 4, 0, "back", 7, 0);
  ctui_screen_flush(s);
  CTUI_TEST_ASSERT(ctui_link_host_read(h) == 0 && got_text(4, "back"),
                   "and frames go on as before");

  /* the host going ends the app's loop */
  ctui_link_host_free(h);
  t = app_step(&ev);
  CTUI_TEST_ASSERT(t == CTUI_DUMMY_EVENT, "the host gone: the loop ends");
  ctui_link_close(l);
  ctui_screen_free(s);
  return ctui_test_summary();
}
