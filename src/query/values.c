/* query.h's typed values: numbers, sizes, lengths, dates */
#include "query_internal.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

/* digits[.digits] at s: the number in *v (a double), the bytes it took */
static size_t number_at(const char *s, size_t len, double *v) {
  size_t i = 0;
  double x = 0;
  while (i < len && s[i] >= '0' && s[i] <= '9') {
    x = x * 10 + (s[i++] - '0');
  }
  if (i && i + 1 < len && s[i] == '.' && s[i + 1] >= '0' && s[i + 1] <= '9') {
    double scale = 0.1;
    for (i++; i < len && s[i] >= '0' && s[i] <= '9'; i++, scale /= 10) {
      x += (s[i] - '0') * scale;
    }
  }
  *v = x;
  return i;
}

static long long clamp(double x) {
  return x >= (double)CTUI_QUERY_BIG ? CTUI_QUERY_BIG : x <= -(double)CTUI_QUERY_BIG ? -CTUI_QUERY_BIG : (long long)x;
}

int ctui_query_word_is(const char *s, size_t len, const char *w) {
  size_t n = strlen(w);
  if (len != n) {
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    char c = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] + 32) : s[i];
    if (c != w[i]) {
      return 0;
    }
  }
  return 1;
}

static int parse_int(const char *s, size_t len, long long *out) {
  int neg = len && s[0] == '-';
  double x;
  size_t k = number_at(s + neg, len - (size_t)neg, &x);
  if (!k || k != len - (size_t)neg) {
    return -1;
  }
  *out = clamp(neg ? -x : x);
  return 0;
}

static int parse_size(const char *s, size_t len, long long *out) {
  double x;
  size_t k = number_at(s, len, &x);
  if (!k) {
    return -1;
  }
  static const char *const UNITS[][3] = {{"b", "", ""},
                                         {"k", "kb", "kib"},
                                         {"m", "mb", "mib"},
                                         {"g", "gb", "gib"},
                                         {"t", "tb", "tib"}};
  double mul = 1;
  for (int u = 0; u < 5; u++, mul *= 1024) {
    for (int a = 0; a < 3; a++) {
      if ((k == len && u == 0) ||
          (UNITS[u][a][0] && ctui_query_word_is(s + k, len - k, UNITS[u][a]))) {
        *out = clamp(x * mul);
        return 0;
      }
    }
  }
  return -1;
}

/* 90, 5m, 1h30m, 2d, 1.5h, 3mo, 1y; 3:30, 1:02:03 */
int ctui_query_parse_duration(const char *s, size_t len, long long *out) {
  if (memchr(s, ':', len)) {
    double total = 0;
    int parts = 0;
    for (size_t i = 0; i <= len; parts++) {
      double x;
      size_t k = number_at(s + i, len - i, &x);
      if (!k || parts == 3) {
        return -1;
      }
      total = total * 60 + x;
      i += k;
      if (i < len && s[i] != ':') {
        return -1;
      }
      if (i == len) {
        break;
      }
      i++;
    }
    *out = clamp(total);
    return 0;
  }
  static const struct {
    const char *name;
    double secs;
  } UNITS[] = {{"s", 1},          {"sec", 1},       {"m", 60},
               {"min", 60},       {"h", 3600},      {"d", 86400},
               {"w", 604800},     {"mo", 2592000},  {"y", 31536000}};
  double total = 0;
  for (size_t i = 0; i < len;) {
    double x;
    size_t k = number_at(s + i, len - i, &x);
    if (!k) {
      return -1;
    }
    i += k;
    size_t u = i;
    while (u < len && ((s[u] >= 'a' && s[u] <= 'z') || (s[u] >= 'A' && s[u] <= 'Z'))) {
      u++;
    }
    double mul = u == i ? 1 : 0;
    for (size_t n = 0; u > i && n < sizeof UNITS / sizeof *UNITS; n++) {
      if (ctui_query_word_is(s + i, u - i, UNITS[n].name)) {
        mul = UNITS[n].secs;
      }
    }
    if (!mul || (u == i && u < len)) {
      return -1;
    }
    total += x * mul;
    i = u;
  }
  *out = clamp(total);
  return 0;
}

/* YYYY[-MM[-DD[THH:MM[:SS]]]], local time: the period it names, [*lo,
 * *hi] */
static int parse_date(const char *s, size_t len, long long *lo,
                      long long *hi) {
  int f[6] = {0, 1, 1, 0, 0, 0}, nf = 0;
  static const char SEP[] = "--T::";
  size_t i = 0;
  while (nf < 6) {
    int v = 0, digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9' && digits < 4) {
      v = v * 10 + (s[i++] - '0');
      digits++;
    }
    if (!digits || (nf == 0 && digits != 4) || (nf > 0 && digits != 2)) {
      return -1;
    }
    f[nf++] = v;
    if (i == len) {
      break;
    }
    if (nf == 6 || s[i] != SEP[nf - 1]) {
      return -1;
    }
    i++;
  }
  if (i != len || nf == 4 || f[1] < 1 || f[1] > 12 || f[2] < 1 || f[2] > 31 ||
      f[3] > 23 || f[4] > 59 || f[5] > 60) {
    return -1;
  }
  struct tm a = {.tm_year = f[0] - 1900,
                 .tm_mon = f[1] - 1,
                 .tm_mday = f[2],
                 .tm_hour = f[3],
                 .tm_min = f[4],
                 .tm_sec = f[5],
                 .tm_isdst = -1};
  struct tm b = a;
  switch (nf) {
  case 1:
    b.tm_year++;
    break;
  case 2:
    b.tm_mon++;
    break;
  case 3:
    b.tm_mday++;
    break;
  case 5:
    b.tm_min++;
    break;
  default:
    b.tm_sec++;
  }
  time_t ta = mktime(&a), tb = mktime(&b);
  if (ta == (time_t)-1 || tb == (time_t)-1) {
    return -1;
  }
  *lo = (long long)ta;
  *hi = (long long)tb - 1;
  return 0;
}

/* one value of kind as the period it stands for: [*lo, *hi] */
int ctui_query_parse_typed(CTUI_QUERY_KIND kind, const char *s, size_t len,
                       long long *lo, long long *hi) {
  int rc;
  switch (kind) {
  case CTUI_QUERY_NUMBER:
    rc = parse_int(s, len, lo);
    break;
  case CTUI_QUERY_SIZE:
    rc = parse_size(s, len, lo);
    break;
  case CTUI_QUERY_DURATION:
  case CTUI_QUERY_AGE:
    rc = ctui_query_parse_duration(s, len, lo);
    break;
  case CTUI_QUERY_DATE:
    return parse_date(s, len, lo, hi);
  default:
    return -1;
  }
  *hi = *lo;
  return rc;
}

