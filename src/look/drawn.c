/* getuid() under plain -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "drawn.h"

#include "icon.h"
#include "png.h"

#include "../core/term.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_cw, g_ch;
static CTUI_LOOK g_look;
static int g_has_look;

static char g_dir[4096];

void ctui_drawn_set_dir(const char *dir) {
  snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : "");
}

const char *ctui_drawn_dir(void) {
  if (!g_dir[0]) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) {
      snprintf(g_dir, sizeof g_dir, "%s/ctui", rt);
    } else {
      snprintf(g_dir, sizeof g_dir, "/tmp/ctui-%u", (unsigned)getuid());
    }
  }
  return g_dir;
}

void ctui_drawn_set_look(const CTUI_LOOK *l) {
  g_has_look = l != NULL;
  if (l) {
    g_look = *l;
  }
}

const CTUI_LOOK *ctui_drawn_look(void) { return g_has_look ? &g_look : NULL; }

int ctui_drawn_on(void) { return g_has_look && ctui_icon_enabled(); }

void ctui_drawn_cell(int cw, int ch) {
  g_cw = cw;
  g_ch = ch;
}

int ctui_drawn_cell_px(int *cw, int *ch) {
  if (g_cw > 0 && g_ch > 0) {
    *cw = g_cw;
    *ch = g_ch;
    return 0;
  }
  return ctui_cell_px(cw, ch);
}

void ctui_drawn_path(const CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l, int w,
                     int h, char *out, size_t cap) {
  snprintf(out, cap, "%s/look-%016" PRIx64 ".png", ctui_drawn_dir(),
           ctui_look_control_key(c, l, w, h));
}

unsigned int ctui_drawn_id(const CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l,
                           int cols, int rows) {
  int cw, ch;
  if (!ctui_icon_enabled() || cols <= 0 || rows <= 0 ||
      ctui_drawn_cell_px(&cw, &ch) != 0) {
    return 0;
  }
  int w = cols * cw, h = rows * ch;
  char path[4096];
  ctui_drawn_path(c, l, w, h, path, sizeof path);
  struct stat st;
  if (stat(path, &st) != 0) {
    mkdir(ctui_drawn_dir(), 0700);
    unsigned char *rgba = malloc((size_t)w * (size_t)h * 4);
    ctui_look_control_paint(c, l, rgba, w, h);
    int r = ctui_look_png_write(path, rgba, w, h);
    free(rgba);
    if (r != 0) {
      ctui_logf(E_WRN, "[CTUI:DRAWN] - can't write %s\n", path);
      return 0;
    }
  }
  return ctui_icon_id_box(path, cols, rows);
}

void ctui_drawn_put(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                    unsigned int id, int cols, int rows, unsigned char bg) {
  if (id) {
    ctui_widget_put_kitty_placeholder(self, comp, row, col, id, cols, rows, bg);
  }
}

int ctui_drawn_or_text(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                       int col, const CTUI_LOOK_CONTROL *c, int cols,
                       const char *text, int max_w, unsigned char fg,
                       unsigned char bg) {
  const CTUI_LOOK *l = ctui_drawn_look();
  unsigned int id = l && c && cols <= max_w && col + cols <= self->w
                        ? ctui_drawn_id(c, l, cols, 1)
                        : 0;
  if (!id) {
    return text
               ? ctui_widget_puts_cut(self, comp, row, col, text, max_w, fg, bg)
               : 0;
  }
  ctui_drawn_put(self, comp, row, col, id, cols, 1, bg);
  return cols;
}

int ctui_drawn_picto(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                     CTUI_LOOK_PICTO p, int value, int max, int off,
                     const char *glyph, int max_w, unsigned char fg,
                     unsigned char bg) {
  CTUI_LOOK_CONTROL c = {.kind = CTUI_LOOK_CTL_PICTO,
                         .picto = p,
                         .value = value,
                         .max = max,
                         .flags = off ? CTUI_LOOK_CTL_OFF : 0};
  return ctui_drawn_or_text(self, comp, row, col, &c, CTUI_DRAWN_PICTO_COLS,
                            glyph, max_w, fg, bg);
}

int ctui_drawn_chip(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                    CTUI_LOOK_GLYPH g, unsigned flags, const char *text,
                    int max_w, unsigned char fg, unsigned char bg) {
  CTUI_LOOK_CONTROL c = {
      .kind = CTUI_LOOK_CTL_CHIP, .glyph = g, .flags = flags};
  return ctui_drawn_or_text(self, comp, row, col, &c, CTUI_DRAWN_PICTO_COLS,
                            text, max_w, fg, bg);
}

void ctui_drawn_end_chip(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                         CTUI_LOOK_GLYPH g, const char *text, unsigned char fg,
                         unsigned char bg) {
  int col = self->w - CTUI_DRAWN_PICTO_COLS;
  if (col < 0 || !ctui_drawn_chip(self, comp, row, col, g, 0, NULL,
                                  CTUI_DRAWN_PICTO_COLS, fg, bg)) {
    ctui_widget_puts_cut(self, comp, row, self->w - 1, text, 1, fg, bg);
  }
}

/* --- controls under text --- */

static unsigned int g_under_bases;

void ctui_drawn_under_begin(CTUI_DRAWN_UNDER *u) {
  if (!u->base) {
    u->base = ++g_under_bases << 16;
  }
  u->nnext = 0;
}

int ctui_drawn_under(CTUI_DRAWN_UNDER *u, const CTUI_WIDGET *self, int row,
                     int col, const CTUI_LOOK_CONTROL *c, int cols) {
  return ctui_drawn_under_rows(u, self, row, col, c, cols, 1);
}

int ctui_drawn_under_rows(CTUI_DRAWN_UNDER *u, const CTUI_WIDGET *self, int row,
                          int col, const CTUI_LOOK_CONTROL *c, int cols,
                          int rows) {
  return ctui_drawn_under_z(u, self, row, col, c, cols, rows,
                            CTUI_DRAWN_UNDER_Z);
}

int ctui_drawn_under_z(CTUI_DRAWN_UNDER *u, const CTUI_WIDGET *self, int row,
                       int col, const CTUI_LOOK_CONTROL *c, int cols, int rows,
                       int z) {
  const CTUI_LOOK *l = ctui_drawn_look();
  if (!l || cols < 1 || rows < 1 || col < 0 || row < 0 ||
      col + cols > self->w || row + rows > self->h || u->nnext >= 0xffff) {
    return 0;
  }
  unsigned int id = ctui_drawn_id(c, l, cols, rows);
  if (!id) {
    return 0;
  }
  if (u->nnext == u->cap_next) {
    int cap = u->cap_next ? u->cap_next * 2 : 16;
    CTUI_DRAWN_PUT *p = realloc(u->next, (size_t)cap * sizeof *p);
    if (p == NULL) {
      return 0;
    }
    u->next = p;
    u->cap_next = cap;
  }
  u->next[u->nnext++] =
      (CTUI_DRAWN_PUT){id, self->y + row + 1, self->x + col + 1, cols, rows, z};
  return 1;
}

/* placement i's id: the slot it has in the list */
static unsigned int put_id(const CTUI_DRAWN_UNDER *u, int i) {
  return u->base + (unsigned)i + 1;
}

void ctui_drawn_under_end(CTUI_DRAWN_UNDER *u) {
  for (int i = 0; i < u->nput || i < u->nnext; i++) {
    const CTUI_DRAWN_PUT *was = i < u->nput ? &u->put[i] : NULL;
    const CTUI_DRAWN_PUT *now = i < u->nnext ? &u->next[i] : NULL;
    if (was && now && !memcmp(was, now, sizeof *was)) {
      continue;
    }
    /* the same pair again moves it; another image's leaves */
    if (was && (!now || was->id != now->id)) {
      ctui_gfx_kitty_unput(was->id, put_id(u, i));
    }
    if (now) {
      ctui_gfx_kitty_put(now->id, put_id(u, i), now->row, now->col, now->cols,
                         now->rows, now->z);
    }
  }
  CTUI_DRAWN_PUT *t = u->put;
  int cap = u->cap_put;
  u->put = u->next;
  u->cap_put = u->cap_next;
  u->nput = u->nnext;
  u->next = t;
  u->cap_next = cap;
  u->nnext = 0;
}

void ctui_drawn_under_clear(CTUI_DRAWN_UNDER *u) {
  ctui_drawn_under_begin(u);
  ctui_drawn_under_end(u);
  free(u->put);
  free(u->next);
  unsigned int base = u->base;
  *u = (CTUI_DRAWN_UNDER){.base = base};
}
