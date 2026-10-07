/* In-place config edits: see jsonedit.h. The edits are byte ranges of
 * the text replaced back to front; the check at the end compares the
 * result, parsed, with the old tree changed the same way. */
#define _GNU_SOURCE /* strdup, strndup, realpath, O_CLOEXEC */
#include "json/jsonedit.h"

#include "ctui.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  size_t start, end;
  char *text;
} EDIT;

#define MAX_EDITS 4

/* --- writing values --- */

/* s as a JSON string, UTF-8 kept: a config is read by people */
static void quote(CTUI_BUF *b, const char *s) {
  ctui_buf_add(b, "\"", 1);
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      ctui_buf_printf(b, "\\%c", c);
    } else if (c == '\n') {
      ctui_buf_add(b, "\\n", 2);
    } else if (c == '\t') {
      ctui_buf_add(b, "\\t", 2);
    } else if (c < 0x20) {
      ctui_buf_printf(b, "\\u%04x", c);
    } else {
      ctui_buf_add(b, s, 1);
    }
  }
  ctui_buf_add(b, "\"", 1);
}

/* the fewest digits that parse back to n: 0.1, not %.17g's
 * 0.10000000000000001 */
static void number(CTUI_BUF *b, double n) {
  char s[64];
  if (fabs(n) < 1e15 && n == (double)(long long)n) { /* the cast in range */
    snprintf(s, sizeof s, "%lld", (long long)n);     /* 600, not 6e+02 */
    ctui_buf_add(b, s, strlen(s));
    return;
  }
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(s, sizeof s, "%.*g", prec, n);
    for (char *p = s; *p; p++) {
      *p = *p == ',' ? '.' : *p;
    }
    CTUI_JSON *back = ctui_json_parse(s, NULL, 0);
    int same = back && back->number == n;
    ctui_json_free(back);
    if (same) {
      break;
    }
  }
  ctui_buf_add(b, s, strlen(s));
}

static void write_inline(CTUI_BUF *b, const CTUI_JSON *v) {
  if (!v || v->type == CTUI_JSON_NULL) {
    ctui_buf_add(b, "null", 4);
  } else if (v->type == CTUI_JSON_BOOL) {
    ctui_buf_printf(b, "%s", v->boolean ? "true" : "false");
  } else if (v->type == CTUI_JSON_NUMBER) {
    number(b, v->number);
  } else if (v->type == CTUI_JSON_STRING) {
    quote(b, v->string);
  } else {
    int obj = v->type == CTUI_JSON_OBJECT;
    ctui_buf_add(b, obj ? "{" : "[", 1);
    for (int i = 0; i < v->count; i++) {
      ctui_buf_add(b, i ? ", " : " ", i ? 2 : 1);
      if (obj) {
        quote(b, v->keys[i]);
        ctui_buf_add(b, ": ", 2);
      }
      write_inline(b, &v->items[i]);
    }
    ctui_buf_add(b, v->count ? " " : "", v->count ? 1 : 0);
    ctui_buf_add(b, obj ? "}" : "]", 1);
  }
}

/* one line up to this long, else a member per line */
#define INLINE_MAX 60

void ctui_json_edit_write(CTUI_BUF *b, const CTUI_JSON *v, const char *indent) {
  CTUI_BUF one = {0};
  write_inline(&one, v);
  int container =
      v && (v->type == CTUI_JSON_ARRAY || v->type == CTUI_JSON_OBJECT);
  if (!container || one.len <= INLINE_MAX) {
    ctui_buf_add(b, one.s, one.len);
    free(one.s);
    return;
  }
  free(one.s);
  int obj = v->type == CTUI_JSON_OBJECT;
  CTUI_BUF inner = {0};
  ctui_buf_printf(&inner, "%s  ", indent);
  ctui_buf_add(b, obj ? "{\n" : "[\n", 2);
  for (int i = 0; i < v->count; i++) {
    ctui_buf_add(b, inner.s, inner.len);
    if (obj) {
      quote(b, v->keys[i]);
      ctui_buf_add(b, ": ", 2);
    }
    ctui_json_edit_write(b, &v->items[i], inner.s);
    ctui_buf_add(b, i + 1 < v->count ? ",\n" : "\n", i + 1 < v->count ? 2 : 1);
  }
  ctui_buf_printf(b, "%s%s", indent, obj ? "}" : "]");
  free(inner.s);
}

/* --- trees: the result the text should parse to --- */

static void drop(CTUI_JSON *v) {
  free(v->string);
  for (int i = 0; i < v->count; i++) {
    drop(&v->items[i]);
    if (v->keys) {
      free(v->keys[i]);
    }
  }
  free(v->items);
  free(v->keys);
  memset(v, 0, sizeof *v);
}

static void clone(CTUI_JSON *dst, const CTUI_JSON *src) {
  *dst = (CTUI_JSON){.type = src->type,
                     .boolean = src->boolean,
                     .number = src->number,
                     .count = src->count};
  dst->string = src->string ? strdup(src->string) : NULL;
  if (src->type == CTUI_JSON_STRING && !dst->string) {
    dst->string = strdup("");
  }
  if (src->count) {
    dst->items = calloc((size_t)src->count, sizeof *dst->items);
    for (int i = 0; i < src->count; i++) {
      clone(&dst->items[i], &src->items[i]);
    }
  }
  if (src->type == CTUI_JSON_OBJECT && src->count) {
    dst->keys = calloc((size_t)src->count, sizeof *dst->keys);
    for (int i = 0; i < src->count; i++) {
      dst->keys[i] = strdup(src->keys[i] ? src->keys[i] : "");
    }
  }
}

static CTUI_JSON *add_member(CTUI_JSON *obj, const char *key) {
  obj->items =
      realloc(obj->items, (size_t)(obj->count + 1) * sizeof *obj->items);
  obj->keys = realloc(obj->keys, (size_t)(obj->count + 1) * sizeof *obj->keys);
  memset(&obj->items[obj->count], 0, sizeof *obj->items);
  obj->keys[obj->count] = strdup(key);
  return &obj->items[obj->count++];
}

static int index_of(const CTUI_JSON *obj, const char *key) {
  for (int i = 0; obj && obj->type == CTUI_JSON_OBJECT && i < obj->count; i++) {
    if (obj->keys[i] && strcmp(obj->keys[i], key) == 0) {
      return i;
    }
  }
  return -1;
}

/* "[N]" -> N; -1 for anything else (an object key) */
static int item_of(const char *p) {
  if (!p || p[0] != '[') {
    return -1;
  }
  char *end;
  long n = strtol(p + 1, &end, 10);
  return end != p + 1 && end[0] == ']' && !end[1] && n >= 0 && n < 1 << 30
             ? (int)n
             : -1;
}

/* v's member or item named by p, or NULL */
static CTUI_JSON *step(const CTUI_JSON *v, const char *p) {
  if (v && v->type == CTUI_JSON_ARRAY) {
    int i = item_of(p);
    return i >= 0 && i < v->count ? &v->items[i] : NULL;
  }
  int i = index_of(v, p);
  return i >= 0 ? &v->items[i] : NULL;
}

static void drop_item(CTUI_JSON *arr, int i) {
  drop(&arr->items[i]);
  memmove(&arr->items[i], &arr->items[i + 1],
          (size_t)(arr->count - i - 1) * sizeof *arr->items);
  arr->count--;
}

/* in root (an object tree): path set to v (v NULL: its member or item
 * removed); objects missing on the way made */
static void tree_edit(CTUI_JSON *root, const char *const *path,
                      const CTUI_JSON *v) {
  CTUI_JSON *obj = root;
  for (; path[1]; path++) {
    CTUI_JSON *next = step(obj, path[0]);
    if (!next) {
      if (!v || obj->type != CTUI_JSON_OBJECT) {
        return;
      }
      next = add_member(obj, path[0]);
      next->type = CTUI_JSON_OBJECT;
    }
    obj = next;
  }
  if (obj->type == CTUI_JSON_ARRAY) {
    int i = item_of(path[0]);
    if (i < 0 || i >= obj->count) {
      return;
    }
    if (!v) {
      drop_item(obj, i);
    } else {
      drop(&obj->items[i]);
      clone(&obj->items[i], v);
    }
    return;
  }
  int i = index_of(obj, path[0]);
  if (!v) {
    if (i >= 0) {
      drop(&obj->items[i]);
      free(obj->keys[i]);
      memmove(&obj->items[i], &obj->items[i + 1],
              (size_t)(obj->count - i - 1) * sizeof *obj->items);
      memmove(&obj->keys[i], &obj->keys[i + 1],
              (size_t)(obj->count - i - 1) * sizeof *obj->keys);
      obj->count--;
    }
    return;
  }
  CTUI_JSON *slot = i >= 0 ? &obj->items[i] : add_member(obj, path[0]);
  drop(slot);
  clone(slot, v);
}

/* --- the text --- */

static size_t line_start(const char *text, size_t pos) {
  while (pos > 0 && text[pos - 1] != '\n') {
    pos--;
  }
  return pos;
}

/* nothing but blanks between pos's line start and pos */
static int first_on_line(const char *text, size_t pos) {
  for (size_t i = line_start(text, pos); i < pos; i++) {
    if (text[i] != ' ' && text[i] != '\t') {
      return 0;
    }
  }
  return 1;
}

/* the blanks that start pos's line, as a new string */
static char *indent_at(const char *text, size_t pos) {
  size_t s = line_start(text, pos), e = s;
  while (text[e] == ' ' || text[e] == '\t') {
    e++;
  }
  return strndup(text + s, e - s);
}

static size_t skip_blanks(const char *text, size_t pos) {
  while (text[pos] == ' ' || text[pos] == '\t') {
    pos++;
  }
  return pos;
}

/* past whitespace and comments (json.c's skip_ws, which isn't ours) */
static size_t skip_ws(const char *text, size_t pos) {
  for (;;) {
    char c = text[pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pos++;
    } else if (c == '/' && text[pos + 1] == '/') {
      while (text[pos] && text[pos] != '\n') {
        pos++;
      }
    } else if (c == '/' && text[pos + 1] == '*') {
      const char *e = strstr(text + pos + 2, "*/");
      pos = e ? (size_t)(e - text) + 2 : strlen(text);
    } else {
      return pos;
    }
  }
}

static void add_edit(EDIT *edits, int *n, size_t start, size_t end,
                     char *text) {
  edits[(*n)++] = (EDIT){.start = start, .end = end, .text = text};
}

static char *buf_take(CTUI_BUF *b) { return b->s ? b->s : strdup(""); }

/* the edits adding key: value as obj's last member */
static int insert_edits(const char *text, const CTUI_JSON *obj, const char *key,
                        const CTUI_JSON *value, EDIT *edits, int *n) {
  size_t close = obj->end - 1;
  const CTUI_JSON *last = obj->count ? &obj->items[obj->count - 1] : NULL;
  size_t after = last ? skip_ws(text, last->end) : 0;
  int trailing = last && text[after] == ',';
  CTUI_BUF b = {0};
  if (first_on_line(text, close)) {
    /* a member per line: a new line before the closing brace's, indented
     * like the last member (or one step in from the brace) */
    char *indent;
    if (last && first_on_line(text, last->key_start)) {
      indent = indent_at(text, last->key_start);
    } else {
      char *outer = indent_at(text, close);
      CTUI_BUF in = {0};
      ctui_buf_printf(&in, "%s  ", outer);
      indent = in.s;
      free(outer);
    }
    ctui_buf_add(&b, indent, strlen(indent));
    quote(&b, key);
    ctui_buf_add(&b, ": ", 2);
    ctui_json_edit_write(&b, value, indent);
    ctui_buf_add(&b, trailing ? ",\n" : "\n", trailing ? 2 : 1);
    free(indent);
    if (last && !trailing) {
      add_edit(edits, n, last->end, last->end, strdup(","));
    }
    size_t at = line_start(text, close);
    add_edit(edits, n, at, at, buf_take(&b));
    return 0;
  }
  char *indent = indent_at(text, obj->start);
  if (!last) {
    /* {} or { }: the object written again, with its member */
    CTUI_JSON one = {.type = CTUI_JSON_OBJECT, .count = 1};
    one.items = (CTUI_JSON *)value;
    one.keys = (char **)&key;
    ctui_json_edit_write(&b, &one, indent);
    add_edit(edits, n, obj->start, obj->end, buf_take(&b));
  } else {
    /* on one line: after the last member */
    ctui_buf_add(&b, trailing ? " " : ", ", trailing ? 1 : 2);
    quote(&b, key);
    ctui_buf_add(&b, ": ", 2);
    ctui_json_edit_write(&b, value, indent);
    if (trailing) {
      ctui_buf_add(&b, ",", 1);
    }
    size_t at = trailing ? after + 1 : last->end;
    add_edit(edits, n, at, at, buf_take(&b));
  }
  free(indent);
  return 0;
}

/* the edits removing obj's member i */
static void remove_edits(const char *text, const CTUI_JSON *obj, int i,
                         EDIT *edits, int *n) {
  const CTUI_JSON *m = &obj->items[i];
  size_t from = m->key_start, to = m->end;
  size_t q = skip_blanks(text, to);
  int comma = text[q] == ',';
  if (comma) {
    to = q + 1;
  }
  size_t eol = skip_blanks(text, to);
  if (text[eol] == '/' && text[eol + 1] == '/') {
    while (text[eol] && text[eol] != '\n') {
      eol++;
    }
  }
  if (first_on_line(text, from) && (text[eol] == '\n' || !text[eol])) {
    /* a line of its own (with the comment after it): the whole line */
    from = line_start(text, from);
    to = eol + (text[eol] == '\n');
  } else if (comma) {
    to = skip_blanks(text, to);
  } else {
    while (from > 0 && (text[from - 1] == ' ' || text[from - 1] == '\t')) {
      from--;
    }
  }
  add_edit(edits, n, from, to, strdup(""));
  if (!comma && i > 0) {
    /* the last member gone: the one before it loses its comma */
    size_t p = skip_blanks(text, obj->items[i - 1].end);
    if (text[p] == ',' && p < from) {
      add_edit(edits, n, p, p + 1, strdup(""));
    }
  }
}

/* the start of the line before the one pos starts, if that line is
 * nothing but a // comment; else pos */
static size_t comment_above(const char *text, size_t pos) {
  size_t ls = line_start(text, pos);
  if (ls == 0) {
    return pos;
  }
  size_t prev = line_start(text, ls - 1);
  size_t c = skip_blanks(text, prev);
  return text[c] == '/' && text[c + 1] == '/' ? prev : pos;
}

/* where item i of arr begins with the comment lines right above it (an
 * item on a line of its own: the doc comment over a widget belongs to
 * it), at the first non-blank */
static size_t item_lead(const char *text, const CTUI_JSON *arr, int i) {
  size_t at = arr->items[i].start;
  if (!first_on_line(text, at)) {
    return at;
  }
  size_t ls = line_start(text, at);
  for (;;) {
    size_t up = comment_above(text, ls);
    if (up == ls) {
      break;
    }
    ls = up;
  }
  return skip_blanks(text, ls);
}

/* the edits removing item i of arr (with its comment lines) */
static void item_remove_edits(const char *text, const CTUI_JSON *arr, int i,
                              EDIT *edits, int *n) {
  const CTUI_JSON *m = &arr->items[i];
  size_t from = item_lead(text, arr, i), to = m->end;
  size_t q = skip_blanks(text, to);
  int comma = text[q] == ',';
  if (comma) {
    to = q + 1;
  }
  size_t eol = skip_blanks(text, to);
  if (text[eol] == '/' && text[eol + 1] == '/') {
    while (text[eol] && text[eol] != '\n') {
      eol++;
    }
  }
  if (first_on_line(text, from) && (text[eol] == '\n' || !text[eol])) {
    from = line_start(text, from);
    to = eol + (text[eol] == '\n');
  } else if (comma) {
    to = skip_blanks(text, to);
    if (text[to] == '\n') {
      /* the last of a line's items: no blanks left at the line's end */
      while (from > 0 && (text[from - 1] == ' ' || text[from - 1] == '\t')) {
        from--;
      }
    }
  } else {
    while (from > 0 && (text[from - 1] == ' ' || text[from - 1] == '\t')) {
      from--;
    }
  }
  if (!comma && i > 0) {
    size_t p = skip_blanks(text, arr->items[i - 1].end);
    if (text[p] == ',' && p < from) {
      size_t w = p + 1;
      while (w < from && (text[w] == ' ' || text[w] == '\t' ||
                          text[w] == '\n' || text[w] == '\r')) {
        w++;
      }
      if (w == from) {
        /* the last item gone, nothing but blanks since the comma before
         * it: the closing bracket moves up after the item before */
        from = p;
      } else {
        add_edit(edits, n, p, p + 1, strdup(""));
      }
    }
  }
  add_edit(edits, n, from, to, strdup(""));
}

/* the edits putting v in arr at index at (count = after the last) */
static void item_insert_edits(const char *text, const CTUI_JSON *arr, int at,
                              const CTUI_JSON *v, EDIT *edits, int *n) {
  CTUI_BUF b = {0};
  if (arr->count == 0) {
    /* [] or [ ]: the array written again, with its item */
    char *indent = indent_at(text, arr->start);
    CTUI_JSON one = {.type = CTUI_JSON_ARRAY, .count = 1};
    one.items = (CTUI_JSON *)v;
    ctui_json_edit_write(&b, &one, indent);
    free(indent);
    add_edit(edits, n, arr->start, arr->end, buf_take(&b));
    return;
  }
  if (at < arr->count) {
    /* before item at (and the comment over it) */
    size_t lead = item_lead(text, arr, at);
    if (first_on_line(text, arr->items[at].start)) {
      char *indent = indent_at(text, arr->items[at].start);
      ctui_buf_add(&b, indent, strlen(indent));
      ctui_json_edit_write(&b, v, indent);
      ctui_buf_add(&b, ",\n", 2);
      free(indent);
      size_t ls = line_start(text, lead);
      add_edit(edits, n, ls, ls, buf_take(&b));
    } else {
      char *indent = indent_at(text, arr->start);
      ctui_json_edit_write(&b, v, indent);
      ctui_buf_add(&b, ", ", 2);
      free(indent);
      add_edit(edits, n, lead, lead, buf_take(&b));
    }
    return;
  }
  const CTUI_JSON *last = &arr->items[arr->count - 1];
  int lines = first_on_line(text, last->start); /* an item per line */
  size_t after = skip_ws(text, last->end);
  int trailing = text[after] == ',';
  size_t close = arr->end - 1;
  if (lines && first_on_line(text, close)) {
    char *indent = indent_at(text, last->start);
    ctui_buf_add(&b, indent, strlen(indent));
    ctui_json_edit_write(&b, v, indent);
    ctui_buf_add(&b, trailing ? ",\n" : "\n", trailing ? 2 : 1);
    free(indent);
    if (!trailing) {
      add_edit(edits, n, last->end, last->end, strdup(","));
    }
    size_t ls = line_start(text, close);
    add_edit(edits, n, ls, ls, buf_take(&b));
    return;
  }
  /* after the last item on its line */
  char *indent = indent_at(text, last->start);
  if (lines) {
    ctui_buf_printf(&b, ",\n%s", indent);
  } else {
    ctui_buf_add(&b, ", ", 2);
  }
  ctui_json_edit_write(&b, v, indent);
  free(indent);
  add_edit(edits, n, last->end, last->end, buf_take(&b));
}

/* the edits swapping arr's items i and i + 1, each with its comments */
static void item_swap_edits(const char *text, const CTUI_JSON *arr, int i,
                            EDIT *edits, int *n) {
  size_t a0 = item_lead(text, arr, i), a1 = arr->items[i].end;
  size_t b0 = item_lead(text, arr, i + 1), b1 = arr->items[i + 1].end;
  add_edit(edits, n, a0, a1, strndup(text + b0, b1 - b0));
  add_edit(edits, n, b0, b1, strndup(text + a0, a1 - a0));
}

static int edit_cmp(const void *a, const void *b) {
  const EDIT *x = a, *y = b;
  return x->start < y->start ? 1 : x->start > y->start ? -1 : 0;
}

/* text with the edits made (freed), or NULL if two overlap */
static char *apply(const char *text, EDIT *edits, int n) {
  qsort(edits, (size_t)n, sizeof *edits, edit_cmp);
  size_t tail = strlen(text), total = tail;
  int ok = 1;
  for (int i = 0; i < n; i++) {
    if (edits[i].end > (i ? edits[i - 1].start : tail)) {
      ok = 0;
    }
    total = total - (edits[i].end - edits[i].start) + strlen(edits[i].text);
  }
  char *res = NULL;
  if (ok) {
    res = malloc(total + 1);
    size_t w = total;
    res[w] = '\0';
    for (int i = 0; i < n; i++) {
      size_t keep = tail - edits[i].end;
      w -= keep;
      memcpy(res + w, text + edits[i].end, keep);
      size_t tl = strlen(edits[i].text);
      w -= tl;
      memcpy(res + w, edits[i].text, tl);
      tail = edits[i].start;
    }
    memcpy(res, text, tail);
  }
  for (int i = 0; i < n; i++) {
    free(edits[i].text);
  }
  return res;
}

/* text with the edits made, checked: it must parse to doc changed by
 * change(doc) (doc is changed); NULL with err if not */
static char *finish(const char *text, CTUI_JSON *doc, EDIT *edits, int n,
                    char *err, size_t err_cap) {
  char perr[256];
  char *res = apply(text, edits, n);
  if (!res) {
    snprintf(err, err_cap, "two changes to one place");
  }
  CTUI_JSON *got = res ? ctui_json_parse(res, perr, sizeof perr) : NULL;
  if (res && !got) {
    snprintf(err, err_cap, "the edit broke the file (%s)", perr);
  } else if (got && !ctui_json_equal(doc, got)) {
    snprintf(err, err_cap, "the edit didn't come out as intended");
    ctui_json_free(got);
    got = NULL;
  }
  if (!got) {
    ctui_logf(E_WRN, "[CTUI:JSONEDIT] - %s: refused\n", err);
    free(res);
    res = NULL;
  }
  ctui_json_free(got);
  return res;
}

/* doc's value at path[0..depth) for an edit at path[depth]: the object
 * or array holding it (down while the path exists; *depth = where it
 * stopped). NULL with err if the path runs through something else */
static const CTUI_JSON *holder(const CTUI_JSON *doc, const char *const *path,
                               int *depth, char *err, size_t err_cap) {
  const CTUI_JSON *obj = doc;
  int d = 0;
  for (; path[d + 1]; d++) {
    const CTUI_JSON *next = step(obj, path[d]);
    if (!next) {
      break;
    }
    obj = next;
  }
  int arr = obj->type == CTUI_JSON_ARRAY;
  if (!(obj->type == CTUI_JSON_OBJECT || (arr && !path[d + 1])) ||
      (arr && item_of(path[d]) < 0) || (!arr && item_of(path[d]) >= 0)) {
    if (d) {
      snprintf(err, err_cap, "\"%s\" isn't %s", path[d - 1],
               item_of(path[d]) >= 0 ? "a list" : "an object");
    } else {
      snprintf(err, err_cap, "the top isn't an object");
    }
    return NULL;
  }
  *depth = d;
  return obj;
}

/* the set (v) or remove (v NULL) */
static char *edit(const char *text, const char *const *path, const CTUI_JSON *v,
                  char *err, size_t err_cap) {
  char perr[256];
  if (!path || !path[0]) {
    snprintf(err, err_cap, "no path");
    return NULL;
  }
  CTUI_JSON *doc = ctui_json_parse(text, perr, sizeof perr);
  if (!doc) {
    snprintf(err, err_cap, "%s", perr);
    return NULL;
  }
  EDIT edits[MAX_EDITS];
  int n = 0, depth = 0;
  const CTUI_JSON *obj = holder(doc, path, &depth, err, err_cap);
  if (!obj) {
    ctui_json_free(doc);
    return NULL;
  }
  if (obj->type == CTUI_JSON_ARRAY) {
    int i = item_of(path[depth]);
    if (i >= obj->count) {
      if (!v) {
        ctui_json_free(doc);
        return strdup(text);
      }
      snprintf(err, err_cap, "no item %d there (%d)", i, obj->count);
      ctui_json_free(doc);
      return NULL;
    }
    if (!v) {
      item_remove_edits(text, obj, i, edits, &n);
    } else {
      const CTUI_JSON *old = &obj->items[i];
      char *indent = indent_at(text, old->start);
      CTUI_BUF b = {0};
      ctui_json_edit_write(&b, v, indent);
      free(indent);
      add_edit(edits, &n, old->start, old->end, buf_take(&b));
    }
  } else {
    int i = index_of(obj, path[depth]);
    if (!v) {
      if (path[depth + 1] || i < 0) {
        ctui_json_free(doc);
        return strdup(text);
      }
      remove_edits(text, obj, i, edits, &n);
    } else if (!path[depth + 1] && i >= 0) {
      const CTUI_JSON *old = &obj->items[i];
      char *indent = indent_at(text, old->key_start);
      CTUI_BUF b = {0};
      ctui_json_edit_write(&b, v, indent);
      free(indent);
      add_edit(edits, &n, old->start, old->end, buf_take(&b));
    } else {
      /* the missing rest of the path as objects around v */
      for (int k = depth + 1; path[k]; k++) {
        if (item_of(path[k]) >= 0) {
          snprintf(err, err_cap, "no list \"%s\" to go into", path[k - 1]);
          ctui_json_free(doc);
          return NULL;
        }
      }
      CTUI_JSON value;
      clone(&value, v);
      int last = depth;
      while (path[last + 1]) {
        last++;
      }
      for (int k = last; k > depth; k--) {
        CTUI_JSON wrap = {.type = CTUI_JSON_OBJECT};
        *add_member(&wrap, path[k]) = value;
        value = wrap;
      }
      insert_edits(text, obj, path[depth], &value, edits, &n);
      drop(&value);
    }
  }
  tree_edit(doc, path, v);
  char *res = finish(text, doc, edits, n, err, err_cap);
  ctui_json_free(doc);
  return res;
}

/* the array at path in doc, or NULL with err */
static CTUI_JSON *array_at(CTUI_JSON *doc, const char *const *path, char *err,
                           size_t err_cap) {
  CTUI_JSON *v = doc;
  for (int d = 0; v && path && path[d]; d++) {
    v = step(v, path[d]);
  }
  if (!v || v->type != CTUI_JSON_ARRAY) {
    snprintf(err, err_cap, "no list there");
    return NULL;
  }
  return v;
}

char *ctui_json_edit_insert(const char *text, const char *const *path, int at,
                            const CTUI_JSON *v, char *err, size_t err_cap) {
  char perr[256];
  CTUI_JSON *doc = ctui_json_parse(text, perr, sizeof perr);
  if (!doc) {
    snprintf(err, err_cap, "%s", perr);
    return NULL;
  }
  CTUI_JSON *arr = array_at(doc, path, err, err_cap);
  if (!arr || !v || at < 0 || at > arr->count) {
    if (arr) {
      snprintf(err, err_cap, "no place %d in a list of %d", at, arr->count);
    }
    ctui_json_free(doc);
    return NULL;
  }
  EDIT edits[MAX_EDITS];
  int n = 0;
  item_insert_edits(text, arr, at, v, edits, &n);
  /* the tree: v at at */
  arr->items =
      realloc(arr->items, (size_t)(arr->count + 1) * sizeof *arr->items);
  memmove(&arr->items[at + 1], &arr->items[at],
          (size_t)(arr->count - at) * sizeof *arr->items);
  clone(&arr->items[at], v);
  arr->count++;
  char *res = finish(text, doc, edits, n, err, err_cap);
  ctui_json_free(doc);
  return res;
}

char *ctui_json_edit_swap(const char *text, const char *const *path, int i,
                          char *err, size_t err_cap) {
  char perr[256];
  CTUI_JSON *doc = ctui_json_parse(text, perr, sizeof perr);
  if (!doc) {
    snprintf(err, err_cap, "%s", perr);
    return NULL;
  }
  CTUI_JSON *arr = array_at(doc, path, err, err_cap);
  if (!arr || i < 0 || i + 1 >= arr->count) {
    if (arr) {
      snprintf(err, err_cap, "no items %d and %d in a list of %d", i, i + 1,
               arr->count);
    }
    ctui_json_free(doc);
    return NULL;
  }
  EDIT edits[MAX_EDITS];
  int n = 0;
  item_swap_edits(text, arr, i, edits, &n);
  CTUI_JSON t = arr->items[i];
  arr->items[i] = arr->items[i + 1];
  arr->items[i + 1] = t;
  char *res = finish(text, doc, edits, n, err, err_cap);
  ctui_json_free(doc);
  return res;
}

char *ctui_json_edit_set(const char *text, const char *const *path,
                         const CTUI_JSON *v, char *err, size_t err_cap) {
  if (!v) {
    snprintf(err, err_cap, "no value");
    return NULL;
  }
  return edit(text, path, v, err, err_cap);
}

char *ctui_json_edit_remove(const char *text, const char *const *path,
                            char *err, size_t err_cap) {
  return edit(text, path, NULL, err, err_cap);
}

/* --- files --- */

char *ctui_json_edit_load(const char *path, char *err, size_t err_cap) {
  FILE *f = fopen(path, "r");
  if (!f) {
    snprintf(err, err_cap, "%s: %s", path, strerror(errno));
    return NULL;
  }
  CTUI_BUF b = {0};
  char chunk[4096];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
    ctui_buf_add(&b, chunk, got);
  }
  int bad = ferror(f);
  fclose(f);
  if (bad) {
    snprintf(err, err_cap, "%s: read error", path);
    free(b.s);
    return NULL;
  }
  return buf_take(&b);
}

/* text into path through a temp file next to it: 0, or -1 with err */
static int write_file(const char *path, const char *text, mode_t mode,
                      char *err, size_t err_cap) {
  char tmp[PATH_MAX + 32];
  snprintf(tmp, sizeof tmp, "%s.tmp-%d", path, (int)getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) {
    snprintf(err, err_cap, "%s: %s", tmp, strerror(errno));
    return -1;
  }
  fchmod(fd, mode);
  size_t len = strlen(text), done = 0;
  while (done < len) {
    ssize_t w = write(fd, text + done, len - done);
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w <= 0) {
      break;
    }
    done += (size_t)w;
  }
  int ok = done == len && fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  if (!ok || rename(tmp, path) != 0) {
    snprintf(err, err_cap, "%s: %s", path, strerror(errno ? errno : EIO));
    unlink(tmp);
    return -1;
  }
  return 0;
}

int ctui_json_edit_save(const char *path, const char *text, char *err,
                        size_t err_cap) {
  char perr[256];
  CTUI_JSON *doc = ctui_json_parse(text, perr, sizeof perr);
  if (!doc) {
    snprintf(err, err_cap, "not saved: %s", perr);
    return -1;
  }
  ctui_json_free(doc);
  /* a symlinked config (dotfiles) keeps its link: the target is written */
  char real[PATH_MAX];
  const char *target = realpath(path, real) ? real : path;
  struct stat st;
  mode_t mode = stat(target, &st) == 0 ? st.st_mode & 07777 : 0644;
  char *old = ctui_json_edit_load(target, perr, sizeof perr);
  if (old) {
    char bak[PATH_MAX + 8];
    snprintf(bak, sizeof bak, "%s.bak", target);
    int rc = write_file(bak, old, mode, err, err_cap);
    free(old);
    if (rc != 0) {
      return -1;
    }
  }
  return write_file(target, text, mode, err, err_cap);
}
