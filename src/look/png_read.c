/* The PNG decoder (look/png.h), promoted from ctui-wm's xdg/png.c by its
 * second consumer (src/vk/'s placed files): every colour type and bit
 * depth, tRNS, Adam7, over ctui's inflate (core/deflate.h). The files come
 * from icon themes and apps, so everything is checked: a bad or truncated
 * file is an error, never a read past its end. Chunk CRCs and the zlib
 * checksum are not verified (a damaged icon draws damaged). */
#include "png.h"

#include "../core/deflate.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- PNG --- */

static uint32_t be32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}

static const unsigned char SIG[8] = {0x89, 'P',  'N',  'G',
                                     '\r', '\n', 0x1a, '\n'};

typedef struct {
  int w, h, depth, type, interlace, channels;
  unsigned char plte[256][4]; /* rgba */
  int plte_n;
  int trns; /* type 0/2: a colour that is clear */
  unsigned trns_v[3];
  /* the largest side and area taken: png.h's, or load_max()'s */
  uint32_t max_side;
  uint64_t max_area;
} INFO;

static int valid_format(int type, int depth) {
  switch (type) {
  case 0:
    return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
  case 3:
    return depth == 1 || depth == 2 || depth == 4 || depth == 8;
  case 2:
  case 4:
  case 6:
    return depth == 8 || depth == 16;
  default:
    return 0;
  }
}

static size_t row_bytes(const INFO *in, int w) {
  return ((size_t)w * (size_t)in->channels * (size_t)in->depth + 7) / 8;
}

/* Adam7: each pass's start and step */
static const int XS[7] = {0, 4, 0, 2, 0, 1, 0}, YS[7] = {0, 0, 4, 0, 2, 0, 1};
static const int DX[7] = {8, 8, 4, 4, 2, 2, 1}, DY[7] = {8, 8, 8, 4, 4, 2, 2};

static void pass_size(const INFO *in, int p, int *pw, int *ph) {
  if (!in->interlace) {
    *pw = p == 0 ? in->w : 0;
    *ph = p == 0 ? in->h : 0;
    return;
  }
  *pw = in->w > XS[p] ? (in->w - XS[p] + DX[p] - 1) / DX[p] : 0;
  *ph = in->h > YS[p] ? (in->h - YS[p] + DY[p] - 1) / DY[p] : 0;
}

static int paeth(int a, int b, int c) {
  int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* undoes the filters of ph rows of rb bytes (each after its filter byte),
 * in place; 0 or -1 */
static int unfilter(unsigned char *d, int ph, size_t rb, size_t bpp) {
  unsigned char *prev = NULL;
  for (int y = 0; y < ph; y++) {
    unsigned char *row = d + (size_t)y * (rb + 1);
    int f = row[0];
    unsigned char *cur = row + 1;
    for (size_t i = 0; i < rb; i++) {
      int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0;
      int c = prev && i >= bpp ? prev[i - bpp] : 0;
      int add;
      switch (f) {
      case 0:
        add = 0;
        break;
      case 1:
        add = a;
        break;
      case 2:
        add = b;
        break;
      case 3:
        add = (a + b) / 2;
        break;
      case 4:
        add = paeth(a, b, c);
        break;
      default:
        return -1;
      }
      cur[i] = (unsigned char)(cur[i] + add);
    }
    prev = cur;
  }
  return 0;
}

/* sample x of a row (channel ch), as the file stores it */
static unsigned sample(const unsigned char *row, const INFO *in, int x,
                       int ch) {
  int d = in->depth;
  size_t i = (size_t)x * (size_t)in->channels + (size_t)ch;
  if (d == 8) {
    return row[i];
  }
  if (d == 16) {
    return (unsigned)row[2 * i] << 8 | row[2 * i + 1];
  }
  size_t bit = i * (size_t)d;
  return (unsigned)(row[bit / 8] >> (8 - d - (int)(bit % 8))) & ((1u << d) - 1);
}

static unsigned char to8(unsigned v, int depth) {
  return depth == 16  ? (unsigned char)(v >> 8)
         : depth == 8 ? (unsigned char)v
                      : (unsigned char)(v * 255 / ((1u << depth) - 1));
}

static void put_px(const INFO *in, const unsigned char *row, int x,
                   unsigned char *o) {
  unsigned v[4] = {0, 0, 0, 0};
  for (int c = 0; c < in->channels; c++) {
    v[c] = sample(row, in, x, c);
  }
  switch (in->type) {
  case 0:
    o[0] = o[1] = o[2] = to8(v[0], in->depth);
    o[3] = in->trns && v[0] == in->trns_v[0] ? 0 : 255;
    break;
  case 2:
    for (int c = 0; c < 3; c++) {
      o[c] = to8(v[c], in->depth);
    }
    o[3] = in->trns && v[0] == in->trns_v[0] && v[1] == in->trns_v[1] &&
                   v[2] == in->trns_v[2]
               ? 0
               : 255;
    break;
  case 3:
    if ((int)v[0] < in->plte_n) {
      memcpy(o, in->plte[v[0]], 4);
    } else {
      memset(o, 0, 4);
    }
    break;
  case 4:
    o[0] = o[1] = o[2] = to8(v[0], in->depth);
    o[3] = to8(v[1], in->depth);
    break;
  default:
    for (int c = 0; c < 4; c++) {
      o[c] = to8(v[c], in->depth);
    }
    break;
  }
}

static int read_ihdr(INFO *in, const unsigned char *d, uint32_t len) {
  if (len != 13) {
    return -1;
  }
  uint32_t w = be32(d), h = be32(d + 4);
  in->depth = d[8];
  in->type = d[9];
  in->interlace = d[12];
  if (!w || !h || w > in->max_side || h > in->max_side ||
      (uint64_t)w * h > in->max_area ||
      !valid_format(in->type, in->depth) || d[10] != 0 || d[11] != 0 ||
      in->interlace > 1) {
    return -1;
  }
  in->w = (int)w;
  in->h = (int)h;
  static const int channels[7] = {1, 0, 3, 1, 2, 0, 4};
  in->channels = channels[in->type];
  return 0;
}

static void read_trns(INFO *in, const unsigned char *d, uint32_t len) {
  if (in->type == 3) {
    for (uint32_t i = 0; i < len && (int)i < in->plte_n; i++) {
      in->plte[i][3] = d[i];
    }
  } else if (in->type == 0 && len >= 2) {
    in->trns = 1;
    in->trns_v[0] = (unsigned)d[0] << 8 | d[1];
  } else if (in->type == 2 && len >= 6) {
    in->trns = 1;
    for (int c = 0; c < 3; c++) {
      in->trns_v[c] = (unsigned)d[2 * c] << 8 | d[2 * c + 1];
    }
  }
}

static int decode(const unsigned char *data, size_t len, uint32_t max_side,
                  uint64_t max_area, unsigned char **rgba, int *w, int *h) {
  *rgba = NULL;
  if (!data || len < 8 || memcmp(data, SIG, 8) != 0) {
    return -1;
  }
  INFO in = {0};
  in.max_side = max_side;
  in.max_area = max_area;
  int have_ihdr = 0, done = 0;
  unsigned char *z = NULL;
  size_t zlen = 0;
  size_t pos = 8;
  while (!done && pos + 12 <= len) {
    uint32_t clen = be32(data + pos);
    const unsigned char *type = data + pos + 4, *d = data + pos + 8;
    if (clen > len - pos - 12) {
      break;
    }
    if (memcmp(type, "IHDR", 4) == 0) {
      if (have_ihdr || read_ihdr(&in, d, clen) != 0) {
        break;
      }
      have_ihdr = 1;
    } else if (!have_ihdr) {
      break; /* IHDR comes first */
    } else if (memcmp(type, "PLTE", 4) == 0) {
      if (clen % 3 || clen / 3 > 256) {
        break;
      }
      in.plte_n = (int)(clen / 3);
      for (int i = 0; i < in.plte_n; i++) {
        memcpy(in.plte[i], d + 3 * i, 3);
        in.plte[i][3] = 255;
      }
    } else if (memcmp(type, "tRNS", 4) == 0) {
      read_trns(&in, d, clen);
    } else if (memcmp(type, "IDAT", 4) == 0) {
      unsigned char *nz = realloc(z, zlen + clen + 1);
      if (!nz) {
        break;
      }
      z = nz;
      memcpy(z + zlen, d, clen);
      zlen += clen;
    } else if (memcmp(type, "IEND", 4) == 0) {
      done = 1;
    }
    pos += 12 + (size_t)clen;
  }
  if (!have_ihdr || !z || (in.type == 3 && !in.plte_n)) {
    free(z);
    return -1;
  }
  /* the filtered rows of every pass, one after another */
  size_t raw_len = 0;
  for (int p = 0; p < 7; p++) {
    int pw, ph;
    pass_size(&in, p, &pw, &ph);
    if (pw && ph) {
      raw_len += (size_t)ph * (row_bytes(&in, pw) + 1);
    }
  }
  unsigned char *raw = malloc(raw_len);
  unsigned char *out = calloc((size_t)in.w * (size_t)in.h, 4);
  int r = raw && out && ctui_inflate_zlib(z, zlen, raw, raw_len) == 0 ? 0 : -1;
  free(z);
  size_t bpp = (size_t)in.channels * (size_t)in.depth / 8;
  bpp = bpp ? bpp : 1;
  unsigned char *at = raw;
  for (int p = 0; r == 0 && p < 7; p++) {
    int pw, ph;
    pass_size(&in, p, &pw, &ph);
    if (!pw || !ph) {
      continue;
    }
    size_t rb = row_bytes(&in, pw);
    if (unfilter(at, ph, rb, bpp) != 0) {
      r = -1;
      break;
    }
    for (int y = 0; y < ph; y++) {
      const unsigned char *row = at + (size_t)y * (rb + 1) + 1;
      int oy = in.interlace ? YS[p] + y * DY[p] : y;
      for (int x = 0; x < pw; x++) {
        int ox = in.interlace ? XS[p] + x * DX[p] : x;
        put_px(&in, row, x, out + ((size_t)oy * (size_t)in.w + (size_t)ox) * 4);
      }
    }
    at += (size_t)ph * (rb + 1);
  }
  free(raw);
  if (r != 0) {
    free(out);
    return -1;
  }
  *rgba = out;
  *w = in.w;
  *h = in.h;
  return 0;
}

int ctui_look_png_decode(const unsigned char *data, size_t len,
                         unsigned char **rgba, int *w, int *h) {
  return decode(data, len, CTUI_LOOK_PNG_MAX_SIDE, CTUI_LOOK_PNG_MAX_AREA,
                rgba, w, h);
}

/* icons are small: a file past this isn't one */
#define MAX_FILE (16 << 20)

static int load(const char *path, size_t max_file, uint32_t max_side,
                uint64_t max_area, unsigned char **rgba, int *w, int *h) {
  *rgba = NULL;
  FILE *f = fopen(path, "rb");
  if (!f) {
    return -1;
  }
  unsigned char *buf = NULL;
  size_t len = 0, cap = 0;
  for (;;) {
    if (len == cap) {
      /* room for one byte past max_file: a file that big fails */
      size_t next = cap ? cap * 2 : 65536;
      next = next > max_file + 1 ? max_file + 1 : next;
      unsigned char *nb = next > cap ? realloc(buf, next) : NULL;
      cap = next;
      if (!nb) {
        free(buf);
        fclose(f);
        return -1;
      }
      buf = nb;
    }
    size_t want = cap - len, got = fread(buf + len, 1, want, f);
    len += got;
    if (got < want) {
      break; /* the end (or an error: then what's read fails to decode) */
    }
  }
  fclose(f);
  int r = len <= max_file ? decode(buf, len, max_side, max_area, rgba, w, h)
                          : -1;
  free(buf);
  return r;
}

int ctui_look_png_load(const char *path, unsigned char **rgba, int *w, int *h) {
  return load(path, MAX_FILE, CTUI_LOOK_PNG_MAX_SIDE, CTUI_LOOK_PNG_MAX_AREA,
              rgba, w, h);
}

int ctui_look_png_load_max(const char *path, long long max_area,
                           size_t max_file, unsigned char **rgba, int *w,
                           int *h) {
  return load(path, max_file, CTUI_LOOK_PNG_BIG_SIDE,
              max_area > 0 ? (uint64_t)max_area : 0, rgba, w, h);
}

int ctui_look_png_size(const char *path, int *w, int *h) {
  unsigned char head[24];
  FILE *f = fopen(path, "rb");
  size_t got = f ? fread(head, 1, sizeof head, f) : 0;
  if (f) {
    fclose(f);
  }
  if (got < sizeof head || memcmp(head, SIG, 8) != 0 ||
      memcmp(head + 12, "IHDR", 4) != 0) {
    return -1;
  }
  uint32_t pw = be32(head + 16), ph = be32(head + 20);
  if (!pw || !ph || pw > CTUI_LOOK_PNG_MAX_SIDE || ph > CTUI_LOOK_PNG_MAX_SIDE) {
    return -1;
  }
  *w = (int)pw;
  *h = (int)ph;
  return 0;
}
