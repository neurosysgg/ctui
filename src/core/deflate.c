#include "deflate.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

/* LZ77 match search bounds (RFC 1951 S3.2.5) */
#define CTUI_DEFLATE_WINDOW 32768
#define CTUI_DEFLATE_MIN_MATCH 3
#define CTUI_DEFLATE_MAX_MATCH 258

/* hash-chain shape: 3-byte hash into a 32768-bucket table, chained via a
 * per-position prev[] array (classic zlib-style hash chain). Chain search
 * depth is capped so a pathological, highly-repetitive input can't turn
 * this into an unbounded scan -- this runs on the GUI thread, once per
 * Kitty frame, so it has to stay cheap regardless of input content. */
#define CTUI_DEFLATE_HASH_BITS 15
#define CTUI_DEFLATE_HASH_SIZE (1 << CTUI_DEFLATE_HASH_BITS)
#define CTUI_DEFLATE_MAX_CHAIN 64

/* measured against ctui-mus's own widgets under pty_harness.py (a real
 * run, not a guess -- see GFX_DESIGN.md's Phase 5b "Resolved open
 * questions" for the numbers): compress time scales linearly with input
 * size at ~5.9us/KB *regardless* of CTUI_DEFLATE_MAX_CHAIN above --
 * border.c's full-terminal background (2.4MB raw RGBA at 180x50-ish
 * geometry) cost ~14ms to compress at every chain depth tried (64 down
 * to 2), because most positions resolve via one or two chain probes on
 * real image content; the O(n) per-byte constant (hash + match-extend +
 * bit-by-bit Huffman emission) dominates, not chain search. A 14ms stall
 * on the GUI thread is exactly what Phase 5 exists to avoid, so
 * ctui_deflate_compress() declines outright above this size rather than
 * attempting a compression whose CPU cost was never going to be worth
 * what it saves on an already-batched (Phase 5a), already-cheap write().
 * 512KB comfortably covers every real per-widget viz image observed
 * (up to ~493KB) while excluding only the full-terminal-scale outliers
 * (border, and a couple of oversized background widgets north of 1MB).
 * Not the same kind of decision as "no size-threshold heuristic" on the
 * compressed-vs-raw *ratio* comparison (deflate.h/GFX_DESIGN.md) -- that
 * one is about small images where the *outcome* is uncertain; this one
 * is a hard ceiling on *attempting* the work at all, independent of
 * whether it would have won. */
#define CTUI_DEFLATE_MAX_INPUT (512 * 1024)

/* growable, LSB-first bit accumulator -- every DEFLATE field (block
 * header bits, length/distance extra bits, Huffman codes) packs into the
 * output byte stream least-significant-bit first (RFC 1951 S3.1.1);
 * Huffman codes are the one exception (see bw_put_huffman() below), which
 * this still expresses in terms of the same LSB-first primitive. */
typedef struct {
  unsigned char *buf;
  size_t len, cap;
  unsigned int bit_buf;
  int bit_count;
} BIT_WRITER;

static void bw_ensure(BIT_WRITER *bw, size_t extra) {
  if (bw->len + extra <= bw->cap) {
    return;
  }
  bw->cap = bw->cap ? bw->cap * 2 : 4096;
  while (bw->len + extra > bw->cap) {
    bw->cap *= 2;
  }
  bw->buf = realloc(bw->buf, bw->cap);
}

/* packs the low nbits of value into the stream, least-significant bit
 * first -- the plain encoding every DEFLATE field except a Huffman code
 * itself uses (block header BFINAL/BTYPE, length/distance extra bits). */
static void bw_put_bits(BIT_WRITER *bw, unsigned int value, int nbits) {
  bw->bit_buf |= (value & ((1u << nbits) - 1u)) << bw->bit_count;
  bw->bit_count += nbits;
  while (bw->bit_count >= 8) {
    bw_ensure(bw, 1);
    bw->buf[bw->len++] = (unsigned char)(bw->bit_buf & 0xff);
    bw->bit_buf >>= 8;
    bw->bit_count -= 8;
  }
}

/* a Huffman code is packed most-significant-bit first (RFC 1951 S3.1.1's
 * one explicit exception to "packed LSB first") -- reversed into an
 * LSB-first value and packed with one bw_put_bits() call instead of
 * nbits individual 1-bit ones (GFX_DESIGN.md's Phase 5 profiling
 * root-caused this bit-at-a-time emission as the dominant per-byte cost
 * of compression, ~5.9us/KB, since it's called once per literal/length/
 * distance symbol emitted -- the reversal loop itself is still O(nbits)
 * but plain shift/or with no function-call or byte-flush overhead per
 * bit, unlike the old nbits-calls-to-bw_put_bits form). */
static void bw_put_huffman(BIT_WRITER *bw, unsigned int code, int nbits) {
  unsigned int rev = 0;
  for (int i = 0; i < nbits; i++) {
    rev = (rev << 1) | ((code >> i) & 1u);
  }
  bw_put_bits(bw, rev, nbits);
}

static void bw_flush_byte(BIT_WRITER *bw) {
  if (bw->bit_count > 0) {
    bw_ensure(bw, 1);
    bw->buf[bw->len++] = (unsigned char)(bw->bit_buf & 0xff);
    bw->bit_buf = 0;
    bw->bit_count = 0;
  }
}

/* RFC 1951 S3.2.6's fixed Huffman literal/length alphabet (symbols
 * 0-287): four contiguous bit-width bands, each a simple offset from the
 * symbol -- no table needed, just the piecewise mapping the RFC gives
 * directly. Covers length codes 257-285 too, since they live in the same
 * alphabet as literals and the end-of-block symbol (256). */
static void fixed_lit_huffman(unsigned int sym, unsigned int *code, int *bits) {
  if (sym <= 143) {
    *bits = 8;
    *code = 0x30 + sym;
  } else if (sym <= 255) {
    *bits = 9;
    *code = 0x190 + (sym - 144);
  } else if (sym <= 279) {
    *bits = 7;
    *code = sym - 256;
  } else {
    *bits = 8;
    *code = 0xc0 + (sym - 280);
  }
}

/* RFC 1951 S3.2.5's length code table: length value -> (code symbol,
 * base length, extra bit count). Entries are contiguous, increasing
 * ranges, so the lookup below can walk from the end and take the first
 * base <= length. */
static const struct {
  int base, extra_bits, code;
} length_table[] = {
    {3, 0, 257},   {4, 0, 258},   {5, 0, 259},   {6, 0, 260},   {7, 0, 261},
    {8, 0, 262},   {9, 0, 263},   {10, 0, 264},  {11, 1, 265},  {13, 1, 266},
    {15, 1, 267},  {17, 1, 268},  {19, 2, 269},  {23, 2, 270},  {27, 2, 271},
    {31, 2, 272},  {35, 3, 273},  {43, 3, 274},  {51, 3, 275},  {59, 3, 276},
    {67, 4, 277},  {83, 4, 278},  {99, 4, 279},  {115, 4, 280}, {131, 5, 281},
    {163, 5, 282}, {195, 5, 283}, {227, 5, 284}, {258, 0, 285},
};
#define CTUI_DEFLATE_LENGTH_TABLE_N                                            \
  (int)(sizeof(length_table) / sizeof(length_table[0]))

/* RFC 1951 S3.2.5's distance code table, same shape as length_table
 * above. Only needs to cover up to CTUI_DEFLATE_WINDOW (32768) since the
 * matcher never reports a longer distance. */
static const struct {
  int base, extra_bits, code;
} dist_table[] = {
    {1, 0, 0},       {2, 0, 1},       {3, 0, 2},      {4, 0, 3},
    {5, 1, 4},       {7, 1, 5},       {9, 2, 6},      {13, 2, 7},
    {17, 3, 8},      {25, 3, 9},      {33, 4, 10},    {49, 4, 11},
    {65, 5, 12},     {97, 5, 13},     {129, 6, 14},   {193, 6, 15},
    {257, 7, 16},    {385, 7, 17},    {513, 8, 18},   {769, 8, 19},
    {1025, 9, 20},   {1537, 9, 21},   {2049, 10, 22}, {3073, 10, 23},
    {4097, 11, 24},  {6145, 11, 25},  {8193, 12, 26}, {12289, 12, 27},
    {16385, 13, 28}, {24577, 13, 29},
};
#define CTUI_DEFLATE_DIST_TABLE_N                                              \
  (int)(sizeof(dist_table) / sizeof(dist_table[0]))

static void emit_length(BIT_WRITER *bw, size_t length) {
  int idx = CTUI_DEFLATE_LENGTH_TABLE_N - 1;
  while (idx > 0 && (size_t)length_table[idx].base > length) {
    idx--;
  }
  unsigned int code;
  int bits;
  fixed_lit_huffman((unsigned int)length_table[idx].code, &code, &bits);
  bw_put_huffman(bw, code, bits);
  if (length_table[idx].extra_bits > 0) {
    bw_put_bits(bw, (unsigned int)(length - (size_t)length_table[idx].base),
                length_table[idx].extra_bits);
  }
}

static void emit_distance(BIT_WRITER *bw, unsigned int dist) {
  int idx = CTUI_DEFLATE_DIST_TABLE_N - 1;
  while (idx > 0 && (unsigned int)dist_table[idx].base > dist) {
    idx--;
  }
  /* fixed Huffman distance codes are their own 5-bit value -- still
   * packed MSB-first like every other Huffman code (RFC 1951's rule
   * applies to all three alphabets alike). */
  bw_put_huffman(bw, (unsigned int)dist_table[idx].code, 5);
  if (dist_table[idx].extra_bits > 0) {
    bw_put_bits(bw, dist - (unsigned int)dist_table[idx].base,
                dist_table[idx].extra_bits);
  }
}

static void emit_literal(BIT_WRITER *bw, unsigned char byte) {
  unsigned int code;
  int bits;
  fixed_lit_huffman(byte, &code, &bits);
  bw_put_huffman(bw, code, bits);
}

static void emit_end_of_block(BIT_WRITER *bw) {
  unsigned int code;
  int bits;
  fixed_lit_huffman(256, &code, &bits);
  bw_put_huffman(bw, code, bits);
}

static unsigned int hash3(const unsigned char *p) {
  return (((unsigned int)p[0] << 10) ^ ((unsigned int)p[1] << 5) ^
          (unsigned int)p[2]) &
         (CTUI_DEFLATE_HASH_SIZE - 1);
}

/* RFC 1950 Adler-32, plain byte-at-a-time form -- simple over fast, same
 * "not a general-purpose library" scope note as deflate.h's own doc
 * comment. Input sizes here (a widget's RGBA buffer) are small enough
 * that the per-byte modulo cost is negligible next to the LZ77 match
 * search it runs alongside. */
static unsigned long adler32(const unsigned char *data, size_t len) {
  unsigned long a = 1, b = 0;
  const unsigned long mod = 65521;
  for (size_t i = 0; i < len; i++) {
    a = (a + data[i]) % mod;
    b = (b + a) % mod;
  }
  return (b << 16) | a;
}

unsigned char *ctui_deflate_compress(const unsigned char *src, size_t len,
                                     size_t *out_len) {
  if (out_len != NULL) {
    *out_len = 0;
  }
  if (src == NULL || len == 0) {
    ctui_logf(E_WRN,
              "[CTUI:DEFLATE] - compress rejected @ tick %d, degenerate "
              "input (src=%p, len=%zu)\n",
              ctui_tick_advance(), (const void *)src, len);
    return NULL;
  }
  if (len > CTUI_DEFLATE_MAX_INPUT) {
    /* not an error -- an expected, deliberate decline (see
     * CTUI_DEFLATE_MAX_INPUT's own comment). E_DBG, not E_WRN: the caller
     * already treats a NULL return as "transmit raw instead," so this
     * isn't a failure any caller needs to react to. */
    ctui_logf(E_DBG,
              "[CTUI:DEFLATE] - compress declined @ tick %d, %zu-byte input "
              "exceeds the %d-byte cap (see CTUI_DEFLATE_MAX_INPUT)\n",
              ctui_tick_advance(), len, CTUI_DEFLATE_MAX_INPUT);
    return NULL;
  }

  int *head = malloc(sizeof(*head) * CTUI_DEFLATE_HASH_SIZE);
  int *prev = malloc(sizeof(*prev) * len);
  if (head == NULL || prev == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:DEFLATE] - compress rejected @ tick %d, malloc failed "
              "for %zu-byte input\n",
              ctui_tick_advance(), len);
    free(head);
    free(prev);
    return NULL;
  }
  for (int i = 0; i < CTUI_DEFLATE_HASH_SIZE; i++) {
    head[i] = -1;
  }

  BIT_WRITER bw = {0};
  bw_ensure(&bw, 2);
  /* RFC 1950 zlib header: CMF=0x78 (CM=8 deflate, CINFO=7 -> 32K window),
   * FLG=0x01 (FLEVEL=0 fastest, FDICT=0, FCHECK chosen so the 16-bit
   * header is a multiple of 31) -- the standard "fastest" zlib header
   * pair, unconditionally valid regardless of what this encoder actually
   * does internally (FLEVEL is advisory metadata, never checked by a
   * decompressor). */
  bw.buf[0] = 0x78;
  bw.buf[1] = 0x01;
  bw.len = 2;

  bw_put_bits(&bw, 1, 1); /* BFINAL: this is the only/last block */
  bw_put_bits(&bw, 1, 2); /* BTYPE: 01 = fixed Huffman */

  size_t i = 0;
  while (i + CTUI_DEFLATE_MIN_MATCH <= len) {
    unsigned int h = hash3(src + i);
    int cand = head[h];
    int chain = 0;
    size_t best_len = 0;
    int best_dist = 0;

    while (cand >= 0 && chain < CTUI_DEFLATE_MAX_CHAIN) {
      if ((int)i - cand > CTUI_DEFLATE_WINDOW) {
        break; /* chain only gets older from here -- nothing left in window */
      }
      size_t max_possible = len - i;
      if (max_possible > CTUI_DEFLATE_MAX_MATCH) {
        max_possible = CTUI_DEFLATE_MAX_MATCH;
      }
      size_t mlen = 0;
      while (mlen < max_possible && src[(size_t)cand + mlen] == src[i + mlen]) {
        mlen++;
      }
      if (mlen > best_len) {
        best_len = mlen;
        best_dist = (int)i - cand;
        if (mlen >= CTUI_DEFLATE_MAX_MATCH) {
          break;
        }
      }
      cand = prev[cand];
      chain++;
    }

    if (best_len >= CTUI_DEFLATE_MIN_MATCH) {
      emit_length(&bw, best_len);
      emit_distance(&bw, (unsigned int)best_dist);
      size_t match_end = i + best_len;
      while (i < match_end) {
        if (i + CTUI_DEFLATE_MIN_MATCH <= len) {
          unsigned int hh = hash3(src + i);
          prev[i] = head[hh];
          head[hh] = (int)i;
        }
        i++;
      }
    } else {
      emit_literal(&bw, src[i]);
      prev[i] = head[h];
      head[h] = (int)i;
      i++;
    }
  }
  for (; i < len; i++) {
    emit_literal(&bw, src[i]);
  }
  emit_end_of_block(&bw);
  bw_flush_byte(&bw);

  free(head);
  free(prev);

  unsigned long adler = adler32(src, len);
  bw_ensure(&bw, 4);
  bw.buf[bw.len++] = (unsigned char)((adler >> 24) & 0xff);
  bw.buf[bw.len++] = (unsigned char)((adler >> 16) & 0xff);
  bw.buf[bw.len++] = (unsigned char)((adler >> 8) & 0xff);
  bw.buf[bw.len++] = (unsigned char)(adler & 0xff);

  if (out_len != NULL) {
    *out_len = bw.len;
  }
  ctui_logf(E_DBG,
            "[CTUI:DEFLATE] - compress @ tick %d (%zu bytes -> %zu bytes)\n",
            ctui_tick_advance(), len, bw.len);
  return bw.buf;
}

/* --- inflate (RFC 1951), after the shape of zlib's puff.c --- */

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
  const unsigned char *in;
  size_t inlen, incnt;
  unsigned bitbuf, bitcnt;
  unsigned char *out;
  size_t outlen, outcnt;
  int err; /* ran out of input */
} INFLATE;

typedef struct {
  short count[MAXBITS + 1];
  short symbol[FIXLCODES];
} HUFF;

static unsigned bits(INFLATE *s, int need) {
  unsigned long val = s->bitbuf;
  while (s->bitcnt < (unsigned)need) {
    if (s->incnt >= s->inlen) {
      s->err = 1;
      return 0;
    }
    val |= (unsigned long)s->in[s->incnt++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = (unsigned)(val >> need);
  s->bitcnt -= (unsigned)need;
  return (unsigned)(val & ((1UL << need) - 1));
}

static int stored(INFLATE *s) {
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->incnt + 4 > s->inlen) {
    return -1;
  }
  unsigned len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
  unsigned nlen = s->in[s->incnt + 2] | (unsigned)s->in[s->incnt + 3] << 8;
  s->incnt += 4;
  if (len != (~nlen & 0xffff) || s->incnt + len > s->inlen ||
      s->outcnt + len > s->outlen) {
    return -1;
  }
  memcpy(s->out + s->outcnt, s->in + s->incnt, len);
  s->outcnt += len;
  s->incnt += len;
  return 0;
}

static int decode(INFLATE *s, const HUFF *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= MAXBITS; len++) {
    code |= (int)bits(s, 1);
    if (s->err) {
      return -1;
    }
    int count = h->count[len];
    if (code - count < first) {
      return h->symbol[index + (code - first)];
    }
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

/* the canonical code for n lengths: 0 complete, > 0 incomplete, < 0
 * over-subscribed */
static int construct(HUFF *h, const short *length, int n) {
  for (int len = 0; len <= MAXBITS; len++) {
    h->count[len] = 0;
  }
  for (int sym = 0; sym < n; sym++) {
    h->count[length[sym]]++;
  }
  if (h->count[0] == n) {
    return 0;
  }
  int left = 1;
  for (int len = 1; len <= MAXBITS; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) {
      return left;
    }
  }
  short offs[MAXBITS + 1];
  offs[1] = 0;
  for (int len = 1; len < MAXBITS; len++) {
    offs[len + 1] = (short)(offs[len] + h->count[len]);
  }
  for (int sym = 0; sym < n; sym++) {
    if (length[sym] != 0) {
      h->symbol[offs[length[sym]]++] = (short)sym;
    }
  }
  return left;
}

static int codes(INFLATE *s, const HUFF *lencode, const HUFF *distcode) {
  static const short lbase[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                  15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                  67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const short dbase[30] = {
      1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
      33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const short dext[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                 4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int sym = decode(s, lencode);
    if (sym < 0) {
      return -1;
    }
    if (sym < 256) {
      if (s->outcnt >= s->outlen) {
        return -1;
      }
      s->out[s->outcnt++] = (unsigned char)sym;
    } else if (sym == 256) {
      return 0;
    } else {
      sym -= 257;
      if (sym >= 29) {
        return -1;
      }
      size_t len = (size_t)lbase[sym] + bits(s, lext[sym]);
      int dsym = decode(s, distcode);
      if (dsym < 0 || dsym >= 30) {
        return -1;
      }
      size_t dist = (size_t)dbase[dsym] + bits(s, dext[dsym]);
      if (s->err || dist > s->outcnt || s->outcnt + len > s->outlen) {
        return -1;
      }
      for (; len; len--, s->outcnt++) {
        s->out[s->outcnt] = s->out[s->outcnt - dist];
      }
    }
  }
}

static int fixed(INFLATE *s) {
  /* built each time: a few hundred steps, cheap next to the block */
  HUFF lencode, distcode;
  short lengths[FIXLCODES];
  int sym = 0;
  for (; sym < 144; sym++) {
    lengths[sym] = 8;
  }
  for (; sym < 256; sym++) {
    lengths[sym] = 9;
  }
  for (; sym < 280; sym++) {
    lengths[sym] = 7;
  }
  for (; sym < FIXLCODES; sym++) {
    lengths[sym] = 8;
  }
  construct(&lencode, lengths, FIXLCODES);
  for (sym = 0; sym < MAXDCODES; sym++) {
    lengths[sym] = 5;
  }
  construct(&distcode, lengths, MAXDCODES);
  return codes(s, &lencode, &distcode);
}

static int dynamic(INFLATE *s) {
  static const short order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                  11, 4,  12, 3, 13, 2, 14, 1, 15};
  short lengths[MAXLCODES + MAXDCODES];
  int nlen = (int)bits(s, 5) + 257;
  int ndist = (int)bits(s, 5) + 1;
  int ncode = (int)bits(s, 4) + 4;
  if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) {
    return -1;
  }
  int index = 0;
  for (; index < ncode; index++) {
    lengths[order[index]] = (short)bits(s, 3);
  }
  for (; index < 19; index++) {
    lengths[order[index]] = 0;
  }
  HUFF lencode, distcode;
  if (s->err || construct(&lencode, lengths, 19) != 0) {
    return -1;
  }
  for (index = 0; index < nlen + ndist;) {
    int sym = decode(s, &lencode);
    if (sym < 0) {
      return -1;
    }
    if (sym < 16) {
      lengths[index++] = (short)sym;
      continue;
    }
    short len = 0;
    int rep;
    if (sym == 16) {
      if (index == 0) {
        return -1;
      }
      len = lengths[index - 1];
      rep = 3 + (int)bits(s, 2);
    } else if (sym == 17) {
      rep = 3 + (int)bits(s, 3);
    } else {
      rep = 11 + (int)bits(s, 7);
    }
    if (s->err || index + rep > nlen + ndist) {
      return -1;
    }
    while (rep--) {
      lengths[index++] = len;
    }
  }
  if (lengths[256] == 0) {
    return -1;
  }
  int err = construct(&lencode, lengths, nlen);
  if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) {
    return -1;
  }
  err = construct(&distcode, lengths + nlen, ndist);
  if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
    return -1;
  }
  return codes(s, &lencode, &distcode);
}

/* the blocks from s's input until the last; 0 or -1 */
static int blocks(INFLATE *s) {
  int last;
  do {
    last = (int)bits(s, 1);
    int type = (int)bits(s, 2);
    if (s->err) {
      return -1;
    }
    int r = type == 0   ? stored(s)
            : type == 1 ? fixed(s)
            : type == 2 ? dynamic(s)
                        : -1;
    if (r != 0 || s->err) {
      return -1;
    }
  } while (!last);
  return s->outcnt == s->outlen ? 0 : -1;
}

int ctui_inflate_raw(const unsigned char *in, size_t in_len, unsigned char *out,
                     size_t out_len) {
  INFLATE s = {.in = in, .inlen = in_len, .out = out, .outlen = out_len};
  return blocks(&s);
}

int ctui_inflate_zlib(const unsigned char *in, size_t in_len,
                      unsigned char *out, size_t out_len) {
  if (in_len < 2 || (in[0] & 0x0f) != 8 || (in[0] >> 4) > 7 ||
      ((unsigned)in[0] << 8 | in[1]) % 31 != 0 || (in[1] & 0x20)) {
    return -1;
  }
  return ctui_inflate_raw(in + 2, in_len - 2, out, out_len);
}
