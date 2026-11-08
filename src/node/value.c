/* strtod_l()/newlocale()/uselocale() are POSIX 2008, not C11 */
#define _GNU_SOURCE

#include "node.h"

#include "../core/utf8.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* numbers in the C locale whatever LC_NUMERIC says (a plugin may set a
 * comma-decimal one, as json.c's parser found) */
static locale_t c_numeric(void) {
  static locale_t loc;
  if (!loc) {
    loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  }
  return loc;
}

static int utf8_ok(const char *s, size_t len);

static size_t text_cap(const CTUI_NODE_TYPE *t) {
  return t->max_len ? t->max_len : CTUI_NODE_TEXT_MAX;
}

static int has_range(const CTUI_NODE_TYPE *t) { return t->min <= t->max; }

static unsigned label_count(const CTUI_NODE_TYPE *t) {
  unsigned n = 0;
  while (t->labels && t->labels[n]) {
    n++;
  }
  return n;
}

int ctui_node_type_ok(const CTUI_NODE_TYPE *t) {
  if (!t || (t->flags & ~(unsigned)(CTUI_NODE_WRITABLE | CTUI_NODE_CLAMP)) ||
      t->max_len > CTUI_NODE_TEXT_MAX ||
      (t->scale != CTUI_NODE_LINEAR && t->scale != CTUI_NODE_LOG) ||
      !isfinite(t->min) || !isfinite(t->max) || !isfinite(t->def)) {
    return 0;
  }
  /* labels and units are UTF-8: quoted for the wire, a stray byte would
   * come back as U+FFFD, three bytes, another label (the fuzzer's) */
  if (t->unit && (strlen(t->unit) > CTUI_NODE_NAME_MAX ||
                  !utf8_ok(t->unit, strlen(t->unit)))) {
    return 0;
  }
  switch (t->kind) {
  case CTUI_NODE_INT:
    return !has_range(t) || (t->def >= t->min && t->def <= t->max &&
                             t->def == floor(t->def));
  case CTUI_NODE_FLOAT:
    return !has_range(t) || (t->def >= t->min && t->def <= t->max);
  case CTUI_NODE_BOOL:
  case CTUI_NODE_STRING:
  case CTUI_NODE_TRIGGER:
    return 1;
  case CTUI_NODE_ENUM: {
    unsigned n = label_count(t);
    if (n == 0 || n > CTUI_NODE_LABELS_MAX || t->def < 0 || t->def >= n ||
        t->def != floor(t->def)) {
      return 0;
    }
    for (unsigned i = 0; i < n; i++) {
      size_t len = strlen(t->labels[i]);
      if (len == 0 || len > CTUI_NODE_NAME_MAX || strchr(t->labels[i], '\n') ||
          !utf8_ok(t->labels[i], len)) {
        return 0;
      }
    }
    return 1;
  }
  }
  return 0;
}

static int name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || strchr("_.:+@-", c) != NULL;
}

int ctui_node_path_ok(const char *path) {
  if (!path || !*path || strlen(path) > CTUI_NODE_PATH_MAX) {
    return 0;
  }
  int depth = 0;
  const char *p = path;
  while (*p) {
    const char *start = p;
    if (*p == '.') {
      return 0;
    }
    while (*p && *p != '/') {
      if (!name_char(*p)) {
        return 0;
      }
      p++;
    }
    if (p == start || p - start > CTUI_NODE_NAME_MAX ||
        ++depth > CTUI_NODE_DEPTH_MAX) {
      return 0;
    }
    if (*p == '/' && !*++p) {
      return 0; /* a trailing slash */
    }
  }
  return 1;
}

int ctui_node_path_reserved(const char *path) {
  static const char *const RESERVED[] = {"name", "events", "batch"};
  size_t len = strcspn(path, "/");
  for (size_t i = 0; i < sizeof RESERVED / sizeof *RESERVED; i++) {
    if (strlen(RESERVED[i]) == len && memcmp(path, RESERVED[i], len) == 0) {
      return 1;
    }
  }
  return 0;
}

/* --- parsing ------------------------------------------------------------- */

static int utf8_ok(const char *s, size_t len) {
  size_t i = 0;
  while (i < len) {
    if (s[i] == '\0') {
      return 0;
    }
    if ((unsigned char)s[i] < 0x80) {
      i++;
      continue;
    }
    char tmp[5] = {0};
    size_t n = len - i < 4 ? len - i : 4;
    memcpy(tmp, s + i, n); /* decode never reads past a NUL */
    uint32_t cp;
    int used = ctui_utf8_decode(tmp, &cp);
    if (cp == 0xFFFD && used == 1) {
      return 0;
    }
    i += (size_t)used;
  }
  return 1;
}

/* "+=" / "-=": 1 / -1, and *s, *len past it; 0 for an absolute write */
static int relative(const char **s, size_t *len) {
  if (*len >= 2 && ((*s)[0] == '+' || (*s)[0] == '-') && (*s)[1] == '=') {
    int sign = (*s)[0] == '+' ? 1 : -1;
    *s += 2;
    *len -= 2;
    return sign;
  }
  return 0;
}

static int digits(const char *s, size_t len) {
  if (len == 0) {
    return 0;
  }
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return 0;
    }
  }
  return 1;
}

/* s[0..len) as a decimal integer, optionally negative: 0, EINVAL, ERANGE */
static int read_int(const char *s, size_t len, int allow_sign, int64_t *out) {
  int neg = 0;
  if (allow_sign && len && s[0] == '-') {
    neg = 1;
    s++;
    len--;
  }
  if (!digits(s, len)) {
    return EINVAL;
  }
  uint64_t v = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned d = (unsigned)(s[i] - '0');
    if (v > (UINT64_MAX - d) / 10) {
      return ERANGE;
    }
    v = v * 10 + d;
  }
  if (v > (uint64_t)INT64_MAX + (uint64_t)neg) {
    return ERANGE;
  }
  *out = neg ? (int64_t)(0 - v) : (int64_t)v;
  return 0;
}

/* a decimal number: -?(D+(.D*)?|.D+)([eE][+-]?D+)? -- no inf, nan, hex */
static int read_float(const char *s, size_t len, int allow_sign, double *out) {
  char buf[64];
  if (len == 0 || len >= sizeof buf) {
    return EINVAL;
  }
  size_t i = 0, int_digits = 0, frac_digits = 0;
  if (allow_sign && s[i] == '-') {
    i++;
  }
  while (i < len && s[i] >= '0' && s[i] <= '9') {
    i++, int_digits++;
  }
  if (i < len && s[i] == '.') {
    i++;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      i++, frac_digits++;
    }
  }
  if (int_digits + frac_digits == 0) {
    return EINVAL;
  }
  if (i < len && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < len && (s[i] == '+' || s[i] == '-')) {
      i++;
    }
    size_t exp_start = i;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      i++;
    }
    if (i == exp_start) {
      return EINVAL;
    }
  }
  if (i != len) {
    return EINVAL;
  }
  memcpy(buf, s, len);
  buf[len] = '\0';
  locale_t loc = c_numeric();
  double v = loc ? strtod_l(buf, NULL, loc) : strtod(buf, NULL);
  if (!isfinite(v)) {
    return ERANGE;
  }
  *out = v;
  return 0;
}

/* v against t's range: 0 (v maybe clamped) or ERANGE */
static int in_range(const CTUI_NODE_TYPE *t, double *v) {
  if (!has_range(t) || (*v >= t->min && *v <= t->max)) {
    return 0;
  }
  if (!(t->flags & CTUI_NODE_CLAMP)) {
    return ERANGE;
  }
  *v = *v < t->min ? t->min : t->max;
  return 0;
}

static int parse_int(const CTUI_NODE_TYPE *t, const char *s, size_t len,
                     const CTUI_NODE_VALUE *cur, CTUI_NODE_VALUE *out) {
  int sign = relative(&s, &len);
  int64_t v;
  int err = read_int(s, len, !sign, &v);
  if (err) {
    return err;
  }
  if (sign) {
    if (!cur) {
      return EINVAL;
    }
    if (sign > 0 ? cur->i > INT64_MAX - v : cur->i < INT64_MIN + v) {
      if (!has_range(t) || !(t->flags & CTUI_NODE_CLAMP)) {
        return ERANGE;
      }
      v = sign > 0 ? INT64_MAX : INT64_MIN;
    } else {
      v = sign > 0 ? cur->i + v : cur->i - v;
    }
  }
  if (has_range(t)) {
    double d = (double)v;
    if (d < t->min || d > t->max) {
      if (!(t->flags & CTUI_NODE_CLAMP)) {
        return ERANGE;
      }
      v = (int64_t)(d < t->min ? t->min : t->max);
    }
  }
  out->i = v;
  return 0;
}

static int parse_float(const CTUI_NODE_TYPE *t, const char *s, size_t len,
                       const CTUI_NODE_VALUE *cur, CTUI_NODE_VALUE *out) {
  int sign = relative(&s, &len);
  double v;
  int err = read_float(s, len, !sign, &v);
  if (err) {
    return err;
  }
  if (sign) {
    if (!cur) {
      return EINVAL;
    }
    v = cur->f + sign * v;
    if (!isfinite(v)) {
      return ERANGE;
    }
  }
  err = in_range(t, &v);
  if (err) {
    return err;
  }
  out->f = v == 0 ? 0 : v; /* no "-0" */
  return 0;
}

static int text_is(const char *s, size_t len, const char *word) {
  return strlen(word) == len && memcmp(s, word, len) == 0;
}

static int parse_bool(const char *s, size_t len, const CTUI_NODE_VALUE *cur,
                      CTUI_NODE_VALUE *out) {
  if (text_is(s, len, "0") || text_is(s, len, "1")) {
    out->b = s[0] == '1';
    return 0;
  }
  if (text_is(s, len, "toggle")) {
    if (!cur) {
      return EINVAL;
    }
    out->b = !cur->b;
    return 0;
  }
  return EINVAL;
}

static int parse_enum(const CTUI_NODE_TYPE *t, const char *s, size_t len,
                      const CTUI_NODE_VALUE *cur, CTUI_NODE_VALUE *out) {
  unsigned n = label_count(t);
  for (unsigned i = 0; i < n; i++) {
    if (text_is(s, len, t->labels[i])) {
      out->e = i;
      return 0;
    }
  }
  int sign = relative(&s, &len);
  int64_t step;
  if (!sign || !cur || read_int(s, len, 0, &step) != 0) {
    return EINVAL;
  }
  int64_t v = (int64_t)cur->e + sign * (step > (int64_t)n ? (int64_t)n : step);
  if (v < 0 || v >= (int64_t)n) {
    if (!(t->flags & CTUI_NODE_CLAMP)) {
      return ERANGE;
    }
    v = v < 0 ? 0 : (int64_t)n - 1;
  }
  out->e = (unsigned)v;
  return 0;
}

int ctui_node_parse(const CTUI_NODE_TYPE *t, const char *text, size_t len,
                    const CTUI_NODE_VALUE *cur, CTUI_NODE_VALUE *out) {
  if (len && text[len - 1] == '\n') {
    len--;
  }
  CTUI_NODE_VALUE v = {0};
  int err;
  switch (t->kind) {
  case CTUI_NODE_INT:
    err = parse_int(t, text, len, cur, &v);
    break;
  case CTUI_NODE_FLOAT:
    err = parse_float(t, text, len, cur, &v);
    break;
  case CTUI_NODE_BOOL:
    err = parse_bool(text, len, cur, &v);
    break;
  case CTUI_NODE_ENUM:
    err = parse_enum(t, text, len, cur, &v);
    break;
  case CTUI_NODE_STRING:
  case CTUI_NODE_TRIGGER:
    err = len > text_cap(t) || !utf8_ok(text, len) ? EINVAL : 0;
    v.s = text;
    v.len = len;
    break;
  default:
    err = EINVAL;
  }
  if (!err) {
    *out = v;
  }
  return err;
}

/* --- formatting ---------------------------------------------------------- */

/* the fewest significant digits that read back as v, written plainly
 * ("40", "0.1") unless the exponent is far out ("1e+20") */
static void format_double(double v, CTUI_BUF *b) {
  char sci[40], buf[48];
  locale_t loc = c_numeric();
  locale_t old = loc ? uselocale(loc) : (locale_t)0;
  int prec = 1;
  for (; prec < 17; prec++) {
    snprintf(sci, sizeof sci, "%.*e", prec - 1, v);
    if (strtod(sci, NULL) == v) {
      break;
    }
  }
  snprintf(sci, sizeof sci, "%.*e", prec - 1, v);
  int exp = atoi(strchr(sci, 'e') + 1);
  if (exp > -5 && exp < 16) {
    int decimals = prec - 1 - exp;
    snprintf(buf, sizeof buf, "%.*f", decimals > 0 ? decimals : 0, v);
  } else {
    snprintf(buf, sizeof buf, "%s", sci);
  }
  if (old) {
    uselocale(old);
  }
  ctui_buf_add(b, buf, strlen(buf));
}

int ctui_node_string_ok(const CTUI_NODE_TYPE *t, const char *s, size_t len) {
  return len <= text_cap(t) && utf8_ok(s, len);
}

void ctui_node_format(const CTUI_NODE_TYPE *t, const CTUI_NODE_VALUE *v,
                      CTUI_BUF *b) {
  switch (t->kind) {
  case CTUI_NODE_INT:
    ctui_buf_printf(b, "%lld", (long long)v->i);
    break;
  case CTUI_NODE_FLOAT:
    format_double(v->f, b);
    break;
  case CTUI_NODE_BOOL:
    ctui_buf_add(b, v->b ? "1" : "0", 1);
    break;
  case CTUI_NODE_ENUM:
    if (v->e < label_count(t)) {
      ctui_buf_add(b, t->labels[v->e], strlen(t->labels[v->e]));
    }
    break;
  case CTUI_NODE_STRING:
    if (v->s) {
      ctui_buf_add(b, v->s, v->len);
    }
    break;
  case CTUI_NODE_TRIGGER:
    break;
  }
  if (!b->s) {
    ctui_buf_add(b, "", 0);
  }
}

/* --- the type on the wire ------------------------------------------------ */

static const char *const KINDS[] = {"int",    "float",  "bool",
                                    "enum",   "string", "trigger"};

void ctui_node_type_json(const CTUI_NODE_TYPE *t, CTUI_BUF *b) {
  ctui_buf_printf(b, "{\"kind\":\"%s\"", KINDS[t->kind]);
  if (t->unit && *t->unit) {
    ctui_buf_add(b, ",\"unit\":", 8);
    ctui_json_quote(b, t->unit);
  }
  if (t->kind == CTUI_NODE_INT || t->kind == CTUI_NODE_FLOAT ||
      t->kind == CTUI_NODE_ENUM || t->kind == CTUI_NODE_BOOL) {
    ctui_buf_add(b, ",\"def\":", 7);
    format_double(t->def, b);
  }
  if ((t->kind == CTUI_NODE_INT || t->kind == CTUI_NODE_FLOAT) &&
      has_range(t)) {
    ctui_buf_add(b, ",\"min\":", 7);
    format_double(t->min, b);
    ctui_buf_add(b, ",\"max\":", 7);
    format_double(t->max, b);
  }
  if (t->scale == CTUI_NODE_LOG) {
    ctui_buf_add(b, ",\"scale\":\"log\"", 14);
  }
  if (t->kind == CTUI_NODE_ENUM) {
    ctui_buf_add(b, ",\"labels\":", 10);
    ctui_json_quote_list(b, t->labels);
  }
  if ((t->kind == CTUI_NODE_STRING || t->kind == CTUI_NODE_TRIGGER) &&
      t->max_len) {
    ctui_buf_printf(b, ",\"len\":%zu", t->max_len);
  }
  if (t->flags & CTUI_NODE_WRITABLE) {
    ctui_buf_add(b, ",\"w\":1", 6);
  }
  if (t->flags & CTUI_NODE_CLAMP) {
    ctui_buf_add(b, ",\"clamp\":1", 10);
  }
  ctui_buf_add(b, "}", 1);
}

static int json_number(const CTUI_JSON *j, const char *key, double *out) {
  const CTUI_JSON *v = ctui_json_get(j, key);
  if (!v) {
    return 1;
  }
  if (v->type != CTUI_JSON_NUMBER || !isfinite(v->number)) {
    return 0;
  }
  *out = v->number;
  return 1;
}

CTUI_NODE_TYPE *ctui_node_type_from_json(const CTUI_JSON *j) {
  if (!j || j->type != CTUI_JSON_OBJECT) {
    return NULL;
  }
  const char *kind = ctui_json_str(ctui_json_get(j, "kind"), "");
  CTUI_NODE_TYPE t = {.min = 1, .max = 0};
  size_t nkinds = sizeof KINDS / sizeof *KINDS, k = 0;
  while (k < nkinds && strcmp(kind, KINDS[k]) != 0) {
    k++;
  }
  if (k == nkinds) {
    return NULL;
  }
  t.kind = (CTUI_NODE_KIND)k;
  if (!json_number(j, "def", &t.def)) {
    return NULL;
  }
  double min = 0, max = 0;
  int has_min = ctui_json_get(j, "min") != NULL,
      has_max = ctui_json_get(j, "max") != NULL;
  if (has_min != has_max || !json_number(j, "min", &min) ||
      !json_number(j, "max", &max)) {
    return NULL;
  }
  if (has_min) {
    if (min > max) {
      return NULL;
    }
    t.min = min;
    t.max = max;
  }
  const char *scale = ctui_json_str(ctui_json_get(j, "scale"), "linear");
  if (strcmp(scale, "log") == 0) {
    t.scale = CTUI_NODE_LOG;
  } else if (strcmp(scale, "linear") != 0) {
    return NULL;
  }
  double len = 0;
  if (!json_number(j, "len", &len) || len < 0 || len > CTUI_NODE_TEXT_MAX ||
      len != floor(len)) {
    return NULL;
  }
  t.max_len = (size_t)len;
  t.flags = (ctui_json_bool(ctui_json_get(j, "w"), 0) ||
                     ctui_json_int(ctui_json_get(j, "w"), 0)
                 ? CTUI_NODE_WRITABLE
                 : 0) |
            (ctui_json_bool(ctui_json_get(j, "clamp"), 0) ||
                     ctui_json_int(ctui_json_get(j, "clamp"), 0)
                 ? CTUI_NODE_CLAMP
                 : 0);

  /* one block: the struct, the labels' pointers, then their text */
  const CTUI_JSON *unit = ctui_json_get(j, "unit");
  if (unit && unit->type != CTUI_JSON_STRING) {
    return NULL;
  }
  const CTUI_JSON *labels = ctui_json_get(j, "labels");
  int nl = 0;
  size_t text = unit ? strlen(unit->string) + 1 : 0;
  if (labels) {
    if (labels->type != CTUI_JSON_ARRAY ||
        labels->count > CTUI_NODE_LABELS_MAX) {
      return NULL;
    }
    nl = labels->count;
    for (int i = 0; i < nl; i++) {
      if (labels->items[i].type != CTUI_JSON_STRING) {
        return NULL;
      }
      text += strlen(labels->items[i].string) + 1;
    }
  }
  size_t ptrs = labels ? ((size_t)nl + 1) * sizeof(char *) : 0;
  CTUI_NODE_TYPE *out = malloc(sizeof *out + ptrs + text);
  if (!out) {
    return NULL;
  }
  *out = t;
  const char **lp = (const char **)(void *)(out + 1);
  char *tp = (char *)(out + 1) + ptrs;
  if (unit) {
    size_t n = strlen(unit->string) + 1;
    memcpy(tp, unit->string, n);
    out->unit = tp;
    tp += n;
  }
  if (labels) {
    for (int i = 0; i < nl; i++) {
      size_t n = strlen(labels->items[i].string) + 1;
      memcpy(tp, labels->items[i].string, n);
      lp[i] = tp;
      tp += n;
    }
    lp[nl] = NULL;
    out->labels = lp;
  }
  if (!ctui_node_type_ok(out)) {
    free(out);
    return NULL;
  }
  return out;
}
