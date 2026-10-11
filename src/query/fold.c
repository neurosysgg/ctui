/* query.h's case folding and globs: Unicode's simple folding over UTF-8 */
/* strndup() under plain -std=c11 */
#define _DEFAULT_SOURCE

#include "query_internal.h"

#include "casefold.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fold_cp(uint32_t cp) {
  if (cp < 0x80) {
    return cp >= 'A' && cp <= 'Z' ? cp + 32 : cp;
  }
  int lo = 0,
      hi = (int)(sizeof ctui_casefold_table / sizeof *ctui_casefold_table) - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (cp < ctui_casefold_table[mid].first) {
      hi = mid - 1;
    } else if (cp > ctui_casefold_table[mid].last) {
      lo = mid + 1;
    } else {
      return (cp - ctui_casefold_table[mid].first) %
                         ctui_casefold_table[mid].stride ==
                     0
                 ? (uint32_t)((int32_t)cp + ctui_casefold_table[mid].delta)
                 : cp;
    }
  }
  return cp;
}

/* one codepoint from s (*len bytes of it), 0 on a byte that starts none
 * (it's copied as it is) */
static uint32_t decode(const unsigned char *s, int *len) {
  int n = s[0] < 0x80             ? 1
          : (s[0] & 0xE0) == 0xC0 ? 2
          : (s[0] & 0xF0) == 0xE0 ? 3
          : (s[0] & 0xF8) == 0xF0 ? 4
                                  : 0;
  uint32_t cp = n == 1   ? s[0]
                : n == 2 ? s[0] & 0x1Fu
                : n == 3 ? s[0] & 0x0Fu
                         : s[0] & 0x07u;
  for (int i = 1; i < n; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      n = 0;
      break;
    }
    cp = cp << 6 | (s[i] & 0x3Fu);
  }
  /* overlong forms, surrogates and past U+10FFFF are bytes: re-encoding
   * them would change the string (folding twice must not) */
  if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
      (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
      (cp >= 0xD800 && cp <= 0xDFFF)) {
    n = 0;
  }
  *len = n ? n : 1;
  return n ? cp : 0;
}

static int encode(uint32_t cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | cp >> 6);
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | cp >> 12);
    out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | cp >> 18);
  out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
  out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

size_t ctui_query_fold(const char *s, char *out, size_t cap) {
  size_t o = 0;
  if (!cap) {
    return 0;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    int len;
    uint32_t cp = decode(p, &len);
    char enc[4];
    int n = cp ? encode(fold_cp(cp), enc) : 1;
    if (!cp) {
      enc[0] = (char)*p;
    }
    if (o + (size_t)n >= cap) {
      break;
    }
    memcpy(out + o, enc, (size_t)n);
    o += (size_t)n;
    p += len;
  }
  out[o] = '\0';
  return o;
}

char *ctui_query_fold_dup(const char *s, size_t len) {
  char *raw = strndup(s, len);
  char *out = raw ? malloc(len * 3 / 2 + 8) : NULL;
  if (out) {
    ctui_query_fold(raw, out, len * 3 / 2 + 8);
  }
  free(raw);
  return out;
}

/* pattern (* any run, ? one codepoint) over the whole of s */
int ctui_query_glob(const char *pat, const char *s) {
  const char *star = NULL, *resume = NULL;
  while (*s) {
    int len;
    if (*pat == '*') {
      star = ++pat;
      resume = s;
    } else if (*pat == '?') {
      decode((const unsigned char *)s, &len);
      s += len;
      pat++;
    } else if (*pat && *pat == *s) {
      pat++;
      s++;
    } else if (star) {
      decode((const unsigned char *)resume, &len);
      resume += len;
      s = resume;
      pat = star;
    } else {
      return 0;
    }
  }
  while (*pat == '*') {
    pat++;
  }
  return !*pat;
}
