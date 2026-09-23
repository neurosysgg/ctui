/* wcwidth() is XSI, not C11 -- same "before the first system header"
 * requirement as term.c's _POSIX_C_SOURCE */
#define _XOPEN_SOURCE 700

#include "utf8.h"

#include "log.h"

#include <locale.h>
#include <stdlib.h>
#include <wchar.h>

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

/* wcwidth() answers per LC_CTYPE, and in the default "C" locale every
 * non-ASCII codepoint is unprintable (-1). Only switch when the app left
 * the locale at "C" (MB_CUR_MAX == 1) -- an app that already picked a
 * UTF-8 locale keeps it. Lazy rather than in ctui_init() so headless
 * callers (tools/ctui_test.h never calls ctui_init()) get the same
 * widths a real run does. */
static void ensure_utf8_ctype(void) {
  static int done = 0;
  if (done) {
    return;
  }
  done = 1;
  if (MB_CUR_MAX == 1 && setlocale(LC_CTYPE, "C.UTF-8") == NULL) {
    ctui_log(E_WRN, "[CTUI:UTF8] - C.UTF-8 locale unavailable, non-ASCII "
                    "widths will fall back to 1\n");
  }
}

int ctui_utf8_cpwidth(uint32_t cp) {
  if (cp < 0x7F) {
    return 1;
  }
  ensure_utf8_ctype();
  int w = wcwidth((wchar_t)cp);
  if (w < 0) {
    return 1;
  }
  return w > 2 ? 2 : w;
}

int ctui_utf8_width(const char *s) {
  int width = 0;
  while (*s) {
    uint32_t cp;
    s += ctui_utf8_decode(s, &cp);
    width += ctui_utf8_cpwidth(cp);
  }
  return width;
}

size_t ctui_utf8_prefix(const char *s, int cols, int *width_out) {
  size_t bytes = 0;
  int width = 0;
  while (s[bytes]) {
    uint32_t cp;
    int n = ctui_utf8_decode(s + bytes, &cp);
    int w = ctui_utf8_cpwidth(cp);
    if (width + w > cols) {
      break;
    }
    width += w;
    bytes += (size_t)n;
  }
  if (width_out) {
    *width_out = width;
  }
  return bytes;
}

int ctui_cell_set_ch(CTUI_CELL *row, int row_len, int col, int limit,
                     uint32_t cp) {
  int w = ctui_utf8_cpwidth(cp);
  if (w == 0) {
    ctui_logf(E_DBG,
              "[CTUI:UTF8] - dropping zero-width U+%04X @ tick %d (no "
              "grapheme clustering)\n",
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
