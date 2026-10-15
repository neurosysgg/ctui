/* tests/utf8_test.c's reference: core/utf8.c's properties and cluster
 * segmentation as they were before the two-stage table and the lone
 * codepoint fast path (ctui-wm's notes/self-improving.md, session 219),
 * kept slow and simple -- a binary search over the generated ranges, every
 * codepoint through the whole UAX #29 state machine. The core must agree
 * with it codepoint by codepoint. Keep it as it is: a fix to the
 * segmentation goes into both. */
#ifndef CTUI_TEST_UTF8_REF_H
#define CTUI_TEST_UTF8_REF_H

#include "core/utf8.h"
#include "utf8_ref_props.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define REF_REPLACEMENT 0xFFFDu

/* Grapheme_Cluster_Break classes, in the order tools/gen_utf8_props.py
 * writes them; LV/LVT come out of REF_GB_HANGUL, RI_PENDING is a state
 * only (an odd regional indicator waiting for its pair) */
enum {
  REF_GB_OTHER,
  REF_GB_CR,
  REF_GB_LF,
  REF_GB_CONTROL,
  REF_GB_EXTEND,
  REF_GB_ZWJ,
  REF_GB_RI,
  REF_GB_PREPEND,
  REF_GB_SPACINGMARK,
  REF_GB_L,
  REF_GB_V,
  REF_GB_T,
  REF_GB_HANGUL,
  REF_GB_LV,
  REF_GB_LVT,
  REF_GB_RI_PENDING,
};

#define REF_INCB_LINKER 1
#define REF_INCB_CONSONANT 2
#define REF_INCB_EXTEND 3
#define REF_PROP_EXTPICT 4
#define REF_PROP_EMOJI_BASE 8

#define REF_VS15 0xFE0Eu
#define REF_VS16 0xFE0Fu

typedef struct {
  unsigned char width, gb, flags;
} REF_PROPS;

static REF_PROPS ref_props(uint32_t cp) {
  REF_PROPS p = {1, REF_GB_OTHER, 0};
  size_t lo = 0;
  size_t hi = sizeof ref_utf8_props_table / sizeof ref_utf8_props_table[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < ref_utf8_props_table[mid].first) {
      hi = mid;
    } else if (cp > ref_utf8_props_table[mid].last) {
      lo = mid + 1;
    } else {
      p.width = ref_utf8_props_table[mid].width;
      p.gb = ref_utf8_props_table[mid].gb;
      p.flags = ref_utf8_props_table[mid].flags;
      break;
    }
  }
  if (p.gb == REF_GB_HANGUL) {
    p.gb = (cp - 0xAC00) % 28 == 0 ? REF_GB_LV : REF_GB_LVT;
  }
  return p;
}

static uint32_t ref_sendable(uint32_t cp) {
  if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) ||
      (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0xFDD0 && cp < 0xFDF0) ||
      (cp & 0xFFFE) == 0xFFFE || cp > 0x10FFFF) {
    return REF_REPLACEMENT;
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
} REF_SEG;

/* steps seg over p (the cluster's first codepoint when seg->gb is 0xff)
 * and returns whether that codepoint joins the cluster */
static int ref_seg_step(REF_SEG *seg, REF_PROPS p) {
  int prev = seg->gb;
  int gb = p.gb;
  int incb = p.flags & 3;
  int join = 0;
  if (prev == 0xff) {
    join = 1;
    if (gb == REF_GB_RI) {
      gb = REF_GB_RI_PENDING;
    }
  } else if (prev == REF_GB_CR && gb == REF_GB_LF) {
    join = 1;
  } else if (prev == REF_GB_CR || prev == REF_GB_LF || prev == REF_GB_CONTROL ||
             gb == REF_GB_CR || gb == REF_GB_LF || gb == REF_GB_CONTROL) {
    join = 0;
  } else if ((prev == REF_GB_L && (gb == REF_GB_L || gb == REF_GB_V ||
                                   gb == REF_GB_LV || gb == REF_GB_LVT)) ||
             ((prev == REF_GB_LV || prev == REF_GB_V) &&
              (gb == REF_GB_V || gb == REF_GB_T)) ||
             ((prev == REF_GB_LVT || prev == REF_GB_T) && gb == REF_GB_T)) {
    join = 1;
  } else if (gb == REF_GB_EXTEND || gb == REF_GB_ZWJ ||
             gb == REF_GB_SPACINGMARK || prev == REF_GB_PREPEND) {
    join = 1;
  } else if (seg->incb_linked && incb == REF_INCB_CONSONANT) {
    join = 1;
  } else if (prev == REF_GB_ZWJ && seg->emoji_seq_before_last &&
             (p.flags & REF_PROP_EXTPICT)) {
    join = 1;
  } else if (gb == REF_GB_RI) {
    if (prev == REF_GB_RI_PENDING) {
      join = 1;
    } else {
      gb = REF_GB_RI_PENDING;
    }
  }

  int linker_or_extend = incb == REF_INCB_LINKER || incb == REF_INCB_EXTEND;
  int consonant_linked = seg->incb_consonant && incb == REF_INCB_LINKER;
  seg->incb_linked = consonant_linked || (seg->incb_linked && linker_or_extend);
  seg->incb_consonant =
      incb == REF_INCB_CONSONANT || (seg->incb_consonant && linker_or_extend);
  seg->emoji_seq_before_last = seg->emoji_seq;
  seg->emoji_seq = (seg->emoji_seq && gb == REF_GB_EXTEND) ||
                   (p.flags & REF_PROP_EXTPICT) != 0;
  seg->gb = (unsigned char)gb;
  return join;
}

static size_t ref_cluster(const char *s, size_t n, uint32_t *cps, int *ncps_out,
                          int *width) {
  const char *end = n == (size_t)-1 ? NULL : s + n;
  int ncps = 0;
  size_t bytes = 0;
  int w = 0;
  REF_SEG seg = {.gb = 0xff};
  REF_PROPS prev = {0, 0, 0};
  int multicell = 0; /* kitty's is_multicell: born or made wide */

  while (s[bytes] && (end == NULL || s + bytes < end)) {
    uint32_t cp;
    int len;
    if (end && end - (s + bytes) < 4) {
      /* a character cut at n decodes as U+FFFD, never reading past n */
      char tail[5] = {0};
      memcpy(tail, s + bytes, (size_t)(end - (s + bytes)));
      len = ctui_utf8_decode(tail, &cp);
    } else {
      len = ctui_utf8_decode(s + bytes, &cp);
    }
    if (ncps > 0 && (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0))) {
      /* a control character is always a cluster of its own (text layout
       * looks for \n and \t at cluster starts). Prepends before it (all
       * a cluster can be when it ends in one) would get kitty's U+FFFD
       * for it joined: dropped instead */
      if (seg.gb == REF_GB_PREPEND) {
        w = 0;
      }
      break;
    }
    cp = ref_sendable(cp);
    REF_PROPS p = ref_props(cp);
    if (ncps == 0) {
      bytes += (size_t)len;
      cps[ncps++] = cp;
      w = p.width;
      if (w == 0) {
        break; /* nothing to join: dropped, and the next one starts over */
      }
      multicell = w == 2;
      ref_seg_step(&seg, p);
      prev = p;
      continue;
    }
    REF_SEG before = seg;
    if (!ref_seg_step(&seg, p)) {
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
    if (cp == REF_VS16 && (prev.flags & REF_PROP_EMOJI_BASE) && !multicell) {
      w = 2;
      multicell = 1;
    } else if (cp == REF_VS15 && (prev.flags & REF_PROP_EMOJI_BASE) &&
               multicell && w == 2) {
      w = 1;
    } else if (seg.gb == REF_GB_SPACINGMARK && p.width > 0 && !multicell) {
      w = 2; /* Thai/Lao SARA AM widens a narrow base */
      multicell = 1;
    }
    if (ncps < CTUI_CLUSTER_MAX) {
      cps[ncps++] = cp;
    }
    prev = p;
  }

  *ncps_out = ncps;
  if (width != NULL) {
    *width = bytes ? w : 0;
  }
  return bytes;
}

#endif
