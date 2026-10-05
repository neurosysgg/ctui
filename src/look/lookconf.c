#include "lookconf.h"

#include "filter.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *key;
  size_t off;
  int lo, hi; /* hi 1 and lo 0 with boolean set: true/false */
  int boolean;
} FIELD;

/* "#rrggbb" into rgb: 0, or -1 for anything else */
static int parse_rgb(const char *s, unsigned char rgb[3]) {
  unsigned v;
  int end = 0;
  if (!s || sscanf(s, "#%6x%n", &v, &end) != 1 || end != 7 || s[7]) {
    return -1;
  }
  rgb[0] = (unsigned char)(v >> 16);
  rgb[1] = (unsigned char)(v >> 8);
  rgb[2] = (unsigned char)v;
  return 0;
}
static const FIELD FIELDS[] = {
    {"bevel", offsetof(CTUI_LOOK, bevel), 0, 2, 0},
    {"groove", offsetof(CTUI_LOOK, groove), 0, 2, 0},
    {"thumb", offsetof(CTUI_LOOK, thumb), 0, 64, 0},
    {"chunk", offsetof(CTUI_LOOK, chunk), 0, 64, 0},
    {"gap", offsetof(CTUI_LOOK, gap), 0, 16, 0},
    {"corner", offsetof(CTUI_LOOK, corner), 0, 4, 0},
    {"outline", offsetof(CTUI_LOOK, outline), 0, 1, 1},
    {"panel", offsetof(CTUI_LOOK, panel), 0, 1, 1},
};

static int base_of(const char *name, CTUI_LOOK *out, char *err,
                   size_t err_cap) {
  if (ctui_look_builtin(name, out) == 0) {
    return 0;
  }
  int n = snprintf(err, err_cap, "look: no look \"%s\" (", name);
  for (int i = 0; ctui_look_names[i] && n >= 0 && (size_t)n < err_cap; i++) {
    n += snprintf(err + n, err_cap - (size_t)n, "%s%s", i ? ", " : "",
                  ctui_look_names[i]);
  }
  if (n >= 0 && (size_t)n < err_cap) {
    snprintf(err + n, err_cap - (size_t)n, ")");
  }
  return -1;
}

static int colors(const CTUI_JSON *c, CTUI_LOOK *l, char *err, size_t err_cap) {
  if (c->type != CTUI_JSON_OBJECT) {
    snprintf(err, err_cap, "look: \"colors\" must be an object");
    return -1;
  }
  for (int i = 0; i < c->count; i++) {
    int r = ctui_look_role(c->keys[i]);
    if (r < 0) {
      snprintf(err, err_cap, "look: no colour role \"%s\"", c->keys[i]);
      return -1;
    }
    const char *s = ctui_json_str(&c->items[i], NULL);
    if (!s || parse_rgb(s, l->color[r]) != 0) {
      snprintf(err, err_cap, "look: colors: \"%s\" is \"#rrggbb\"", c->keys[i]);
      return -1;
    }
  }
  return 0;
}

static int field(const CTUI_JSON *v, const char *key, CTUI_LOOK *l, char *err,
                 size_t err_cap) {
  for (size_t i = 0; i < sizeof FIELDS / sizeof *FIELDS; i++) {
    const FIELD *f = &FIELDS[i];
    if (strcmp(f->key, key) != 0) {
      continue;
    }
    int *dst = (int *)((char *)l + f->off);
    if (f->boolean) {
      if (v->type != CTUI_JSON_BOOL) {
        snprintf(err, err_cap, "look: \"%s\" is true or false", key);
        return -1;
      }
      *dst = v->boolean;
      return 0;
    }
    if (v->type != CTUI_JSON_NUMBER || v->number != (double)(int)v->number ||
        v->number < f->lo || v->number > f->hi) {
      snprintf(err, err_cap, "look: \"%s\" is a whole number from %d to %d",
               key, f->lo, f->hi);
      return -1;
    }
    *dst = (int)v->number;
    return 0;
  }
  snprintf(err, err_cap, "look: unknown setting \"%s\"", key);
  return -1;
}

/* a name from names (NULL-terminated): its index, or -1 */
static int name_in(const char *const *names, const char *s) {
  for (int i = 0; s && names[i]; i++) {
    if (strcmp(names[i], s) == 0) {
      return i;
    }
  }
  return -1;
}

/* "filter": false, true (the base's, or 95's), or its settings over it */
static int filter(const CTUI_JSON *v, CTUI_LOOK *l, char *err, size_t err_cap) {
  CTUI_LOOK_FILTER *f = &l->filter;
  if (v->type == CTUI_JSON_BOOL) {
    if (v->boolean && !f->on) {
      CTUI_LOOK w95;
      ctui_look_builtin("win95", &w95);
      *f = w95.filter;
    }
    f->on = v->boolean;
    return 0;
  }
  if (v->type != CTUI_JSON_OBJECT) {
    snprintf(err, err_cap, "look: \"filter\" is true, false or an object");
    return -1;
  }
  if (!f->on) {
    CTUI_LOOK w95;
    ctui_look_builtin("win95", &w95);
    *f = w95.filter;
  }
  f->on = 1;
  for (int i = 0; i < v->count; i++) {
    const char *k = v->keys[i];
    const CTUI_JSON *x = &v->items[i];
    int n = x->type == CTUI_JSON_NUMBER && x->number == (double)(int)x->number
                ? (int)x->number
                : -1;
    if (strcmp(k, "size") == 0 && (n == 16 || n == 32)) {
      f->size = n;
    } else if (strcmp(k, "palette") == 0 && x->type == CTUI_JSON_STRING &&
               name_in(ctui_look_filter_palette_names, x->string) >= 0) {
      f->palette = name_in(ctui_look_filter_palette_names, x->string);
    } else if (strcmp(k, "dither") == 0 && x->type == CTUI_JSON_STRING &&
               name_in(ctui_look_filter_dither_names, x->string) >= 0) {
      f->dither = name_in(ctui_look_filter_dither_names, x->string);
    } else if (strcmp(k, "alpha") == 0 && n >= 0 && n <= 255) {
      f->alpha = n;
    } else if (strcmp(k, "boost") == 0 && n >= 0 && n <= 200) {
      f->boost = n;
    } else if ((strcmp(k, "outline") == 0 || strcmp(k, "shadow") == 0) &&
               x->type == CTUI_JSON_BOOL) {
      *(k[0] == 'o' ? &f->outline : &f->shadow) = x->boolean;
    } else {
      snprintf(err, err_cap,
               "look: filter: \"%s\" isn't one of size (16, 32), palette "
               "(none, win95, vga16, halftone, grey, roles), dither (none, "
               "ordered, fs), alpha (0-255), boost (0-200 %%), outline, "
               "shadow (true/false)",
               k);
      return -1;
    }
  }
  return 0;
}

int ctui_look_parse(const CTUI_JSON *v, CTUI_LOOK *out, char *err,
                    size_t err_cap) {
  if (!v || v->type == CTUI_JSON_NULL ||
      (v->type == CTUI_JSON_BOOL && !v->boolean)) {
    return 0;
  }
  CTUI_LOOK l;
  if (v->type == CTUI_JSON_STRING) {
    if (base_of(v->string, &l, err, err_cap) != 0) {
      return -1;
    }
    *out = l;
    return 1;
  }
  if (v->type != CTUI_JSON_OBJECT) {
    snprintf(err, err_cap,
             "\"look\" is a look's name or an object (\"base\", \"colors\", "
             "...)");
    return -1;
  }
  const CTUI_JSON *base = ctui_json_get(v, "base");
  if (base && base->type != CTUI_JSON_STRING) {
    snprintf(err, err_cap, "look: \"base\" is a look's name");
    return -1;
  }
  if (base_of(base ? base->string : ctui_look_names[0], &l, err, err_cap) !=
      0) {
    return -1;
  }
  for (int i = 0; i < v->count; i++) {
    const char *k = v->keys[i];
    int r = strcmp(k, "base") == 0     ? 0
            : strcmp(k, "colors") == 0 ? colors(&v->items[i], &l, err, err_cap)
            : strcmp(k, "filter") == 0
                ? filter(&v->items[i], &l, err, err_cap)
                : field(&v->items[i], k, &l, err, err_cap);
    if (r != 0) {
      return -1;
    }
  }
  *out = l;
  return 1;
}
