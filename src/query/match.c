/* query.h's matching, running and sorting: a compiled query is only read */
/* strdup() under plain -std=c11 */
#define _DEFAULT_SOURCE

#include "query_internal.h"

#include <stdlib.h>
#include <string.h>

/* what a match reads values into: one per ctui_query_match() call, on its
 * stack, so a compiled query is never written to */
typedef struct {
  char buf[CTUI_QUERY_VALUE_MAX];
  char fold[CTUI_QUERY_VALUE_MAX * 3 / 2 + 8];
} SCRATCH;

static void get(const CTUI_QUERY *q, int field, const void *rec,
                CTUI_QUERY_VALUE *v, SCRATCH *sc) {
  memset(v, 0, sizeof *v);
  sc->buf[0] = '\0';
  q->schema->get(rec, field, v, sc->buf, sizeof sc->buf, q->schema->ud);
  if (q->schema->fields[field].kind == CTUI_QUERY_TEXT && !v->s) {
    v->s = "";
  }
}

static const char *folded(const CTUI_QUERY *q, int field, const void *rec,
                          SCRATCH *sc) {
  CTUI_QUERY_VALUE v;
  get(q, field, rec, &v, sc);
  ctui_query_fold(v.s, sc->fold, sizeof sc->fold);
  return sc->fold;
}

/* v against an N_TEXTCMP: as numbers when the text is one (a value without
 * a number then matches nothing), else bytewise */
static int textcmp(const char *v, const NODE *n) {
  long long x;
  int c;
  if (n->numeric) {
    if (!ctui_query_leading_number(v, &x)) {
      return 0;
    }
    c = x < n->number ? -1 : x > n->number;
  } else {
    c = strcmp(v, n->text);
  }
  return n->op == '>' ? c > 0 : n->op == '<' ? c < 0 : n->op == 'g' ? c >= 0 : c <= 0;
}

static int eval(const CTUI_QUERY *q, int i, const void *rec, SCRATCH *sc) {
  const NODE *n = &q->nodes[i];
  CTUI_QUERY_VALUE v;
  switch (n->kind) {
  case N_ALL:
    return 1;
  case N_HAS:
    return strstr(folded(q, n->field, rec, sc), n->text) != NULL;
  case N_IS:
    return strcmp(folded(q, n->field, rec, sc), n->text) == 0;
  case N_GLOB:
    return ctui_query_glob(n->text, folded(q, n->field, rec, sc));
  case N_TEXTCMP:
    get(q, n->field, rec, &v, sc);
    return textcmp(v.s, n);
  case N_RANGE:
    get(q, n->field, rec, &v, sc);
    return v.has && v.n >= n->lo && v.n <= n->hi;
  case N_FLAG:
    get(q, n->field, rec, &v, sc);
    return v.has && v.n != 0;
  case N_MISSING:
  case N_PRESENT:
    get(q, n->field, rec, &v, sc);
    return (q->schema->fields[n->field].kind == CTUI_QUERY_TEXT ? v.s[0] != '\0'
                                                                 : v.has) ==
           (n->kind == N_PRESENT);
  case N_BARE:
    for (int f = 0; f < q->schema->nfields; f++) {
      if ((q->schema->fields[f].flags & CTUI_QUERY_BARE) &&
          strstr(folded(q, f, rec, sc), n->text)) {
        return 1;
      }
    }
    return 0;
  case N_AND:
    for (int k = 0; k < n->nkids; k++) {
      if (!eval(q, q->kids[n->kid0 + k], rec, sc)) {
        return 0;
      }
    }
    return 1;
  case N_OR:
    for (int k = 0; k < n->nkids; k++) {
      if (eval(q, q->kids[n->kid0 + k], rec, sc)) {
        return 1;
      }
    }
    return 0;
  case N_NOT:
    return !eval(q, q->kids[n->kid0], rec, sc);
  }
  return 0;
}

int ctui_query_match(const CTUI_QUERY *q, const void *record) {
  SCRATCH sc;
  return eval(q, q->root, record, &sc);
}

int ctui_query_sorted(const CTUI_QUERY *q) { return q->keys > 0; }

int ctui_query_fields(const CTUI_QUERY *q, int *out, int cap) {
  int count = 0, nf = q->schema->nfields;
  unsigned char *seen = calloc((size_t)nf + 1, 1);
  if (!seen) {
    return 0;
  }
  for (int i = 0; i < q->count + q->keys; i++) {
    const NODE *n = i < q->count ? &q->nodes[i] : NULL;
    int lo = -1, hi = -1;
    if (!n) {
      lo = hi = q->key_field[i - q->count];
    } else if (n->kind == N_BARE) {
      lo = 0;
      hi = nf - 1;
    } else if (n->field >= 0) {
      lo = hi = n->field;
    }
    for (int f = lo; f >= 0 && f <= hi; f++) {
      if ((!n || n->kind != N_BARE ||
           (q->schema->fields[f].flags & CTUI_QUERY_BARE)) &&
          !seen[f]) {
        seen[f] = 1;
        if (count < cap) {
          out[count] = f;
        }
        count++;
      }
    }
  }
  free(seen);
  return count;
}

/* --- running and sorting --- */

/* a match and its sort keys: text folded (malloc'd), else a number */
typedef struct {
  const CTUI_QUERY *q;
  int index;
  char *s[CTUI_QUERY_MAX_KEYS];
  long long n[CTUI_QUERY_MAX_KEYS];
  signed char has[CTUI_QUERY_MAX_KEYS];
} HIT;

/* two text keys: as numbers when both start with one and they differ,
 * else their folded bytes */
static int key_cmp(const char *a, const char *b) {
  long long x, y;
  if (ctui_query_leading_number(a, &x) && ctui_query_leading_number(b, &y) && x != y) {
    return x < y ? -1 : 1;
  }
  int c = strcmp(a, b);
  return c < 0 ? -1 : c > 0;
}

static int hit_cmp(const void *pa, const void *pb) {
  const HIT *a = pa, *b = pb;
  const CTUI_QUERY *q = a->q;
  for (int k = 0; k < q->keys; k++) {
    int c;
    if (q->schema->fields[q->key_field[k]].kind == CTUI_QUERY_TEXT) {
      c = key_cmp(a->s[k], b->s[k]);
    } else {
      c = a->has[k] != b->has[k] ? a->has[k] - b->has[k]
          : a->n[k] != b->n[k]   ? (a->n[k] < b->n[k] ? -1 : 1)
                                 : 0;
    }
    if (c) {
      return q->key_desc[k] ? -c : c;
    }
  }
  return a->index - b->index;
}

int ctui_query_run(const CTUI_QUERY *q, const void *records, size_t stride,
                   int n, int **out) {
  *out = NULL;
  int count = 0, cap = 0, failed = 0;
  HIT *hits = NULL;
  SCRATCH *sc = malloc(sizeof *sc);
  failed = !sc;
  for (int i = 0; i < n && !failed; i++) {
    const void *rec = (const char *)records + (size_t)i * stride;
    if (!eval(q, q->root, rec, sc)) {
      continue;
    }
    if (count == cap) {
      cap = cap ? cap * 2 : 256;
      HIT *grown = realloc(hits, (size_t)cap * sizeof *grown);
      if (!grown) {
        failed = 1;
        break;
      }
      hits = grown;
    }
    HIT *h = &hits[count++];
    memset(h, 0, sizeof *h);
    h->q = q;
    h->index = i;
    for (int k = 0; k < q->keys; k++) {
      int f = q->key_field[k];
      if (q->schema->fields[f].kind == CTUI_QUERY_TEXT) {
        h->s[k] = strdup(folded(q, f, rec, sc));
        failed |= !h->s[k];
      } else {
        CTUI_QUERY_VALUE v;
        get(q, f, rec, &v, sc);
        h->has[k] = (signed char)(v.has != 0);
        h->n[k] = v.n;
      }
    }
  }
  free(sc);
  if (!failed && q->keys > 0 && count > 1) {
    qsort(hits, (size_t)count, sizeof *hits, hit_cmp);
  }
  int *idx = !failed && count > 0 ? malloc((size_t)count * sizeof *idx) : NULL;
  failed |= count > 0 && !idx;
  for (int i = 0; i < count; i++) {
    if (idx) {
      idx[i] = hits[i].index;
    }
    for (int k = 0; k < q->keys; k++) {
      free(hits[i].s[k]);
    }
  }
  free(hits);
  if (failed) {
    free(idx);
    return -1;
  }
  *out = idx;
  return count;
}
