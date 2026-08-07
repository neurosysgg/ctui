#ifndef CTUI_DEFLATE_H
#define CTUI_DEFLATE_H

#include <stddef.h>

/* compress-only zlib-wrapped DEFLATE (RFC 1950 framing around RFC 1951
 * compressed data) -- ctui's one consumer (core/gfx.c's Kitty transport,
 * `o=z`) never reads a Kitty APC reply and has no other need for
 * compressed data either, so there's no inflate path here, by design (see
 * GFX_DESIGN.md's Phase 5b).
 *
 * v1 scope, deliberately narrow: fixed Huffman codes only (RFC 1951
 * S3.2.6) -- no dynamic Huffman tables (real zlib's biggest ratio win,
 * and meaningfully more implementation complexity: a frequency pass +
 * code-length optimization, not just a fixed lookup table). Matching is a
 * plain greedy LZ77 hash-chain search (min-match 3, max-match 258, 32KB
 * window per spec, capped chain-search depth) -- no lazy matching or any
 * of real zlib's optimal-parse tricks. Good enough to consistently beat
 * raw RGBA (real screen content is never uniformly random bytes) without
 * costing more GUI-thread CPU than the write() time it's meant to save.
 *
 * Not a general-purpose compression library: no window >32KB, no
 * streaming/incremental API, no compression-level knob. Promote past
 * this narrow scope only once a second consumer actually needs more
 * (same "second consumer" promotion bar CLAUDE.md uses elsewhere). */

/* compresses src (exactly len bytes) into a newly malloc'd buffer
 * containing a complete RFC 1950 zlib stream (2-byte header, one fixed-
 * Huffman DEFLATE block, 4-byte big-endian Adler-32 trailer). Returns
 * NULL and sets *out_len = 0 if src is NULL, len is 0 (logging E_WRN --
 * a caller bug), any allocation fails (E_WRN), or len exceeds
 * CTUI_DEFLATE_MAX_INPUT (core/deflate.c; E_DBG, a deliberate decline,
 * not an error -- see that constant's own comment for the measured
 * per-byte cost that makes attempting compression above it a net loss
 * regardless of how it would have turned out). Callers already have to
 * handle "compression didn't happen" as a normal outcome (see
 * ctui_gfx_kitty_display()'s always-compress-compare-keep-smaller
 * policy), so every one of these folds into the same NULL-means-fall-
 * back-to-raw convention rather than needing to distinguish why. On
 * success, *out_len is set to the returned buffer's length and the
 * caller owns it (free() when done). Never returns a buffer larger than
 * necessary -- it's sized exactly to the compressed stream, not to some
 * encode-time upper bound. */
unsigned char *ctui_deflate_compress(const unsigned char *src, size_t len,
                                     size_t *out_len);

#endif
