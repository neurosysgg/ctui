#include "buf.h"

#include "../ctui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ctui_buf_add(CTUI_BUF *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    /* s may point into b itself, which the realloc moves */
    int own = b->s && s >= b->s && s < b->s + b->cap;
    size_t at = own ? (size_t)(s - b->s) : 0;
    b->cap = (b->len + n + 1) * 2;
    b->s = realloc(b->s, b->cap);
    if (own) {
      s = b->s + at;
    }
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}

int ctui_buf_move(CTUI_BUF *b, char *out, size_t cap) {
  int fits = b->s && b->len < cap;
  if (fits) {
    memcpy(out, b->s, b->len + 1);
  } else {
    if (cap) {
      out[0] = '\0';
    }
    if (b->s) {
      ctui_logf(E_WRN,
                "[CTUI:BUF] - a %zu-byte command doesn't fit in "
                "%zu: not sent\n",
                b->len, cap);
    }
  }
  free(b->s);
  *b = (CTUI_BUF){0};
  return fits;
}

void ctui_buf_printf(CTUI_BUF *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n <= 0) {
    return;
  }
  /* straight into the grown buffer, not through a stack copy of it */
  ctui_buf_add(b, "", 0);
  if (b->len + (size_t)n + 1 > b->cap) {
    b->cap = (b->len + (size_t)n + 1) * 2;
    b->s = realloc(b->s, b->cap);
  }
  va_start(ap, fmt);
  vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}
