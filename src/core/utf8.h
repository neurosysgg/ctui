#ifndef CTUI_UTF8_H
#define CTUI_UTF8_H

#include "cell.h"

#include <stddef.h>
#include <stdint.h>

/* UTF-8 <-> codepoint plumbing plus terminal column widths. CTUI_CELL.ch
 * holds one codepoint; a width-2 glyph occupies its own cell plus a
 * CTUI_CELL_CONT cell immediately to its right (see cell.h). */

/* decodes one codepoint from s into *cp and returns how many bytes it
 * consumed (1-4). Malformed/truncated input decodes as U+FFFD and consumes
 * exactly 1 byte, so a caller walking a string always makes progress. A
 * NUL byte decodes as 0, consuming 1. */
int ctui_utf8_decode(const char *s, uint32_t *cp);

/* encodes cp into out (at least 4 bytes, not NUL-terminated) and returns
 * the byte count (1-4). Invalid codepoints (surrogates, > U+10FFFF) encode
 * as U+FFFD. */
int ctui_utf8_encode(uint32_t cp, char *out);

/* terminal column width of cp: 0 (combining/zero-width -- ctui has no
 * grapheme clustering, so callers drop these), 1, or 2 (East Asian wide,
 * emoji). Backed by libc wcwidth() so it agrees with what the terminal
 * itself decides. Control characters and anything wcwidth() rejects
 * report 1, since ctui_screen_flush() substitutes them with U+FFFD rather
 * than emitting them raw. */
int ctui_utf8_cpwidth(uint32_t cp);

/* total column width of the NUL-terminated UTF-8 string s -- the sum of
 * ctui_utf8_cpwidth() over its codepoints. What ctui_widget_puts() will
 * actually occupy, as opposed to strlen()'s byte count. */
int ctui_utf8_width(const char *s);

/* byte length of the longest prefix of s whose column width is <= cols,
 * never splitting a codepoint. The width that prefix actually covers is
 * stored in *width_out if non-NULL (can be cols - 1 when a wide glyph
 * would straddle the limit). */
size_t ctui_utf8_prefix(const char *s, int cols, int *width_out);

/* writes cp at row[col], keeping wide glyphs consistent with their
 * CTUI_CELL_CONT partner: overwriting either half of an existing wide
 * glyph blanks its other half, and a width-2 cp that wouldn't fit before
 * limit (exclusive, in row-relative columns) is written as a space
 * instead. row_len bounds neighbor access. Only touches .ch -- the caller
 * owns colors, and must copy them onto row[col + 1] when this returns 2.
 * Returns the columns consumed (0 for a dropped zero-width cp, 1, or 2).
 * Core-internal plumbing shared by ctui_widget_putc*() and
 * ctui_screen_putc(); widgets never call this directly. */
int ctui_cell_set_ch(CTUI_CELL *row, int row_len, int col, int limit,
                     uint32_t cp);

#endif
