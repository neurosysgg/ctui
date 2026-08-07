/* Exercises core/deflate.c's compress-only zlib/DEFLATE encoder. There's
 * no inflate path anywhere in core/ (ctui never needs to decompress
 * anything it didn't just compress itself -- see deflate.h), so "is the
 * output actually valid DEFLATE" can't be checked by round-tripping
 * through ctui's own code the way every other headless test in this
 * suite verifies itself. Resolved per GFX_DESIGN.md's flagged-open
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
                                  const unsigned char *orig,
                                  size_t orig_len) {
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
  const char *msg =
      "the quick brown fox jumps over the lazy dog "
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

int main(void) {
  ctui_log_init(E_ALL);

  test_degenerate_input();
  test_small_text_roundtrips_and_shrinks();
  test_large_repetitive_spans_the_window();
  test_incompressible_input_still_roundtrips();

  return ctui_test_summary();
}
