#ifndef CTUI_LINK_WIRE_H
#define CTUI_LINK_WIRE_H

/* The link's messages (link.h), src/link/'s own. One SOCK_SEQPACKET
 * message each: a CTUI_LINK_HDR, then its fixed fields, then (for a few)
 * bytes of its own; pixels and the cell grid travel as memfds beside them
 * (SCM_RIGHTS). Both ends are the same build of ctui (a host and the
 * apps it starts), so the structs go as they are; HELLO checks that. */

#include "ctui.h"

#include <stdint.h>

#define CTUI_LINK_MAGIC 0x4b4c5443u /* "CTLK" */
#define CTUI_LINK_VERSION 1u
/* a FRAME's runs at most; more, and the whole grid is the frame */
#define CTUI_LINK_RUNS 8192u
/* the largest message, its fds aside */
#define CTUI_LINK_MAX (sizeof(CTUI_LINK_HDR) + 16u + 6u * CTUI_LINK_RUNS)

enum {
  /* app -> host */
  CTUI_LINK_HELLO = 1, /* CTUI_LINK_M_HELLO */
  CTUI_LINK_FRAME,     /* CTUI_LINK_M_FRAME + runs (u16 row, col, n) */
  CTUI_LINK_DEFAULT,   /* CTUI_LINK_M_DEFAULT */
  CTUI_LINK_G_DISPLAY, /* CTUI_LINK_M_GFX + an fd: the pixels */
  CTUI_LINK_G_COMMIT,  /* CTUI_LINK_M_GFX + an fd */
  CTUI_LINK_G_FREE,    /* CTUI_LINK_M_GFX: image_free */
  CTUI_LINK_G_DELETE,  /* CTUI_LINK_M_GFX */
  CTUI_LINK_G_FILE,    /* CTUI_LINK_M_GFX + the path, NUL-terminated */
  CTUI_LINK_G_RGBA,    /* CTUI_LINK_M_GFX + an fd: place_rgba */
  CTUI_LINK_G_PX,      /* CTUI_LINK_M_GFX + an fd: display_px */
  CTUI_LINK_G_WIPE,    /* CTUI_LINK_M_WIPE */
  CTUI_LINK_G_PUT,     /* CTUI_LINK_M_GFX */
  CTUI_LINK_G_UNPUT,   /* CTUI_LINK_M_GFX */
  CTUI_LINK_G_FLUSH,   /* nothing */
  CTUI_LINK_CLUSTER,   /* CTUI_LINK_M_CLUSTER: a cluster cells will name,
                        * before the frame that first does */
  /* host -> app */
  CTUI_LINK_GRID = 32, /* CTUI_LINK_M_GRID + an fd: the cells */
  CTUI_LINK_ACK,       /* nothing: the frame in flight was taken */
  CTUI_LINK_EVENT,     /* CTUI_LINK_M_EVENT */
  /* either way: the app's own, bytes the link only carries */
  CTUI_LINK_APP = 64,
};

typedef struct {
  uint32_t type, len; /* len: what follows the header */
} CTUI_LINK_HDR;

typedef struct {
  uint32_t magic, version, cell_size;
} CTUI_LINK_M_HELLO;

typedef struct {
  uint32_t gen;  /* the grid it's in: an older one's is dropped */
  uint32_t runs; /* 0: the whole grid */
} CTUI_LINK_M_FRAME;

typedef struct {
  uint16_t row, col, n;
} CTUI_LINK_RUN;

typedef struct {
  uint32_t fg, bg;
} CTUI_LINK_M_DEFAULT;

/* the app's interned cluster (its CTUI_CELL_CLUSTER index) by its
 * codepoints: the host interns them in its own table */
typedef struct {
  uint32_t index, n;
  uint32_t cps[CTUI_CLUSTER_MAX];
} CTUI_LINK_M_CLUSTER;

/* every image call's fields, each using what its own needs */
typedef struct {
  uint32_t id, id2; /* id2: a put's placement */
  int32_t row, col, cols, rows, z;
  int32_t width, height; /* the pixels' (an fd's: width * height * 4) */
  int32_t x, y, w, h;    /* display_px's rect */
} CTUI_LINK_M_GFX;

typedef struct {
  uint32_t id, left, marker;
  double x, px_per_s, x_end;
} CTUI_LINK_M_WIPE;

typedef struct {
  uint32_t gen;
  int32_t rows, cols, cw, ch;
} CTUI_LINK_M_GRID;

typedef struct {
  int32_t type; /* CTUI_KEYPRESS_EVENT, KEYRELEASE, MOUSE, FOCUS */
  union {
    CTUI_KEYPRESS_EVENT_DATA key;
    CTUI_MOUSE_EVENT_DATA mouse;
    CTUI_FOCUS_EVENT_DATA focus;
  } d;
} CTUI_LINK_M_EVENT;

/* a message out, with fd (-1: none) beside it: 0, or -1 (the peer's gone,
 * or a nonblocking socket full) */
int ctui_link_send(int sock, uint32_t type, const void *fixed, size_t n,
                   const void *tail, size_t tn, int fd);
/* the next message into buf (CTUI_LINK_MAX), its fd into *fd (-1: none):
 * its length, 0 when nothing's there (nonblocking), -1 at the end */
long ctui_link_recv(int sock, void *buf, int *fd);
/* an fd of n bytes holding p, for a message (-1: none could be made) */
int ctui_link_memfd(const void *p, size_t n);

#endif
