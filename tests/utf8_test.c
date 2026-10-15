/* Exercises core/utf8.c and the UTF-8 paths it feeds: codepoint
 * decode/encode, column widths, wide-glyph placement (lead cell +
 * CTUI_CELL_CONT) through ctui_widget_puts(), the pair-breaking rules when
 * something overwrites half a wide glyph, width-aware util helpers, and
 * the literal UTF-8 bytes ctui_screen_flush() emits. */
#include "ctui.h"

#include "core/utf8_props.h"
#include "ctui_test.h"
#include "utf8_ref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void noop_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  (void)comp;
}

static void test_decode_encode(void) {
  uint32_t cp;
  CTUI_TEST_ASSERT(ctui_utf8_decode("A", &cp) == 1 && cp == 'A',
                   "ASCII decodes as itself, 1 byte");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xc3\xa9", &cp) == 2 && cp == 0xE9,
                   "2-byte sequence decodes (U+00E9)");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xe4\xbd\xa0", &cp) == 3 && cp == 0x4F60,
                   "3-byte sequence decodes (U+4F60)");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xf0\x9f\x98\x80", &cp) == 4 &&
                       cp == 0x1F600,
                   "4-byte sequence decodes (U+1F600)");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xe4\xbd", &cp) == 1 && cp == 0xFFFD,
                   "a sequence truncated by the NUL terminator decodes as "
                   "U+FFFD consuming 1 byte, never reading past the string");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xc0\xaf", &cp) == 1 && cp == 0xFFFD,
                   "an overlong encoding is rejected as U+FFFD");
  CTUI_TEST_ASSERT(ctui_utf8_decode("\xed\xa0\x80", &cp) == 1 &&
                       cp == 0xFFFD,
                   "an encoded surrogate is rejected as U+FFFD");

  char buf[4];
  CTUI_TEST_ASSERT(ctui_utf8_encode(0x1F600, buf) == 4 &&
                       memcmp(buf, "\xf0\x9f\x98\x80", 4) == 0,
                   "encode round-trips a 4-byte codepoint");
  CTUI_TEST_ASSERT(ctui_utf8_encode(0xD800, buf) == 3 &&
                       memcmp(buf, "\xef\xbf\xbd", 3) == 0,
                   "encoding a surrogate emits U+FFFD instead");
}

static void test_widths(void) {
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth('a') == 1 &&
                       ctui_utf8_cpwidth(0xE9) == 1,
                   "ASCII and Latin-1 letters are 1 column");
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth(0x4F60) == 2 &&
                       ctui_utf8_cpwidth(0x1F600) == 2,
                   "CJK and emoji are 2 columns");
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth(0x0301) == 0,
                   "a combining mark is 0 columns");
  /* where libc wcwidth() and kitty disagree (tools/check_widths.py
   * compares every codepoint): kitty is the reference */
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth(0xFA6E) == 2 &&
                       ctui_utf8_cpwidth(0x1FAE8) == 2 &&
                       ctui_utf8_cpwidth(0x261D) == 2,
                   "newer CJK, newer emoji and emoji-presentation bases are "
                   "2 columns, as kitty draws them");
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth(0xAD) == 0 &&
                       ctui_utf8_cpwidth(0x0600) == 0 &&
                       ctui_utf8_cpwidth(0x200B) == 0,
                   "soft hyphen, format characters and zero width space "
                   "are 0 columns");
  CTUI_TEST_ASSERT(
      ctui_utf8_cpwidth(0x1161) == 1 && ctui_utf8_cpwidth(0xE000) == 1 &&
          ctui_utf8_cpwidth(0xFFFE) == 1 && ctui_utf8_cpwidth(0x0378) == 1,
      "Hangul vowel jamo, private use, noncharacters (sent as "
      "U+FFFD) and unassigned codepoints are 1 column");
  CTUI_TEST_ASSERT(ctui_utf8_width("a\xe4\xbd\xa0" "b") == 4,
                   "string width sums per-glyph widths, not bytes (5 bytes, "
                   "4 columns)");

  int w;
  size_t n = ctui_utf8_prefix("ab\xe4\xbd\xa0" "c", 3, &w);
  CTUI_TEST_ASSERT(n == 2 && w == 2,
                   "prefix stops before a wide glyph that would straddle "
                   "the column limit, reporting the narrower width");
}

static void test_clusters(void) {
  CTUI_TEST_ASSERT(ctui_utf8_width("\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa") == 2 &&
                       ctui_utf8_width("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd") == 2,
                   "a flag and a skin-toned emoji are one 2-column glyph each "
                   "(4 columns summed per codepoint: the row drifted)");
  CTUI_TEST_ASSERT(ctui_utf8_width("e\xcc\x81x") == 2 &&
                       ctui_utf8_width("\xf0\x9f\x91\xa8\xe2\x80\x8d"
                                       "\xf0\x9f\x91\xa9\xe2\x80\x8d"
                                       "\xf0\x9f\x91\xa7") == 2 &&
                       ctui_utf8_width("\xe1\x84\x80\xe1\x85\xa1") == 2,
                   "a decomposed e + acute, a ZWJ family and Hangul L+V jamo "
                   "are one glyph each");
  CTUI_TEST_ASSERT(ctui_utf8_width("\xe2\x9d\xa4") == 1 &&
                       ctui_utf8_width("\xe2\x9d\xa4\xef\xb8\x8f") == 2 &&
                       ctui_utf8_width("\xe2\x8c\x9a\xef\xb8\x8e") == 1 &&
                       ctui_utf8_width("\xe0\xb8\x81\xe0\xb8\xb3") == 2,
                   "VS16 widens a text-style emoji, VS15 narrows a wide one, "
                   "Thai SARA AM widens its base: as kitty draws them");

  uint32_t a, b;
  int w;
  size_t n = ctui_utf8_cluster("e\xcc\x81!", (size_t)-1, &a, &w);
  ctui_utf8_cluster("e\xcc\x81", (size_t)-1, &b, NULL);
  int m;
  const uint32_t *cps = ctui_cell_cluster(a, &m);
  CTUI_TEST_ASSERT(n == 3 && w == 1 && (a & CTUI_CELL_CLUSTER) && a == b &&
                       cps != NULL && m == 2 && cps[0] == 'e' &&
                       cps[1] == 0x301,
                   "a cluster is one interned cell value: the same text gives "
                   "the same value, and it keeps its codepoints");
  CTUI_TEST_ASSERT(
      ctui_utf8_cluster("x\xe2\x80\x8b\xcc\x81y", (size_t)-1, &a, &w) == 6 &&
          ctui_cell_cluster(a, &m) != NULL && m == 2 && w == 1,
      "a zero width space inside a word is dropped, the mark "
      "after it still joins the x");
  CTUI_TEST_ASSERT(ctui_utf8_cluster("\xcc\x81x", (size_t)-1, &a, &w) == 2 &&
                       w == 0,
                   "a leading combining mark has nothing to join: width 0, "
                   "the caller drops it");
  CTUI_TEST_ASSERT(
      ctui_utf8_cluster("a\n\xcc\x81", (size_t)-1, NULL, &w) == 1 &&
          ctui_utf8_cluster("\n\xcc\x81", (size_t)-1, &a, &w) == 3 && w == 1,
      "a control character never joins the glyph before it "
      "(layout looks for \\n at glyph starts)");
  a = 1;
  CTUI_TEST_ASSERT(ctui_utf8_cluster("", (size_t)-1, &a, &w) == 0 && a == 0 &&
                       w == 0,
                   "the end of the string: 0 bytes, width 0, cell value 0");
  size_t p = ctui_utf8_prefix("ab\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa", 3, &w);
  CTUI_TEST_ASSERT(p == 2 && w == 2,
                   "prefix never splits a cluster (a flag straddling the "
                   "limit stays whole, outside)");

  /* a character cut at n, the bytes after it continuations: not read */
  const char cut[] = "a\xe4\xbd\xa0";
  CTUI_TEST_ASSERT(ctui_utf8_cluster(cut + 1, 2, &a, &w) == 1 &&
                       a == 0xFFFD,
                   "a 3-byte character cut by n after 2 bytes: U+FFFD, 1 "
                   "byte, nothing read past n");
  CTUI_TEST_ASSERT(ctui_utf8_cluster(cut, 2, &a, &w) == 1 && a == 'a',
                   "a cluster ending where a cut character starts stops "
                   "before it");
}

static void test_widget_wide_glyphs(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(1, 8);
  CTUI_WIDGET w = ctui_widget_make(0, 0, 6, 1, NULL, noop_render, NULL);
  ctui_widget_init(&w, comp);

  ctui_widget_puts(&w, comp, 0, 0, "a\xe4\xbd\xa0" "b", CTUI_COLOR_RED,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[0].ch == 'a' && comp->cells[1].ch == 0x4F60 &&
                       comp->cells[2].ch == CTUI_CELL_CONT &&
                       comp->cells[3].ch == 'b',
                   "puts lays a wide glyph out as lead + CONT, and the next "
                   "glyph lands after both");
  CTUI_TEST_ASSERT(comp->cells[2].fg == CTUI_COLOR_RED,
                   "the CONT cell carries the glyph's colors too, so a "
                   "background color covers both columns");

  ctui_widget_putc(&w, comp, 0, 2, 'x', CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[1].ch == ' ' && comp->cells[2].ch == 'x',
                   "writing over a CONT blanks the orphaned lead to its left");

  ctui_widget_putc(&w, comp, 0, 3, 0x4F60, CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  ctui_widget_putc(&w, comp, 0, 3, 'y', CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[3].ch == 'y' && comp->cells[4].ch == ' ',
                   "writing a narrow glyph over a lead blanks its orphaned "
                   "CONT to the right");

  ctui_widget_putc(&w, comp, 0, 5, 0x4F60, CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[5].ch == ' ' && comp->cells[6].ch == ' ',
                   "a wide glyph in the widget's last column is clipped to a "
                   "space instead of spilling past the widget's right edge");

  ctui_widget_putc(&w, comp, 0, 0, 0x0301, CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[0].ch == 'a',
                   "a zero-width codepoint is dropped, leaving the cell alone");

  ctui_widget_puts(&w, comp, 0, 0, "e\xcc\x81\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbdz",
                   CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  int m1, m2;
  CTUI_TEST_ASSERT(
      ctui_cell_cluster(comp->cells[0].ch, &m1) != NULL && m1 == 2 &&
          ctui_cell_cluster(comp->cells[1].ch, &m2) != NULL && m2 == 2 &&
          comp->cells[2].ch == CTUI_CELL_CONT && comp->cells[3].ch == 'z',
      "puts lays clusters out one per cell: e + acute, then the "
      "skin-toned thumb as lead + CONT, then z");

  ctui_compositor_free(comp);
}

static void test_util_widths(void) {
  char line[16];
  memset(line, ' ', 8);
  line[8] = '\0';
  CTUI_TEST_ASSERT(
      ctui_util_center_h("\xe4\xbd\xa0\xe5\xa5\xbd", line,
                         (CTUI_CELL){.ch = '-'}) == 0 &&
          strcmp(line, "-\xe4\xbd\xa0\xe5\xa5\xbd-") == 0,
      "center_h never writes past strlen(line): a 4-column, 6-byte string "
      "in an 8-byte line gets the 2 bytes of padding that fit, not the 4 "
      "that width-centering would want");

  char tight[] = "      "; /* 6 bytes, a caller sized for ASCII */
  CTUI_TEST_ASSERT(ctui_util_center_h("\xe4\xbd\xa0\xe5\xa5\xbd", tight,
                                      (CTUI_CELL){.ch = ' '}) == 0 &&
                       strcmp(tight, "\xe4\xbd\xa0\xe5\xa5\xbd") == 0,
                   "an exactly-ASCII-sized buffer gets the glyphs with no "
                   "padding rather than an overflow");

  CTUI_TEST_ASSERT(ctui_util_center_col("\xe4\xbd\xa0\xe5\xa5\xbd", 8) == 2 &&
                       ctui_util_center_col("abcdefghij", 8) == 0,
                   "center_col centers by columns (4 wide in 8 starts at 2), "
                   "0 when it doesn't fit");

  char s[] = "\xe4\xbd\xa0\xe5\xa5\xbd\xe5\x90\x97"; /* 3 wide glyphs */
  CTUI_TEST_ASSERT(ctui_util_truncate_str(s, 5, "..") == 0 &&
                       strcmp(s, "\xe4\xbd\xa0..") == 0,
                   "truncate_str cuts by columns at a glyph boundary: 6 "
                   "columns into 5 keeps one wide glyph (2) + \"..\" (2), "
                   "since a second wide glyph would straddle the cut");
}

static size_t capture_flush(CTUI_SCREEN *s, char *out, size_t out_cap) {
  int pipefd[2];
  pipe(pipefd);
  int saved_stdout = dup(STDOUT_FILENO);
  dup2(pipefd[1], STDOUT_FILENO);
  close(pipefd[1]);
  ctui_screen_flush(s);
  dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
  ssize_t n = read(pipefd[0], out, out_cap - 1);
  close(pipefd[0]);
  if (n < 0) {
    n = 0;
  }
  out[n] = '\0';
  return (size_t)n;
}

static void test_flush_utf8(void) {
  CTUI_SCREEN *s = ctui_screen_create(1, 5);
  char out[256];
  capture_flush(s, out, sizeof out); /* prime the shadow buffer */

  ctui_screen_puts(s, 0, 0, "\xe4\xbd\xa0" "b", CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strcmp(out, "\x1b[1;1H\x1b[39;49m\xe4\xbd\xa0"
                               "b\x1b[0m") == 0,
                   "flush emits the wide glyph's UTF-8 bytes, nothing for its "
                   "CONT cell, and the next glyph without a reposition (the "
                   "terminal cursor already advanced 2 columns)");

  ctui_screen_putc(s, 0, 4, '\x1b', CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strcmp(out, "\x1b[1;5H\x1b[39;49m\xef\xbf\xbd\x1b[0m") ==
                       0,
                   "a raw control byte in a cell is emitted as U+FFFD, never "
                   "as itself");

  ctui_screen_puts(s, 0, 0, "e\xcc\x81", CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  capture_flush(s, out, sizeof out);
  CTUI_TEST_ASSERT(strcmp(out, "\x1b[1;1H\x1b[39;49me\xcc\x81 \x1b[0m") == 0,
                   "flush emits every codepoint of a cluster cell (the space: "
                   "the orphaned half of the wide glyph it replaced)");

  ctui_screen_free(s);

  /* a screen whose every cell is a long cluster outgrows the per-cell
   * flush budget: the buffer grows instead of overflowing */
  s = ctui_screen_create(1, 40);
  capture_flush(s, out, sizeof out);
  char lng[80] = "e"; /* e + 15 four-byte combining marks: 61 bytes */
  for (int i = 0; i < CTUI_CLUSTER_MAX - 1; i++) {
    strcat(lng, "\xf0\x9d\x85\xa7");
  }
  for (int c = 0; c < 40; c++) {
    ctui_screen_puts(s, 0, c, lng, c % 2 ? CTUI_COLOR_RED : CTUI_COLOR_BLUE,
                     CTUI_COLOR_DEFAULT);
  }
  static char big[16384];
  size_t len = capture_flush(s, big, sizeof big);
  CTUI_TEST_ASSERT(len > (size_t)40 * 64 + 64 && strstr(big, lng) != NULL,
                   "40 cells of 61-byte clusters go out whole, past the "
                   "flush's per-cell budget (the buffer grows)");
  ctui_screen_free(s);
}

/* the two-stage table holds what the ranges say, for every codepoint
 * (the Hangul syllables' LV/LVT resolved the way ref_props() does) */
static void test_props_table(void) {
  uint32_t bad = 0, first_bad = 0;
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    REF_PROPS r = ref_props(cp);
    unsigned block = ctui_utf8_props_block[cp >> CTUI_UTF8_PROPS_SHIFT];
    unsigned v = ctui_utf8_props[block << CTUI_UTF8_PROPS_SHIFT |
                                 (cp & ((1u << CTUI_UTF8_PROPS_SHIFT) - 1))];
    if ((v & 3) != r.width || (v >> 2 & 15) != r.gb || (v >> 6) != r.flags ||
        ctui_utf8_cpwidth(cp) != r.width) {
      first_bad = bad++ ? first_bad : cp;
    }
  }
  if (bad) {
    fprintf(stderr, "  first differing codepoint: U+%04X\n", first_bad);
  }
  CTUI_TEST_ASSERT(bad == 0, "every codepoint's width, class and flags in "
                             "the two-stage table match the ranges");
  CTUI_TEST_ASSERT(ctui_utf8_cpwidth(0x110000) == 1 &&
                       ctui_utf8_cpwidth(0xFFFFFFFFu) == 1,
                   "past U+10FFFF: width 1 like anything unlisted, no read "
                   "past the table");
}

/* ctui_utf8_cluster() against ref_cluster() on s[0..n): the same bytes,
 * width and codepoints. A cluster of several codepoints only compared
 * with intern set: each distinct one stays interned (CTUI_CLUSTER_LIMIT,
 * 2^20), too few for every codepoint with a mark after it. 0: they agree */
static int cluster_differs(const char *s, size_t n, int intern) {
  uint32_t ref[CTUI_CLUSTER_MAX];
  int nref = 0, wref = -1, w = -1;
  uint32_t ch = 0xDEAD;
  size_t rb = ref_cluster(s, n, ref, &nref, &wref);
  int with_ch = intern || nref <= 1;
  size_t b = ctui_utf8_cluster(s, n, with_ch ? &ch : NULL, &w);
  int w2 = -1;
  if (b != rb || w != wref || ctui_utf8_cluster(s, n, NULL, &w2) != b ||
      w2 != w) {
    return 1;
  }
  if (!with_ch) {
    return 0;
  }
  int m = 1;
  const uint32_t *cps = ctui_cell_cluster(ch, &m);
  if (cps == NULL) {
    cps = &ch;
    m = nref == 0 ? 0 : 1;
  }
  if (nref == 0) {
    return ch != 0;
  }
  return m != nref || memcmp(cps, ref, (size_t)m * sizeof *cps) != 0;
}

/* one codepoint of each kind the segmentation tells apart */
static const uint32_t probes[] = {
    'a',    '#',    ' ',    '\n',    '\r',    0x01,   0x7F,   0x85,
    0xA0,   0xAD,   0x301,  0x200D,  0x903,   0x600,  0x1100, 0x1161,
    0x11A8, 0xAC00, 0xAC01, 0x1F1E6, 0x1F1E9, 0xFE0F, 0xFE0E, 0x200B,
    0x915,  0x94D,  0x93C,  0x1F600, 0x1F3FD, 0x2764, 0x231A, 0x4E00,
    0xE33,  0xFFFD, 0xFFFE, 0xD800,  0xE0067, 0x2500, 0x2580, 0xFF21};
#define NPROBES (sizeof probes / sizeof probes[0])

/* cps encoded into buf (a surrogate or a noncharacter as its raw
 * three-byte form, the way a careless producer writes it) */
static size_t encode_raw(const uint32_t *cps, int n, char *buf) {
  size_t len = 0;
  for (int i = 0; i < n; i++) {
    uint32_t c = cps[i];
    if (c >= 0xD800 && c <= 0xDFFF) {
      buf[len++] = (char)(0xE0 | (c >> 12));
      buf[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (c & 0x3F));
    } else {
      len += (size_t)ctui_utf8_encode(c, buf + len);
    }
  }
  buf[len] = '\0';
  return len;
}

/* the codepoints the segmentation can tell apart: the first and the last
 * of each class -- the same properties (the table is checked for every
 * codepoint above), the same encoded length, replaced or not -- and every
 * codepoint the code names (controls, the variation selectors) as a class
 * of its own */
static uint32_t *class_reps(int *n) {
  enum { KEYS = 8192 }; /* the key: 10 bits of properties, 3 more */
  static int first[KEYS], last[KEYS];
  memset(first, -1, sizeof first);
  for (uint32_t cp = 1; cp <= 0x10FFFF; cp++) {
    REF_PROPS p = ref_props(cp);
    char tmp[4];
    int len = ctui_utf8_encode(cp, tmp);
    int key = p.width | p.gb << 2 | p.flags << 6;
    key = key << 3 | (len - 1) << 1 | (ref_sendable(cp) != cp);
    if (cp < 0xA0 || cp == REF_VS15 || cp == REF_VS16) {
      key = KEYS - 1 - (int)(cp < 0xA0 ? cp : cp - REF_VS15 + 0xA0);
    }
    if (first[key] < 0) {
      first[key] = (int)cp;
    }
    last[key] = (int)cp;
  }
  uint32_t *reps = malloc(2 * KEYS * sizeof *reps);
  *n = 0;
  for (int k = 0; reps && k < KEYS; k++) {
    if (first[k] >= 0) {
      reps[(*n)++] = (uint32_t)first[k];
      if (last[k] != first[k]) {
        reps[(*n)++] = (uint32_t)last[k];
      }
    }
  }
  return reps;
}

static void test_clusters_vs_ref(void) {
  char buf[64];
  uint32_t seq[4];
  int bad = 0;
  /* every codepoint alone, cut short too */
  for (uint32_t cp = 1; cp <= 0x10FFFF; cp++) {
    seq[0] = cp;
    size_t len = encode_raw(seq, 1, buf);
    bad +=
        cluster_differs(buf, (size_t)-1, 0) | cluster_differs(buf, len - 1, 0);
  }
  CTUI_TEST_ASSERT(bad == 0, "every codepoint alone (and cut short): the same "
                             "cluster as the reference segmentation");

  /* every class next to every class, cut in the second */
  int nreps = 0;
  uint32_t *reps = class_reps(&nreps);
  bad = reps == NULL;
  for (int i = 0; i < nreps; i++) {
    for (int j = 0; j < nreps; j++) {
      seq[0] = reps[i];
      seq[1] = reps[j];
      size_t len = encode_raw(seq, 2, buf);
      bad += cluster_differs(buf, (size_t)-1, 0) |
             cluster_differs(buf, len - 1, 0);
    }
  }
  free(reps);
  CTUI_TEST_ASSERT(bad == 0 && nreps > 100,
                   "a codepoint of each class before one of each class: the "
                   "same clusters as the reference segmentation");

  /* every run of three probes, and random strings of probes and random
   * codepoints cut at every length */
  bad = 0;
  for (size_t i = 0; i < NPROBES; i++) {
    for (size_t j = 0; j < NPROBES; j++) {
      for (size_t k = 0; k < NPROBES; k++) {
        seq[0] = probes[i];
        seq[1] = probes[j];
        seq[2] = probes[k];
        encode_raw(seq, 3, buf);
        bad += cluster_differs(buf, (size_t)-1, 1);
      }
    }
  }
  srand(219);
  for (int r = 0; r < 20000; r++) {
    uint32_t run[8];
    int n = 1 + rand() % 8;
    for (int i = 0; i < n; i++) {
      run[i] = rand() % 3 ? probes[(size_t)rand() % NPROBES]
                          : (uint32_t)rand() % 0x110000;
    }
    size_t len = encode_raw(run, n, buf);
    for (size_t cut = 0; cut <= len; cut++) {
      bad += cluster_differs(buf, cut, 1);
    }
    /* stray continuation and lead bytes */
    buf[rand() % (int)len] = (char)(0x80 | rand() % 0x80);
    bad += cluster_differs(buf, (size_t)-1, 1) | cluster_differs(buf, len, 1);
  }
  CTUI_TEST_ASSERT(bad == 0, "runs of probes and random strings (cut at "
                             "every byte, broken bytes in them): the same "
                             "clusters as the reference");
}

int main(void) {
  ctui_log_init(E_WRN | E_ERR);

  test_decode_encode();
  test_widths();
  test_clusters();
  test_props_table();
  test_clusters_vs_ref();
  test_widget_wide_glyphs();
  test_util_widths();
  test_flush_utf8();

  ctui_log_shutdown();
  return ctui_test_summary();
}
