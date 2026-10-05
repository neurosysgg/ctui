/* rename() and getpid() under plain -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "png.h"

#include "../core/deflate.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t crc32_of(uint32_t crc, const unsigned char *p, size_t n) {
  static uint32_t table[256];
  if (!table[1]) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) {
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      table[i] = c;
    }
  }
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

static void be32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

static int chunk(FILE *f, const char *type, const unsigned char *data,
                 size_t len) {
  unsigned char head[8], tail[4];
  be32(head, (uint32_t)len);
  memcpy(head + 4, type, 4);
  uint32_t crc = crc32_of(crc32_of(0, head + 4, 4), data, len);
  be32(tail, crc);
  return fwrite(head, 1, 8, f) == 8 && fwrite(data, 1, len, f) == len &&
                 fwrite(tail, 1, 4, f) == 4
             ? 0
             : -1;
}

/* raw as a zlib stream of stored (uncompressed) blocks: what
 * ctui_deflate_compress() declines (past its input cap) still makes a
 * PNG, ~4 bytes a pixel; NULL out of memory */
static unsigned char *zlib_stored(const unsigned char *raw, size_t len,
                                  size_t *out_len) {
  size_t blocks = len / 65535 + 1;
  unsigned char *z = malloc(2 + blocks * 5 + len + 4);
  if (!z) {
    return NULL;
  }
  unsigned char *o = z;
  *o++ = 0x78;
  *o++ = 0x01;
  uint32_t a = 1, b = 0;
  size_t at = 0;
  do {
    size_t n = len - at > 65535 ? 65535 : len - at;
    *o++ = at + n == len; /* BFINAL, BTYPE 00 */
    o[0] = (unsigned char)n;
    o[1] = (unsigned char)(n >> 8);
    o[2] = (unsigned char)~n;
    o[3] = (unsigned char)(~n >> 8);
    o += 4;
    memcpy(o, raw + at, n);
    o += n;
    /* Adler-32, the sums reduced every 5552 bytes (zlib's NMAX) */
    for (size_t i = 0; i < n;) {
      size_t end = n - i > 5552 ? i + 5552 : n;
      for (; i < end; i++) {
        a += raw[at + i];
        b += a;
      }
      a %= 65521;
      b %= 65521;
    }
    at += n;
  } while (at < len);
  be32(o, b << 16 | a);
  o += 4;
  *out_len = (size_t)(o - z);
  return z;
}

int ctui_look_png_write(const char *path, const unsigned char *rgba, int w,
                        int h) {
  if (w <= 0 || h <= 0) {
    return -1;
  }
  /* each row starts with its filter type: 0, none */
  size_t row = (size_t)w * 4, raw_len = (row + 1) * (size_t)h;
  unsigned char *raw = malloc(raw_len);
  if (!raw) {
    return -1;
  }
  for (int y = 0; y < h; y++) {
    raw[(row + 1) * (size_t)y] = 0;
    memcpy(raw + (row + 1) * (size_t)y + 1, rgba + row * (size_t)y, row);
  }
  size_t z_len;
  unsigned char *z = ctui_deflate_compress(raw, raw_len, &z_len);
  if (!z) {
    /* past ctui's cap (a page's picture): stored, kitty reads it once */
    z = zlib_stored(raw, raw_len, &z_len);
  }
  free(raw);
  if (!z) {
    return -1;
  }
  /* written next to it and renamed in, so a zone never reads half a file */
  char tmp[4096 + 32];
  snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
  FILE *f = fopen(tmp, "wb");
  unsigned char ihdr[13];
  be32(ihdr, (uint32_t)w);
  be32(ihdr + 4, (uint32_t)h);
  memcpy(ihdr + 8, "\x08\x06\x00\x00\x00", 5); /* 8-bit RGBA */
  int r = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 &&
                  chunk(f, "IHDR", ihdr, sizeof ihdr) == 0 &&
                  chunk(f, "IDAT", z, z_len) == 0 &&
                  chunk(f, "IEND", NULL, 0) == 0
              ? 0
              : -1;
  if (f && fclose(f) != 0) {
    r = -1;
  }
  free(z);
  if (r == 0 && rename(tmp, path) != 0) {
    r = -1;
  }
  if (r != 0) {
    unlink(tmp);
  }
  return r;
}
