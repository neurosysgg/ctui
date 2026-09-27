/* core/kitty.h: the kitty command builder -- the bytes it writes, and the
 * refusals it names instead of emitting (tools/check_kitty_protocol.py
 * checks both against kitty itself). */
#include "ctui.h"

#include "ctui_test.h"

#include <string.h>

static int has(const char *err, const char *code) {
  return err && strncmp(err, code, strlen(code)) == 0;
}

static void test_gfx_bytes(void) {
  char out[256];
  const char *err = NULL;
  CTUI_KITTY_GFX g = {.a = 'd', .d = 'I', .i = 42, .q = 2};
  size_t n = ctui_kitty_gfx_build(&g, NULL, 0, out, sizeof out, &err);
  CTUI_TEST_ASSERT(n == strlen("\x1b_Ga=d,d=I,i=42,q=2\x1b\\") &&
                       memcmp(out, "\x1b_Ga=d,d=I,i=42,q=2\x1b\\", n) == 0,
                   "a delete: keys in kitty's order, no payload, no ';'");

  g = (CTUI_KITTY_GFX){.a = 'T', .t = 's', .f = 32, .s = 2, .v = 1, .z = -5};
  n = ctui_kitty_gfx_build(&g, "/x", 2, out, sizeof out, &err);
  const char *want = "\x1b_Ga=T,t=s,f=32,v=1,s=2,z=-5;L3g=\x1b\\";
  CTUI_TEST_ASSERT(n == strlen(want) && memcmp(out, want, n) == 0,
                   "a shm transmission: the name base64'd, a negative z");

  g = (CTUI_KITTY_GFX){.a = 'T', .f = 32, .s = 1, .v = 1, .m = 1};
  n = ctui_kitty_gfx_head(&g, 1, out, sizeof out);
  CTUI_TEST_ASSERT(n > 0 && strcmp(out, "\x1b_Ga=T,f=32,m=1,v=1,s=1;") == 0,
                   "head: the keys and ';', the caller appends the chunk");
  CTUI_KITTY_GFX last = {0};
  n = ctui_kitty_gfx_head(&last, 1, out, sizeof out);
  CTUI_TEST_ASSERT(n == 4 && strcmp(out, "\x1b_G;") == 0,
                   "a last chunk (m=0, kitty's default) has no keys at all");
  CTUI_TEST_ASSERT(ctui_kitty_gfx_head(&g, 1, out, 8) == 0,
                   "head: 0 when the keys don't fit");
}

static void test_gfx_refusals(void) {
  static unsigned char px[64];
  const char *err;
  CTUI_KITTY_GFX g = {.a = 't', .i = 1, .I = 2, .s = 1, .v = 1};
  err = ctui_kitty_gfx_check(&g, px, 4);
  CTUI_TEST_ASSERT(has(err, "EINVAL: Must not specify both"),
                   "an id and a number together: kitty's EINVAL");

  g = (CTUI_KITTY_GFX){.a = 'y', .i = 1};
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, NULL, 0), "EINVAL: Malformed"),
                   "a flag outside its set is malformed");
  g = (CTUI_KITTY_GFX){.a = 'p', .i = 1, .z = INT32_MIN};
  CTUI_TEST_ASSERT(ctui_kitty_gfx_check(&g, NULL, 0) != NULL,
                   "z = INT32_MIN is refused (kitty negates it: overflow)");

  g = (CTUI_KITTY_GFX){.a = 't', .i = 1, .f = 32, .s = 4, .v = 4};
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, px, 63), "ENODATA") &&
                       ctui_kitty_gfx_check(&g, px, 64) == NULL &&
                       ctui_kitty_gfx_check(&g, px, 74) == NULL &&
                       has(ctui_kitty_gfx_check(&g, px, 75), "EFBIG"),
                   "raw RGBA 4x4: 64 bytes, up to 10 more (kitty's spare), "
                   "fewer is ENODATA, beyond is EFBIG");
  g.m = 1;
  CTUI_TEST_ASSERT(ctui_kitty_gfx_check(&g, px, 12) == NULL,
                   "a first chunk (m=1) may carry less than the image");
  g = (CTUI_KITTY_GFX){.a = 't', .i = 1, .f = 32, .s = 10001, .v = 1};
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, NULL, 40004), "EINVAL: Image "
                                                              "too large"),
                   "a side past 10000 pixels");
  g = (CTUI_KITTY_GFX){.a = 't', .i = 1, .f = 24, .v = 3};
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, px, 0), "EINVAL: Zero"),
                   "zero width for raw pixels");
  g = (CTUI_KITTY_GFX){.a = 't', .i = 1, .f = 99, .s = 1, .v = 1};
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, px, 4), "EINVAL: Unknown image "
                                                        "format"),
                   "a format other than 24, 32, 100");

  char name[2100];
  memset(name, 'a', sizeof name);
  name[0] = '/';
  g = (CTUI_KITTY_GFX){.a = 'T', .t = 'f', .f = 100, .i = 1};
  CTUI_TEST_ASSERT(ctui_kitty_gfx_check(&g, name, 2048) == NULL &&
                       has(ctui_kitty_gfx_check(&g, name, 2049),
                           "EINVAL: Filename too long"),
                   "a file name: 2048 bytes at most (ctui allowed 4096)");
  g.t = 's';
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, "x", 1), "EBADF: POSIX SHM"),
                   "a shm name must start with /");
  g = (CTUI_KITTY_GFX){
      .a = 't', .t = 'f', .f = 32, .s = 2, .v = 2, .S = 15, .i = 1};
  CTUI_TEST_ASSERT(
      has(ctui_kitty_gfx_check(&g, "/f", 2), "EBADF: Insufficient"),
      "a file's S short of the image");

  g = (CTUI_KITTY_GFX){.a = 'p', .c = 1};
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, NULL, 0), "ENOENT"),
                   "a put with neither id nor number");
  g = (CTUI_KITTY_GFX){.a = 'p', .i = 1, .U = 1, .P = 2};
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, NULL, 0), "EINVAL: Put command "
                                                          "creating a virtual"),
                   "a virtual placement with a parent");
  g = (CTUI_KITTY_GFX){.a = 'p', .i = 1, .p = 2, .P = 1, .Q = 2};
  CTUI_TEST_ASSERT(has(ctui_kitty_gfx_check(&g, NULL, 0), "EINVAL: Put command "
                                                          "refers to itself"),
                   "a placement as its own parent");

  char out[64];
  err = NULL;
  g = (CTUI_KITTY_GFX){.a = 't', .i = 1, .f = 32, .s = 4, .v = 4};
  CTUI_TEST_ASSERT(ctui_kitty_gfx_build(&g, px, 63, out, sizeof out, &err) ==
                           0 &&
                       has(err, "ENODATA"),
                   "build: refused, nothing written, kitty's reason in err");
  err = NULL;
  CTUI_TEST_ASSERT(ctui_kitty_gfx_build(&g, px, 64, out, sizeof out, &err) ==
                           0 &&
                       has(err, "ENOBUFS"),
                   "build: 64 bytes of pixels don't fit a 64-byte buffer");
}

static void test_dnd(void) {
  char out[128];
  const char *err = NULL;
  CTUI_KITTY_DND d = {.t = 'o', .x = 1};
  size_t n =
      ctui_kitty_dnd_build(&d, "text/uri-list", 13, out, sizeof out, &err);
  const char *want = "\x1b]72;t=o:x=1;text/uri-list\x1b\\";
  CTUI_TEST_ASSERT(n == strlen(want) && memcmp(out, want, n) == 0,
                   "dnd: keys joined by ':', the payload as is");
  d = (CTUI_KITTY_DND){.t = 'p', .x = -1, .y = 32, .X = 1, .Y = 1, .o = 1024};
  n = ctui_kitty_dnd_build(&d, NULL, 0, out, sizeof out, &err);
  want = "\x1b]72;t=p:o=1024:x=-1:y=32:X=1:Y=1\x1b\\";
  CTUI_TEST_ASSERT(n == strlen(want) && memcmp(out, want, n) == 0,
                   "dnd: a thumbnail header, negative x, no payload");
  d.y = 16;
  CTUI_TEST_ASSERT(has(ctui_kitty_dnd_check(&d, NULL, 0), "EINVAL: unknown "
                                                          "drag thumbnail"),
                   "dnd: a thumbnail format other than 0, 24, 32, 100");
  d = (CTUI_KITTY_DND){0};
  CTUI_TEST_ASSERT(ctui_kitty_dnd_check(&d, NULL, 0) != NULL,
                   "dnd: a command needs its type");
  d = (CTUI_KITTY_DND){.t = 'm'};
  CTUI_TEST_ASSERT(ctui_kitty_dnd_check(&d, "a\x1b\\", 3) != NULL &&
                       ctui_kitty_dnd_check(&d, "a\a", 2) != NULL,
                   "dnd: ESC or BEL in the payload would end the escape");
}

int main(void) {
  ctui_log_init(E_WRN | E_ERR);
  test_gfx_bytes();
  test_gfx_refusals();
  test_dnd();
  ctui_log_shutdown();
  return ctui_test_summary();
}
