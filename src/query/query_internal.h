#ifndef CTUI_QUERY_INTERNAL_H
#define CTUI_QUERY_INTERNAL_H

#include <stddef.h>

#include "query.h"

/* what query.c, fold.c and values.c share; not part of the public surface */

#define CTUI_QUERY_MAX_KEYS 8 /* SORT BY's fields */

typedef enum {
  N_ALL,
  N_HAS,
  N_IS,
  N_GLOB,
  N_TEXTCMP,
  N_RANGE,
  N_MISSING,
  N_PRESENT,
  N_FLAG,
  N_BARE,
  N_AND,
  N_OR,
  N_NOT,
} NODE_KIND;

typedef struct {
  NODE_KIND kind;
  int field;
  char op;     /* N_TEXTCMP's */
  char *text;  /* folded for HAS / IS / GLOB / BARE; as given for TEXTCMP */
  int numeric; /* N_TEXTCMP: the text is a number */
  long long number;
  long long lo, hi; /* N_RANGE: inclusive */
  int kid0, nkids;  /* AND / OR / NOT: q->kids[kid0 ..] */
  int cost;
} NODE;

struct CTUI_QUERY {
  const CTUI_QUERY_SCHEMA *schema;
  NODE *nodes;
  int count, cap;
  int *kids;
  int nkids, kids_cap;
  int root;
  int key_field[CTUI_QUERY_MAX_KEYS], key_desc[CTUI_QUERY_MAX_KEYS];
  int keys;
};

#define CTUI_QUERY_BIG                                                         \
  4000000000000000000LL /* past any value: saturates there */

/* query.c: a value's leading integer: 1 if it has one (an optional '-',
 * digits) */
int ctui_query_leading_number(const char *s, long long *out);

/* fold.c: s's first len bytes folded, malloc'd */
char *ctui_query_fold_dup(const char *s, size_t len);
/* pattern (* any run, ? one codepoint) over the whole of s */
int ctui_query_glob(const char *pat, const char *s);

/* values.c: s (len bytes) is w, any ASCII case */
int ctui_query_word_is(const char *s, size_t len, const char *w);
/* 90, 5m, 1h30m, 2d, 1.5h, 3mo, 1y; 3:30, 1:02:03: seconds into *out */
int ctui_query_parse_duration(const char *s, size_t len, long long *out);
/* one value of kind as the period it stands for: [*lo, *hi] */
int ctui_query_parse_typed(CTUI_QUERY_KIND kind, const char *s, size_t len,
                           long long *lo, long long *hi);

#endif
