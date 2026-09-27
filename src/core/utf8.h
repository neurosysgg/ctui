#ifndef CTUI_UTF8_H
#define CTUI_UTF8_H

#include "cell.h"

#include <stddef.h>
#include <stdint.h>

/* UTF-8 <-> codepoint plumbing plus terminal column widths. CTUI_CELL.ch
 * holds one codepoint or a grapheme cluster (CTUI_CELL_CLUSTER); a
 * width-2 glyph occupies its own cell plus a CTUI_CELL_CONT cell
 * immediately to its right (see cell.h). Strings are laid out cluster by
 * cluster, the way kitty segments them (UAX #29), so a flag, 👍🏽 or a
 * decomposed é takes the cells kitty gives it. */

/* the most codepoints a cluster keeps; further joining ones are dropped
 * (they never change its width) */
#define CTUI_CLUSTER_MAX 16

/* decodes one codepoint from s into *cp and returns how many bytes it
 * consumed (1-4). Malformed/truncated input decodes as U+FFFD and consumes
 * exactly 1 byte, so a caller walking a string always makes progress. A
 * NUL byte decodes as 0, consuming 1. */
int ctui_utf8_decode(const char *s, uint32_t *cp);

/* encodes cp into out (at least 4 bytes, not NUL-terminated) and returns
 * the byte count (1-4). Invalid codepoints (surrogates, > U+10FFFF) encode
 * as U+FFFD. */
int ctui_utf8_encode(uint32_t cp, char *out);

/* terminal column width of the single codepoint cp: 0 (joins the cell
 * before it), 1, or 2 (East Asian wide, emoji). From a table generated
 * out of the Unicode data with kitty's rules (core/utf8_props.h,
 * tools/gen_utf8_props.py), so it agrees with kitty whatever libc and
 * locale the app runs under. Control characters, surrogates and
 * noncharacters report 1, since the flush substitutes them with U+FFFD
 * rather than emitting them raw. A string's width is its clusters' --
 * ctui_utf8_width(), not a sum of this. */
int ctui_utf8_cpwidth(uint32_t cp);

/* decodes one grapheme cluster from s (at most n bytes; (size_t)-1 = up
 * to the NUL) and returns the bytes it spans, 0 at the end. *width (if
 * non-NULL) gets the columns kitty draws it in: the first codepoint's,
 * widened by VS16 after an emoji, narrowed by VS15, widened by Thai/Lao
 * SARA AM. A cluster starting with a zero-width codepoint is just that
 * codepoint, width 0 (nothing to join it to: callers drop it). *ch (if
 * non-NULL) gets the cell value: the codepoint itself for a
 * one-codepoint cluster, else an interned CTUI_CELL_CLUSTER (main
 * thread only). Control characters and noncharacters come out as
 * U+FFFD, what the flush would send for them. */
size_t ctui_utf8_cluster(const char *s, size_t n, uint32_t *ch, int *width);

/* a cell value's width: ctui_utf8_cpwidth() for a codepoint, the
 * cluster's for a CTUI_CELL_CLUSTER, 0 for CTUI_CELL_CONT */
int ctui_cell_width(uint32_t ch);

/* the codepoints of a CTUI_CELL_CLUSTER cell value (count in *n), NULL
 * for anything else */
const uint32_t *ctui_cell_cluster(uint32_t ch, int *n);

/* encodes a cell value as UTF-8 into out (at least CTUI_CLUSTER_MAX * 4
 * bytes, not NUL-terminated) and returns the byte count: every codepoint
 * of a cluster, U+FFFD for what must not reach the terminal raw */
int ctui_cell_encode(uint32_t ch, char *out);

/* total column width of the NUL-terminated UTF-8 string s -- the sum of
 * its clusters' widths (ctui_utf8_cluster()). What ctui_widget_puts() will
 * actually occupy, as opposed to strlen()'s byte count. */
int ctui_utf8_width(const char *s);

/* byte length of the longest prefix of s whose column width is <= cols,
 * never splitting a cluster. The width that prefix actually covers is
 * stored in *width_out if non-NULL (can be cols - 1 when a wide glyph
 * would straddle the limit). */
size_t ctui_utf8_prefix(const char *s, int cols, int *width_out);

/* writes cp (a codepoint or a CTUI_CELL_CLUSTER) at row[col], keeping
 * wide glyphs consistent with their
 * CTUI_CELL_CONT partner: overwriting either half of an existing wide
 * glyph blanks its other half, and a width-2 cp that wouldn't fit before
 * limit (exclusive, in row-relative columns) is written as a space
 * instead. row_len bounds neighbor access. Only touches .ch -- the caller
 * owns colors, and must copy them onto row[col + 1] when this returns 2.
 * Returns the columns consumed (0 for a dropped zero-width cp, 1, or 2).
 * A lone zero-width cp has nothing to join here (putc is one cell):
 * joining happens in ctui_utf8_cluster(), which puts goes through.
 * Core-internal plumbing shared by ctui_widget_putc*() and
 * ctui_screen_putc(); widgets never call this directly. */
int ctui_cell_set_ch(CTUI_CELL *row, int row_len, int col, int limit,
                     uint32_t cp);

#endif
