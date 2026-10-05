/* A box filter in integers, separable (resample.h). */
#include "resample.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* a weight's whole: an axis' weights for one output pixel sum to it */
#define ONE (1 << 14)

/* the source pixels output pixel d of an axis covers: from at[d], n[d] of
 * them, their weights in w[off[d]...] */
typedef struct {
  int *at, *n, *off;
  uint16_t *w;
} AXIS;

static void axis_free(AXIS *a) {
  free(a->at);
  free(a->n);
  free(a->off);
  free(a->w);
}

/* s source pixels onto d output ones: output i spans [i*s, (i+1)*s) and
 * source j [j*d, (j+1)*d) in units of 1/d source pixel, so the overlaps
 * are exact; the weights rounded where they add up (each the difference
 * of two rounded running sums), so they sum to ONE exactly. 0, or -1
 * without memory */
static int axis_make(AXIS *a, int s, int d) {
  a->at = calloc((size_t)d, sizeof *a->at);
  a->n = calloc((size_t)d, sizeof *a->n);
  a->off = calloc((size_t)d, sizeof *a->off);
  /* each output covers at most s/d + 2 source pixels */
  size_t most = (size_t)(s / d + 2);
  a->w = calloc(most * (size_t)d, sizeof *a->w);
  if (!a->at || !a->n || !a->off || !a->w) {
    axis_free(a);
    return -1;
  }
  int off = 0;
  for (int i = 0; i < d; i++) {
    long long lo = (long long)i * s, hi = lo + s;
    int j0 = (int)(lo / d), j1 = (int)((hi + d - 1) / d);
    j1 = j1 > s ? s : j1;
    a->at[i] = j0;
    a->n[i] = j1 - j0;
    a->off[i] = off;
    long long run = 0, was = 0;
    for (int j = j0; j < j1; j++) {
      long long p0 = (long long)j * d, p1 = p0 + d;
      run += (p1 < hi ? p1 : hi) - (p0 > lo ? p0 : lo);
      long long now = (run * ONE + s / 2) / s;
      a->w[off++] = (uint16_t)(now - was);
      was = now;
    }
  }
  return 0;
}

int ctui_look_resample_area(const unsigned char *src, int sw, int sh,
                            unsigned char *dst, int stride, int dw, int dh) {
  if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || dw > sw ||
      dh > sh) {
    return -1;
  }
  AXIS ax = {0}, ay = {0};
  /* each output row: its source rows, each summed along the output's
   * columns (premultiplied r, g, b, and a), added in by their weights */
  uint64_t *acc = malloc(sizeof *acc * 4 * (size_t)dw);
  if (!acc || axis_make(&ax, sw, dw) != 0) {
    free(acc);
    return -1;
  }
  if (axis_make(&ay, sh, dh) != 0) {
    axis_free(&ax);
    free(acc);
    return -1;
  }
  for (int y = 0; y < dh; y++) {
    memset(acc, 0, sizeof *acc * 4 * (size_t)dw);
    for (int k = 0; k < ay.n[y]; k++) {
      uint64_t wy = ay.w[ay.off[y] + k];
      const unsigned char *s = src + (size_t)(ay.at[y] + k) * (size_t)sw * 4;
      for (int x = 0; x < dw; x++) {
        /* r*a <= 65025, times ONE: fits 32 bits */
        uint32_t r = 0, g = 0, b = 0, a = 0;
        const uint16_t *w = ax.w + ax.off[x];
        const unsigned char *p = s + (size_t)ax.at[x] * 4;
        for (int j = 0; j < ax.n[x]; j++, p += 4) {
          uint32_t wa = (uint32_t)w[j] * p[3];
          r += wa * p[0];
          g += wa * p[1];
          b += wa * p[2];
          a += wa;
        }
        uint64_t *o = acc + 4 * (size_t)x;
        o[0] += wy * r;
        o[1] += wy * g;
        o[2] += wy * b;
        o[3] += wy * a;
      }
    }
    unsigned char *out = dst + (size_t)y * (size_t)stride * 4;
    for (int x = 0; x < dw; x++, out += 4) {
      const uint64_t *o = acc + 4 * (size_t)x;
      if (!o[3]) {
        continue;
      }
      for (int c = 0; c < 3; c++) {
        uint64_t v = (o[c] + o[3] / 2) / o[3];
        out[c] = (unsigned char)(v > 255 ? 255 : v);
      }
      uint64_t a = (o[3] + ((uint64_t)ONE * ONE) / 2) / ((uint64_t)ONE * ONE);
      out[3] = (unsigned char)(a > 255 ? 255 : a);
    }
  }
  axis_free(&ax);
  axis_free(&ay);
  free(acc);
  return 0;
}
