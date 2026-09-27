#include "utf8.h"

#include "log.h"
#include "utf8_width.h"

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

int ctui_utf8_cpwidth(uint32_t cp) {
  /* U+00AD is the table's first entry */
  if (cp < 0xAD) {
    return 1;
  }
  size_t lo = 0;
  size_t hi = sizeof ctui_utf8_width_table / sizeof ctui_utf8_width_table[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < ctui_utf8_width_table[mid].first) {
      hi = mid;
    } else if (cp > ctui_utf8_width_table[mid].last) {
      lo = mid + 1;
    } else {
      return ctui_utf8_width_table[mid].width;
    }
  }
  return 1;
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
