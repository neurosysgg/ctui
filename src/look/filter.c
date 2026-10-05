#include "filter.h"

#include "resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *const ctui_look_filter_palette_names[] = {
    "none", "win95", "vga16", "halftone", "grey", "roles", NULL};
const char *const ctui_look_filter_dither_names[] = {"none", "ordered", "fs",
                                                     NULL};

/* bumped when the pipeline changes: the cached icons are filtered anew */
#define VERSION 2

/* the 16 VGA colours, then 95's four more (money green, sky blue, cream,
 * medium grey) */
static const uint32_t SYSTEM[20] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080,
    0xc0c0c0, 0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff,
    0x00ffff, 0xffffff, 0xc0dcc0, 0xa6caf0, 0xfffbf0, 0xa0a0a4};
static const uint32_t GREYS[4] = {0x000000, 0x808080, 0xc0c0c0, 0xffffff};

typedef struct {
  unsigned char rgb[256][3];
  int n;
  int spread; /* an ordered dither's reach: half a step between colours
               * (a whole one checkers every flat area) */
} PALETTE;

static void add(PALETTE *p, uint32_t c) {
  p->rgb[p->n][0] = (unsigned char)(c >> 16);
  p->rgb[p->n][1] = (unsigned char)(c >> 8);
  p->rgb[p->n][2] = (unsigned char)c;
  p->n++;
}

static void palette_of(const CTUI_LOOK *l, PALETTE *p) {
  p->n = 0;
  switch (l->filter.palette) {
  case CTUI_LOOK_FILTER_PALETTE_WIN95:
  case CTUI_LOOK_FILTER_PALETTE_VGA16:
    for (int i = 0;
         i < (l->filter.palette == CTUI_LOOK_FILTER_PALETTE_WIN95 ? 20 : 16);
         i++) {
      add(p, SYSTEM[i]);
    }
    p->spread = 32;
    break;
  case CTUI_LOOK_FILTER_PALETTE_HALFTONE:
    for (int r = 0; r < 6; r++) {
      for (int g = 0; g < 6; g++) {
        for (int b = 0; b < 6; b++) {
          add(p, (uint32_t)(r * 51) << 16 | (uint32_t)(g * 51) << 8 |
                     (uint32_t)(b * 51));
        }
      }
    }
    for (int i = 0; i < 20; i++) {
      add(p, SYSTEM[i]);
    }
    p->spread = 20;
    break;
  case CTUI_LOOK_FILTER_PALETTE_GREY:
    for (int i = 0; i < 4; i++) {
      add(p, GREYS[i]);
    }
    p->spread = 48;
    break;
  case CTUI_LOOK_FILTER_PALETTE_ROLES:
    for (int r = 0; r < CTUI_LOOK_ROLES; r++) {
      memcpy(p->rgb[p->n++], l->color[r], 3);
    }
    p->spread = 32;
    break;
  default:
    break;
  }
}

/* the palette entry nearest (r, g, b), by the "redmean" distance */
static const unsigned char *nearest(const PALETTE *p, float r, float g,
                                    float b) {
  int best = 0;
  float best_d = INFINITY;
  for (int i = 0; i < p->n; i++) {
    float pr = p->rgb[i][0], rm = (r + pr) / 2;
    float dr = r - pr, dg = g - p->rgb[i][1], db = b - p->rgb[i][2];
    float d = (2 + rm / 256) * dr * dr + 4 * dg * dg +
              (2 + (255 - rm) / 256) * db * db;
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return p->rgb[best];
}

static float clampf(float v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* src fitted into the dw x dh box at (ox, oy) of out (size pixels a row),
 * by the area each source pixel covers, over premultiplied colour */
static void downscale(const unsigned char *src, int sw, int sh,
                      unsigned char *out, int size, int ox, int oy, int dw,
                      int dh) {
  ctui_look_resample_area(src, sw, sh,
                          out + ((size_t)oy * (size_t)size + (size_t)ox) * 4,
                          size, dw, dh);
}

static void boost(unsigned char *px, int n, int percent) {
  float k = 1 + (float)percent / 100;
  for (int i = 0; i < n; i++, px += 4) {
    if (!px[3]) {
      continue;
    }
    float v[3];
    for (int c = 0; c < 3; c++) {
      v[c] = 128 + ((float)px[c] - 128) * k;
    }
    float luma = 0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2];
    for (int c = 0; c < 3; c++) {
      px[c] = (unsigned char)clampf(luma + (v[c] - luma) * k + 0.5f);
    }
  }
}

static const int BAYER[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* a table entry not found yet (palettes have fewer colours) */
#define LUT_UNSET 255

/* lut: the palette entry nearest each colour at 5 bits a channel (r << 10
 * | g << 5 | b), LUT_UNSET until a pixel needs it; NULL: searched for each
 * pixel */
static void quantize(unsigned char *px, int w, int h, const PALETTE *p,
                     int dither, unsigned char *lut) {
  float *err = NULL;
  if (dither == CTUI_LOOK_FILTER_DITHER_FS) {
    err = calloc((size_t)w * (size_t)h * 3, sizeof *err);
  }
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      unsigned char *o = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
      if (!o[3]) {
        continue;
      }
      float v[3] = {o[0], o[1], o[2]};
      if (dither == CTUI_LOOK_FILTER_DITHER_ORDERED) {
        float t = ((float)BAYER[y & 3][x & 3] + 0.5f) / 16 - 0.5f;
        for (int c = 0; c < 3; c++) {
          v[c] = clampf(v[c] + t * (float)p->spread);
        }
      } else if (err) {
        float *e = err + ((size_t)y * (size_t)w + (size_t)x) * 3;
        for (int c = 0; c < 3; c++) {
          v[c] = clampf(v[c] + e[c]);
        }
      }
      const unsigned char *q;
      if (lut) {
        int k = (int)v[0] >> 3 << 10 | (int)v[1] >> 3 << 5 | (int)v[2] >> 3;
        if (lut[k] == LUT_UNSET) {
          /* the colours a picture has, found as it meets them */
          q = nearest(p, (float)((k >> 10) * 8 + 4),
                      (float)((k >> 5 & 31) * 8 + 4),
                      (float)((k & 31) * 8 + 4));
          lut[k] = (unsigned char)((q - p->rgb[0]) / 3);
        }
        q = p->rgb[lut[k]];
      } else {
        q = nearest(p, v[0], v[1], v[2]);
      }
      if (err) {
        /* 7/16 right, 3/16 down-left, 5/16 down, 1/16 down-right: onto
         * opaque pixels only (a clear one would carry it off the icon) */
        static const int DX[4] = {1, -1, 0, 1}, DY[4] = {0, 1, 1, 1};
        static const float W[4] = {7.f / 16, 3.f / 16, 5.f / 16, 1.f / 16};
        for (int k = 0; k < 4; k++) {
          int nx = x + DX[k], ny = y + DY[k];
          if (nx < 0 || nx >= w || ny >= h ||
              !px[((size_t)ny * (size_t)w + (size_t)nx) * 4 + 3]) {
            continue;
          }
          float *e = err + ((size_t)ny * (size_t)w + (size_t)nx) * 3;
          for (int c = 0; c < 3; c++) {
            e[c] += (v[c] - q[c]) * W[k];
          }
        }
      }
      memcpy(o, q, 3);
    }
  }
  free(err);
}

/* clear pixels next to the mask (4-neighbours; dx, dy = 1, 1: only the
 * one up-left, a drop shadow) in rgb */
static void ring(unsigned char *px, int size, const unsigned char *rgb,
                 int shadow) {
  unsigned char *mask = calloc((size_t)size * (size_t)size, 1);
  for (int i = 0; i < size * size; i++) {
    mask[i] = px[(size_t)i * 4 + 3] != 0;
  }
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      unsigned char *o = px + ((size_t)y * (size_t)size + (size_t)x) * 4;
      if (mask[y * size + x]) {
        continue;
      }
      int near = shadow ? x > 0 && y > 0 && mask[(y - 1) * size + x - 1]
                        : (x > 0 && mask[y * size + x - 1]) ||
                              (x + 1 < size && mask[y * size + x + 1]) ||
                              (y > 0 && mask[(y - 1) * size + x]) ||
                              (y + 1 < size && mask[(y + 1) * size + x]);
      if (near) {
        memcpy(o, rgb, 3);
        o[3] = 255;
      }
    }
  }
  free(mask);
}

void ctui_look_filter_icon(const unsigned char *src, int sw, int sh,
                           const CTUI_LOOK *l, unsigned char *out) {
  const CTUI_LOOK_FILTER *f = &l->filter;
  int size = f->size;
  memset(out, 0, (size_t)size * (size_t)size * 4);
  if (!src || sw <= 0 || sh <= 0) {
    return;
  }
  /* room for the outline all round and the shadow down-right */
  int margin = f->outline ? 1 : 0, room = size - 2 * margin - !!f->shadow;
  int dw = sw >= sh ? room : (int)((float)room * (float)sw / (float)sh + 0.5f);
  int dh = sh >= sw ? room : (int)((float)room * (float)sh / (float)sw + 0.5f);
  dw = dw < 1 ? 1 : dw;
  dh = dh < 1 ? 1 : dh;
  downscale(src, sw, sh, out, size, margin + (room - dw) / 2,
            margin + (room - dh) / 2, dw, dh);
  int n = size * size;
  if (f->boost) {
    boost(out, n, f->boost);
  }
  if (f->alpha) {
    for (int i = 0; i < n; i++) {
      unsigned char *o = out + (size_t)i * 4;
      if (o[3] >= f->alpha) {
        o[3] = 255;
      } else {
        memset(o, 0, 4);
      }
    }
  }
  PALETTE p;
  palette_of(l, &p);
  if (p.n) {
    quantize(out, size, size, &p, f->dither, NULL);
  }
  if (f->outline) {
    ring(out, size, l->color[CTUI_LOOK_DARK], 0);
  }
  if (f->shadow) {
    ring(out, size, l->color[CTUI_LOOK_SHADOW], 1);
  }
}

static uint64_t mix(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    h = (h ^ ((v >> (8 * i)) & 0xff)) * 0x100000001b3u;
  }
  return h;
}

static uint32_t rgb_of(const unsigned char c[3]) {
  return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
}

uint64_t ctui_look_filter_hash(const CTUI_LOOK *l) {
  const CTUI_LOOK_FILTER *f = &l->filter;
  const int v[] = {VERSION,  f->on,    f->size,    f->palette, f->dither,
                   f->alpha, f->boost, f->outline, f->shadow};
  uint64_t h = 0xcbf29ce484222325u;
  for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
    h = mix(h, (uint32_t)v[i]);
  }
  if (f->palette == CTUI_LOOK_FILTER_PALETTE_ROLES) {
    for (int r = 0; r < CTUI_LOOK_ROLES; r++) {
      h = mix(h, rgb_of(l->color[r]));
    }
  }
  if (f->outline) {
    h = mix(h, rgb_of(l->color[CTUI_LOOK_DARK]));
  }
  if (f->shadow) {
    h = mix(h, rgb_of(l->color[CTUI_LOOK_SHADOW]));
  }
  return h;
}

void ctui_look_filter_web(const unsigned char *src, int sw, int sh,
                          unsigned char *out, int w, int h) {
  memset(out, 0, (size_t)w * (size_t)h * 4);
  if (!src || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) {
    return;
  }
  if (sw >= w && sh >= h) {
    downscale(src, sw, sh, out, w, 0, 0, w, h);
  } else {
    /* bigger: nearest neighbour (its pixels show, as on a 90s screen) */
    for (int y = 0; y < h; y++) {
      size_t sy = (size_t)y * (size_t)sh / (size_t)h;
      for (int x = 0; x < w; x++) {
        size_t sx = (size_t)x * (size_t)sw / (size_t)w;
        memcpy(out + ((size_t)y * (size_t)w + (size_t)x) * 4,
               src + (sy * (size_t)sw + sx) * 4, 4);
      }
    }
  }
  /* GIF's 1-bit transparency */
  for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
    unsigned char *o = out + i * 4;
    if (o[3] >= 128) {
      o[3] = 255;
    } else {
      memset(o, 0, 4);
    }
  }
  /* the palette made once, its table filled as pictures need it (all
   * 32K colours at once: ~20 ms before the first picture) */
  static PALETTE p;
  static unsigned char lut[1 << 15];
  if (!p.n) {
    palette_of(
        &(CTUI_LOOK){.filter.palette = CTUI_LOOK_FILTER_PALETTE_HALFTONE}, &p);
    memset(lut, LUT_UNSET, sizeof lut);
  }
  quantize(out, w, h, &p, CTUI_LOOK_FILTER_DITHER_FS, lut);
}

int ctui_look_filter_upscale(const unsigned char *src, int sw, int sh,
                             unsigned char *out, int w, int h) {
  int k = sw > 0 && sh > 0 ? (w / sw < h / sh ? w / sw : h / sh) : 0;
  if (k < 1) {
    return -1;
  }
  memset(out, 0, (size_t)w * (size_t)h * 4);
  int ox = (w - sw * k) / 2, oy = (h - sh * k) / 2;
  for (int y = 0; y < sh * k; y++) {
    for (int x = 0; x < sw * k; x++) {
      memcpy(out + ((size_t)(oy + y) * (size_t)w + (size_t)(ox + x)) * 4,
             src + ((size_t)(y / k) * (size_t)sw + (size_t)(x / k)) * 4, 4);
    }
  }
  return 0;
}
