/* JSON objects into fixed records (record.h) */
#include "record.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 65536

struct CTUI_JSON_ARENA_BLOCK {
  CTUI_JSON_ARENA_BLOCK *next;
  size_t used, cap;
  char data[];
};

char *ctui_json_arena_alloc(CTUI_JSON_ARENA *a, size_t n) {
  CTUI_JSON_ARENA_BLOCK *b = a->head;
  if (!b || b->cap - b->used < n) {
    size_t cap = n > BLOCK ? n : BLOCK;
    CTUI_JSON_ARENA_BLOCK *nb = malloc(sizeof *nb + cap);
    if (!nb) {
      return NULL;
    }
    nb->used = 0;
    nb->cap = cap;
    if (b && n > BLOCK) { /* a big one: behind the head, which stays */
      nb->next = b->next;
      b->next = nb;
      nb->used = n;
      return nb->data;
    }
    nb->next = b;
    a->head = b = nb;
  }
  char *p = b->data + b->used;
  b->used += n;
  return p;
}

void ctui_json_arena_reset(CTUI_JSON_ARENA *a) {
  CTUI_JSON_ARENA_BLOCK *b = a->head;
  if (!b) {
    return;
  }
  /* keep one ordinary block (the last of the chain is the oldest) */
  CTUI_JSON_ARENA_BLOCK *keep = NULL;
  while (b) {
    CTUI_JSON_ARENA_BLOCK *next = b->next;
    if (!keep && b->cap == BLOCK) {
      keep = b;
    } else {
      free(b);
    }
    b = next;
  }
  if (keep) {
    keep->used = 0;
    keep->next = NULL;
  }
  a->head = keep;
}

void ctui_json_arena_free(CTUI_JSON_ARENA *a) {
  CTUI_JSON_ARENA_BLOCK *b = a->head;
  while (b) {
    CTUI_JSON_ARENA_BLOCK *next = b->next;
    free(b);
    b = next;
  }
  a->head = NULL;
}

struct CTUI_JSON_REC {
  CTUI_JSON_REC_FIELD *fields;
  int n;
  unsigned mask;  /* slots - 1 */
  int *slot;      /* a field's index + 1, 0 empty */
  size_t *keylen; /* each field's */
};

static unsigned hash(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ (unsigned char)s[i]) * 16777619u;
  }
  return h;
}

CTUI_JSON_REC *ctui_json_rec_new(const CTUI_JSON_REC_FIELD *fields, int n) {
  CTUI_JSON_REC *r = calloc(1, sizeof *r);
  unsigned slots = 8;
  while (slots < (unsigned)(n > 0 ? n : 1) * 2) {
    slots *= 2;
  }
  if (r) {
    r->fields = malloc(sizeof *fields * (size_t)(n > 0 ? n : 1));
    r->keylen = malloc(sizeof *r->keylen * (size_t)(n > 0 ? n : 1));
    r->slot = calloc(slots, sizeof *r->slot);
  }
  if (!r || !r->fields || !r->keylen || !r->slot) {
    ctui_json_rec_free(r);
    return NULL;
  }
  r->n = n;
  r->mask = slots - 1;
  memcpy(r->fields, fields, sizeof *fields * (size_t)n);
  for (int i = 0; i < n; i++) {
    r->keylen[i] = strlen(fields[i].key);
    unsigned h = hash(fields[i].key, r->keylen[i]) & r->mask;
    while (r->slot[h]) {
      h = (h + 1) & r->mask;
    }
    r->slot[h] = i + 1;
  }
  return r;
}

void ctui_json_rec_free(CTUI_JSON_REC *r) {
  if (r) {
    free(r->fields);
    free(r->keylen);
    free(r->slot);
    free(r);
  }
}

/* the field a key (its raw bytes between the quotes) names: -1 none. A key
 * with escapes in it is never one of ours (they're plain names) */
static int lookup(const CTUI_JSON_REC *r, const char *k, size_t n) {
  unsigned h = hash(k, n) & r->mask;
  for (int i; (i = r->slot[h]) != 0; h = (h + 1) & r->mask) {
    if (r->keylen[i - 1] == n && !memcmp(r->fields[i - 1].key, k, n)) {
      return i - 1;
    }
  }
  return -1;
}

typedef struct {
  const char *p, *e;
} CUR;

static void ws(CUR *c) {
  while (c->p < c->e &&
         (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
    c->p++;
  }
}

static int is_hex(char h) {
  return (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
         (h >= 'A' && h <= 'F');
}

/* past a string's closing quote (c->p after its opening one): 0, or -1
 * (unclosed, a raw control character or a bad escape in it: a string
 * stepped over is as strict as one read); *esc whether it has escapes */
static int str_end(CUR *c, int *esc) {
  *esc = 0;
  for (const char *p = c->p; p < c->e; p++) {
    unsigned char ch = (unsigned char)*p;
    if (ch == '"') {
      c->p = p + 1;
      return 0;
    }
    if (ch == '\\') {
      *esc = 1;
      if (++p >= c->e || !strchr("\"\\/bfnrtu", *p) || !*p) {
        return -1;
      }
      if (*p == 'u' && (c->e - p < 5 || !is_hex(p[1]) || !is_hex(p[2]) ||
                        !is_hex(p[3]) || !is_hex(p[4]))) {
        return -1;
      }
    } else if (ch < 0x20) {
      return -1;
    }
  }
  return -1;
}

static int hex4(const char *p, unsigned *v) {
  *v = 0;
  for (int i = 0; i < 4; i++) {
    char h = p[i];
    int d = h >= '0' && h <= '9'   ? h - '0'
            : h >= 'a' && h <= 'f' ? h - 'a' + 10
            : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                   : -1;
    if (d < 0) {
      return -1;
    }
    *v = *v << 4 | (unsigned)d;
  }
  return 0;
}

/* s[0..n) (a string's inside) decoded into the arena: NULL if it's bad */
static char *decode(const char *s, size_t n, int esc, CTUI_JSON_ARENA *a) {
  char *out = ctui_json_arena_alloc(a, n + 1);
  if (!out) {
    return NULL;
  }
  if (!esc) {
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
  }
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] != '\\') {
      out[o++] = s[i];
      continue;
    }
    if (++i >= n) {
      return NULL;
    }
    unsigned cp;
    switch (s[i]) {
    case '"':
    case '\\':
    case '/':
      out[o++] = s[i];
      continue;
    case 'b':
      out[o++] = '\b';
      continue;
    case 'f':
      out[o++] = '\f';
      continue;
    case 'n':
      out[o++] = '\n';
      continue;
    case 'r':
      out[o++] = '\r';
      continue;
    case 't':
      out[o++] = '\t';
      continue;
    case 'u':
      if (i + 4 >= n || hex4(s + i + 1, &cp) != 0) {
        return NULL;
      }
      i += 4;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < n && s[i + 1] == '\\' &&
          s[i + 2] == 'u') {
        unsigned lo;
        if (hex4(s + i + 3, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 6;
        }
      }
      /* a lone surrogate; NUL, where a C string would end silently (as
       * json.c does) */
      if ((cp >= 0xD800 && cp <= 0xDFFF) || cp == 0) {
        cp = 0xFFFD;
      }
      /* \uXXXX is 6 bytes, its UTF-8 at most 4 (a pair: 12 for 4) */
      if (cp < 0x80) {
        out[o++] = (char)cp;
      } else if (cp < 0x800) {
        out[o++] = (char)(0xC0 | cp >> 6);
        out[o++] = (char)(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        out[o++] = (char)(0xE0 | cp >> 12);
        out[o++] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
      } else {
        out[o++] = (char)(0xF0 | cp >> 18);
        out[o++] = (char)(0x80 | (cp >> 12 & 0x3F));
        out[o++] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
      }
      continue;
    default:
      return NULL;
    }
  }
  out[o] = '\0';
  return out;
}

/* a number's text at c->p: its end (strict JSON's grammar), or NULL */
static const char *num_end(const char *p, const char *e, int *integer) {
  const char *s = p;
  *integer = 1;
  if (p < e && *p == '-') {
    p++;
  }
  if (p >= e || *p < '0' || *p > '9') {
    return NULL;
  }
  if (*p == '0') {
    p++;
  } else {
    while (p < e && *p >= '0' && *p <= '9') {
      p++;
    }
  }
  if (p < e && *p == '.') {
    *integer = 0;
    if (++p >= e || *p < '0' || *p > '9') {
      return NULL;
    }
    while (p < e && *p >= '0' && *p <= '9') {
      p++;
    }
  }
  if (p < e && (*p == 'e' || *p == 'E')) {
    *integer = 0;
    p++;
    if (p < e && (*p == '+' || *p == '-')) {
      p++;
    }
    if (p >= e || *p < '0' || *p > '9') {
      return NULL;
    }
    while (p < e && *p >= '0' && *p <= '9') {
      p++;
    }
  }
  return p > s ? p : NULL;
}

static double to_double(const char *s, const char *e) {
  char buf[64], *p = buf;
  size_t n = (size_t)(e - s);
  /* a long one (zeros padding an exponent, say) whole, not cut */
  if (n >= sizeof buf && !(p = malloc(n + 1))) {
    return 0;
  }
  memcpy(p, s, n);
  p[n] = '\0';
  double d = strtod(p, NULL);
  if (p != buf) {
    free(p);
  }
  return d;
}

static long long to_llong(const char *s, const char *e, int integer) {
  if (!integer) {
    double d = to_double(s, e);
    return d >= 9.2e18 ? LLONG_MAX : d <= -9.2e18 ? LLONG_MIN : (long long)d;
  }
  int neg = *s == '-';
  unsigned long long v = 0;
  for (const char *p = s + neg; p < e; p++) {
    unsigned d = (unsigned)(*p - '0');
    if (v > (ULLONG_MAX - d) / 10) {
      return neg ? LLONG_MIN : LLONG_MAX;
    }
    v = v * 10 + d;
  }
  if (neg) {
    return v > (unsigned long long)LLONG_MAX + 1    ? LLONG_MIN
           : v == (unsigned long long)LLONG_MAX + 1 ? LLONG_MIN
                                                    : -(long long)v;
  }
  return v > (unsigned long long)LLONG_MAX ? LLONG_MAX : (long long)v;
}

/* past any value (nested ones by their brackets): 0, or -1 */
static int skip(CUR *c, int depth) {
  ws(c);
  if (c->p >= c->e || depth > 512) {
    return -1;
  }
  int esc, integer;
  switch (*c->p) {
  case '"':
    c->p++;
    return str_end(c, &esc);
  case '{':
  case '[': {
    char close = *c->p == '{' ? '}' : ']';
    int obj = close == '}';
    c->p++;
    ws(c);
    if (c->p < c->e && *c->p == close) {
      c->p++;
      return 0;
    }
    for (;;) {
      if (obj) {
        ws(c);
        if (c->p >= c->e || *c->p != '"') {
          return -1;
        }
        c->p++;
        if (str_end(c, &esc) != 0) {
          return -1;
        }
        ws(c);
        if (c->p >= c->e || *c->p != ':') {
          return -1;
        }
        c->p++;
      }
      if (skip(c, depth + 1) != 0) {
        return -1;
      }
      ws(c);
      if (c->p < c->e && *c->p == ',') {
        c->p++;
        continue;
      }
      if (c->p < c->e && *c->p == close) {
        c->p++;
        return 0;
      }
      return -1;
    }
  }
  case 't':
  case 'f':
  case 'n': {
    const char *w = *c->p == 't' ? "true" : *c->p == 'f' ? "false" : "null";
    size_t n = strlen(w);
    if ((size_t)(c->e - c->p) < n || memcmp(c->p, w, n)) {
      return -1;
    }
    c->p += n;
    return 0;
  }
  default: {
    const char *end = num_end(c->p, c->e, &integer);
    if (!end) {
      return -1;
    }
    c->p = end;
    return 0;
  }
  }
}

static void set_has(const CTUI_JSON_REC_FIELD *f, char *rec) {
  if (f->has >= 0) {
    int one = 1;
    memcpy(rec + f->has, &one, sizeof one);
  }
}

/* a member's value at c->p into field f of rec */
static int value(const CTUI_JSON_REC_FIELD *f, CUR *c, char *rec,
                 CTUI_JSON_ARENA *a) {
  ws(c);
  if (c->p >= c->e) {
    return -1;
  }
  const char *s = c->p;
  int esc, integer;
  if (*s == '"') {
    c->p++;
    if (str_end(c, &esc) != 0) {
      return -1;
    }
    if (f->kind != CTUI_JSON_REC_STR) {
      return 0;
    }
    char *t = decode(s + 1, (size_t)(c->p - s - 2), esc, a);
    if (!t) {
      return -1;
    }
    memcpy(rec + f->at, &t, sizeof t);
    set_has(f, rec);
    return 0;
  }
  if (*s == '-' || (*s >= '0' && *s <= '9')) {
    const char *end = num_end(s, c->e, &integer);
    if (!end) {
      return -1;
    }
    c->p = end;
    if (f->kind == CTUI_JSON_REC_LLONG) {
      long long v = to_llong(s, end, integer);
      memcpy(rec + f->at, &v, sizeof v);
      set_has(f, rec);
    } else if (f->kind == CTUI_JSON_REC_DOUBLE) {
      double v = to_double(s, end);
      memcpy(rec + f->at, &v, sizeof v);
      set_has(f, rec);
    }
    return 0;
  }
  if ((*s == 't' || *s == 'f') && f->kind == CTUI_JSON_REC_BOOL) {
    if (skip(c, 0) != 0) {
      return -1;
    }
    int v = *s == 't';
    memcpy(rec + f->at, &v, sizeof v);
    set_has(f, rec);
    return 0;
  }
  return skip(c, 0);
}

int ctui_json_rec_read(const CTUI_JSON_REC *r, const char *text, size_t len,
                       void *rec, CTUI_JSON_ARENA *a) {
  CUR c = {text, text + len};
  ws(&c);
  if (c.p >= c.e || *c.p != '{') {
    return -1;
  }
  c.p++;
  ws(&c);
  if (c.p < c.e && *c.p == '}') {
    c.p++;
  } else {
    for (;;) {
      ws(&c);
      if (c.p >= c.e || *c.p != '"') {
        return -1;
      }
      const char *k = ++c.p;
      int esc;
      if (str_end(&c, &esc) != 0) {
        return -1;
      }
      int fi = esc ? -1 : lookup(r, k, (size_t)(c.p - k - 1));
      ws(&c);
      if (c.p >= c.e || *c.p != ':') {
        return -1;
      }
      c.p++;
      int rc = fi >= 0 ? value(&r->fields[fi], &c, rec, a) : skip(&c, 0);
      if (rc != 0) {
        return -1;
      }
      ws(&c);
      if (c.p < c.e && *c.p == ',') {
        c.p++;
        continue;
      }
      if (c.p < c.e && *c.p == '}') {
        c.p++;
        break;
      }
      return -1;
    }
  }
  ws(&c);
  return c.p == c.e ? 0 : -1;
}
