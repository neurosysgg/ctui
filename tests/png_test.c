/* The PNG decoder (look/png_read.c, from ctui-wm): the cases tools/png_cases.py writes with
 * Python's zlib into tests/data/png/ (every filter, colour type and bit
 * depth, tRNS, Adam7, stored / fixed / dynamic blocks, split IDATs)
 * against the pixels they must give; our own writer's output back; and
 * broken files: refused, never read past (tools/fuzz/png.c goes on from
 * here). */
/* mkstemp() is POSIX, not C11 */
#define _POSIX_C_SOURCE 200809L

#include "look/png.h"

#include "ctui.h"
#include "ctui_test.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
  *len = fread(buf, 1, (size_t)(n > 0 ? n : 0), f);
  fclose(f);
  return buf;
}

static void test_cases(void) {
  static const char *const names[] = {
      "rgba8_dynamic", "rgba8_stored", "rgba8_fixed",  "rgb8_filter0",
      "rgb8_filter1",  "rgb8_filter2", "rgb8_filter3", "rgb8_filter4",
      "rgb16_trns",    "grey1",        "grey2",        "grey4",
      "grey8",         "grey16",       "plte1",        "plte2",
      "plte4",         "plte8",        "ga8",          "ga16",
      "rgba16",        "adam7_rgba8",  "adam7_grey1",  "adam7_tiny",
  };
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    char p[256], e[256];
    snprintf(p, sizeof p, "tests/data/png/%s.png", names[i]);
    snprintf(e, sizeof e, "tests/data/png/%s.rgba", names[i]);
    size_t elen = 0;
    unsigned char *want = slurp(e, &elen);
    unsigned char *got = NULL;
    int w = 0, h = 0;
    int r = ctui_look_png_load(p, &got, &w, &h);
    int ew = want && elen >= 8
                 ? (int)((uint32_t)want[0] << 24 | (uint32_t)want[1] << 16 |
                         (uint32_t)want[2] << 8 | want[3])
                 : -1;
    int eh = want && elen >= 8
                 ? (int)((uint32_t)want[4] << 24 | (uint32_t)want[5] << 16 |
                         (uint32_t)want[6] << 8 | want[7])
                 : -1;
    int same = r == 0 && w == ew && h == eh &&
               elen == 8 + (size_t)w * (size_t)h * 4 &&
               memcmp(got, want + 8, elen - 8) == 0;
    int wh = 0;
    CTUI_TEST_ASSERT(same && ctui_look_png_size(p, &wh, &wh) == 0 && wh == eh,
                     "%s: %dx%d, the pixels Python encoded", names[i], w, h);
    free(got);
    free(want);
  }
}

static void test_ours(void) {
  unsigned char px[5 * 3 * 4];
  for (size_t i = 0; i < sizeof px; i++) {
    px[i] = (unsigned char)(i * 37 + 11);
  }
  char path[] = "/tmp/png_test.XXXXXX";
  int fd = mkstemp(path);
  if (fd >= 0) {
    close(fd);
  }
  unsigned char *got = NULL;
  int w = 0, h = 0;
  CTUI_TEST_ASSERT(ctui_look_png_write(path, px, 5, 3) == 0 &&
                       ctui_look_png_load(path, &got, &w, &h) == 0 && w == 5 &&
                       h == 3 && memcmp(got, px, sizeof px) == 0,
                   "our writer's PNG (ctui's deflate) reads back the same");
  free(got);
  /* past ctui_deflate_compress()'s cap (a page's picture): stored
   * blocks, many of them, still the same pixels */
  int bw = 500, bh = 400;
  unsigned char *big = malloc((size_t)bw * (size_t)bh * 4);
  for (size_t i = 0; big && i < (size_t)bw * (size_t)bh * 4; i++) {
    big[i] = (unsigned char)(i * 131 + i / 977);
  }
  got = NULL;
  struct stat st;
  CTUI_TEST_ASSERT(
      big && ctui_look_png_write(path, big, bw, bh) == 0 &&
          stat(path, &st) == 0 && st.st_size > (off_t)bw * bh * 4 &&
          ctui_look_png_load(path, &got, &w, &h) == 0 && w == bw && h == bh &&
          memcmp(got, big, (size_t)bw * (size_t)bh * 4) == 0,
      "%dx%d px, past deflate's cap: stored, reads back the same", bw, bh);
  free(got);
  free(big);
  unlink(path);
}

/* past an icon's limits: a screenshot's size through load_max() */
static void test_max(void) {
  int bw = 2600, bh = 1700; /* 4.4 M px: past CTUI_LOOK_PNG_MAX_AREA */
  size_t n = (size_t)bw * (size_t)bh * 4;
  unsigned char *big = malloc(n);
  for (size_t i = 0; big && i < n; i++) {
    big[i] = (unsigned char)(i * 7 + i / 4093);
  }
  char path[] = "/tmp/png_test_max.XXXXXX";
  int fd = mkstemp(path);
  if (fd >= 0) {
    close(fd);
  }
  struct stat st;
  int made = big && ctui_look_png_write(path, big, bw, bh) == 0 &&
             stat(path, &st) == 0;
  size_t size = made ? (size_t)st.st_size : 0;
  unsigned char *got = NULL;
  int w = 0, h = 0;
  int icon = made ? ctui_look_png_load(path, &got, &w, &h) : 0;
  CTUI_TEST_ASSERT(made && icon != 0 && !got,
                   "%dx%d px (a %zu-byte file): not an icon, refused", bw, bh,
                   size);
  int ok = made && ctui_look_png_load_max(path, (long long)bw * bh, size, &got,
                                          &w, &h) == 0;
  CTUI_TEST_ASSERT(ok && w == bw && h == bh && memcmp(got, big, n) == 0,
                   "load_max(): read back the same at its exact limits");
  free(got);
  got = NULL;
  CTUI_TEST_ASSERT(
      made &&
          ctui_look_png_load_max(path, (long long)bw * bh - 1, size, &got, &w,
                                 &h) != 0 &&
          ctui_look_png_load_max(path, (long long)bw * bh, size - 1, &got, &w,
                                 &h) != 0 &&
          !got,
      "a pixel or a byte past its limits: refused");
  free(big);
  unlink(path);
}

static void test_broken(void) {
  size_t len = 0;
  unsigned char *good = slurp("tests/data/png/rgba8_dynamic.png", &len);
  unsigned char *out = NULL;
  int w, h;
  int refused = 0, cut = 0;
  /* every prefix: refused (the checks read nothing past the end) */
  for (size_t n = 0; good && n < len - 12; n++) {
    unsigned char *copy = malloc(n ? n : 1);
    memcpy(copy, good, n);
    cut++;
    refused += ctui_look_png_decode(copy, n, &out, &w, &h) != 0 && !out;
    free(out);
    out = NULL;
    free(copy);
  }
  CTUI_TEST_ASSERT(cut > 100 && refused == cut,
                   "every cut short file is refused (%d of %d)", refused, cut);

  /* flipped bytes: whatever it says, it stays in bounds (SAN runs) */
  int answered = 0;
  for (size_t i = 8; good && i < len; i += 3) {
    unsigned char *copy = malloc(len);
    memcpy(copy, good, len);
    copy[i] ^= 0x5a;
    if (ctui_look_png_decode(copy, len, &out, &w, &h) == 0) {
      answered += w == 32 && h == 32;
    }
    free(out);
    out = NULL;
    free(copy);
  }
  CTUI_TEST_ASSERT(1, "flipped bytes: no crash (%d still decoded)", answered);

  unsigned char big[64] = {0};
  if (good && len >= 33) {
    memcpy(big, good, 33);
  }
  big[16] = 0x7f; /* width 2 G */
  CTUI_TEST_ASSERT(ctui_look_png_decode(big, 33, &out, &w, &h) != 0 && !out,
                   "a huge header: refused before allocating");
  CTUI_TEST_ASSERT(
      ctui_look_png_decode((const unsigned char *)"GIF89a", 6, &out, &w, &h) !=
              0 &&
          ctui_look_png_load("/nonexistent.png", &out, &w, &h) != 0 &&
          ctui_look_png_size("tests/png_test.c", &w, &h) != 0,
      "not a PNG, no file: refused");
  free(good);
}

int main(void) {
  ctui_log_init(0);
  test_cases();
  test_ours();
  test_broken();
  test_max();
  return ctui_test_summary();
}
