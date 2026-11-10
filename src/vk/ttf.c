#include "ttf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* sized past JetBrains Mono's own maxp (440 points, 110 contours): an
 * outline past them is refused, drawn as nothing */
#define MAX_CONTOURS 128
#define MAX_POINTS 512
#define MAX_DEPTH 4
#define QUAD_SEGMENTS 8
#define MAX_EDGES (MAX_POINTS * QUAD_SEGMENTS)
#define MAX_CROSSINGS (MAX_CONTOURS * 4)
#define SAMPLES_Y 16

typedef struct {
  int16_t x, y;
  uint8_t on;
} PT;

typedef struct {
  uint16_t contours, points;
  uint16_t end[MAX_CONTOURS];
  PT pt[MAX_POINTS];
} OUTLINE;

typedef struct {
  float x, y;
  uint8_t on;
} FPT;

typedef struct {
  float x0, y0, x1, y1;
} EDGE;

typedef struct {
  float x;
  int wind;
} CROSS;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int fits(size_t size, uint32_t off, uint32_t len) {
  return off <= size && len <= size - off;
}

/* the format 4 subtable: Windows Unicode BMP first, then Unicode's */
static void pick_cmap4(CTUI_TTF *f, uint32_t cmap) {
  if (!cmap || !fits(f->size, cmap, 4)) {
    return;
  }
  uint16_t n = be16(f->data + cmap + 2);
  if (!fits(f->size, cmap + 4, (uint32_t)n * 8)) {
    return;
  }
  int best = 100;
  uint32_t at = 0;
  for (uint16_t i = 0; i < n; i++) {
    const uint8_t *rec = f->data + cmap + 4 + (uint32_t)i * 8;
    uint16_t pid = be16(rec), eid = be16(rec + 2);
    uint32_t sub = cmap + be32(rec + 4);
    if (!fits(f->size, sub, 14) || be16(f->data + sub) != 4) {
      continue;
    }
    int rank = pid == 3 && eid == 1 ? 0 : pid == 0 && (eid == 3 || eid == 4) ? 1 : 2;
    if (rank < best) {
      best = rank;
      at = sub;
    }
  }
  if (!at) {
    return;
  }
  uint16_t len = be16(f->data + at + 2), segx2 = be16(f->data + at + 6);
  if (!fits(f->size, at, len) || 14u + (uint32_t)segx2 * 4 + 2 > len) {
    return;
  }
  f->cmap4 = at;
  f->cmap4_len = len;
}

int ctui_ttf_load(CTUI_TTF *f, const uint8_t *data, size_t size) {
  memset(f, 0, sizeof *f);
  f->data = data;
  f->size = size;
  if (!fits(size, 0, 12) || be32(data) != 0x00010000u) {
    return -1; /* not glyf TrueType ('OTTO' is CFF) */
  }
  uint16_t n = be16(data + 4);
  if (!fits(size, 12, (uint32_t)n * 16)) {
    return -1;
  }
  uint32_t head = 0, maxp = 0, hhea = 0, cmap = 0;
  for (uint16_t i = 0; i < n; i++) {
    const uint8_t *rec = data + 12 + (uint32_t)i * 16;
    uint32_t off = be32(rec + 8), len = be32(rec + 12);
    if (!fits(size, off, len)) {
      return -1;
    }
    if (!memcmp(rec, "head", 4)) {
      head = off;
    } else if (!memcmp(rec, "maxp", 4)) {
      maxp = off;
    } else if (!memcmp(rec, "hhea", 4)) {
      hhea = off;
    } else if (!memcmp(rec, "hmtx", 4)) {
      f->hmtx = off;
    } else if (!memcmp(rec, "cmap", 4)) {
      cmap = off;
    } else if (!memcmp(rec, "loca", 4)) {
      f->loca = off;
      f->loca_len = len;
    } else if (!memcmp(rec, "glyf", 4)) {
      f->glyf = off;
      f->glyf_len = len;
    }
  }
  if (!head || !maxp || !hhea || !f->hmtx || !f->loca || !f->glyf ||
      !fits(size, head, 54) || be32(data + head + 12) != 0x5F0F3CF5u ||
      !fits(size, maxp, 6) || be32(data + maxp) != 0x00010000u ||
      !fits(size, hhea, 36)) {
    return -1;
  }
  f->upem = be16(data + head + 18);
  f->loca_long = (int16_t)be16(data + head + 50);
  f->glyphs = be16(data + maxp + 4);
  f->ascender = (int16_t)be16(data + hhea + 4);
  f->descender = (int16_t)be16(data + hhea + 6);
  f->line_gap = (int16_t)be16(data + hhea + 8);
  f->hmetrics = be16(data + hhea + 34);
  if (!f->upem || !f->hmetrics || f->hmetrics > f->glyphs ||
      !fits(size, f->hmtx, (uint32_t)f->hmetrics * 4) ||
      f->ascender <= f->descender) {
    return -1;
  }
  pick_cmap4(f, cmap);
  return 0;
}

uint16_t ctui_ttf_glyph(const CTUI_TTF *f, uint32_t cp) {
  if (!f->cmap4 || cp > 0xffffu) {
    return 0;
  }
  const uint8_t *base = f->data + f->cmap4;
  uint16_t segx2 = be16(base + 6), segs = segx2 / 2;
  const uint8_t *ends = base + 14, *starts = ends + segx2 + 2;
  const uint8_t *deltas = starts + segx2, *ranges = deltas + segx2;
  for (uint16_t i = 0; i < segs; i++) {
    if (cp > be16(ends + i * 2u)) {
      continue;
    }
    uint16_t start = be16(starts + i * 2u);
    if (cp < start) {
      return 0;
    }
    uint16_t delta = be16(deltas + i * 2u), range = be16(ranges + i * 2u);
    if (!range) {
      return (uint16_t)(cp + delta);
    }
    /* idRangeOffset counts from its own slot */
    uint32_t at = (uint32_t)(ranges + i * 2u - f->data) + range + (cp - start) * 2u;
    if (at + 2 > f->cmap4 + f->cmap4_len) {
      return 0;
    }
    uint16_t g = be16(f->data + at);
    return g ? (uint16_t)(g + delta) : 0;
  }
  return 0;
}

static uint16_t advance(const CTUI_TTF *f, uint16_t g) {
  uint16_t i = g < f->hmetrics ? g : (uint16_t)(f->hmetrics - 1);
  return be16(f->data + f->hmtx + i * 4u);
}

static int glyph_span(const CTUI_TTF *f, uint16_t g, uint32_t *off, uint32_t *len) {
  uint32_t a, b;
  if (!f->loca_long) {
    if (!fits(f->loca_len, (uint32_t)g * 2, 4)) {
      return -1;
    }
    a = be16(f->data + f->loca + g * 2u) * 2u;
    b = be16(f->data + f->loca + g * 2u + 2) * 2u;
  } else {
    if (!fits(f->loca_len, (uint32_t)g * 4, 8)) {
      return -1;
    }
    a = be32(f->data + f->loca + g * 4u);
    b = be32(f->data + f->loca + g * 4u + 4);
  }
  if (b < a || !fits(f->glyf_len, a, b - a)) {
    return -1;
  }
  *off = f->glyf + a;
  *len = b - a;
  return 0;
}

static int decode_simple(const CTUI_TTF *f, uint32_t off, uint32_t len, uint16_t nc,
                         OUTLINE *o, int dx, int dy) {
  const uint8_t *g = f->data + off;
  uint32_t p = 10;
  if (o->contours + nc > MAX_CONTOURS || !fits(len, p, nc * 2u)) {
    return -1;
  }
  uint16_t first = o->points, count = 0;
  for (uint16_t c = 0; c < nc; c++, p += 2) {
    uint16_t end = be16(g + p);
    if (c && end < count) {
      return -1; /* contours out of order: their ranges would overlap */
    }
    o->end[o->contours + c] = (uint16_t)(first + end);
    count = (uint16_t)(end + 1);
  }
  o->contours = (uint16_t)(o->contours + nc);
  if (!fits(len, p, 2)) {
    return -1;
  }
  p += 2u + be16(g + p); /* past the hinting instructions */
  if (p > len || first + count > MAX_POINTS) {
    return -1;
  }
  uint8_t flags[MAX_POINTS];
  uint16_t n = 0;
  while (n < count) {
    if (!fits(len, p, 1)) {
      return -1;
    }
    uint8_t fl = g[p++];
    flags[n++] = fl;
    if (fl & 0x08u) { /* repeated */
      if (!fits(len, p, 1)) {
        return -1;
      }
      for (uint8_t r = g[p++]; r && n < count; r--) {
        flags[n++] = fl;
      }
    }
  }
  int x = 0;
  for (uint16_t i = 0; i < count; i++) {
    uint8_t fl = flags[i];
    if (fl & 0x02u) {
      if (!fits(len, p, 1)) {
        return -1;
      }
      x += fl & 0x10u ? g[p] : -g[p];
      p++;
    } else if (!(fl & 0x10u)) {
      if (!fits(len, p, 2)) {
        return -1;
      }
      x += (int16_t)be16(g + p);
      p += 2;
    }
    o->pt[first + i].x = (int16_t)(x + dx);
  }
  int y = 0;
  for (uint16_t i = 0; i < count; i++) {
    uint8_t fl = flags[i];
    if (fl & 0x04u) {
      if (!fits(len, p, 1)) {
        return -1;
      }
      y += fl & 0x20u ? g[p] : -g[p];
      p++;
    } else if (!(fl & 0x20u)) {
      if (!fits(len, p, 2)) {
        return -1;
      }
      y += (int16_t)be16(g + p);
      p += 2;
    }
    o->pt[first + i].y = (int16_t)(y + dy);
    o->pt[first + i].on = fl & 0x01u;
  }
  o->points = (uint16_t)(first + count);
  return 0;
}

static int decode(const CTUI_TTF *f, uint16_t gid, OUTLINE *o, int dx, int dy, int depth);

/* components placed by an x, y offset; point matching and scaled or
 * turned ones refused (none in JetBrains Mono) */
static int decode_composite(const CTUI_TTF *f, uint32_t off, uint32_t len, OUTLINE *o,
                            int dx, int dy, int depth) {
  const uint8_t *g = f->data + off;
  uint32_t p = 10;
  for (int more = 1; more;) {
    if (!fits(len, p, 4)) {
      return -1;
    }
    uint16_t fl = be16(g + p), gid = be16(g + p + 2);
    p += 4;
    if (!(fl & 0x0002u) || (fl & (0x0008u | 0x0040u | 0x0080u))) {
      return -1;
    }
    int a, b;
    if (fl & 0x0001u) {
      if (!fits(len, p, 4)) {
        return -1;
      }
      a = (int16_t)be16(g + p);
      b = (int16_t)be16(g + p + 2);
      p += 4;
    } else {
      if (!fits(len, p, 2)) {
        return -1;
      }
      a = (int8_t)g[p];
      b = (int8_t)g[p + 1];
      p += 2;
    }
    if (decode(f, gid, o, dx + a, dy + b, depth + 1)) {
      return -1;
    }
    more = fl & 0x0020u;
  }
  return 0;
}

static int decode(const CTUI_TTF *f, uint16_t gid, OUTLINE *o, int dx, int dy, int depth) {
  uint32_t off, len;
  if (gid >= f->glyphs || depth >= MAX_DEPTH || glyph_span(f, gid, &off, &len)) {
    return -1;
  }
  if (!len) {
    return 0; /* a space */
  }
  if (len < 10) {
    return -1;
  }
  int16_t nc = (int16_t)be16(f->data + off);
  return nc >= 0 ? decode_simple(f, off, len, (uint16_t)nc, o, dx, dy)
                 : decode_composite(f, off, len, o, dx, dy, depth);
}

void ctui_ttf_cell(const CTUI_TTF *f, float em, int *cw, int *ch, int *base) {
  float s = em / f->upem;
  /* the advance rounded, as FreeType hints it and kitty takes it (13.2 px
   * at 22 px -- 11 pt at 1.5 -- is 13, not 14); the height rounded up */
  int w = (int)lroundf(advance(f, ctui_ttf_glyph(f, 'M')) * s);
  *cw = w > 0 ? w : 1;
  *ch = (int)ceilf((f->ascender - f->descender + f->line_gap) * s - 1e-3f);
  *base = (int)ceilf(f->ascender * s - 1e-3f);
}

static void edge(EDGE *e, int *n, float x0, float y0, float x1, float y1) {
  if (y0 != y1 && *n < MAX_EDGES) {
    e[(*n)++] = (EDGE){x0, y0, x1, y1};
  }
}

static void quad(EDGE *e, int *n, float x0, float y0, float cx, float cy, float x1,
                 float y1) {
  float px = x0, py = y0;
  for (int i = 1; i <= QUAD_SEGMENTS; i++) {
    float t = (float)i / QUAD_SEGMENTS, u = 1.0f - t;
    float qx = u * u * x0 + 2.0f * u * t * cx + t * t * x1;
    float qy = u * u * y0 + 2.0f * u * t * cy + t * t * y1;
    edge(e, n, px, py, qx, qy);
    px = qx;
    py = qy;
  }
}

/* a contour into edges: on-curve points joined by lines, an off-curve
 * one a quadratic's control, two off-curve in a row an implied on-curve
 * point halfway between them */
static void flatten(const FPT *pt, int start, int count, EDGE *e, int *n) {
  if (count < 2) {
    return;
  }
  int first = -1;
  for (int i = 0; i < count && first < 0; i++) {
    if (pt[start + i].on) {
      first = i;
    }
  }
  float sx, sy;
  if (first >= 0) {
    sx = pt[start + first].x;
    sy = pt[start + first].y;
  } else { /* none on the curve: start halfway from the last to the first */
    sx = (pt[start + count - 1].x + pt[start].x) * 0.5f;
    sy = (pt[start + count - 1].y + pt[start].y) * 0.5f;
  }
#define AT(k) (pt[start + ((first + 1 + (k)) % count + count) % count])
  float ax = sx, ay = sy;
  for (int i = 0; i < count;) {
    if (AT(i).on) {
      edge(e, n, ax, ay, AT(i).x, AT(i).y);
      ax = AT(i).x;
      ay = AT(i).y;
      i++;
      continue;
    }
    float cx = AT(i).x, cy = AT(i).y, nx = sx, ny = sy;
    int next_on = 1;
    if (i + 1 < count) {
      nx = AT(i + 1).x;
      ny = AT(i + 1).y;
      next_on = AT(i + 1).on;
    }
    if (next_on) {
      quad(e, n, ax, ay, cx, cy, nx, ny);
      ax = nx;
      ay = ny;
      i += 2;
    } else {
      float mx = (cx + nx) * 0.5f, my = (cy + ny) * 0.5f;
      quad(e, n, ax, ay, cx, cy, mx, my);
      ax = mx;
      ay = my;
      i++;
    }
  }
#undef AT
  if (ax != sx || ay != sy) {
    edge(e, n, ax, ay, sx, sy);
  }
}

static void span(float *row, int w, float x0, float x1, float weight) {
  x0 = x0 < 0 ? 0 : x0;
  x1 = x1 > (float)w ? (float)w : x1;
  if (x1 <= x0) {
    return;
  }
  int c0 = (int)x0, c1 = (int)x1;
  if (c1 >= w) {
    c1 = w - 1;
  }
  if (c0 == c1) {
    row[c0] += weight * (x1 - x0);
    return;
  }
  row[c0] += weight * ((float)(c0 + 1) - x0);
  for (int c = c0 + 1; c < c1; c++) {
    row[c] += weight;
  }
  row[c1] += weight * (x1 - (float)c1);
}

static void scan_row(const EDGE *e, int n, int y, int w, float *row, CROSS *cr) {
  memset(row, 0, sizeof *row * (size_t)w);
  for (int sub = 0; sub < SAMPLES_Y; sub++) {
    float ys = (float)y + ((float)sub + 0.5f) / SAMPLES_Y;
    int nc = 0;
    for (int i = 0; i < n && nc < MAX_CROSSINGS; i++) {
      float y0 = e[i].y0, y1 = e[i].y1;
      if (ys < (y0 < y1 ? y0 : y1) || ys >= (y0 < y1 ? y1 : y0)) {
        continue;
      }
      cr[nc].x = e[i].x0 + (ys - y0) / (y1 - y0) * (e[i].x1 - e[i].x0);
      cr[nc].wind = y1 > y0 ? 1 : -1;
      nc++;
    }
    for (int a = 1; a < nc; a++) {
      CROSS k = cr[a];
      int b = a - 1;
      for (; b >= 0 && cr[b].x > k.x; b--) {
        cr[b + 1] = cr[b];
      }
      cr[b + 1] = k;
    }
    int wind = 0;
    for (int c = 0; c + 1 < nc; c++) {
      wind += cr[c].wind;
      if (wind) {
        span(row, w, cr[c].x, cr[c + 1].x, 1.0f / SAMPLES_Y);
      }
    }
  }
}

int ctui_ttf_draw(const CTUI_TTF *f, uint16_t glyph, float em, int cw, int ch,
                  int base, int stretch, uint8_t *cov) {
  memset(cov, 0, (size_t)cw * (size_t)ch);
  OUTLINE *o = malloc(sizeof *o);
  if (!o) {
    return -1;
  }
  o->contours = o->points = 0;
  if (decode(f, glyph, o, 0, 0, 0)) {
    free(o);
    return -1;
  }
  if (!o->points) {
    free(o);
    return 0;
  }
  float s = em / f->upem, sx = s, sy = s;
  float ox = ((float)cw - advance(f, glyph) * s) * 0.5f, oy = (float)base;
  if (stretch) {
    sx = (float)cw / advance(f, glyph);
    sy = (float)ch / (float)(f->ascender - f->descender);
    ox = 0;
    oy = f->ascender * sy;
  }
  FPT *pt = malloc(sizeof *pt * o->points);
  EDGE *e = malloc(sizeof *e * MAX_EDGES);
  CROSS *cr = malloc(sizeof *cr * MAX_CROSSINGS);
  float *row = malloc(sizeof *row * (size_t)cw);
  int ok = pt && e && cr && row;
  if (ok) {
    for (uint16_t i = 0; i < o->points; i++) {
      pt[i] = (FPT){ox + o->pt[i].x * sx, oy - o->pt[i].y * sy, o->pt[i].on};
    }
    int n = 0, start = 0;
    for (uint16_t c = 0; c < o->contours; c++) {
      flatten(pt, start, o->end[c] - start + 1, e, &n);
      start = o->end[c] + 1;
    }
    for (int y = 0; y < ch; y++) {
      scan_row(e, n, y, cw, row, cr);
      for (int x = 0; x < cw; x++) {
        float v = row[x] < 0 ? 0 : row[x] > 1 ? 1 : row[x];
        cov[(size_t)y * cw + x] = (uint8_t)(v * 255.0f + 0.5f);
      }
    }
  }
  free(pt);
  free(e);
  free(cr);
  free(row);
  free(o);
  return ok ? 0 : -1;
}
