#ifndef CTUI_BUF_H
#define CTUI_BUF_H

#include <stddef.h>

/* A growable string: JSON written out (json.h), commands put together
 * piece by piece, text taken apart. */

typedef struct {
  char *s; /* NUL-terminated once anything was added; free() when done */
  size_t len, cap;
} CTUI_BUF;

void ctui_buf_add(CTUI_BUF *b, const char *s, size_t n);
/* b's text into out (cap bytes) if it fits whole: a command cut short
 * would be broken JSON, so one that doesn't fit leaves out "" (logged).
 * Frees b either way; 1 if copied. */
int ctui_buf_move(CTUI_BUF *b, char *out, size_t cap);
/* formats straight into b: no argument may point into b itself */
void ctui_buf_printf(CTUI_BUF *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
