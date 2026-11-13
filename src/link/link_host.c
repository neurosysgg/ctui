#define _GNU_SOURCE /* memfd_create */
#include "link.h"
#include "wire.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* an image's sides at most: what a message's fields may say */
#define MAX_SIDE 16384

struct CTUI_LINK_HOST {
  int fd;
  CTUI_LINK_HOST_SINKS s;
  int hello;
  uint32_t gen;
  int rows, cols;
  CTUI_CELL *grid; /* the app writes it, the host copies frames out */
  size_t grid_bytes;
  CTUI_SCREEN *screen; /* the host's copy, flushed into s.screen */
  /* the app's clusters, by its index: the host's own cell values */
  uint32_t *clusters;
  uint32_t n_clusters;
  char *buf;
  unsigned dropped; /* events that found the socket full */
  int ack_owed;     /* an ack that found it full: sent at the next chance */
};

CTUI_LINK_HOST *ctui_link_host_new(int fd, const CTUI_LINK_HOST_SINKS *sinks) {
  CTUI_LINK_HOST *h = calloc(1, sizeof *h);
  if (h) {
    h->buf = malloc(CTUI_LINK_MAX);
  }
  if (!h || !h->buf) {
    free(h);
    close(fd);
    return NULL;
  }
  h->fd = fd;
  h->s = *sinks;
  int fl = fcntl(fd, F_GETFL);
  if (fl >= 0) {
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
  return h;
}

CTUI_LINK_HOST *ctui_link_host_adopt(int fd,
                                     const CTUI_LINK_HOST_SINKS *sinks) {
  CTUI_LINK_HOST *h = ctui_link_host_new(fd, sinks);
  if (h) {
    h->hello = 1;
    ctui_logf(E_INF, "[CTUI:LINK] - a link adopted (fd %d) @ tick %d\n", fd,
              ctui_tick_advance());
  }
  return h;
}

void ctui_link_host_set_sinks(CTUI_LINK_HOST *h,
                              const CTUI_LINK_HOST_SINKS *sinks) {
  static const CTUI_LINK_HOST_SINKS none;
  h->s = sinks ? *sinks : none;
  if (h->screen) {
    ctui_screen_set_sink(h->screen, h->s.screen, h->s.screen_ctx);
  }
}

int ctui_link_host_reset(CTUI_LINK_HOST *h) {
  /* its clusters come again as its frames name them; the frame it had in
   * flight is forgotten on its side, so an ack owed for it would only
   * free the next one early */
  if (h->clusters) {
    memset(h->clusters, 0, h->n_clusters * sizeof *h->clusters);
  }
  h->ack_owed = 0;
  ctui_logf(E_INF, "[CTUI:LINK] - reset sent @ tick %d\n", ctui_tick_advance());
  return ctui_link_send(h->fd, CTUI_LINK_RESET, NULL, 0, NULL, 0, -1);
}

void ctui_link_host_free(CTUI_LINK_HOST *h) {
  if (!h) {
    return;
  }
  if (h->grid) {
    munmap(h->grid, h->grid_bytes);
  }
  if (h->screen) {
    ctui_screen_free(h->screen);
  }
  free(h->clusters);
  free(h->buf);
  close(h->fd);
  free(h);
}

int ctui_link_host_fd(const CTUI_LINK_HOST *h) { return h->fd; }

int ctui_link_host_grid(CTUI_LINK_HOST *h, int rows, int cols, int cw, int ch) {
  if (rows <= 0 || cols <= 0 || rows > 0xffff || cols > 0xffff) {
    return -1;
  }
  size_t bytes = (size_t)rows * (size_t)cols * sizeof(CTUI_CELL);
  int fd = memfd_create("ctui-link-grid", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)bytes) != 0) {
    if (fd >= 0) {
      close(fd);
    }
    return -1;
  }
  void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    close(fd);
    return -1;
  }
  if (!h->screen) {
    h->screen = ctui_screen_create(rows, cols);
  } else {
    ctui_screen_resize(h->screen, rows, cols);
  }
  if (!h->screen) {
    munmap(map, bytes);
    close(fd);
    return -1;
  }
  /* a new size repaints everything from the app's next frame */
  ctui_screen_set_sink(h->screen, h->s.screen, h->s.screen_ctx);
  if (h->grid) {
    munmap(h->grid, h->grid_bytes);
  }
  h->grid = map;
  h->grid_bytes = bytes;
  h->rows = rows;
  h->cols = cols;
  CTUI_LINK_M_GRID g = {
      .gen = ++h->gen, .rows = rows, .cols = cols, .cw = cw, .ch = ch};
  int r = ctui_link_send(h->fd, CTUI_LINK_GRID, &g, sizeof g, NULL, 0, fd);
  close(fd);
  ctui_logf(E_INF, "[CTUI:LINK] - grid %u sent: %dx%d @ tick %d\n", g.gen, cols,
            rows, ctui_tick_advance());
  return r;
}

/* a cell from the app's grid as the host's: its cluster translated */
static CTUI_CELL host_cell(const CTUI_LINK_HOST *h, CTUI_CELL c) {
  if ((c.ch & CTUI_CELL_CLUSTER) && !(c.ch & CTUI_CELL_CONT)) {
    uint32_t i = c.ch & ~CTUI_CELL_CLUSTER;
    c.ch = i < h->n_clusters && h->clusters[i] ? h->clusters[i] : 0xfffdu;
  }
  return c;
}

/* the frame taken: without an ack the app never sends another, so one
 * that found the socket full goes at the next read or event */
static void ack(CTUI_LINK_HOST *h) {
  if (h->ack_owed &&
      ctui_link_send(h->fd, CTUI_LINK_ACK, NULL, 0, NULL, 0, -1) == 0) {
    h->ack_owed = 0;
  }
}

static int frame(CTUI_LINK_HOST *h, const char *body, size_t n) {
  CTUI_LINK_M_FRAME f;
  if (n < sizeof f) {
    return -1;
  }
  memcpy(&f, body, sizeof f);
  if (f.gen == h->gen && h->grid) {
    const CTUI_LINK_RUN *runs = (const CTUI_LINK_RUN *)(body + sizeof f);
    if (f.runs > CTUI_LINK_RUNS ||
        n < sizeof f + (size_t)f.runs * sizeof *runs) {
      return -1;
    }
    if (!f.runs) {
      size_t all = (size_t)h->rows * (size_t)h->cols;
      for (size_t i = 0; i < all; i++) {
        h->screen->cells[i] = host_cell(h, h->grid[i]);
      }
    }
    for (uint32_t k = 0; k < f.runs; k++) {
      CTUI_LINK_RUN r;
      memcpy(&r, &runs[k], sizeof r);
      if (r.row >= h->rows || r.col + r.n > h->cols) {
        return -1;
      }
      size_t at = (size_t)r.row * (size_t)h->cols + r.col;
      for (unsigned i = 0; i < r.n; i++) {
        h->screen->cells[at + i] = host_cell(h, h->grid[at + i]);
      }
    }
    if (h->s.screen) {
      ctui_screen_flush(h->screen);
    }
  }
  /* an older grid's frame is dropped, acked all the same: the app's one
   * in flight */
  h->ack_owed = 1;
  ack(h);
  return 0;
}

static int cluster(CTUI_LINK_HOST *h, const char *body, size_t n) {
  CTUI_LINK_M_CLUSTER m;
  if (n < sizeof m) {
    return -1;
  }
  memcpy(&m, body, sizeof m);
  if (m.n == 0 || m.n > CTUI_CLUSTER_MAX || m.index > 0xffffffu) {
    return -1;
  }
  char utf8[CTUI_CLUSTER_MAX * 4 + 1];
  size_t len = 0;
  for (uint32_t i = 0; i < m.n; i++) {
    len += (size_t)ctui_utf8_encode(m.cps[i], utf8 + len);
  }
  uint32_t val = 0;
  int width = 0;
  ctui_utf8_cluster(utf8, len, &val, &width);
  if (m.index >= h->n_clusters) {
    uint32_t cap = m.index + 64;
    uint32_t *t = realloc(h->clusters, cap * sizeof *t);
    if (!t) {
      return 0;
    }
    memset(t + h->n_clusters, 0, (cap - h->n_clusters) * sizeof *t);
    h->clusters = t;
    h->n_clusters = cap;
  }
  h->clusters[m.index] = val;
  return 0;
}

/* an image message's pixels: mapped from its fd, checked against what it
 * says they are; NULL for none */
static unsigned char *pixels(const CTUI_LINK_M_GFX *g, int fd, size_t *bytes) {
  if (fd < 0 || g->width <= 0 || g->height <= 0 || g->width > MAX_SIDE ||
      g->height > MAX_SIDE) {
    return NULL;
  }
  *bytes = (size_t)g->width * (size_t)g->height * 4u;
  struct stat st;
  if (fstat(fd, &st) != 0 || (size_t)st.st_size < *bytes) {
    return NULL;
  }
  void *p = mmap(NULL, *bytes, PROT_READ, MAP_PRIVATE, fd, 0);
  return p == MAP_FAILED ? NULL : p;
}

static int gfx(CTUI_LINK_HOST *h, uint32_t type, const char *body, size_t n,
               int fd) {
  const CTUI_GFX_SINK *s = h->s.gfx;
  void *c = h->s.gfx_ctx;
  if (type == CTUI_LINK_G_FLUSH) {
    if (s) {
      s->flush(c);
    }
    return 0;
  }
  if (type == CTUI_LINK_G_WIPE) {
    CTUI_LINK_M_WIPE w;
    if (n < sizeof w) {
      return -1;
    }
    memcpy(&w, body, sizeof w);
    if (s) {
      s->wipe(c, w.id, w.left, w.marker, w.x, w.px_per_s, w.x_end);
    }
    return 0;
  }
  CTUI_LINK_M_GFX g;
  if (n < sizeof g) {
    return -1;
  }
  memcpy(&g, body, sizeof g);
  if (!s) {
    return 0;
  }
  size_t bytes = 0;
  unsigned char *px = NULL;
  switch (type) {
  case CTUI_LINK_G_DISPLAY:
  case CTUI_LINK_G_COMMIT:
  case CTUI_LINK_G_RGBA:
  case CTUI_LINK_G_PX:
    if (!(px = pixels(&g, fd, &bytes))) {
      ctui_logf(E_WRN, "[CTUI:LINK] - an image (%u) without its pixels\n",
                g.id);
      return 0;
    }
    break;
  default:
    break;
  }
  switch (type) {
  case CTUI_LINK_G_DISPLAY:
    s->display(c, g.row, g.col, g.cols, g.rows, px, g.width, g.height, g.id,
               g.z);
    break;
  case CTUI_LINK_G_COMMIT: {
    unsigned char *to = s->image_begin(c, g.id, g.width, g.height, 0);
    if (to) {
      memcpy(to, px, bytes);
      s->image_commit(c, g.id, g.row, g.col, g.cols, g.rows, g.z);
    }
    break;
  }
  case CTUI_LINK_G_RGBA:
    s->place_rgba(c, g.id, px, g.width, g.height, g.cols, g.rows);
    break;
  case CTUI_LINK_G_PX:
    s->display_px(c, g.row, g.col, g.cols, g.rows, g.x, g.y, g.w, g.h, px,
                  g.width, g.height, g.id, g.z);
    break;
  case CTUI_LINK_G_FREE:
    s->image_free(c, g.id);
    break;
  case CTUI_LINK_G_DELETE:
    s->delete(c, g.id);
    break;
  case CTUI_LINK_G_FILE: {
    const char *path = body + sizeof g;
    size_t pn = n - sizeof g;
    if (!pn || memchr(path, '\0', pn) == NULL) {
      return -1;
    }
    s->place_file(c, g.id, path, g.cols, g.rows);
    break;
  }
  case CTUI_LINK_G_PUT:
    s->put(c, g.id, g.id2, g.row, g.col, g.cols, g.rows, g.z);
    break;
  case CTUI_LINK_G_UNPUT:
    s->unput(c, g.id, g.id2);
    break;
  default:
    break;
  }
  if (px) {
    munmap(px, bytes);
  }
  return 0;
}

/* one message: 0 applied, -1 broken */
static int take(CTUI_LINK_HOST *h, long len, int fd) {
  const CTUI_LINK_HDR *hd = (const CTUI_LINK_HDR *)h->buf;
  const char *body = h->buf + sizeof *hd;
  size_t n = (size_t)len - sizeof *hd;
  if (!h->hello && hd->type != CTUI_LINK_HELLO) {
    return -1;
  }
  switch (hd->type) {
  case CTUI_LINK_HELLO: {
    CTUI_LINK_M_HELLO m;
    if (n < sizeof m) {
      return -1;
    }
    memcpy(&m, body, sizeof m);
    if (m.magic != CTUI_LINK_MAGIC || m.version != CTUI_LINK_VERSION ||
        m.cell_size != sizeof(CTUI_CELL)) {
      ctui_logf(E_ERR,
                "[CTUI:LINK] - an app of another build (version %u, cells "
                "of %u bytes)\n",
                m.version, m.cell_size);
      return -1;
    }
    h->hello = 1;
    return 0;
  }
  case CTUI_LINK_FRAME:
    return frame(h, body, n);
  case CTUI_LINK_CLUSTER:
    return cluster(h, body, n);
  case CTUI_LINK_DEFAULT: {
    CTUI_LINK_M_DEFAULT m;
    if (n < sizeof m) {
      return -1;
    }
    memcpy(&m, body, sizeof m);
    if (h->s.set_default) {
      h->s.set_default(h->s.ctx, m.fg, m.bg);
    }
    return 0;
  }
  case CTUI_LINK_POINTER: {
    /* a name, NUL-terminated, of what a CSS cursor's could be */
    if (n < 2 || n > CTUI_LINK_POINTER_MAX || body[n - 1] != '\0') {
      return -1;
    }
    for (size_t i = 0; i + 1 < n; i++) {
      char c = body[i];
      if (!((c >= 'a' && c <= 'z') || c == '-')) {
        return -1;
      }
    }
    if (h->s.pointer) {
      h->s.pointer(h->s.ctx, body);
    }
    return 0;
  }
  case CTUI_LINK_APP:
    if (h->s.app) {
      h->s.app(h->s.ctx, body, n);
    }
    return 0;
  default:
    if (hd->type >= CTUI_LINK_G_DISPLAY && hd->type <= CTUI_LINK_G_FLUSH) {
      return gfx(h, hd->type, body, n, fd);
    }
    return -1;
  }
}

int ctui_link_host_read(CTUI_LINK_HOST *h) {
  ack(h);
  for (;;) {
    int fd;
    long len = ctui_link_recv(h->fd, h->buf, &fd);
    if (len == 0) {
      return 0;
    }
    if (len < 0) {
      ctui_logf(E_INF, "[CTUI:LINK] - the app's gone @ tick %d\n",
                ctui_tick_advance());
      return -1;
    }
    int r = take(h, len, fd);
    if (fd >= 0) {
      close(fd);
    }
    if (r < 0) {
      ctui_logf(E_ERR, "[CTUI:LINK] - a broken message from the app (%u)\n",
                ((const CTUI_LINK_HDR *)h->buf)->type);
      return -1;
    }
  }
}

int ctui_link_host_event(CTUI_LINK_HOST *h, const CTUI_EVENT *ev) {
  CTUI_LINK_M_EVENT m;
  memset(&m, 0, sizeof m);
  m.type = (int32_t)ev->type;
  switch (ev->type) {
  case CTUI_KEYPRESS_EVENT:
  case CTUI_KEYRELEASE_EVENT:
    m.d.key = *(const CTUI_KEYPRESS_EVENT_DATA *)ev->event_data;
    break;
  case CTUI_MOUSE_EVENT:
    m.d.mouse = *(const CTUI_MOUSE_EVENT_DATA *)ev->event_data;
    break;
  case CTUI_FOCUS_EVENT:
    m.d.focus = *(const CTUI_FOCUS_EVENT_DATA *)ev->event_data;
    break;
  default:
    return -1;
  }
  ack(h);
  if (ctui_link_send(h->fd, CTUI_LINK_EVENT, &m, sizeof m, NULL, 0, -1) != 0) {
    if (h->dropped++ % 64 == 0) {
      ctui_logf(E_WRN, "[CTUI:LINK] - an app's input dropped (%u so far)\n",
                h->dropped);
    }
    return -1;
  }
  return 0;
}

int ctui_link_host_app(CTUI_LINK_HOST *h, const void *msg, size_t n) {
  if (n > CTUI_LINK_MAX - sizeof(CTUI_LINK_HDR)) {
    return -1;
  }
  return ctui_link_send(h->fd, CTUI_LINK_APP, msg, n, NULL, 0, -1);
}
