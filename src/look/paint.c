#include "paint.h"

#include <math.h>
#include <stdlib.h>
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
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  size_t stride = (size_t)c->w * 4, n = (size_t)(x1 - x0) * 4;
  unsigned char *row = c->px + (size_t)y0 * stride + (size_t)x0 * 4;
  memcpy(row, rgb, 3);
  row[3] = 255;
  for (size_t done = 4; done < n;) {
    size_t k = done < n - done ? done : n - done;
    memcpy(row + done, row, k);
    done += k;
  }
  for (int yy = y0 + 1; yy < y1; yy++) {
    memcpy(row + (size_t)(yy - y0) * stride, row, n);
  }
}

static int min2(int a, int b) { return a < b ? a : b; }

static void box_px(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                   const CTUI_LOOK *l, CTUI_LOOK_PAINT_EDGE edge, int depth,
                   int outline, const unsigned char *fill, int i, int j) {
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
  int t = j, lt = i, b = h - 1 - j, r = w - 1 - i;
  if (l->corner && min2(t, b) + min2(lt, r) < l->corner) {
    return;
  }
  int k = min2(min2(t, b), min2(lt, r));
  const unsigned char *rgb = fill;
  if (outline && k == 0) {
    rgb = l->color[CTUI_LOOK_DARK];
  } else if ((k -= !!outline) < depth) {
    int br = b - !!outline == k || r - !!outline == k;
    const int (*tab)[2][2] = edge == CTUI_LOOK_PAINT_RAISED ? RAISED : SUNKEN;
    rgb = l->color[tab[depth - 1][k][br]];
  }
  if (rgb) {
    ctui_look_paint_px(c, x + i, y + j, rgb);
  }
}

/* only the ring (and the corner cut's reach) goes pixel by pixel: a row's
 * middle, `m` px in from either side, is all fill and never cut */
void ctui_look_paint_box(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                         const CTUI_LOOK *l, CTUI_LOOK_PAINT_EDGE edge,
                         int depth, int outline, const unsigned char *fill) {
  depth = edge == CTUI_LOOK_PAINT_FLAT ? 0 : depth < 0 ? 0 : min2(depth, 2);
  int ring = depth + !!outline;
  for (int j = 0; j < h; j++) {
    int tb = min2(j, h - 1 - j);
    int m = l->corner - tb > ring ? l->corner - tb : ring;
    if (tb < ring || 2 * m >= w) {
      for (int i = 0; i < w; i++) {
        box_px(c, x, y, w, h, l, edge, depth, outline, fill, i, j);
      }
      continue;
    }
    for (int i = 0; i < m; i++) {
      box_px(c, x, y, w, h, l, edge, depth, outline, fill, i, j);
      box_px(c, x, y, w, h, l, edge, depth, outline, fill, w - 1 - i, j);
    }
    if (fill) {
      ctui_look_paint_rect(c, x + m, y + j, w - 2 * m, 1, fill);
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

/* a clip's rectangle inside the canvas: x0, y0, x1, y1 (end exclusive) */
static void clip_box(const CTUI_LOOK_CANVAS *c, const CTUI_LOOK_RECT *clip,
                     int box[4]) {
  box[0] = 0;
  box[1] = 0;
  box[2] = c->w;
  box[3] = c->h;
  if (clip) {
    box[0] = clip->x > 0 ? clip->x : 0;
    box[1] = clip->y > 0 ? clip->y : 0;
    box[2] = clip->x + clip->w < c->w ? clip->x + clip->w : c->w;
    box[3] = clip->y + clip->h < c->h ? clip->y + clip->h : c->h;
  }
}

/* a segment a-b and a line hw px either side of it, set up once: how much
 * of a pixel it covers is then seg_cover()'s */
typedef struct {
  float ax, ay, dx, dy, inv_l2; /* inv_l2 0: a point */
  float reach, full2; /* (hw + 0.5)^2: none past; (hw - 0.5)^2: whole */
} SEG;

static void seg_init(SEG *g, float ax, float ay, float bx, float by, float hw) {
  g->ax = ax;
  g->ay = ay;
  g->dx = bx - ax;
  g->dy = by - ay;
  float l2 = g->dx * g->dx + g->dy * g->dy;
  g->inv_l2 = l2 > 0.0f ? 1.0f / l2 : 0.0f;
  g->reach = hw + 0.5f;
  g->full2 = hw > 0.5f ? (hw - 0.5f) * (hw - 0.5f) : 0.0f;
}

/* how much of the pixel centred at (px, py) the line covers, of 1: a box
 * filter's width, 1 px, across the edge */
static float seg_cover(const SEG *g, float px, float py) {
  float t = ((px - g->ax) * g->dx + (py - g->ay) * g->dy) * g->inv_l2;
  t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
  float ex = px - (g->ax + t * g->dx), ey = py - (g->ay + t * g->dy);
  float d2 = ex * ex + ey * ey;
  if (d2 >= g->reach * g->reach) {
    return 0.0f;
  }
  if (d2 <= g->full2) {
    return 1.0f;
  }
  float cov = g->reach - sqrtf(d2);
  return cov > 1.0f ? 1.0f : cov;
}

static float min_f(float a, float b) { return a < b ? a : b; }
static float max_f(float a, float b) { return a > b ? a : b; }

static int floor_i(float v) { return (int)floorf(v); }

/* rgb over the pixel p at alpha a (of 255): "over", a clear pixel taking
 * rgb at a */
static void cover_px(unsigned char *p, const unsigned char rgb[3], int a) {
  if (a == 0) {
    return;
  }
  if (a == 255 || p[3] == 0) {
    memcpy(p, rgb, 3);
    p[3] = (unsigned char)a;
    return;
  }
  /* out_a = a + da (1 - a); out = (rgb a + d da (1 - a)) / out_a */
  int da = p[3] * (255 - a), oa = a * 255 + da;
  for (int k = 0; k < 3; k++) {
    p[k] = (unsigned char)((rgb[k] * a * 255 + p[k] * da + oa / 2) / oa);
  }
  p[3] = (unsigned char)((oa + 127) / 255);
}

void ctui_look_paint_polyline(CTUI_LOOK_CANVAS *c, const float *xy, int n,
                              float width, const CTUI_LOOK_RECT *clip,
                              const unsigned char rgb[3]) {
  if (n < 1 || !(width > 0.0f)) {
    return;
  }
  /* thinner than a pixel: a pixel's width, fainter */
  float hw = width < 1.0f ? 0.5f : width * 0.5f;
  float gain = width < 1.0f ? width : 1.0f;
  int box[4];
  clip_box(c, clip, box);
  int cx0 = box[0], cy0 = box[1], cx1 = box[2], cy1 = box[3];
  /* the mask: the points' box grown by the line's reach, clipped */
  float lo_x = xy[0], hi_x = xy[0], lo_y = xy[1], hi_y = xy[1];
  for (int i = 1; i < n; i++) {
    lo_x = min_f(lo_x, xy[2 * i]);
    hi_x = max_f(hi_x, xy[2 * i]);
    lo_y = min_f(lo_y, xy[2 * i + 1]);
    hi_y = max_f(hi_y, xy[2 * i + 1]);
  }
  float reach = hw + 0.5f;
  int mx0 = floor_i(lo_x - reach) + 1, my0 = floor_i(lo_y - reach) + 1;
  int mx1 = floor_i(hi_x + reach) + 1, my1 = floor_i(hi_y + reach) + 1;
  mx0 = mx0 > cx0 ? mx0 : cx0;
  my0 = my0 > cy0 ? my0 : cy0;
  mx1 = mx1 < cx1 ? mx1 : cx1;
  my1 = my1 < cy1 ? my1 : cy1;
  if (mx1 <= mx0 || my1 <= my0) {
    return;
  }
  int mw = mx1 - mx0, mh = my1 - my0;
  /* each column's rows the line reached: only they are blended (a curve
   * across a tall box touches a few rows of each column) */
  unsigned char *mask = calloc((size_t)mw * (size_t)mh, 1);
  int *span = malloc((size_t)mw * 2 * sizeof *span);
  if (!mask || !span) {
    free(mask);
    free(span);
    return;
  }
  for (int x = 0; x < mw; x++) {
    span[2 * x] = mh;
    span[2 * x + 1] = -1;
  }
  for (int i = 0; i < (n > 1 ? n - 1 : 1); i++) {
    float ax = xy[2 * i], ay = xy[2 * i + 1];
    float bx = n > 1 ? xy[2 * i + 2] : ax, by = n > 1 ? xy[2 * i + 3] : ay;
    SEG g;
    seg_init(&g, ax, ay, bx, by, hw);
    float sx0 = min_f(ax, bx), sx1 = max_f(ax, bx);
    int x0 = floor_i(sx0 - reach) + 1, x1 = floor_i(sx1 + reach) + 1;
    int y0 = floor_i(min_f(ay, by) - reach) + 1;
    int y1 = floor_i(max_f(ay, by) + reach) + 1;
    x0 = x0 > mx0 ? x0 : mx0;
    x1 = x1 < mx1 ? x1 : mx1;
    y0 = y0 > my0 ? y0 : my0;
    y1 = y1 < my1 ? y1 : my1;
    /* a long slanted segment column by column, only the rows within
     * reach of its part within reach of the column; a short one its box */
    int band = (x1 - x0) * (y1 - y0) > 64 && sx1 - sx0 > 1e-6f;
    float slope = band ? (by - ay) / (bx - ax) : 0.0f;
    for (int x = x0; x < x1; x++) {
      int ry0 = y0, ry1 = y1;
      if (band) {
        float xa = max_f(sx0, (float)x - reach);
        float xb = min_f(sx1, (float)x + reach);
        float ya = ay + (xa - ax) * slope, yb = ay + (xb - ax) * slope;
        int b0 = floor_i(min_f(ya, yb) - reach) + 1;
        int b1 = floor_i(max_f(ya, yb) + reach) + 1;
        ry0 = b0 > y0 ? b0 : y0;
        ry1 = b1 < y1 ? b1 : y1;
      }
      int *sp = span + 2 * (x - mx0);
      for (int y = ry0; y < ry1; y++) {
        unsigned char *m = mask + (size_t)(y - my0) * (size_t)mw + (x - mx0);
        float cov = seg_cover(&g, (float)x, (float)y) * gain;
        int a = (int)(cov * 255.0f + 0.5f);
        if (a > *m) {
          *m = (unsigned char)a;
          sp[0] = y - my0 < sp[0] ? y - my0 : sp[0];
          sp[1] = y - my0 > sp[1] ? y - my0 : sp[1];
        }
      }
    }
  }
  for (int x = 0; x < mw; x++) {
    for (int y = span[2 * x]; y <= span[2 * x + 1]; y++) {
      cover_px(c->px +
                   ((size_t)(y + my0) * (size_t)c->w + (size_t)(x + mx0)) * 4,
               rgb, mask[(size_t)y * (size_t)mw + (size_t)x]);
    }
  }
  free(mask);
  free(span);
}

void ctui_look_paint_disc(CTUI_LOOK_CANVAS *c, float cx, float cy, float r,
                          float r_in, const CTUI_LOOK_RECT *clip,
                          const unsigned char rgb[3]) {
  if (!(r > 0.0f) || !(r > r_in)) {
    return;
  }
  int box[4];
  clip_box(c, clip, box);
  float reach = r + 0.5f;
  int x0 = floor_i(cx - reach) + 1, x1 = floor_i(cx + reach) + 1;
  int y0 = floor_i(cy - reach) + 1, y1 = floor_i(cy + reach) + 1;
  x0 = x0 > box[0] ? x0 : box[0];
  y0 = y0 > box[1] ? y0 : box[1];
  x1 = x1 < box[2] ? x1 : box[2];
  y1 = y1 < box[3] ? y1 : box[3];
  for (int y = y0; y < y1; y++) {
    unsigned char *p = c->px + ((size_t)y * (size_t)c->w + (size_t)x0) * 4;
    for (int x = x0; x < x1; x++, p += 4) {
      float ex = (float)x - cx, ey = (float)y - cy;
      float d = sqrtf(ex * ex + ey * ey);
      /* the outer edge's coverage, then the hole's */
      float cov = reach - d;
      if (r_in > 0.0f) {
        float in = d - r_in + 0.5f;
        cov = in < cov ? in : cov;
      }
      cov = cov < 0.0f ? 0.0f : cov > 1.0f ? 1.0f : cov;
      cover_px(p, rgb, (int)(cov * 255.0f + 0.5f));
    }
  }
}
