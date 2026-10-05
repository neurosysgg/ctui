/* strtod_l()/newlocale() are GNU/POSIX, not C11 */
#define _GNU_SOURCE

#include "json.h"

#include "../ctui.h"

#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTUI_JSON_MAX_DEPTH 64

typedef struct {
  const char *p;
  const char *start;
  char *err;
  size_t err_cap;
  int failed;
} PARSER;

static void fail(PARSER *ps, const char *what) {
  if (ps->failed) {
    return;
  }
  ps->failed = 1;
  int line = 1, col = 1;
  for (const char *c = ps->start; c < ps->p; c++) {
    if (*c == '\n') {
      line++;
      col = 1;
    } else {
      col++;
    }
  }
  snprintf(ps->err, ps->err_cap, "line %d, col %d: %s", line, col, what);
}

static void skip_ws(PARSER *ps) {
  for (;;) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
           *ps->p == '\r') {
      ps->p++;
    }
    if (ps->p[0] == '/' && ps->p[1] == '/') {
      while (*ps->p && *ps->p != '\n') {
        ps->p++;
      }
    } else if (ps->p[0] == '/' && ps->p[1] == '*') {
      const char *end = strstr(ps->p + 2, "*/");
      if (!end) {
        fail(ps, "unterminated /* comment");
        ps->p += strlen(ps->p);
        return;
      }
      ps->p = end + 2;
    } else {
      return;
    }
  }
}

static int hex4(const char *s, unsigned *out) {
  *out = 0;
  for (int i = 0; i < 4; i++) {
    char c = s[i];
    unsigned d;
    if (c >= '0' && c <= '9') {
      d = (unsigned)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = (unsigned)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      d = (unsigned)(c - 'A' + 10);
    } else {
      return 0;
    }
    *out = *out * 16 + d;
  }
  return 1;
}

/* the decoded string is never longer than its escaped source, so one
 * allocation of the source span is always enough */
static char *parse_string(PARSER *ps) {
  ps->p++; /* opening quote */
  const char *end = ps->p;
  while (*end && *end != '"') {
    end += (*end == '\\' && end[1]) ? 2 : 1;
  }
  if (*end != '"') {
    fail(ps, "unterminated string");
    return NULL;
  }
  char *out = malloc((size_t)(end - ps->p) + 1);
  size_t n = 0;
  while (ps->p < end) {
    char c = *ps->p++;
    if ((unsigned char)c < 0x20) {
      fail(ps, "raw control character in string");
      free(out);
      return NULL;
    }
    if (c != '\\') {
      out[n++] = c;
      continue;
    }
    char e = *ps->p++;
    switch (e) {
    case '"':
    case '\\':
    case '/':
      out[n++] = e;
      break;
    case 'b':
      out[n++] = '\b';
      break;
    case 'f':
      out[n++] = '\f';
      break;
    case 'n':
      out[n++] = '\n';
      break;
    case 'r':
      out[n++] = '\r';
      break;
    case 't':
      out[n++] = '\t';
      break;
    case 'u': {
      unsigned cp;
      if (!hex4(ps->p, &cp)) {
        fail(ps, "bad \\u escape");
        free(out);
        return NULL;
      }
      ps->p += 4;
      unsigned lo;
      if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u' &&
          hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ps->p += 6;
      }
      if (cp == 0) {
        /* a C string would end there, silently dropping the rest: kept
         * visible instead, as a lone surrogate is (3 bytes for 6) */
        cp = 0xFFFD;
      }
      n += (size_t)ctui_utf8_encode(cp, out + n);
      break;
    }
    default:
      fail(ps, "unknown escape in string");
      free(out);
      return NULL;
    }
  }
  out[n] = '\0';
  ps->p = end + 1;
  return out;
}

static int digit(char c) { return c >= '0' && c <= '9'; }

static size_t digits(const char *s) {
  size_t n = 0;
  while (digit(s[n])) {
    n++;
  }
  return n;
}
static void parse_value(PARSER *ps, CTUI_JSON *v, int depth);
static void parse_scalar_or_container(PARSER *ps, CTUI_JSON *v, int depth);

static void push_item(CTUI_JSON *v, int *cap) {
  if (v->count == *cap) {
    *cap = *cap ? *cap * 2 : 4;
    v->items = realloc(v->items, (size_t)*cap * sizeof(*v->items));
    if (v->type == CTUI_JSON_OBJECT) {
      v->keys = realloc(v->keys, (size_t)*cap * sizeof(*v->keys));
    }
  }
  memset(&v->items[v->count], 0, sizeof(v->items[0]));
  v->count++;
}

static void parse_container(PARSER *ps, CTUI_JSON *v, int depth) {
  int is_obj = *ps->p == '{';
  char close = is_obj ? '}' : ']';
  v->type = is_obj ? CTUI_JSON_OBJECT : CTUI_JSON_ARRAY;
  ps->p++;
  int cap = 0;
  for (;;) {
    skip_ws(ps);
    if (ps->failed) {
      return;
    }
    if (*ps->p == close) {
      ps->p++;
      return;
    }
    push_item(v, &cap);
    size_t key_start = (size_t)(ps->p - ps->start);
    if (is_obj) {
      v->keys[v->count - 1] = NULL;
      if (*ps->p != '"') {
        fail(ps, "expected a \"key\"");
        return;
      }
      v->keys[v->count - 1] = parse_string(ps);
      if (ps->failed) {
        return;
      }
      skip_ws(ps);
      if (*ps->p != ':') {
        fail(ps, "expected ':' after key");
        return;
      }
      ps->p++;
    }
    parse_value(ps, &v->items[v->count - 1], depth + 1);
    v->items[v->count - 1].key_start = is_obj ? key_start : 0;
    if (ps->failed) {
      return;
    }
    skip_ws(ps);
    if (*ps->p == ',') {
      ps->p++;
    } else if (*ps->p != close) {
      fail(ps, is_obj ? "expected ',' or '}'" : "expected ',' or ']'");
      return;
    }
  }
}

static void parse_value(PARSER *ps, CTUI_JSON *v, int depth) {
  if (depth > CTUI_JSON_MAX_DEPTH) {
    fail(ps, "nesting too deep");
    return;
  }
  skip_ws(ps);
  v->start = (size_t)(ps->p - ps->start);
  parse_scalar_or_container(ps, v, depth);
  v->end = (size_t)(ps->p - ps->start);
}

static void parse_scalar_or_container(PARSER *ps, CTUI_JSON *v, int depth) {
  char c = *ps->p;
  if (c == '{' || c == '[') {
    parse_container(ps, v, depth);
  } else if (c == '"') {
    v->type = CTUI_JSON_STRING;
    v->string = parse_string(ps);
  } else if (strncmp(ps->p, "true", 4) == 0) {
    v->type = CTUI_JSON_BOOL;
    v->boolean = 1;
    ps->p += 4;
  } else if (strncmp(ps->p, "false", 5) == 0) {
    v->type = CTUI_JSON_BOOL;
    ps->p += 5;
  } else if (strncmp(ps->p, "null", 4) == 0) {
    v->type = CTUI_JSON_NULL;
    ps->p += 4;
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    /* JSON's grammar first: strtod alone takes hex, inf, nan, 01 and 1. */
    const char *q = ps->p + (c == '-');
    int ok = digit(*q);
    q += *q == '0' ? 1 : digits(q);
    ok = ok && !digit(*q);
    if (ok && *q == '.') {
      q++;
      ok = digit(*q);
      q += digits(q);
    }
    if (ok && (*q == 'e' || *q == 'E')) {
      q += q[1] == '+' || q[1] == '-' ? 2 : 1;
      ok = digit(*q);
      q += digits(q);
    }
    /* in the C locale: under a comma-decimal LC_NUMERIC (a plugin or
     * library may set it) plain strtod would stop at the "." */
    static locale_t c_locale;
    if (!c_locale) {
      c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    }
    char *end = NULL;
    v->type = CTUI_JSON_NUMBER;
    v->number = !ok        ? 0
                : c_locale ? strtod_l(ps->p, &end, c_locale)
                           : strtod(ps->p, &end);
    if (!ok || end != q) {
      fail(ps, "invalid number");
    } else if (!isfinite(v->number)) {
      fail(ps, "number out of range");
    } else {
      ps->p = q;
    }
  } else {
    fail(ps, c ? "unexpected character" : "unexpected end of input");
  }
}

CTUI_JSON *ctui_json_parse(const char *text, char *err, size_t err_cap) {
  PARSER ps = {.p = text, .start = text, .err = err, .err_cap = err_cap};
  CTUI_JSON *root = calloc(1, sizeof(*root));
  parse_value(&ps, root, 0);
  skip_ws(&ps);
  if (!ps.failed && *ps.p) {
    fail(&ps, "trailing content after the top-level value");
  }
  if (ps.failed) {
    ctui_json_free(root);
    return NULL;
  }
  return root;
}

static void free_inner(CTUI_JSON *v) {
  free(v->string);
  for (int i = 0; i < v->count; i++) {
    free_inner(&v->items[i]);
    if (v->keys) {
      free(v->keys[i]);
    }
  }
  free(v->items);
  free(v->keys);
}

void ctui_json_free(CTUI_JSON *v) {
  if (v) {
    free_inner(v);
    free(v);
  }
}

CTUI_JSON *ctui_json_get(const CTUI_JSON *v, const char *key) {
  if (!v || v->type != CTUI_JSON_OBJECT) {
    return NULL;
  }
  for (int i = 0; i < v->count; i++) {
    if (v->keys[i] && strcmp(v->keys[i], key) == 0) {
      return &v->items[i];
    }
  }
  return NULL;
}

const char *ctui_json_str(const CTUI_JSON *v, const char *fallback) {
  return v && v->type == CTUI_JSON_STRING ? v->string : fallback;
}

double ctui_json_num(const CTUI_JSON *v, double fallback) {
  return v && v->type == CTUI_JSON_NUMBER ? v->number : fallback;
}

/* v's number clamped to [lo, hi] into *out, 0 if it has none: a cast of
   a double outside the target's range (or of NaN) is undefined */
static int json_whole(const CTUI_JSON *v, double lo, double hi, double *out) {
  if (!v || v->type != CTUI_JSON_NUMBER || isnan(v->number)) {
    return 0;
  }
  *out = v->number < lo ? lo : v->number > hi ? hi : v->number;
  return 1;
}

int ctui_json_int(const CTUI_JSON *v, int fallback) {
  double d;
  return json_whole(v, INT_MIN, INT_MAX, &d) ? (int)d : fallback;
}

unsigned ctui_json_uint(const CTUI_JSON *v, unsigned fallback) {
  double d;
  return json_whole(v, 0, UINT_MAX, &d) ? (unsigned)d : fallback;
}

long long ctui_json_llong(const CTUI_JSON *v, long long fallback) {
  double d;
  /* LLONG_MAX isn't a double: 2^63 is, and one past it */
  if (!json_whole(v, -0x1p63, 0x1p63, &d)) {
    return fallback;
  }
  return d >= 0x1p63 ? LLONG_MAX : (long long)d;
}

int ctui_json_bool(const CTUI_JSON *v, int fallback) {
  return v && v->type == CTUI_JSON_BOOL ? v->boolean : fallback;
}

/* a hand-built tree may leave a string or key NULL: as "" */
static const char *nz(const char *s) { return s ? s : ""; }

int ctui_json_equal(const CTUI_JSON *a, const CTUI_JSON *b) {
  if (!a || !b) {
    return a == b;
  }
  if (a->type != b->type) {
    return 0;
  }
  switch (a->type) {
  case CTUI_JSON_NULL:
    return 1;
  case CTUI_JSON_BOOL:
    return a->boolean == b->boolean;
  case CTUI_JSON_NUMBER:
    return a->number == b->number;
  case CTUI_JSON_STRING:
    return strcmp(nz(a->string), nz(b->string)) == 0;
  case CTUI_JSON_ARRAY:
  case CTUI_JSON_OBJECT:
    if (a->count != b->count) {
      return 0;
    }
    for (int i = 0; i < a->count; i++) {
      if ((a->keys && strcmp(nz(a->keys[i]), nz(b->keys[i])) != 0) ||
          !ctui_json_equal(&a->items[i], &b->items[i])) {
        return 0;
      }
    }
    return 1;
  }
  return 0;
}

/* --- writing --- */

void ctui_json_quote(CTUI_BUF *b, const char *s) {
  s = s ? s : ""; /* a hand-built tree's missing string */
  ctui_buf_add(b, "\"", 1);
  while (*s) {
    uint32_t cp;
    s += ctui_utf8_decode(s, &cp);
    if (cp == '"' || cp == '\\') {
      ctui_buf_printf(b, "\\%c", (int)cp);
    } else if (cp >= 0x20 && cp < 0x7f) {
      char c = (char)cp;
      ctui_buf_add(b, &c, 1);
    } else if (cp < 0x10000) {
      ctui_buf_printf(b, "\\u%04x", (unsigned)cp);
    } else {
      cp -= 0x10000;
      ctui_buf_printf(b, "\\u%04x\\u%04x", 0xd800 + (unsigned)(cp >> 10),
                      0xdc00 + (unsigned)(cp & 0x3ff));
    }
  }
  ctui_buf_add(b, "\"", 1);
}

void ctui_json_quote_list(CTUI_BUF *b, const char *const *list) {
  ctui_buf_add(b, "[", 1);
  for (int i = 0; list[i]; i++) {
    if (i) {
      ctui_buf_add(b, ",", 1);
    }
    ctui_json_quote(b, list[i]);
  }
  ctui_buf_add(b, "]", 1);
}

void ctui_json_write(CTUI_BUF *b, const CTUI_JSON *v) {
  if (!v || v->type == CTUI_JSON_NULL) {
    ctui_buf_add(b, "null", 4);
  } else if (v->type == CTUI_JSON_BOOL) {
    ctui_buf_printf(b, "%s", v->boolean ? "true" : "false");
  } else if (v->type == CTUI_JSON_NUMBER) {
    if (isfinite(v->number)) {
      /* "." whatever LC_NUMERIC says (%.17g never groups digits) */
      size_t at = b->len;
      ctui_buf_printf(b, "%.17g", v->number);
      for (char *p = b->s + at; *p; p++) {
        *p = *p == ',' ? '.' : *p;
      }
    } else {
      ctui_buf_add(b, "null", 4); /* JSON has no inf or nan */
    }
  } else if (v->type == CTUI_JSON_STRING) {
    ctui_json_quote(b, v->string);
  } else {
    int obj = v->type == CTUI_JSON_OBJECT;
    ctui_buf_add(b, obj ? "{" : "[", 1);
    for (int i = 0; i < v->count; i++) {
      if (i) {
        ctui_buf_add(b, ",", 1);
      }
      if (obj) {
        ctui_json_quote(b, v->keys[i]);
        ctui_buf_add(b, ":", 1);
      }
      ctui_json_write(b, &v->items[i]);
    }
    ctui_buf_add(b, obj ? "}" : "]", 1);
  }
}
