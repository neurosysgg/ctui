/* Exercises core/deflate.c's zlib/DEFLATE encoder, and its inflate. The
 * encoder's output is checked against an outside decoder, not only
 * round-tripped through ctui's own inflate (a bug both shared would pass
 * that). Resolved per GFX_DESIGN.md's flagged-open
 * Phase 5b test-strategy question: shell out to python3's stdlib zlib
 * module at test time (confirmed present in this environment, and
 * already the project's precedent for a python-at-test-time dependency
 * -- tools/pty_harness.py requires the same interpreter). This is a
 * test-only dependency, not a runtime one: ctui-demo and friends link
 * nothing new. */
#define _POSIX_C_SOURCE 200809L /* mkstemp()/fdopen() */

#include "ctui.h"

#include "ctui_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* writes compressed (comp_len bytes) to a temp file, asks a python3
 * subprocess to zlib.decompress() it into a second temp file, then reads
 * that back and compares against orig/orig_len byte-for-byte. Returns 1
 * on an exact match, 0 on any mismatch, decompress failure, or a missing
 * python3 (logged to stderr either way so a CI run without python3 fails
 * loudly instead of silently passing). */
static int zlib_roundtrip_matches(const unsigned char *comp, size_t comp_len,
                                  const unsigned char *orig, size_t orig_len) {
  char in_path[] = "/tmp/ctui_deflate_test_in_XXXXXX";
  char out_path[] = "/tmp/ctui_deflate_test_out_XXXXXX";
  int ok = 0;

  int in_fd = mkstemp(in_path);
  int out_fd = mkstemp(out_path);
  if (in_fd < 0 || out_fd < 0) {
    fprintf(stderr, "deflate_test: mkstemp failed\n");
    goto cleanup;
  }
  close(out_fd); /* python writes this one; we just needed a unique name */

  FILE *inf = fdopen(in_fd, "wb");
  if (inf == NULL || fwrite(comp, 1, comp_len, inf) != comp_len) {
    fprintf(stderr, "deflate_test: writing compressed temp file failed\n");
    if (inf != NULL) {
      fclose(inf);
    }
    goto cleanup;
  }
  fclose(inf);

  char cmd[512];
  snprintf(cmd, sizeof cmd,
           "python3 -c \"import zlib,sys; "
           "d=open(sys.argv[1],'rb').read(); "
           "open(sys.argv[2],'wb').write(zlib.decompress(d))\" %s %s",
           in_path, out_path);
  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr,
            "deflate_test: python3 zlib.decompress failed (rc=%d) -- is "
            "python3 on PATH?\n",
            rc);
    goto cleanup;
  }

  FILE *outf = fopen(out_path, "rb");
  if (outf == NULL) {
    fprintf(stderr, "deflate_test: couldn't reopen decompressed output\n");
    goto cleanup;
  }
  unsigned char *got = malloc(orig_len + 1);
  size_t got_len = fread(got, 1, orig_len + 1, outf);
  fclose(outf);
  ok = (got_len == orig_len) && (memcmp(got, orig, orig_len) == 0);
  free(got);

cleanup:
  unlink(in_path);
  unlink(out_path);
  return ok;
}

static void test_degenerate_input(void) {
  size_t out_len = 123;
  unsigned char *c = ctui_deflate_compress(NULL, 10, &out_len);
  CTUI_TEST_ASSERT(c == NULL && out_len == 0,
                   "compress(NULL, 10) rejects and zeroes out_len");

  out_len = 123;
  c = ctui_deflate_compress((const unsigned char *)"x", 0, &out_len);
  CTUI_TEST_ASSERT(c == NULL && out_len == 0,
                   "compress(src, 0) rejects and zeroes out_len");
}

static void test_small_text_roundtrips_and_shrinks(void) {
  const char *msg = "the quick brown fox jumps over the lazy dog "
                    "the quick brown fox jumps over the lazy dog";
  size_t len = strlen(msg);

  size_t out_len = 0;
  unsigned char *c =
      ctui_deflate_compress((const unsigned char *)msg, len, &out_len);
  CTUI_TEST_ASSERT(c != NULL && out_len > 0,
                   "compress() succeeds on a small repeated phrase");
  CTUI_TEST_ASSERT(out_len < len,
                   "repeated text actually compresses smaller than input");
  CTUI_TEST_ASSERT(
      zlib_roundtrip_matches(c, out_len, (const unsigned char *)msg, len),
      "python3 zlib.decompress() recovers exactly the original bytes");
  free(c);
}

static void test_large_repetitive_spans_the_window(void) {
  /* window is capped at 32768 -- this deliberately exceeds it, so a bug
   * in the "chain only gets older" window-boundary check would surface
   * as either a wrong (too-far) back-reference or a round-trip mismatch,
   * not just a ratio regression. */
  size_t len = 100000;
  unsigned char *buf = malloc(len);
  for (size_t i = 0; i < len; i++) {
    buf[i] = (unsigned char)((i / 7) % 4);
  }

  size_t out_len = 0;
  unsigned char *c = ctui_deflate_compress(buf, len, &out_len);
  CTUI_TEST_ASSERT(c != NULL && out_len > 0,
                   "compress() succeeds on a 100000-byte input");
  CTUI_TEST_ASSERT(out_len < len / 10,
                   "highly repetitive, window-spanning input compresses to "
                   "well under 10%% of its original size");
  CTUI_TEST_ASSERT(zlib_roundtrip_matches(c, out_len, buf, len),
                   "large repetitive input round-trips correctly through "
                   "python3 zlib.decompress()");
  free(buf);
  free(c);
}

static void test_incompressible_input_still_roundtrips(void) {
  /* random bytes are the case DEFLATE can't win on -- literal-only fixed
   * Huffman coding averages slightly over 8 bits/byte, so output is
   * expected to be a little LARGER than input. ctui_gfx_kitty_display()'s
   * "always compress, compare, keep smaller" policy is what makes that
   * safe at the call site; this test only needs the encoder to still
   * produce something python3 can decompress back to the exact input,
   * not to have won the size comparison. */
  size_t len = 5000;
  unsigned char *buf = malloc(len);
  unsigned int seed = 12345;
  for (size_t i = 0; i < len; i++) {
    seed = seed * 1103515245u + 12345u;
    buf[i] = (unsigned char)(seed >> 16);
  }

  size_t out_len = 0;
  unsigned char *c = ctui_deflate_compress(buf, len, &out_len);
  CTUI_TEST_ASSERT(c != NULL && out_len > 0,
                   "compress() succeeds on incompressible random input");
  CTUI_TEST_ASSERT(
      zlib_roundtrip_matches(c, out_len, buf, len),
      "incompressible input still round-trips correctly, even though it "
      "doesn't shrink");
  free(buf);
  free(c);
}

/* a raw DEFLATE stream with a dynamic Huffman block (python3's zlib at
 * level 9, wbits -15) of 24 lines like
 * '<Note time="0.0" duration="0.5" key="36" vel="0.8"/>\n' */
static const unsigned char dynamic_raw[] = {
    0x9d, 0xd2, 0x4d, 0x0a, 0x02, 0x31, 0x0c, 0x86, 0xe1, 0xbd, 0xa7, 0x28,
    0x39, 0x80, 0x36, 0x69, 0xfa, 0x07, 0x33, 0x1e, 0xc1, 0x3b, 0x08, 0x76,
    0x31, 0xf8, 0x33, 0x20, 0x55, 0xf0, 0xf6, 0x32, 0x3b, 0x37, 0x5f, 0x91,
    0x2c, 0x13, 0x78, 0x56, 0xdf, 0x3b, 0x9d, 0xd6, 0xde, 0x5c, 0x5f, 0xee,
    0x6d, 0x26, 0xbf, 0xf7, 0xe4, 0x2e, 0xaf, 0xe7, 0xb9, 0x2f, 0xeb, 0x63,
    0x3b, 0x23, 0xb9, 0x6b, 0xfb, 0xcc, 0x14, 0x12, 0xb9, 0x77, 0xbb, 0x6d,
    0xaf, 0x42, 0x87, 0xe3, 0x6e, 0xfa, 0x41, 0x0c, 0x51, 0xc6, 0x48, 0x20,
    0x2a, 0x18, 0x05, 0x88, 0x2a, 0x46, 0x8a, 0x90, 0x7a, 0x8c, 0x22, 0x44,
    0x8c, 0x51, 0x82, 0x48, 0x30, 0xca, 0x10, 0x05, 0x8c, 0x0a, 0x44, 0x8a,
    0x51, 0x85, 0x28, 0x0e, 0xc6, 0x85, 0x49, 0xe8, 0x28, 0x09, 0xd8, 0x84,
    0x0e, 0x9a, 0x60, 0x31, 0xe5, 0x17, 0x2c, 0xfd, 0xb1, 0x5a, 0x02, 0xe4,
    0x68, 0x29, 0x90, 0x93, 0x25, 0x41, 0xce, 0x96, 0x06, 0xb9, 0x58, 0x22,
    0xe4, 0x6a, 0xa9, 0x50, 0xbc, 0x25, 0x43, 0x61, 0x4b, 0x87, 0x22, 0x96,
    0x0e, 0x25, 0xfc, 0xdb, 0xe1, 0x17,
};

static void test_inflate(void) {
  /* ctui's own compressor (fixed blocks) back through the inflate */
  size_t len = 100000;
  unsigned char *buf = malloc(len), *back = malloc(len);
  for (size_t i = 0; i < len; i++) {
    buf[i] = (unsigned char)((i / 7) % 4);
  }
  size_t c_len = 0;
  unsigned char *c = ctui_deflate_compress(buf, len, &c_len);
  CTUI_TEST_ASSERT(c && ctui_inflate_zlib(c, c_len, back, len) == 0 &&
                       memcmp(buf, back, len) == 0,
                   "the compressor's zlib stream inflates back, past the "
                   "32 KB window");
  CTUI_TEST_ASSERT(ctui_inflate_zlib(c, c_len, back, len - 1) == -1 &&
                       ctui_inflate_zlib(c, c_len / 2, back, len) == -1,
                   "a stream longer than expected, or cut short, fails");
  unsigned char bad[2] = {c[0], (unsigned char)(c[1] + 1)};
  CTUI_TEST_ASSERT(ctui_inflate_zlib(bad, 2, back, len) == -1 &&
                       ctui_inflate_raw(c + 2, c_len - 2, back, len) == 0,
                   "a zlib header that fails its check is refused; the "
                   "same stream without it inflates raw");
  free(c);

  /* the line the fixture was made from, 24 times with its time and key */
  char want[2048];
  size_t n = 0;
  for (int i = 0; i < 24; i++) {
    n += (size_t)snprintf(want + n, sizeof want - n,
                          "<Note time=\"%d.0\" duration=\"0.5\" key=\"%d\" "
                          "vel=\"0.8\"/>\n",
                          i, 36 + i % 12);
  }
  CTUI_TEST_ASSERT(ctui_inflate_raw(dynamic_raw, sizeof dynamic_raw, back, n) ==
                           0 &&
                       memcmp(back, want, n) == 0,
                   "a dynamic Huffman block (zlib's level 9)");

  /* a stored block: BFINAL 1, BTYPE 00, LEN 5, NLEN ~5 */
  const unsigned char stored[] = {0x01, 0x05, 0x00, 0xfa, 0xff,
                                  'h',  'e',  'l',  'l',  'o'};
  CTUI_TEST_ASSERT(ctui_inflate_raw(stored, sizeof stored, back, 5) == 0 &&
                       memcmp(back, "hello", 5) == 0,
                   "a stored block");
  unsigned char nlen[sizeof stored];
  memcpy(nlen, stored, sizeof stored);
  nlen[3] = 0;
  CTUI_TEST_ASSERT(ctui_inflate_raw(nlen, sizeof nlen, back, 5) == -1,
                   "a stored block whose NLEN isn't LEN's complement fails");
  free(buf);
  free(back);
}

int main(void) {
  ctui_log_init(E_ALL);

  test_degenerate_input();
  test_small_text_roundtrips_and_shrinks();
  test_large_repetitive_spans_the_window();
  test_incompressible_input_still_roundtrips();
  test_inflate();

  return ctui_test_summary();
}
