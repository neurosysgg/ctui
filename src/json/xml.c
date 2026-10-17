/* strncasecmp is POSIX, not C11 */
#define _POSIX_C_SOURCE 200809L

#include "xml.h"

#include "../core/utf8.h"

#include <stdint.h>

#include <string.h>
#include <strings.h>

void ctui_xml_init(CTUI_XML *x, const char *s, size_t n) {
  memset(x, 0, sizeof *x);
  x->s = s;
  x->n = n;
}

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* a name's bytes: anything but spaces and the markup around names (UTF-8
 * names pass as their bytes) */
static int name_byte(char c) {
  return !is_space(c) && c != '>' && c != '/' && c != '=' && c != '<' &&
         c != '"' && c != '\'' && c != '\0';
}

/* where needle (nlen bytes) starts from at, or n if it doesn't */
static size_t find(const CTUI_XML *x, size_t at, const char *needle,
                   size_t nlen) {
  for (; at + nlen <= x->n; at++) {
    if (x->s[at] == needle[0] && memcmp(x->s + at, needle, nlen) == 0) {
      return at;
    }
  }
  return x->n;
}

static int starts(const CTUI_XML *x, const char *lit) {
  size_t l = strlen(lit);
  return x->at + l <= x->n && memcmp(x->s + x->at, lit, l) == 0;
}

/* the declaration's encoding="...", from <?xml's inside s[0..n) */
static void declaration(CTUI_XML *x, const char *s, size_t n) {
  CTUI_XML d = {.type = CTUI_XML_OPEN, .attrs = s, .attrs_len = n};
  const char *v;
  size_t len;
  if (ctui_xml_attr(&d, "encoding", &v, &len) && len < sizeof x->encoding) {
    memcpy(x->encoding, v, len);
    x->encoding[len] = '\0';
  }
}

/* a tag at x->at ('<' then a name byte or '/'): 1 with the token set,
 * 0 if it isn't one (its '<' is text then) */
static int tag(CTUI_XML *x) {
  size_t i = x->at + 1;
  int close = i < x->n && x->s[i] == '/';
  i += (size_t)close;
  size_t name = i;
  while (i < x->n && name_byte(x->s[i])) {
    i++;
  }
  if (i == name) {
    return 0;
  }
  size_t name_end = i, attrs = i;
  /* the attributes, quotes honoured, up to '>' */
  char q = 0;
  for (; i < x->n; i++) {
    char c = x->s[i];
    if (q) {
      q = c == q ? 0 : q;
    } else if (c == '"' || c == '\'') {
      q = c;
    } else if (c == '>') {
      break;
    }
  }
  if (i >= x->n) {
    return 0;
  }
  size_t attrs_end = i;
  x->self = !close && attrs_end > attrs && x->s[attrs_end - 1] == '/';
  x->type = close ? CTUI_XML_CLOSE : CTUI_XML_OPEN;
  x->name = x->s + name;
  x->name_len = name_end - name;
  x->attrs = x->s + attrs;
  x->attrs_len = attrs_end - attrs - (size_t)x->self;
  x->at = i + 1;
  return 1;
}

int ctui_xml_next(CTUI_XML *x) {
  x->name = x->attrs = x->text = NULL;
  x->name_len = x->attrs_len = x->text_len = 0;
  x->self = 0;
  while (x->at < x->n) {
    x->tok_at = x->at;
    if (x->s[x->at] == '<') {
      if (starts(x, "<!--")) {
        size_t e = find(x, x->at + 4, "-->", 3);
        x->at = e < x->n ? e + 3 : x->n;
        continue;
      }
      if (starts(x, "<![CDATA[")) {
        size_t b = x->at + 9, e = find(x, b, "]]>", 3);
        x->type = CTUI_XML_CDATA;
        x->text = x->s + b;
        x->text_len = e - b;
        x->at = e < x->n ? e + 3 : x->n;
        return x->type;
      }
      if (starts(x, "<?")) {
        size_t e = find(x, x->at + 2, "?>", 2);
        if (starts(x, "<?xml ")) {
          declaration(x, x->s + x->at + 6, e - x->at - 6);
        }
        x->at = e < x->n ? e + 2 : x->n;
        continue;
      }
      if (starts(x, "<!")) {
        /* a doctype; its internal subset in [...] may hold '>' */
        size_t e = find(x, x->at + 2, ">", 1), sub = find(x, x->at + 2, "[", 1);
        if (sub < e) {
          e = find(x, sub, "]>", 2);
          e = e < x->n ? e + 1 : x->n;
        }
        x->at = e < x->n ? e + 1 : x->n;
        continue;
      }
      if (tag(x)) {
        return x->type;
      }
    }
    /* text up to the next '<' (a '<' that started nothing is in it) */
    size_t e = x->at + 1;
    while (e < x->n && x->s[e] != '<') {
      e++;
    }
    x->type = CTUI_XML_TEXT;
    x->text = x->s + x->at;
    x->text_len = e - x->at;
    x->at = e;
    return x->type;
  }
  x->tok_at = x->n;
  x->type = CTUI_XML_END;
  return x->type;
}

int ctui_xml_is(const CTUI_XML *x, const char *name) {
  size_t l = strlen(name);
  return x->name && x->name_len == l && strncasecmp(x->name, name, l) == 0;
}

int ctui_xml_attr(const CTUI_XML *x, const char *name, const char **v,
                  size_t *len) {
  if (x->type != CTUI_XML_OPEN) {
    return 0;
  }
  size_t nl = strlen(name);
  const char *s = x->attrs, *end = x->attrs + x->attrs_len;
  while (s < end) {
    while (s < end && (is_space(*s) || *s == '/')) {
      s++;
    }
    const char *an = s;
    while (s < end && name_byte(*s)) {
      s++;
    }
    size_t al = (size_t)(s - an);
    while (s < end && is_space(*s)) {
      s++;
    }
    const char *val = NULL;
    size_t vl = 0;
    if (s < end && *s == '=') {
      s++;
      while (s < end && is_space(*s)) {
        s++;
      }
      if (s < end && (*s == '"' || *s == '\'')) {
        char q = *s++;
        val = s;
        while (s < end && *s != q) {
          s++;
        }
        vl = (size_t)(s - val);
        s += s < end;
      } else {
        val = s;
        while (s < end && !is_space(*s)) {
          s++;
        }
        vl = (size_t)(s - val);
      }
    } else if (al == 0) {
      s++; /* a stray byte */
      continue;
    }
    if (val && al == nl && strncasecmp(an, name, nl) == 0) {
      *v = val;
      *len = vl;
      return 1;
    }
  }
  return 0;
}

size_t ctui_xml_entity(const char *s, size_t n, char out[8]) {
  static const struct {
    const char *name;
    char c;
  } named[] = {
      {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}};
  if (n < 3 || s[0] != '&') {
    return 0;
  }
  size_t i = 1;
  if (s[1] == '#') {
    int hex = s[2] == 'x' || s[2] == 'X';
    i = hex ? 3 : 2;
    size_t start = i;
    uint32_t cp = 0;
    for (; i < n && i - start < 7; i++) {
      char c = s[i];
      int d = c >= '0' && c <= '9'          ? c - '0'
              : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10
              : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10
                                            : -1;
      if (d < 0) {
        break;
      }
      cp = cp * (hex ? 16u : 10u) + (uint32_t)d;
    }
    if (i == start || i >= n || s[i] != ';') {
      return 0;
    }
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
      cp = 0xfffd;
    }
    out[ctui_utf8_encode(cp, out)] = '\0';
    return i + 1;
  }
  for (size_t k = 0; k < sizeof named / sizeof *named; k++) {
    size_t l = strlen(named[k].name);
    if (n > l + 1 && memcmp(s + 1, named[k].name, l) == 0 && s[l + 1] == ';') {
      out[0] = named[k].c;
      out[1] = '\0';
      return l + 2;
    }
  }
  return 0;
}

void ctui_xml_unescape(const char *s, size_t n, CTUI_XML_ENTITY entity,
                       CTUI_BUF *b) {
  if (!entity) {
    entity = ctui_xml_entity;
  }
  size_t run = 0;
  for (size_t i = 0; i < n;) {
    char ent[8];
    size_t k = s[i] == '&' ? entity(s + i, n - i, ent) : 0;
    if (k == 0) {
      i++;
      continue;
    }
    ctui_buf_add(b, s + run, i - run);
    ctui_buf_add(b, ent, strlen(ent));
    i += k;
    run = i;
  }
  ctui_buf_add(b, s + run, n - run);
}

void ctui_xml_content(CTUI_XML *x, int raw, CTUI_BUF *b) {
  if (x->type != CTUI_XML_OPEN || x->self) {
    return;
  }
  const char *name = x->name;
  size_t name_len = x->name_len, start = x->at;
  int depth = 1;
  for (;;) {
    int t = ctui_xml_next(x);
    if (t == CTUI_XML_END) {
      if (raw && b) {
        ctui_buf_add(b, x->s + start, x->n - start);
      }
      return;
    }
    int same = (t == CTUI_XML_OPEN || t == CTUI_XML_CLOSE) &&
               x->name_len == name_len && memcmp(x->name, name, name_len) == 0;
    if (same && t == CTUI_XML_OPEN && !x->self) {
      depth++;
    } else if (same && t == CTUI_XML_CLOSE && --depth == 0) {
      if (raw && b) {
        ctui_buf_add(b, x->s + start, x->tok_at - start);
      }
      return;
    }
    if (!raw && b && t == CTUI_XML_TEXT) {
      ctui_xml_unescape(x->text, x->text_len, x->entity, b);
    } else if (!raw && b && t == CTUI_XML_CDATA) {
      ctui_buf_add(b, x->text, x->text_len);
    }
  }
}

void ctui_xml_skip(CTUI_XML *x) { ctui_xml_content(x, 0, NULL); }
