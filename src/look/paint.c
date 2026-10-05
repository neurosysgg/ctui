#include "paint.h"

#include <string.h>

void ctui_look_paint_px(CTUI_LOOK_CANVAS *c, int x, int y,
                        const unsigned char rgb[3]) {
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) {
    return;
  }
  unsigned char *p = c->px + ((size_t)y * (size_t)c->w + (size_t)x) * 4;
  memcpy(p, rgb, 3);
  p[3] = 255;
}

void ctui_look_paint_rect(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                          const unsigned char rgb[3]) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
  for (int yy = y0; yy < y1; yy++) {
    for (int xx = x0; xx < x1; xx++) {
      ctui_look_paint_px(c, xx, yy, rgb);
    }
  }
}

static int min2(int a, int b) { return a < b ? a : b; }

void ctui_look_paint_box(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                         const CTUI_LOOK *l, CTUI_LOOK_PAINT_EDGE edge,
                         int depth, int outline, const unsigned char *fill) {
  static const int RAISED[2][2][2] = {
      /* depth 1: [ring][tl, br] */
      {{CTUI_LOOK_HIGHLIGHT, CTUI_LOOK_SHADOW}, {0, 0}},
      /* depth 2 */
      {{CTUI_LOOK_HIGHLIGHT, CTUI_LOOK_DARK},
       {CTUI_LOOK_LIGHT, CTUI_LOOK_SHADOW}},
  };
  static const int SUNKEN[2][2][2] = {
      {{CTUI_LOOK_SHADOW, CTUI_LOOK_HIGHLIGHT}, {0, 0}},
      {{CTUI_LOOK_SHADOW, CTUI_LOOK_HIGHLIGHT},
       {CTUI_LOOK_DARK, CTUI_LOOK_LIGHT}},
  };
  depth = edge == CTUI_LOOK_PAINT_FLAT ? 0 : depth < 0 ? 0 : min2(depth, 2);
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int t = j, lt = i, b = h - 1 - j, r = w - 1 - i;
      if (l->corner && min2(t, b) + min2(lt, r) < l->corner) {
        continue;
      }
      int k = min2(min2(t, b), min2(lt, r));
      const unsigned char *rgb = fill;
      if (outline && k == 0) {
        rgb = l->color[CTUI_LOOK_DARK];
      } else if ((k -= !!outline) < depth) {
        int br = b - !!outline == k || r - !!outline == k;
        const int (*tab)[2][2] =
            edge == CTUI_LOOK_PAINT_RAISED ? RAISED : SUNKEN;
        rgb = l->color[tab[depth - 1][k][br]];
      }
      if (rgb) {
        ctui_look_paint_px(c, x + i, y + j, rgb);
      }
    }
  }
}

void ctui_look_paint_fill(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                          int fill, const CTUI_LOOK *l,
                          const unsigned char rgb[3]) {
  fill = fill < 0 ? 0 : min2(fill, w);
  if (!l->chunk) {
    ctui_look_paint_rect(c, x, y, fill, h, rgb);
    return;
  }
  for (int at = 0; at < w; at += l->chunk + l->gap) {
    int cw = min2(l->chunk, w - at);
    if (fill < at + (cw + 1) / 2) {
      break;
    }
    ctui_look_paint_rect(c, x + at, y, cw, h, rgb);
  }
}

static int mask_role(char ch) {
  switch (ch) {
  case 'f':
    return CTUI_LOOK_FACE;
  case 'h':
    return CTUI_LOOK_HIGHLIGHT;
  case 'l':
    return CTUI_LOOK_LIGHT;
  case 's':
    return CTUI_LOOK_SHADOW;
  case 'd':
    return CTUI_LOOK_DARK;
  case 'a':
    return CTUI_LOOK_ACCENT;
  case 'A':
    return CTUI_LOOK_ACCENT_TEXT;
  case 'g':
    return CTUI_LOOK_GROOVE;
  case 'x':
    return CTUI_LOOK_DISABLED;
  case 't':
    return CTUI_LOOK_TEXT;
  case 'w':
    return CTUI_LOOK_WARM;
  default:
    return -1;
  }
}

void ctui_look_paint_mask(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                          const CTUI_LOOK_MASK *m, const CTUI_LOOK *l,
                          int off) {
  int s = min2(w / m->w, h / m->h);
  s = s < 1 ? 1 : s;
  int ox = x + (w - m->w * s) / 2, oy = y + (h - m->h * s) / 2;
  for (int j = 0; j < m->h; j++) {
    for (int i = 0; i < m->w; i++) {
      int role = mask_role(m->px[j * m->w + i]);
      if (role < 0) {
        continue;
      }
      const unsigned char *rgb = l->color[off ? CTUI_LOOK_DISABLED : role];
      int px = ox + i * s, py = oy + j * s;
      /* clipped to the box, not just the canvas */
      int x0 = px < x ? x : px, y0 = py < y ? y : py;
      int x1 = min2(px + s, x + w), y1 = min2(py + s, y + h);
      if (x1 > x0 && y1 > y0) {
        ctui_look_paint_rect(c, x0, y0, x1 - x0, y1 - y0, rgb);
      }
    }
  }
}
