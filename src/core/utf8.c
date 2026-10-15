#include "utf8.h"

#include "log.h"
#include "utf8_props.h"

#include <stdlib.h>
#include <string.h>

#define CTUI_UTF8_REPLACEMENT 0xFFFDu

int ctui_utf8_decode(const char *s, uint32_t *cp) {
  const unsigned char *u = (const unsigned char *)s;
  uint32_t c;
  int len;

  if (u[0] < 0x80) {
    *cp = u[0];
    return 1;
  } else if ((u[0] & 0xE0) == 0xC0) {
    c = u[0] & 0x1F;
    len = 2;
  } else if ((u[0] & 0xF0) == 0xE0) {
    c = u[0] & 0x0F;
    len = 3;
  } else if ((u[0] & 0xF8) == 0xF0) {
    c = u[0] & 0x07;
    len = 4;
  } else {
    *cp = CTUI_UTF8_REPLACEMENT;
    return 1;
  }

  /* the continuation-byte check also stops at the NUL terminator, so a
   * truncated sequence at the end of a string never reads past it */
  for (int i = 1; i < len; i++) {
    if ((u[i] & 0xC0) != 0x80) {
      *cp = CTUI_UTF8_REPLACEMENT;
      return 1;
    }
    c = (c << 6) | (u[i] & 0x3F);
  }

  static const uint32_t min_for_len[] = {0, 0, 0x80, 0x800, 0x10000};
  if (c < min_for_len[len] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
    *cp = CTUI_UTF8_REPLACEMENT;
    return 1;
  }
  *cp = c;
  return len;
}

int ctui_utf8_encode(uint32_t cp, char *out) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = CTUI_UTF8_REPLACEMENT;
  }
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Grapheme_Cluster_Break classes, in the order tools/gen_utf8_props.py
 * writes them; the table holds LV/LVT, never HANGUL (only its ranges for
 * the tests do), RI_PENDING is a state only (an odd regional indicator
 * waiting for its pair) */
enum {
  CTUI_GB_OTHER,
  CTUI_GB_CR,
  CTUI_GB_LF,
  CTUI_GB_CONTROL,
  CTUI_GB_EXTEND,
  CTUI_GB_ZWJ,
  CTUI_GB_RI,
  CTUI_GB_PREPEND,
  CTUI_GB_SPACINGMARK,
  CTUI_GB_L,
  CTUI_GB_V,
  CTUI_GB_T,
  CTUI_GB_HANGUL,
  CTUI_GB_LV,
  CTUI_GB_LVT,
  CTUI_GB_RI_PENDING,
};

#define CTUI_INCB_LINKER 1
#define CTUI_INCB_CONSONANT 2
#define CTUI_INCB_EXTEND 3
#define CTUI_PROP_EXTPICT 4
#define CTUI_PROP_EMOJI_BASE 8

#define CTUI_VS15 0xFE0Eu
#define CTUI_VS16 0xFE0Fu

typedef struct {
  unsigned char width, gb, flags;
} CTUI_CP_PROPS;

static CTUI_CP_PROPS cp_props(uint32_t cp) {
  if (cp > 0x10FFFF) {
    return (CTUI_CP_PROPS){1, CTUI_GB_OTHER, 0};
  }
  unsigned block = ctui_utf8_props_block[cp >> CTUI_UTF8_PROPS_SHIFT];
  unsigned v = ctui_utf8_props[block << CTUI_UTF8_PROPS_SHIFT |
                               (cp & ((1u << CTUI_UTF8_PROPS_SHIFT) - 1))];
  return (CTUI_CP_PROPS){(unsigned char)(v & 3), (unsigned char)(v >> 2 & 15),
                         (unsigned char)(v >> 6)};
}

int ctui_utf8_cpwidth(uint32_t cp) {
  /* printable ASCII is 1 wide, no need to look it up */
  if (cp >= 0x20 && cp < 0x7F) {
    return 1;
  }
  return cp_props(cp).width;
}

/* what the terminal must never see raw (control characters) or draws
 * nothing for (surrogates, noncharacters): sent as U+FFFD, so segmented
 * and measured as U+FFFD too */
static uint32_t sendable(uint32_t cp) {
  if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) ||
      (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0xFDD0 && cp < 0xFDF0) ||
      (cp & 0xFFFE) == 0xFFFE || cp > 0x10FFFF) {
    return CTUI_UTF8_REPLACEMENT;
  }
  return cp;
}

/* UAX #29 extended grapheme clusters, the rules kitty applies (GB3-GB13
 * and GB9c): gb is the last codepoint's class, the rest what GB9c and
 * GB11 look back for */
typedef struct {
  unsigned char gb;
  unsigned char incb_consonant;        /* consonant {extend|linker}* */
  unsigned char incb_linked;           /* ... linker {extend|linker}* */
  unsigned char emoji_seq;             /* ExtPict Extend* */
  unsigned char emoji_seq_before_last; /* emoji_seq before the last cp */
} CTUI_SEG;

/* steps seg over p (the cluster's first codepoint when seg->gb is 0xff)
 * and returns whether that codepoint joins the cluster */
static int seg_step(CTUI_SEG *seg, CTUI_CP_PROPS p) {
  int prev = seg->gb;
  int gb = p.gb;
  int incb = p.flags & 3;
  int join = 0;
  if (prev == 0xff) {
    join = 1;
    if (gb == CTUI_GB_RI) {
      gb = CTUI_GB_RI_PENDING;
    }
  } else if (prev == CTUI_GB_CR && gb == CTUI_GB_LF) {
    join = 1;
  } else if (prev == CTUI_GB_CR || prev == CTUI_GB_LF ||
             prev == CTUI_GB_CONTROL || gb == CTUI_GB_CR || gb == CTUI_GB_LF ||
             gb == CTUI_GB_CONTROL) {
    join = 0;
  } else if ((prev == CTUI_GB_L && (gb == CTUI_GB_L || gb == CTUI_GB_V ||
                                    gb == CTUI_GB_LV || gb == CTUI_GB_LVT)) ||
             ((prev == CTUI_GB_LV || prev == CTUI_GB_V) &&
              (gb == CTUI_GB_V || gb == CTUI_GB_T)) ||
             ((prev == CTUI_GB_LVT || prev == CTUI_GB_T) && gb == CTUI_GB_T)) {
    join = 1;
  } else if (gb == CTUI_GB_EXTEND || gb == CTUI_GB_ZWJ ||
             gb == CTUI_GB_SPACINGMARK || prev == CTUI_GB_PREPEND) {
    join = 1;
  } else if (seg->incb_linked && incb == CTUI_INCB_CONSONANT) {
    join = 1;
  } else if (prev == CTUI_GB_ZWJ && seg->emoji_seq_before_last &&
             (p.flags & CTUI_PROP_EXTPICT)) {
    join = 1;
  } else if (gb == CTUI_GB_RI) {
    if (prev == CTUI_GB_RI_PENDING) {
      join = 1;
    } else {
      gb = CTUI_GB_RI_PENDING;
    }
  }

  int linker_or_extend = incb == CTUI_INCB_LINKER || incb == CTUI_INCB_EXTEND;
  int consonant_linked = seg->incb_consonant && incb == CTUI_INCB_LINKER;
  seg->incb_linked = consonant_linked || (seg->incb_linked && linker_or_extend);
  seg->incb_consonant =
      incb == CTUI_INCB_CONSONANT || (seg->incb_consonant && linker_or_extend);
  seg->emoji_seq_before_last = seg->emoji_seq;
  seg->emoji_seq = (seg->emoji_seq && gb == CTUI_GB_EXTEND) ||
                   (p.flags & CTUI_PROP_EXTPICT) != 0;
  seg->gb = (unsigned char)gb;
  return join;
}

/* the interned clusters: cell value CTUI_CELL_CLUSTER | index. Grows
 * with the distinct clusters a process draws (a few dozen in practice),
 * never shrinks; lives until exit like the rest of ctui's statics. */
static struct {
  uint32_t *cps;
  size_t ncps, cps_cap;
  struct {
    uint32_t off;
    unsigned char len, width;
  } *ent;
  size_t n, cap;
  uint32_t *slots; /* index + 1, 0 = empty; open addressing */
  size_t nslots;
} g_clusters;

#define CTUI_CLUSTER_LIMIT (1u << 20)

static uint32_t cluster_hash(const uint32_t *cps, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) {
    h = (h ^ cps[i]) * 16777619u;
  }
  return h;
}

static int cluster_rehash(size_t nslots) {
  uint32_t *slots = calloc(nslots, sizeof *slots);
  if (slots == NULL) {
    return -1;
  }
  for (size_t i = 0; i < g_clusters.n; i++) {
    size_t h = cluster_hash(g_clusters.cps + g_clusters.ent[i].off,
                            g_clusters.ent[i].len) &
               (nslots - 1);
    while (slots[h]) {
      h = (h + 1) & (nslots - 1);
    }
    slots[h] = (uint32_t)i + 1;
  }
  free(g_clusters.slots);
  g_clusters.slots = slots;
  g_clusters.nslots = nslots;
  return 0;
}

/* the cell value for cps[0..n) (n >= 2) of the given width, 0 when the
 * table can't take it (out of memory, or CTUI_CLUSTER_LIMIT clusters) */
static uint32_t cluster_intern(const uint32_t *cps, int n, int width) {
  if (g_clusters.nslots == 0 && cluster_rehash(64) < 0) {
    return 0;
  }
  size_t mask = g_clusters.nslots - 1;
  size_t h = cluster_hash(cps, n) & mask;
  for (; g_clusters.slots[h]; h = (h + 1) & mask) {
    uint32_t i = g_clusters.slots[h] - 1;
    if (g_clusters.ent[i].len == n &&
        memcmp(g_clusters.cps + g_clusters.ent[i].off, cps,
               (size_t)n * sizeof *cps) == 0) {
      return CTUI_CELL_CLUSTER | i;
    }
  }
  if (g_clusters.n >= CTUI_CLUSTER_LIMIT) {
    return 0;
  }
  if (g_clusters.n == g_clusters.cap) {
    size_t cap = g_clusters.cap ? g_clusters.cap * 2 : 32;
    void *p = realloc(g_clusters.ent, cap * sizeof *g_clusters.ent);
    if (p == NULL) {
      return 0;
    }
    g_clusters.ent = p;
    g_clusters.cap = cap;
  }
  if (g_clusters.ncps + (size_t)n > g_clusters.cps_cap) {
    size_t cap = g_clusters.cps_cap ? g_clusters.cps_cap * 2 : 256;
    while (cap < g_clusters.ncps + (size_t)n) {
      cap *= 2;
    }
    void *p = realloc(g_clusters.cps, cap * sizeof *g_clusters.cps);
    if (p == NULL) {
      return 0;
    }
    g_clusters.cps = p;
    g_clusters.cps_cap = cap;
  }
  uint32_t i = (uint32_t)g_clusters.n++;
  g_clusters.ent[i].off = (uint32_t)g_clusters.ncps;
  g_clusters.ent[i].len = (unsigned char)n;
  g_clusters.ent[i].width = (unsigned char)width;
  memcpy(g_clusters.cps + g_clusters.ncps, cps, (size_t)n * sizeof *cps);
  g_clusters.ncps += (size_t)n;
  g_clusters.slots[h] = i + 1;
  if (g_clusters.n * 2 > g_clusters.nslots) {
    cluster_rehash(g_clusters.nslots * 2); /* a failure only slows lookups */
  }
  return CTUI_CELL_CLUSTER | i;
}

/* one codepoint at s, end (NULL: none) never read past: a character cut
 * there decodes as U+FFFD */
static int decode_before(const char *s, const char *end, uint32_t *cp) {
  if (end && end - s < 4) {
    char tail[5] = {0};
    memcpy(tail, s, (size_t)(end - s));
    return ctui_utf8_decode(tail, cp);
  }
  return ctui_utf8_decode(s, cp);
}

/* the common case, ahead of the segmentation: a codepoint of class Other
 * whose next one can't join it (neither Extend, ZWJ nor SpacingMark, nor
 * zero width: dropped into the cluster) is a cluster of one. Nothing after
 * Other joins but those (GB6-8 need Hangul, GB9b a Prepend, GB9c a
 * consonant and a linker, GB11 a ZWJ, GB12-13 a regional indicator before
 * it). Returns its length, 0 when it isn't the case -- the segmentation
 * below decides those (tests/utf8_test.c checks the two agree, codepoint
 * by codepoint). On screen text ~8x faster than segmenting everything
 * (ctui-wm's notes/self-improving.md, session 219). */
static size_t cluster_lone(const char *s, const char *end, uint32_t *ch,
                           int *width) {
  const unsigned char *u = (const unsigned char *)s;
  uint32_t cp;
  int len;
  int w = 1;
  if (u[0] >= 0x20 && u[0] < 0x7F) {
    cp = u[0];
    len = 1;
  } else {
    len = decode_before(s, end, &cp);
    if (sendable(cp) != cp) { /* a control, a surrogate, ... */
      return 0;
    }
    CTUI_CP_PROPS p = cp_props(cp);
    if (p.gb != CTUI_GB_OTHER || p.width == 0) {
      return 0;
    }
    w = p.width;
  }
  const char *next = s + len;
  if ((end == NULL || next < end) && (unsigned char)*next >= 0x80) {
    uint32_t nc; /* a C1 control: U+FFFD, which joins nothing either */
    decode_before(next, end, &nc);
    CTUI_CP_PROPS q = cp_props(sendable(nc));
    if (q.width == 0 || q.gb == CTUI_GB_EXTEND || q.gb == CTUI_GB_ZWJ ||
        q.gb == CTUI_GB_SPACINGMARK) {
      return 0;
    }
  }
  if (ch != NULL) {
    *ch = cp;
  }
  if (width != NULL) {
    *width = w;
  }
  return (size_t)len;
}

size_t ctui_utf8_cluster(const char *s, size_t n, uint32_t *ch, int *width) {
  const char *end = n == (size_t)-1 ? NULL : s + n;
  if (n > 0 && s[0] != '\0') {
    size_t lone = cluster_lone(s, end, ch, width);
    if (lone > 0) {
      return lone;
    }
  }
  uint32_t cps[CTUI_CLUSTER_MAX];
  int ncps = 0;
  size_t bytes = 0;
  int w = 0;
  CTUI_SEG seg = {.gb = 0xff};
  CTUI_CP_PROPS prev = {0, 0, 0};
  int multicell = 0; /* kitty's is_multicell: born or made wide */

  while (s[bytes] && (end == NULL || s + bytes < end)) {
    uint32_t cp;
    int len = decode_before(s + bytes, end, &cp);
    if (ncps > 0 && (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0))) {
      /* a control character is always a cluster of its own (text layout
       * looks for \n and \t at cluster starts). Prepends before it (all
       * a cluster can be when it ends in one) would get kitty's U+FFFD
       * for it joined: dropped instead */
      if (seg.gb == CTUI_GB_PREPEND) {
        w = 0;
      }
      break;
    }
    cp = sendable(cp);
    CTUI_CP_PROPS p = cp_props(cp);
    if (ncps == 0) {
      bytes += (size_t)len;
      cps[ncps++] = cp;
      w = p.width;
      if (w == 0) {
        break; /* nothing to join: dropped, and the next one starts over */
      }
      multicell = w == 2;
      seg_step(&seg, p);
      prev = p;
      continue;
    }
    CTUI_SEG before = seg;
    if (!seg_step(&seg, p)) {
      if (p.width != 0) {
        break;
      }
      /* zero width where UAX #29 breaks (ZWSP, soft hyphen, a Prepend
       * mid-string): invisible, and kitty would glue it to this cell and
       * segment what follows differently (no flag pairing after ZWSP):
       * dropped, as if it weren't there */
      seg = before;
      bytes += (size_t)len;
      continue;
    }
    bytes += (size_t)len;
    /* kitty's draw_combining_char(): the variation selectors look at the
     * codepoint right before them, a cell once wide stays "multicell" */
    if (cp == CTUI_VS16 && (prev.flags & CTUI_PROP_EMOJI_BASE) && !multicell) {
      w = 2;
      multicell = 1;
    } else if (cp == CTUI_VS15 && (prev.flags & CTUI_PROP_EMOJI_BASE) &&
               multicell && w == 2) {
      w = 1;
    } else if (seg.gb == CTUI_GB_SPACINGMARK && p.width > 0 && !multicell) {
      w = 2; /* Thai/Lao SARA AM widens a narrow base */
      multicell = 1;
    }
    if (ncps < CTUI_CLUSTER_MAX) {
      cps[ncps++] = cp;
    }
    prev = p;
  }

  if (ch != NULL && ncps == 0) {
    *ch = 0; /* the end of s */
  } else if (ch != NULL) {
    *ch = ncps > 1 ? cluster_intern(cps, ncps, w) : cps[0];
    if (ncps > 1 && *ch == 0) {
      *ch = cps[0];
      w = cp_props(cps[0]).width;
    }
  }
  if (width != NULL) {
    *width = bytes ? w : 0;
  }
  return bytes;
}

const uint32_t *ctui_cell_cluster(uint32_t ch, int *n) {
  if ((ch & CTUI_CELL_CLUSTER) && !(ch & CTUI_CELL_CONT)) {
    uint32_t i = ch & ~CTUI_CELL_CLUSTER;
    if (i < g_clusters.n) {
      *n = g_clusters.ent[i].len;
      return g_clusters.cps + g_clusters.ent[i].off;
    }
  }
  *n = 0;
  return NULL;
}

int ctui_cell_width(uint32_t ch) {
  if (ch == CTUI_CELL_CONT) {
    return 0;
  }
  if (ch & CTUI_CELL_CLUSTER) {
    uint32_t i = ch & ~CTUI_CELL_CLUSTER;
    return i < g_clusters.n ? g_clusters.ent[i].width : 1;
  }
  return ctui_utf8_cpwidth(ch);
}

int ctui_cell_encode(uint32_t ch, char *out) {
  int n;
  const uint32_t *cps = ctui_cell_cluster(ch, &n);
  if (cps == NULL) {
    return ctui_utf8_encode(sendable(ch), out);
  }
  int len = 0;
  for (int i = 0; i < n; i++) {
    len += ctui_utf8_encode(cps[i], out + len);
  }
  return len;
}

int ctui_utf8_width(const char *s) {
  int width = 0;
  while (*s) {
    int w;
    s += ctui_utf8_cluster(s, (size_t)-1, NULL, &w);
    width += w;
  }
  return width;
}

size_t ctui_utf8_prefix(const char *s, int cols, int *width_out) {
  size_t bytes = 0;
  int width = 0;
  while (s[bytes]) {
    int w;
    size_t n = ctui_utf8_cluster(s + bytes, (size_t)-1, NULL, &w);
    if (width + w > cols) {
      break;
    }
    width += w;
    bytes += n;
  }
  if (width_out) {
    *width_out = width;
  }
  return bytes;
}

int ctui_cell_set_ch(CTUI_CELL *row, int row_len, int col, int limit,
                     uint32_t cp) {
  int w = ctui_cell_width(cp);
  if (w == 0) {
    ctui_logf(E_DBG,
              "[CTUI:UTF8] - dropping zero-width U+%04X @ tick %d (nothing "
              "to join)\n",
              cp, ctui_tick_advance());
    return 0;
  }
  if (limit > row_len) {
    limit = row_len;
  }
  if (w == 2 && col + 1 >= limit) {
    cp = ' ';
    w = 1;
  }

  /* break up whatever wide glyph(s) this write lands on: our left edge
   * landing on a CONT orphans the lead to its left, and our right edge
   * landing on a lead orphans the CONT to its right */
  if (row[col].ch == CTUI_CELL_CONT && col > 0) {
    row[col - 1].ch = ' ';
  }
  int right = col + w - 1;
  if (right + 1 < row_len && row[right + 1].ch == CTUI_CELL_CONT) {
    row[right + 1].ch = ' ';
  }

  row[col].ch = cp;
  if (w == 2) {
    row[col + 1].ch = CTUI_CELL_CONT;
  }
  return w;
}
