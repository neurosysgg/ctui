#define _GNU_SOURCE
#include "link.h"
#include "wire.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* an image being painted (image_begin()'s buffer), kept between frames:
 * a keep needs the last committed pixels */
typedef struct {
  uint32_t id;
  int w, h;
  unsigned char *px;
} PIX;

struct CTUI_LINK {
  int fd;
  CTUI_SCREEN *screen;
  CTUI_INPUT_SOURCE src;
  /* the host's grid (shared), its generation */
  uint32_t gen;
  int rows, cols;
  CTUI_CELL *grid;
  size_t grid_bytes;
  /* the newest cells and which changed since the last frame shipped; one
   * frame in flight until the host acks it */
  CTUI_CELL *latest;
  unsigned char *dirty;
  int any_dirty, inflight;
  CTUI_LINK_RUN *runs;
  /* clusters the host knows, by the app's index */
  unsigned char *told;
  uint32_t n_told;
  int resized;
  CTUI_RESIZE_EVENT_DATA resize;
  CTUI_LINK_M_EVENT ev;
  PIX *pix;
  int n_pix;
  CTUI_LINK_APP_FN app;
  void *app_ctx;
  char *buf; /* a message in */
};

static int send0(CTUI_LINK *l, uint32_t type, const void *fixed, size_t n) {
  if (ctui_link_send(l->fd, type, fixed, n, NULL, 0, -1) != 0) {
    ctui_logf(E_WRN, "[CTUI:LINK] - a message (%u) didn't go\n", type);
    return -1;
  }
  return 0;
}

/* --- frames --- */

/* the host told about cluster ch before a frame names it */
static void tell_cluster(CTUI_LINK *l, uint32_t ch) {
  uint32_t i = ch & ~CTUI_CELL_CLUSTER;
  if (i < l->n_told && l->told[i]) {
    return;
  }
  if (i >= l->n_told) {
    uint32_t n = i + 64;
    unsigned char *t = realloc(l->told, n);
    if (!t) {
      return;
    }
    memset(t + l->n_told, 0, n - l->n_told);
    l->told = t;
    l->n_told = n;
  }
  int n = 0;
  const uint32_t *cps = ctui_cell_cluster(ch, &n);
  if (!cps || n <= 0 || n > CTUI_CLUSTER_MAX) {
    return;
  }
  CTUI_LINK_M_CLUSTER m = {.index = i, .n = (uint32_t)n};
  memcpy(m.cps, cps, (size_t)n * sizeof *cps);
  if (send0(l, CTUI_LINK_CLUSTER, &m, sizeof m) == 0) {
    l->told[i] = 1;
  }
}

/* what changed since the last frame shipped, into the grid and to the
 * host as its runs: one frame in flight */
static void ship(CTUI_LINK *l) {
  if (l->inflight || !l->any_dirty || !l->grid) {
    return;
  }
  uint32_t nr = 0;
  int all = 0;
  size_t n = (size_t)l->rows * (size_t)l->cols;
  for (size_t i = 0; i < n; i++) {
    if (!l->dirty[i]) {
      continue;
    }
    size_t j = i;
    int row = (int)(i / (size_t)l->cols);
    while (j < n && l->dirty[j] && (int)(j / (size_t)l->cols) == row) {
      uint32_t ch = l->latest[j].ch;
      if ((ch & CTUI_CELL_CLUSTER) && !(ch & CTUI_CELL_CONT)) {
        tell_cluster(l, ch);
      }
      l->dirty[j++] = 0;
    }
    memcpy(&l->grid[i], &l->latest[i], (j - i) * sizeof(CTUI_CELL));
    if (nr < CTUI_LINK_RUNS) {
      l->runs[nr] = (CTUI_LINK_RUN){.row = (uint16_t)row,
                                    .col = (uint16_t)(i % (size_t)l->cols),
                                    .n = (uint16_t)(j - i)};
    }
    nr++;
    i = j - 1;
  }
  if (nr > CTUI_LINK_RUNS) {
    all = 1;
  }
  CTUI_LINK_M_FRAME f = {.gen = l->gen, .runs = all ? 0 : nr};
  l->any_dirty = 0;
  if (ctui_link_send(l->fd, CTUI_LINK_FRAME, &f, sizeof f, l->runs,
                     all ? 0 : nr * sizeof *l->runs, -1) != 0) {
    ctui_logf(E_WRN, "[CTUI:LINK] - a frame didn't go\n");
    return;
  }
  l->inflight = 1;
}

/* the screen sink: always takes the frame, the host gets it when it's
 * done with the one before */
static int sink_frame(void *ctx, const CTUI_SCREEN *s) {
  CTUI_LINK *l = ctx;
  if (s->rows != l->rows || s->cols != l->cols) {
    return 0; /* a grid that's gone: the resize redraws all of it */
  }
  size_t n = (size_t)s->rows * (size_t)s->cols;
  for (size_t i = 0; i < n; i++) {
    if (memcmp(&s->cells[i], &s->buffer[i], sizeof(CTUI_CELL)) != 0) {
      l->latest[i] = s->cells[i];
      l->dirty[i] = 1;
      l->any_dirty = 1;
    }
  }
  ship(l);
  return 0;
}

static const CTUI_SCREEN_SINK k_screen = {.frame = sink_frame};

/* --- images --- */

static PIX *pix_find(CTUI_LINK *l, uint32_t id) {
  for (int i = 0; i < l->n_pix; i++) {
    if (l->pix[i].id == id) {
      return &l->pix[i];
    }
  }
  return NULL;
}

static void pix_drop(CTUI_LINK *l, uint32_t id) {
  PIX *p = pix_find(l, id);
  if (p) {
    free(p->px);
    *p = l->pix[--l->n_pix];
  }
}

/* an image call with pixels (w x h RGBA) in an fd of their own */
static void send_px(CTUI_LINK *l, uint32_t type, const CTUI_LINK_M_GFX *g,
                    const unsigned char *rgba) {
  size_t bytes = (size_t)g->width * (size_t)g->height * 4u;
  int fd = rgba && bytes ? ctui_link_memfd(rgba, bytes) : -1;
  if (fd < 0) {
    ctui_logf(E_WRN, "[CTUI:LINK] - an image's pixels couldn't be sent\n");
    return;
  }
  if (ctui_link_send(l->fd, type, g, sizeof *g, NULL, 0, fd) != 0) {
    ctui_logf(E_WRN, "[CTUI:LINK] - an image call (%u) didn't go\n", type);
  }
  close(fd);
}

static void g_display(void *ctx, int row, int col, int cell_cols, int cell_rows,
                      const unsigned char *rgba, int width, int height,
                      unsigned int image_id, int z) {
  CTUI_LINK_M_GFX g = {.id = image_id,
                       .row = row,
                       .col = col,
                       .cols = cell_cols,
                       .rows = cell_rows,
                       .z = z,
                       .width = width,
                       .height = height};
  send_px(ctx, CTUI_LINK_G_DISPLAY, &g, rgba);
}

static unsigned char *g_image_begin(void *ctx, unsigned int image_id, int width,
                                    int height, int keep) {
  (void)keep; /* the buffer holds the last commit's pixels either way */
  CTUI_LINK *l = ctx;
  if (width <= 0 || height <= 0) {
    return NULL;
  }
  PIX *p = pix_find(l, image_id);
  if (!p) {
    PIX *t = realloc(l->pix, (size_t)(l->n_pix + 1) * sizeof *t);
    if (!t) {
      return NULL;
    }
    l->pix = t;
    p = &l->pix[l->n_pix++];
    *p = (PIX){.id = image_id};
  }
  if (p->w != width || p->h != height) {
    unsigned char *px = calloc((size_t)width * (size_t)height, 4);
    if (!px) {
      return NULL;
    }
    free(p->px);
    p->px = px;
    p->w = width;
    p->h = height;
  }
  return p->px;
}

static void g_image_commit(void *ctx, unsigned int image_id, int row, int col,
                           int cell_cols, int cell_rows, int z) {
  CTUI_LINK *l = ctx;
  PIX *p = pix_find(l, image_id);
  if (!p) {
    return;
  }
  CTUI_LINK_M_GFX g = {.id = image_id,
                       .row = row,
                       .col = col,
                       .cols = cell_cols,
                       .rows = cell_rows,
                       .z = z,
                       .width = p->w,
                       .height = p->h};
  send_px(l, CTUI_LINK_G_COMMIT, &g, p->px);
}

static void g_image_free(void *ctx, unsigned int image_id) {
  CTUI_LINK *l = ctx;
  pix_drop(l, image_id);
  CTUI_LINK_M_GFX g = {.id = image_id};
  send0(l, CTUI_LINK_G_FREE, &g, sizeof g);
}

static void g_delete(void *ctx, unsigned int image_id) {
  CTUI_LINK *l = ctx;
  pix_drop(l, image_id);
  CTUI_LINK_M_GFX g = {.id = image_id};
  send0(l, CTUI_LINK_G_DELETE, &g, sizeof g);
}

static void g_place_file(void *ctx, unsigned int image_id, const char *path,
                         int cols, int rows) {
  CTUI_LINK *l = ctx;
  CTUI_LINK_M_GFX g = {.id = image_id, .cols = cols, .rows = rows};
  if (ctui_link_send(l->fd, CTUI_LINK_G_FILE, &g, sizeof g, path,
                     strlen(path) + 1, -1) != 0) {
    ctui_logf(E_WRN, "[CTUI:LINK] - a placed file didn't go\n");
  }
}

static void g_place_rgba(void *ctx, unsigned int image_id,
                         const unsigned char *rgba, int width, int height,
                         int cols, int rows) {
  CTUI_LINK_M_GFX g = {.id = image_id,
                       .cols = cols,
                       .rows = rows,
                       .width = width,
                       .height = height};
  send_px(ctx, CTUI_LINK_G_RGBA, &g, rgba);
}

static void g_display_px(void *ctx, int row, int col, int cell_cols,
                         int cell_rows, int x, int y, int w, int h,
                         const unsigned char *rgba, int width, int height,
                         unsigned int image_id, int z) {
  CTUI_LINK_M_GFX g = {.id = image_id,
                       .row = row,
                       .col = col,
                       .cols = cell_cols,
                       .rows = cell_rows,
                       .z = z,
                       .width = width,
                       .height = height,
                       .x = x,
                       .y = y,
                       .w = w,
                       .h = h};
  send_px(ctx, CTUI_LINK_G_PX, &g, rgba);
}

static void g_wipe(void *ctx, unsigned int image_id, unsigned int left_id,
                   unsigned int marker_id, double x, double px_per_s,
                   double x_end) {
  CTUI_LINK_M_WIPE m = {.id = image_id,
                        .left = left_id,
                        .marker = marker_id,
                        .x = x,
                        .px_per_s = px_per_s,
                        .x_end = x_end};
  send0(ctx, CTUI_LINK_G_WIPE, &m, sizeof m);
}

static void g_put(void *ctx, unsigned int image_id, unsigned int placement_id,
                  int row, int col, int cols, int rows, int z) {
  CTUI_LINK_M_GFX g = {.id = image_id,
                       .id2 = placement_id,
                       .row = row,
                       .col = col,
                       .cols = cols,
                       .rows = rows,
                       .z = z};
  send0(ctx, CTUI_LINK_G_PUT, &g, sizeof g);
}

static void g_unput(void *ctx, unsigned int image_id,
                    unsigned int placement_id) {
  CTUI_LINK_M_GFX g = {.id = image_id, .id2 = placement_id};
  send0(ctx, CTUI_LINK_G_UNPUT, &g, sizeof g);
}

static void g_flush(void *ctx) { send0(ctx, CTUI_LINK_G_FLUSH, NULL, 0); }

static const CTUI_GFX_SINK k_gfx = {
    .display = g_display,
    .image_begin = g_image_begin,
    .image_commit = g_image_commit,
    .image_free = g_image_free,
    .delete = g_delete,
    .place_file = g_place_file,
    .place_rgba = g_place_rgba,
    .display_px = g_display_px,
    .wipe = g_wipe,
    .put = g_put,
    .unput = g_unput,
    .flush = g_flush,
};

/* --- the host's messages --- */

/* a new grid from the host: mapped, the newest cells sized to it */
static int take_grid(CTUI_LINK *l, const CTUI_LINK_M_GRID *g, int fd) {
  if (fd < 0 || g->rows <= 0 || g->cols <= 0 || g->rows > 0xffff ||
      g->cols > 0xffff) {
    return -1;
  }
  size_t n = (size_t)g->rows * (size_t)g->cols;
  void *map = mmap(NULL, n * sizeof(CTUI_CELL), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  CTUI_CELL *latest = calloc(n, sizeof *latest);
  unsigned char *dirty = calloc(n, 1);
  if (map == MAP_FAILED || !latest || !dirty) {
    if (map != MAP_FAILED) {
      munmap(map, n * sizeof(CTUI_CELL));
    }
    free(latest);
    free(dirty);
    return -1;
  }
  if (l->grid) {
    munmap(l->grid, l->grid_bytes);
  }
  free(l->latest);
  free(l->dirty);
  l->grid = map;
  l->grid_bytes = n * sizeof(CTUI_CELL);
  l->latest = latest;
  l->dirty = dirty;
  l->any_dirty = 0;
  l->gen = g->gen;
  l->rows = g->rows;
  l->cols = g->cols;
  l->resize.rows = g->rows;
  l->resize.cols = g->cols;
  l->resized = 1;
  ctui_cell_px_set(g->cw, g->ch);
  ctui_logf(E_INF, "[CTUI:LINK] - grid %dx%d, cells %dx%d px @ tick %d\n",
            g->cols, g->rows, g->cw, g->ch, ctui_tick_advance());
  return 0;
}

/* one message: 1 an event for the loop in l->ev, 0 handled, -1 broken */
static int take(CTUI_LINK *l, long len, int fd) {
  const CTUI_LINK_HDR *h = (const CTUI_LINK_HDR *)l->buf;
  const char *body = l->buf + sizeof *h;
  size_t blen = (size_t)len - sizeof *h;
  int r = 0;
  switch (h->type) {
  case CTUI_LINK_GRID:
    if (blen < sizeof(CTUI_LINK_M_GRID)) {
      r = -1;
      break;
    }
    CTUI_LINK_M_GRID g;
    memcpy(&g, body, sizeof g);
    r = take_grid(l, &g, fd);
    break;
  case CTUI_LINK_ACK:
    l->inflight = 0;
    ship(l);
    break;
  case CTUI_LINK_EVENT:
    if (blen < sizeof l->ev) {
      r = -1;
      break;
    }
    memcpy(&l->ev, body, sizeof l->ev);
    r = 1;
    break;
  case CTUI_LINK_APP:
    if (l->app) {
      l->app(l->app_ctx, body, blen);
    }
    break;
  default:
    ctui_logf(E_WRN, "[CTUI:LINK] - a message the app doesn't know (%u)\n",
              h->type);
  }
  if (fd >= 0) {
    close(fd);
  }
  return r;
}

static int src_next(void *ctx, CTUI_EVENT *ev, int readable) {
  CTUI_LINK *l = ctx;
  for (;;) {
    if (l->resized) {
      l->resized = 0;
      ev->type = CTUI_RESIZE_EVENT;
      ev->scope = CTUI_EVENT_SCOPE_GLOBAL;
      ev->ev_source = "terminal";
      ev->event_data = &l->resize;
      ev->origin = NULL;
      return 1;
    }
    if (!readable) {
      return 0;
    }
    int fd;
    long len = ctui_link_recv(l->fd, l->buf, &fd);
    if (len == 0) {
      return 0;
    }
    if (len < 0) {
      ctui_logf(E_INF, "[CTUI:LINK] - the host's gone @ tick %d\n",
                ctui_tick_advance());
      return -1;
    }
    int r = take(l, len, fd);
    if (r < 0) {
      ctui_logf(E_ERR, "[CTUI:LINK] - a broken message from the host\n");
      return -1;
    }
    if (r > 0) {
      ev->type = (CTUI_EVENTTYPE)l->ev.type;
      ev->scope = CTUI_EVENT_SCOPE_GLOBAL;
      ev->ev_source = "input";
      ev->event_data = &l->ev.d;
      ev->origin = NULL;
      return 1;
    }
  }
}

/* --- open, attach, close --- */

CTUI_LINK *ctui_link_open(int fd, unsigned mode, int *rows, int *cols,
                          char *err, size_t n) {
  CTUI_LINK *l = calloc(1, sizeof *l);
  if (l) {
    l->buf = malloc(CTUI_LINK_MAX);
    l->runs = malloc(CTUI_LINK_RUNS * sizeof *l->runs);
  }
  if (!l || !l->buf || !l->runs) {
    snprintf(err, n, "out of memory");
    if (l) {
      free(l->buf);
      free(l->runs);
      free(l);
    }
    close(fd);
    return NULL;
  }
  l->fd = fd;
  CTUI_LINK_M_HELLO hello = {.magic = CTUI_LINK_MAGIC,
                             .version = CTUI_LINK_VERSION,
                             .cell_size = sizeof(CTUI_CELL)};
  if (send0(l, CTUI_LINK_HELLO, &hello, sizeof hello) != 0) {
    snprintf(err, n, "the host isn't there");
    ctui_link_close(l);
    return NULL;
  }
  /* the first grid: what to draw at */
  while (!l->grid) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 5000) <= 0) {
      snprintf(err, n, "no grid from the host");
      ctui_link_close(l);
      return NULL;
    }
    int mfd;
    long len = ctui_link_recv(fd, l->buf, &mfd);
    if (len < 0 || (len > 0 && take(l, len, mfd) < 0)) {
      snprintf(err, n, "the host went before its grid");
      ctui_link_close(l);
      return NULL;
    }
  }
  l->resized = 0; /* the app's first size, not a resize */
  *rows = l->rows;
  *cols = l->cols;
  ctui_gfx_set_mode(mode);
  ctui_gfx_set_sink(&k_gfx, l);
  l->src = (CTUI_INPUT_SOURCE){.fd = fd, .next = src_next, .ctx = l};
  return l;
}

void ctui_link_attach(CTUI_LINK *l, CTUI_SCREEN *screen) {
  l->screen = screen;
  ctui_screen_set_sink(screen, &k_screen, l);
  ctui_input_set_source(&l->src);
}

void ctui_link_close(CTUI_LINK *l) {
  if (!l) {
    return;
  }
  if (l->src.ctx) {
    ctui_input_set_source(NULL);
    ctui_gfx_set_sink(NULL, NULL);
  }
  if (l->screen) {
    ctui_screen_set_sink(l->screen, NULL, NULL);
  }
  if (l->grid) {
    munmap(l->grid, l->grid_bytes);
  }
  for (int i = 0; i < l->n_pix; i++) {
    free(l->pix[i].px);
  }
  free(l->pix);
  free(l->latest);
  free(l->dirty);
  free(l->told);
  free(l->runs);
  free(l->buf);
  close(l->fd);
  free(l);
}

void ctui_link_set_default(CTUI_LINK *l, uint32_t fg, uint32_t bg) {
  CTUI_LINK_M_DEFAULT m = {.fg = fg, .bg = bg};
  send0(l, CTUI_LINK_DEFAULT, &m, sizeof m);
}

int ctui_link_app(CTUI_LINK *l, const void *msg, size_t n) {
  if (n > CTUI_LINK_MAX - sizeof(CTUI_LINK_HDR)) {
    return -1;
  }
  return ctui_link_send(l->fd, CTUI_LINK_APP, msg, n, NULL, 0, -1);
}

void ctui_link_on_app(CTUI_LINK *l, CTUI_LINK_APP_FN fn, void *ctx) {
  l->app = fn;
  l->app_ctx = ctx;
}
