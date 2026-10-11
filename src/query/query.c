/* strdup()/strndup() under plain -std=c11 */
#define _DEFAULT_SOURCE

#include "query.h"

#include "query_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DEPTH 64 /* parentheses and NOTs nested */
#define IS_FIELD (-2) /* "is:" names a flag field */

typedef enum {
  T_END,
  T_WORD,   /* a bare word (or a field's name, or a keyword: see kw) */
  T_STRING, /* "..." or '...' */
  T_LPAREN,
  T_RPAREN,
  T_COMMA,
  T_SHORT, /* field:value, field>value, ... (field, op, the value in s) */
  T_NEG,   /* a '-' before a term */
} TOKEN_KIND;

typedef enum {
  K_NONE,
  K_HAS,
  K_IS,
  K_GREATER,
  K_LESS,
  K_MISSING,
  K_PRESENT,
  K_AND,
  K_OR,
  K_NOT,
  K_ALL,
  K_SORT,
  K_BY,
  K_ASCENDING,
  K_DESCENDING,
} KEYWORD;

static const char *const KEYWORDS[] = {
    "",    "HAS", "IS", "GREATER", "LESS",      "MISSING",   "PRESENT", "AND",
    "OR",  "NOT", "ALL", "SORT",   "BY",        "ASCENDING", "DESCENDING"};

/* the operators: ':' '=' '>' '<', 'g' >=, 'l' <= */
typedef struct {
  TOKEN_KIND kind;
  KEYWORD kw;
  const char *s; /* its text in the source (a string's, without quotes) */
  size_t len;
  size_t at; /* its byte in the source */
  int field; /* T_SHORT's */
  char op;   /* T_SHORT's */
  int quoted; /* T_SHORT's value was quoted */
} TOKEN;

typedef struct {
  const char *src;
  TOKEN *tok;
  int ntok, pos;
  CTUI_QUERY *q;
  long long now;
  char *err;
  size_t err_cap;
  int failed, depth;
} PARSER;

/* --- parsing --- */

/* the code point column of byte at in src, from 1 */
static int col_of(const char *src, size_t at) {
  int col = 1;
  for (size_t i = 0; i < at && src[i]; i++) {
    col += ((unsigned char)src[i] & 0xC0) != 0x80;
  }
  return col;
}

static int fail(PARSER *p, size_t at, const char *what) {
  if (!p->failed) {
    p->failed = 1;
    if (p->err && p->err_cap) {
      snprintf(p->err, p->err_cap, "col %d: %s", col_of(p->src, at), what);
    }
  }
  return -1;
}

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_ident(char c, int first) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         (!first && ((c >= '0' && c <= '9') || c == '-' || c == '.'));
}

static int find_field(const CTUI_QUERY_SCHEMA *sc, const char *s, size_t len) {
  for (int i = 0; i < sc->nfields; i++) {
    if (strlen(sc->fields[i].name) == len &&
        memcmp(sc->fields[i].name, s, len) == 0) {
      return i;
    }
  }
  return -1;
}

/* at s[i]: "name:", "name>=", ...: the operator's byte (*op, *oplen set),
 * else 0 */
static size_t short_op(const char *s, size_t i, size_t n, char *op,
                       size_t *oplen) {
  if (!is_ident(s[i], 1)) {
    return 0;
  }
  size_t j = i + 1;
  while (j < n && is_ident(s[j], 0)) {
    j++;
  }
  if (j >= n) {
    return 0;
  }
  *oplen = 1;
  if (s[j] == ':' || s[j] == '=') {
    *op = s[j];
  } else if (s[j] == '<' || s[j] == '>') {
    *op = s[j];
    if (j + 1 < n && s[j + 1] == '=') {
      *op = s[j] == '<' ? 'l' : 'g';
      *oplen = 2;
    }
  } else {
    return 0;
  }
  return j;
}

static int tokenize(PARSER *p) {
  const char *s = p->src;
  size_t n = strlen(s), i = 0;
  p->tok = calloc(n + 1, sizeof *p->tok); /* never more tokens than bytes + 1 */
  if (!p->tok) {
    return fail(p, 0, "no memory");
  }
  while (i < n) {
    if (is_space(s[i])) {
      i++;
      continue;
    }
    TOKEN *t = &p->tok[p->ntok++];
    t->at = i;
    char op;
    size_t oplen, opat;
    if (s[i] == '(' || s[i] == ')' || s[i] == ',') {
      t->kind = s[i] == '(' ? T_LPAREN : s[i] == ')' ? T_RPAREN : T_COMMA;
      i++;
    } else if (s[i] == '"' || s[i] == '\'') {
      char quote = s[i];
      size_t end = i + 1;
      while (end < n && s[end] != quote) {
        end++;
      }
      if (end >= n) {
        return fail(p, i, "a quote isn't closed");
      }
      t->kind = T_STRING;
      t->s = s + i + 1;
      t->len = end - i - 1;
      i = end + 1;
    } else if (s[i] == '-' && i + 1 < n && !is_space(s[i + 1]) &&
               s[i + 1] != ')' && s[i + 1] != ',') {
      t->kind = T_NEG;
      i++;
    } else if ((opat = short_op(s, i, n, &op, &oplen)) != 0) {
      int field = opat - i == 2 && !memcmp(s + i, "is", 2)
                      ? IS_FIELD
                      : find_field(p->q->schema, s + i, opat - i);
      if (field == -1) {
        char what[96];
        snprintf(what, sizeof what, "no field \"%.*s\" (quote a word with %c)",
                 (int)(opat - i > 40 ? 40 : opat - i), s + i,
                 op == 'l' || op == 'g' ? s[opat] : op);
        return fail(p, i, what);
      }
      t->kind = T_SHORT;
      t->field = field;
      t->op = op;
      size_t v = opat + oplen, end = v;
      if (v < n && (s[v] == '"' || s[v] == '\'')) {
        char quote = s[v];
        end = v + 1;
        while (end < n && s[end] != quote) {
          end++;
        }
        if (end >= n) {
          return fail(p, v, "a quote isn't closed");
        }
        t->s = s + v + 1;
        t->len = end - v - 1;
        t->quoted = 1;
        i = end + 1;
      } else {
        while (end < n && !is_space(s[end]) && s[end] != ')' &&
               s[end] != '"' && s[end] != '\'') {
          end++;
        }
        if (end == v) {
          char what[96];
          snprintf(what, sizeof what, "%.*s needs a value after it",
                   (int)(v - i > 40 ? 40 : v - i), s + i);
          return fail(p, i, what);
        }
        t->s = s + v;
        t->len = end - v;
        i = end;
      }
    } else {
      size_t end = i;
      while (end < n && !is_space(s[end]) && s[end] != '(' && s[end] != ')' &&
             s[end] != ',' && s[end] != '"' && s[end] != '\'') {
        end++;
      }
      t->kind = T_WORD;
      t->s = s + i;
      t->len = end - i;
      for (int k = 1; k < (int)(sizeof KEYWORDS / sizeof *KEYWORDS); k++) {
        if (strlen(KEYWORDS[k]) == t->len &&
            memcmp(KEYWORDS[k], t->s, t->len) == 0) {
          t->kw = (KEYWORD)k;
        }
      }
      i = end;
    }
  }
  p->tok[p->ntok].kind = T_END;
  p->tok[p->ntok].at = n;
  return 0;
}

static TOKEN *peek(PARSER *p) { return &p->tok[p->pos]; }
static TOKEN *peek2(PARSER *p) {
  return &p->tok[p->pos < p->ntok ? p->pos + 1 : p->pos];
}

/* a word naming a field ("artist", "%artist%"): its number, else -1 */
static int field_of(PARSER *p, const TOKEN *t) {
  if (t->kind != T_WORD || t->kw) {
    return -1;
  }
  const char *s = t->s;
  size_t len = t->len;
  if (len > 2 && s[0] == '%' && s[len - 1] == '%') {
    s++;
    len -= 2;
  }
  return find_field(p->q->schema, s, len);
}

static int add_node(PARSER *p, NODE n) {
  CTUI_QUERY *q = p->q;
  if (q->count == q->cap) {
    int cap = q->cap ? q->cap * 2 : 16;
    NODE *grown = realloc(q->nodes, (size_t)cap * sizeof *grown);
    if (!grown) {
      free(n.text);
      return fail(p, 0, "no memory");
    }
    q->nodes = grown;
    q->cap = cap;
  }
  q->nodes[q->count] = n;
  return q->count++;
}

/* a cost for each node: its field's, the dearest of its children's */
static int field_cost(const CTUI_QUERY *q, int field) {
  return field >= 0 ? q->schema->fields[field].cost : 0;
}

static int bare_cost(const CTUI_QUERY *q) {
  int c = 0;
  for (int i = 0; i < q->schema->nfields; i++) {
    if ((q->schema->fields[i].flags & CTUI_QUERY_BARE) &&
        q->schema->fields[i].cost > c) {
      c = q->schema->fields[i].cost;
    }
  }
  return c;
}

static int add_leaf(PARSER *p, NODE n) {
  n.cost = n.kind == N_ALL    ? 0
           : n.kind == N_BARE ? bare_cost(p->q)
                              : field_cost(p->q, n.field);
  return add_node(p, n);
}

/* an AND / OR / NOT over kids (n of them): kept as one node with its
 * children in q->kids; an AND's cheapest first (stable: same cost, the
 * order typed) */
static int add_parent(PARSER *p, NODE_KIND kind, int *kids, int n) {
  CTUI_QUERY *q = p->q;
  if (n == 1 && kind != N_NOT) {
    return kids[0];
  }
  if (kind == N_AND) {
    for (int i = 1; i < n; i++) {
      for (int j = i; j > 0 && q->nodes[kids[j]].cost < q->nodes[kids[j - 1]].cost;
           j--) {
        int t = kids[j];
        kids[j] = kids[j - 1];
        kids[j - 1] = t;
      }
    }
  }
  if (q->nkids + n > q->kids_cap) {
    int cap = q->kids_cap ? q->kids_cap * 2 : 16;
    while (cap < q->nkids + n) {
      cap *= 2;
    }
    int *grown = realloc(q->kids, (size_t)cap * sizeof *grown);
    if (!grown) {
      return fail(p, 0, "no memory");
    }
    q->kids = grown;
    q->kids_cap = cap;
  }
  NODE node = {.kind = kind, .field = -1, .kid0 = q->nkids, .nkids = n};
  for (int i = 0; i < n; i++) {
    q->kids[q->nkids++] = kids[i];
    if (q->nodes[kids[i]].cost > node.cost) {
      node.cost = q->nodes[kids[i]].cost;
    }
  }
  return add_node(p, node);
}

int ctui_query_leading_number(const char *s, long long *out) {
  const char *p = s;
  int neg = *p == '-';
  p += neg;
  if (*p < '0' || *p > '9') {
    return 0;
  }
  long long v = 0;
  for (; *p >= '0' && *p <= '9'; p++) {
    v = v < 100000000000000000LL ? v * 10 + (*p - '0') : v; /* capped */
  }
  *out = neg ? -v : v;
  return 1;
}

static const char *kind_example(CTUI_QUERY_KIND kind) {
  switch (kind) {
  case CTUI_QUERY_NUMBER:
    return "a number";
  case CTUI_QUERY_SIZE:
    return "a size (10M, 1.5G)";
  case CTUI_QUERY_DURATION:
    return "a length (90, 5m, 1h30m, 3:30)";
  case CTUI_QUERY_AGE:
    return "an age (7d, 2w, 1y)";
  case CTUI_QUERY_DATE:
    return "a date (2026, 2026-01-31)";
  default:
    return "a value";
  }
}

/* the age field of the date field `date`, or -1 */
static int age_of(const CTUI_QUERY_SCHEMA *sc, int date) {
  for (int i = 0; i < sc->nfields; i++) {
    if (sc->fields[i].kind == CTUI_QUERY_AGE && sc->fields[i].of == date) {
      return i;
    }
  }
  return -1;
}

static int typed_fail(PARSER *p, size_t at, int field, const char *s,
                      size_t len) {
  const CTUI_QUERY_SCHEMA *sc = p->q->schema;
  CTUI_QUERY_KIND kind = sc->fields[field].kind;
  char what[160];
  long long d;
  int age = kind == CTUI_QUERY_DATE ? age_of(sc, field) : -1;
  if (age >= 0 && ctui_query_parse_duration(s, len, &d) == 0) {
    snprintf(what, sizeof what, "%s takes %s; an age is %s<%.*s",
             sc->fields[field].name, kind_example(kind), sc->fields[age].name,
             (int)(len > 20 ? 20 : len), s);
  } else {
    snprintf(what, sizeof what, "%s takes %s, not \"%.*s\"",
             sc->fields[field].name, kind_example(kind),
             (int)(len > 30 ? 30 : len), s);
  }
  return fail(p, at, what);
}

/* field op value (op ':' '=' '>' '<' 'g' 'l'; a range "a..b" with ':' or
 * '=') as an N_RANGE */
static int typed_node(PARSER *p, size_t at, int field, char op, const char *s,
                      size_t len) {
  const CTUI_QUERY_SCHEMA *sc = p->q->schema;
  CTUI_QUERY_KIND kind = sc->fields[field].kind;
  long long lo = -CTUI_QUERY_BIG, hi = CTUI_QUERY_BIG, a_lo, a_hi, b_lo, b_hi;
  const char *dots = NULL;
  for (size_t i = 0; i + 1 < len; i++) {
    if (s[i] == '.' && s[i + 1] == '.') {
      dots = s + i;
      break;
    }
  }
  if (dots && (op == ':' || op == '=')) {
    size_t alen = (size_t)(dots - s), blen = len - alen - 2;
    if (!alen && !blen) {
      return typed_fail(p, at, field, s, len);
    }
    if (alen && ctui_query_parse_typed(kind, s, alen, &a_lo, &a_hi) != 0) {
      return typed_fail(p, at, field, s, alen);
    }
    if (blen && ctui_query_parse_typed(kind, dots + 2, blen, &b_lo, &b_hi) != 0) {
      return typed_fail(p, at, field, dots + 2, blen);
    }
    lo = alen ? a_lo : -CTUI_QUERY_BIG;
    hi = blen ? b_hi : CTUI_QUERY_BIG;
  } else {
    if (ctui_query_parse_typed(kind, s, len, &a_lo, &a_hi) != 0) {
      return typed_fail(p, at, field, s, len);
    }
    switch (op) {
    case '>':
      lo = a_hi + 1;
      break;
    case 'g':
      lo = a_lo;
      break;
    case '<':
      hi = a_lo - 1;
      break;
    case 'l':
      hi = a_hi;
      break;
    default:
      if (kind == CTUI_QUERY_AGE) {
        char what[96];
        snprintf(what, sizeof what, "%s takes <, > or a range (%s<%.*s)",
                 sc->fields[field].name, sc->fields[field].name,
                 (int)(len > 20 ? 20 : len), s);
        return fail(p, at, what);
      }
      lo = a_lo;
      hi = a_hi;
    }
  }
  if (kind == CTUI_QUERY_AGE) {
    /* an age in [lo, hi] is a date in [now - hi, now - lo] */
    long long dlo = hi >= CTUI_QUERY_BIG ? -CTUI_QUERY_BIG : p->now - hi;
    long long dhi = lo <= -CTUI_QUERY_BIG ? CTUI_QUERY_BIG : p->now - lo;
    field = sc->fields[field].of;
    lo = dlo;
    hi = dhi;
  }
  return add_leaf(p, (NODE){.kind = N_RANGE, .field = field, .lo = lo, .hi = hi});
}

static int has_wild(const char *s, size_t len) {
  return memchr(s, '*', len) || memchr(s, '?', len);
}

/* field op one value (no commas left in it) */
static int value_node(PARSER *p, size_t at, int field, char op, const char *s,
                      size_t len) {
  const CTUI_QUERY_SCHEMA *sc = p->q->schema;
  if (field == IS_FIELD) {
    int f = find_field(sc, s, len);
    if (f < 0 || sc->fields[f].kind != CTUI_QUERY_FLAG || op != ':') {
      char what[160];
      int w = snprintf(what, sizeof what, "is: takes a flag:");
      for (int i = 0; i < sc->nfields && w < (int)sizeof what - 24; i++) {
        if (sc->fields[i].kind == CTUI_QUERY_FLAG) {
          w += snprintf(what + w, sizeof what - (size_t)w, " %s",
                        sc->fields[i].name);
        }
      }
      return fail(p, at, what);
    }
    return add_leaf(p, (NODE){.kind = N_FLAG, .field = f});
  }
  CTUI_QUERY_KIND kind = sc->fields[field].kind;
  if (kind == CTUI_QUERY_TEXT) {
    NODE n = {.field = field};
    if (op == ':' || op == '=') {
      n.kind = has_wild(s, len) ? N_GLOB
               : op == '=' || (sc->fields[field].flags & CTUI_QUERY_EXACT)
                   ? N_IS
                   : N_HAS;
      n.text = ctui_query_fold_dup(s, len);
    } else {
      n.kind = N_TEXTCMP;
      n.op = op;
      n.text = strndup(s, len);
      if (n.text) {
        n.numeric = ctui_query_leading_number(n.text, &n.number);
      }
    }
    if (!n.text) {
      return fail(p, at, "no memory");
    }
    return add_leaf(p, n);
  }
  if (kind == CTUI_QUERY_FLAG) {
    int on = ctui_query_word_is(s, len, "yes") || ctui_query_word_is(s, len, "true") ||
             ctui_query_word_is(s, len, "1");
    int off = ctui_query_word_is(s, len, "no") || ctui_query_word_is(s, len, "false") ||
              ctui_query_word_is(s, len, "0");
    if ((!on && !off) || (op != ':' && op != '=')) {
      char what[96];
      snprintf(what, sizeof what, "%s takes yes or no", sc->fields[field].name);
      return fail(p, at, what);
    }
    int leaf = add_leaf(p, (NODE){.kind = N_FLAG, .field = field});
    return on || leaf < 0 ? leaf : add_parent(p, N_NOT, &leaf, 1);
  }
  return typed_node(p, at, field, op, s, len);
}

/* field op value, a value list "a,b,c" (any of them) */
static int short_node(PARSER *p, const TOKEN *t) {
  if (t->quoted) {
    return value_node(p, t->at, t->field, t->op, t->s, t->len);
  }
  int kids[64], n = 0;
  for (size_t i = 0; i <= t->len;) {
    size_t len = 0;
    while (i + len < t->len && t->s[i + len] != ',') {
      len++;
    }
    if (!len) {
      return fail(p, t->at, "an empty value in a list");
    }
    if (n == 64) {
      return fail(p, t->at, "a list of more than 64 values");
    }
    kids[n] = value_node(p, t->at, t->field, t->op, t->s + i, len);
    if (kids[n++] < 0) {
      return -1;
    }
    i += len + 1;
  }
  return add_parent(p, N_OR, kids, n);
}

/* the text after an operator: a quoted string, or the words up to the
 * next keyword / parenthesis / comma, one space between them */
static char *value_text(PARSER *p, size_t op_at, const char *op) {
  TOKEN *t = peek(p);
  if (t->kind == T_STRING) {
    p->pos++;
    char *v = strndup(t->s, t->len);
    if (!v) {
      fail(p, t->at, "no memory");
    }
    return v;
  }
  size_t cap = 1, len = 0;
  for (int i = p->pos; p->tok[i].kind == T_WORD && !p->tok[i].kw; i++) {
    cap += p->tok[i].len + 1;
  }
  if (cap == 1) {
    char what[64];
    snprintf(what, sizeof what, "%s needs a text after it", op);
    fail(p, op_at, what);
    return NULL;
  }
  char *v = malloc(cap);
  if (!v) {
    fail(p, t->at, "no memory");
    return NULL;
  }
  while (peek(p)->kind == T_WORD && !peek(p)->kw) {
    t = &p->tok[p->pos++];
    if (len) {
      v[len++] = ' ';
    }
    memcpy(v + len, t->s, t->len);
    len += t->len;
  }
  v[len] = '\0';
  return v;
}

static int parse_or(PARSER *p);

/* FIELD HAS / IS / GREATER / LESS text */
static int keyword_node(PARSER *p, int field, KEYWORD op, size_t op_at) {
  const CTUI_QUERY_SCHEMA *sc = p->q->schema;
  char *text = value_text(p, op_at, KEYWORDS[op]);
  if (!text) {
    return -1;
  }
  int rc;
  if (sc->fields[field].kind == CTUI_QUERY_TEXT) {
    NODE n = {.field = field};
    if (op == K_HAS || op == K_IS) {
      n.kind = op == K_HAS ? N_HAS : N_IS;
      n.text = ctui_query_fold_dup(text, strlen(text));
      free(text);
      if (!n.text) {
        return fail(p, op_at, "no memory");
      }
    } else {
      n.kind = N_TEXTCMP;
      n.op = op == K_GREATER ? '>' : '<';
      n.text = text;
      n.numeric = ctui_query_leading_number(n.text, &n.number);
    }
    return add_leaf(p, n);
  }
  char sop = op == K_GREATER ? '>' : op == K_LESS ? '<' : op == K_IS ? '=' : ':';
  rc = value_node(p, op_at, field, sop, text, strlen(text));
  free(text);
  return rc;
}

static int parse_unary(PARSER *p);

static int parse_primary(PARSER *p) {
  TOKEN *t = peek(p);
  if (t->kind == T_LPAREN) {
    p->pos++;
    if (++p->depth > MAX_DEPTH) {
      return fail(p, t->at, "nested too deeply");
    }
    int e = parse_or(p);
    p->depth--;
    if (e < 0) {
      return -1;
    }
    if (peek(p)->kind != T_RPAREN) {
      return fail(p, t->at, "a parenthesis isn't closed");
    }
    p->pos++;
    return e;
  }
  if (t->kind == T_SHORT) {
    p->pos++;
    return short_node(p, t);
  }
  if (t->kind == T_WORD && t->kw == K_ALL) {
    p->pos++;
    return add_leaf(p, (NODE){.kind = N_ALL, .field = -1});
  }
  int field = field_of(p, t);
  KEYWORD op = peek2(p)->kind == T_WORD ? peek2(p)->kw : K_NONE;
  if (field >= 0 && op >= K_HAS && op <= K_PRESENT) {
    size_t op_at = peek2(p)->at;
    p->pos += 2;
    if (op == K_MISSING || op == K_PRESENT) {
      if (p->q->schema->fields[field].kind == CTUI_QUERY_AGE) {
        field = p->q->schema->fields[field].of;
      }
      return add_leaf(p, (NODE){.kind = op == K_MISSING ? N_MISSING : N_PRESENT,
                                .field = field});
    }
    return keyword_node(p, field, op, op_at);
  }
  if (t->kind == T_STRING || (t->kind == T_WORD && !t->kw)) {
    p->pos++;
    NODE n = {.kind = N_BARE, .field = -1, .text = ctui_query_fold_dup(t->s, t->len)};
    if (!n.text) {
      return fail(p, t->at, "no memory");
    }
    return add_leaf(p, n);
  }
  if (t->kind == T_END) {
    return fail(p, t->at, "a search ends too soon");
  }
  if (t->kind == T_WORD && t->kw >= K_HAS && t->kw <= K_PRESENT) {
    char what[64];
    snprintf(what, sizeof what, "%s needs a field before it", KEYWORDS[t->kw]);
    return fail(p, t->at, what);
  }
  char what[64];
  snprintf(what, sizeof what, "\"%.*s\" isn't expected here",
           (int)(t->len ? t->len : 1), t->len ? t->s : p->src + t->at);
  return fail(p, t->at, what);
}

static int parse_unary(PARSER *p) {
  TOKEN *t = peek(p);
  if (t->kind == T_NEG || (t->kind == T_WORD && t->kw == K_NOT)) {
    p->pos++;
    if (++p->depth > MAX_DEPTH) {
      return fail(p, t->at, "nested too deeply");
    }
    int a = parse_unary(p);
    p->depth--;
    return a < 0 ? -1 : add_parent(p, N_NOT, &a, 1);
  }
  return parse_primary(p);
}

/* what starts a term: an AND between two of them may be left out */
static int starts_term(const TOKEN *t) {
  return t->kind == T_LPAREN || t->kind == T_STRING || t->kind == T_SHORT ||
         t->kind == T_NEG ||
         (t->kind == T_WORD && (!t->kw || t->kw == K_NOT || t->kw == K_ALL));
}

/* a run of terms joined by kw (AND, else OR), each parsed by sub */
static int parse_run(PARSER *p, KEYWORD kw, int (*sub)(PARSER *)) {
  int fixed[16], *kids = fixed, n = 0, cap = 16;
  int a = sub(p);
  while (a >= 0) {
    if (n == cap) {
      int *grown = malloc((size_t)cap * 2 * sizeof *grown);
      if (!grown) {
        a = fail(p, 0, "no memory");
        break;
      }
      memcpy(grown, kids, (size_t)n * sizeof *grown);
      if (kids != fixed) {
        free(kids);
      }
      kids = grown;
      cap *= 2;
    }
    kids[n++] = a;
    TOKEN *t = peek(p);
    if (t->kind == T_WORD && t->kw == kw) {
      p->pos++;
    } else if (kw == K_OR || !starts_term(t)) {
      break;
    }
    a = sub(p);
  }
  int rc = a < 0 ? -1 : add_parent(p, kw == K_AND ? N_AND : N_OR, kids, n);
  if (kids != fixed) {
    free(kids);
  }
  return rc;
}

static int parse_and(PARSER *p) { return parse_run(p, K_AND, parse_unary); }

static int parse_or(PARSER *p) { return parse_run(p, K_OR, parse_and); }

static int parse_sort(PARSER *p) {
  TOKEN *t = peek(p);
  p->pos++;
  if (peek(p)->kind != T_WORD || peek(p)->kw != K_BY) {
    return fail(p, t->at, "SORT needs BY after it");
  }
  p->pos++;
  for (;;) {
    TOKEN *f = peek(p);
    int field = field_of(p, f);
    if (field < 0) {
      return fail(p, f->at,
                  f->kind == T_END ? "SORT BY needs a field"
                                   : "no such field to sort by");
    }
    if (p->q->keys == CTUI_QUERY_MAX_KEYS) {
      return fail(p, f->at, "SORT BY takes at most 8 fields");
    }
    p->pos++;
    if (p->q->schema->fields[field].kind == CTUI_QUERY_AGE) {
      field = p->q->schema->fields[field].of; /* oldest first: by date */
    }
    int desc = 0;
    if (peek(p)->kind == T_WORD &&
        (peek(p)->kw == K_ASCENDING || peek(p)->kw == K_DESCENDING)) {
      desc = peek(p)->kw == K_DESCENDING;
      p->pos++;
    }
    p->q->key_field[p->q->keys] = field;
    p->q->key_desc[p->q->keys++] = desc;
    if (peek(p)->kind == T_COMMA) {
      p->pos++;
    } else if (peek(p)->kind == T_END) {
      return 0;
    }
  }
}

void ctui_query_free(CTUI_QUERY *q) {
  if (!q) {
    return;
  }
  for (int i = 0; i < q->count; i++) {
    free(q->nodes[i].text);
  }
  free(q->nodes);
  free(q->kids);
  free(q);
}

int ctui_query_compile(const CTUI_QUERY_SCHEMA *schema, const char *src,
                       long long now, CTUI_QUERY **out, char *err,
                       size_t err_cap) {
  *out = NULL;
  if (err && err_cap) {
    err[0] = '\0';
  }
  PARSER p = {.src = src, .now = now, .err = err, .err_cap = err_cap};
  if (strlen(src) > CTUI_QUERY_SOURCE_MAX) {
    return fail(&p, 0, "the search is too long");
  }
  p.q = calloc(1, sizeof *p.q);
  if (!p.q) {
    return fail(&p, 0, "no memory");
  }
  p.q->schema = schema;
  int rc = tokenize(&p);
  if (rc == 0) {
    TOKEN *t = peek(&p);
    if (t->kind == T_END || (t->kind == T_WORD && t->kw == K_SORT)) {
      p.q->root = add_leaf(&p, (NODE){.kind = N_ALL, .field = -1});
    } else {
      p.q->root = parse_or(&p);
    }
    rc = p.q->root < 0 ? -1 : 0;
  }
  if (rc == 0 && peek(&p)->kind == T_WORD && peek(&p)->kw == K_SORT) {
    rc = parse_sort(&p);
  }
  if (rc == 0 && peek(&p)->kind != T_END) {
    TOKEN *t = peek(&p);
    rc = fail(&p, t->at,
              t->kind == T_RPAREN ? "a parenthesis closes nothing"
                                  : "the search doesn't end here");
  }
  free(p.tok);
  if (rc != 0) {
    ctui_query_free(p.q);
    return -1;
  }
  *out = p.q;
  return 0;
}

