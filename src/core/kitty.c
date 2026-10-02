#include "kitty.h"

#include "util.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* one key of a kitty command: its character (= the struct field's name),
 * how its value is written, where it lives, and a flag's allowed values.
 * tools/check_kitty_protocol.py reads these tables (the KFLAG/KUINT/KINT
 * lines) and diffs them against kitty's gen/apc_parsers.py. */
typedef struct {
  char key;
  char type; /* 'f' one flag character, 'u' uint32, 'i' int32 */
  size_t off;
  const char *flags;
} KITTY_KEY;

#define KFLAG(T, k, set) {#k[0], 'f', offsetof(T, k), set}
#define KUINT(T, k) {#k[0], 'u', offsetof(T, k), NULL}
#define KINT(T, k) {#k[0], 'i', offsetof(T, k), NULL}

/* clang-format off */
static const KITTY_KEY gfx_keys[] = {
    KFLAG(CTUI_KITTY_GFX, a, "tTqpdfac"),
    KFLAG(CTUI_KITTY_GFX, d, "aAiIcCfFnNpPqQrRxXyYzZ"),
    KFLAG(CTUI_KITTY_GFX, t, "dfts"),
    KFLAG(CTUI_KITTY_GFX, o, "z"),
    KUINT(CTUI_KITTY_GFX, f),
    KUINT(CTUI_KITTY_GFX, m),
    KUINT(CTUI_KITTY_GFX, i),
    KUINT(CTUI_KITTY_GFX, I),
    KUINT(CTUI_KITTY_GFX, p),
    KUINT(CTUI_KITTY_GFX, q),
    KUINT(CTUI_KITTY_GFX, w),
    KUINT(CTUI_KITTY_GFX, h),
    KUINT(CTUI_KITTY_GFX, x),
    KUINT(CTUI_KITTY_GFX, y),
    KUINT(CTUI_KITTY_GFX, v),
    KUINT(CTUI_KITTY_GFX, s),
    KUINT(CTUI_KITTY_GFX, S),
    KUINT(CTUI_KITTY_GFX, O),
    KUINT(CTUI_KITTY_GFX, c),
    KUINT(CTUI_KITTY_GFX, r),
    KUINT(CTUI_KITTY_GFX, X),
    KUINT(CTUI_KITTY_GFX, Y),
    KINT(CTUI_KITTY_GFX, z),
    KUINT(CTUI_KITTY_GFX, C),
    KUINT(CTUI_KITTY_GFX, U),
    KUINT(CTUI_KITTY_GFX, P),
    KUINT(CTUI_KITTY_GFX, Q),
    KUINT(CTUI_KITTY_GFX, N),
    KINT(CTUI_KITTY_GFX, H),
    KINT(CTUI_KITTY_GFX, V),
};

static const KITTY_KEY dnd_keys[] = {
    KFLAG(CTUI_KITTY_DND, t, "aAmMrRopPqeEk"),
    KUINT(CTUI_KITTY_DND, m),
    KUINT(CTUI_KITTY_DND, i),
    KUINT(CTUI_KITTY_DND, o),
    KINT(CTUI_KITTY_DND, x),
    KINT(CTUI_KITTY_DND, y),
    KINT(CTUI_KITTY_DND, X),
    KINT(CTUI_KITTY_DND, Y),
};
/* clang-format on */

#define NKEYS(t) (sizeof(t) / sizeof((t)[0]))

/* kitty's largest t=d transmission (graphics.c MAX_DATA_SZ) */
#define KITTY_MAX_DATA (4u * 100000000u)

/* the flags hold a character of their set, the ints aren't INT32_MIN
 * (kitty reads "-2147483648" as 0 - (int32_t)2147483648: overflow) */
static const char *check_keys(const KITTY_KEY *keys, size_t nkeys,
                              const void *cmd) {
  for (size_t k = 0; k < nkeys; k++) {
    const char *field = (const char *)cmd + keys[k].off;
    if (keys[k].type == 'f') {
      char c = *field;
      if (c && (c == ',' || !strchr(keys[k].flags, c))) {
        return "EINVAL: Malformed control block, unknown flag value";
      }
    } else if (keys[k].type == 'i') {
      int32_t v;
      memcpy(&v, field, sizeof v);
      if (v == INT32_MIN) {
        return "EINVAL: Malformed control block, number is too large";
      }
    }
  }
  return NULL;
}

/* key=value pairs, sep between them, NUL-terminated; the bytes, or
 * (size_t)-1 when cap is too small */
static size_t write_keys(const KITTY_KEY *keys, size_t nkeys, const void *cmd,
                         char sep, char *out, size_t cap) {
  size_t len = 0;
  for (size_t k = 0; k < nkeys; k++) {
    const char *field = (const char *)cmd + keys[k].off;
    char val[16];
    if (keys[k].type == 'f') {
      if (!*field) {
        continue;
      }
      val[0] = *field;
      val[1] = '\0';
    } else if (keys[k].type == 'u') {
      uint32_t v;
      memcpy(&v, field, sizeof v);
      if (!v) {
        continue;
      }
      snprintf(val, sizeof val, "%lu", (unsigned long)v);
    } else {
      int32_t v;
      memcpy(&v, field, sizeof v);
      if (!v) {
        continue;
      }
      snprintf(val, sizeof val, "%ld", (long)v);
    }
    int n = snprintf(out + len, cap - len, "%s%c=%s",
                     len ? (char[]){sep, 0} : "", keys[k].key, val);
    if (n < 0 || (size_t)n >= cap - len) {
      return (size_t)-1;
    }
    len += (size_t)n;
  }
  if (len >= cap) {
    return (size_t)-1;
  }
  out[len] = '\0';
  return len;
}

static int is_raw(uint32_t f) { return f == 0 || f == 24 || f == 32; }

const char *ctui_kitty_gfx_check(const CTUI_KITTY_GFX *g, const void *payload,
                                 size_t n) {
  const char *err = check_keys(gfx_keys, NKEYS(gfx_keys), g);
  if (err) {
    return err;
  }
  if (g->i && g->I) {
    return "EINVAL: Must not specify both image id and image number";
  }
  char a = g->a ? g->a : 't';
  if (a == 'q' && !g->i) {
    return "EINVAL: Query graphics command without image id";
  }

  /* a frame with data (a=f) is transmitted like an image */
  if (a == 't' || a == 'T' || a == 'q' || (a == 'f' && n)) {
    char t = g->t ? g->t : 'd';
    if (g->f != 0 && g->f != 24 && g->f != 32 && g->f != 100) {
      return "EINVAL: Unknown image format";
    }
    if (g->s > CTUI_KITTY_MAX_SIDE || g->v > CTUI_KITTY_MAX_SIDE) {
      return "EINVAL: Image too large, width or height greater than 10000";
    }
    size_t need = (size_t)g->s * g->v * (g->f == 24 ? 3 : 4);
    if (is_raw(g->f) && need == 0) {
      return "EINVAL: Zero width/height not allowed";
    }
    if (t == 'd') {
      if (n > KITTY_MAX_DATA) {
        return "EFBIG: Too much data";
      }
      /* kitty's buffer for raw data: the image plus 10 bytes, plus 1024
       * compressed; more is refused, less (uncompressed) too short */
      if (is_raw(g->f) && n > need + (g->o ? 1024 : 10)) {
        return "EFBIG: Too much data";
      }
      if (is_raw(g->f) && !g->o && !g->m && n < need) {
        return "ENODATA: Insufficient image data";
      }
    } else {
      if (n > CTUI_KITTY_MAX_NAME) {
        return "EINVAL: Filename too long";
      }
      const char *name = payload;
      if (n == 0 || (name && memchr(name, '\0', n))) {
        return "EBADF: Failed to open file for graphics transmission";
      }
      if (t == 's' && name && name[0] != '/') {
        return "EBADF: POSIX SHM names must start with /";
      }
      if (is_raw(g->f) && !g->o && g->S && g->S < need) {
        return "EBADF: Insufficient image data";
      }
    }
    if (g->f == 100 && g->S > KITTY_MAX_DATA) {
      return "EINVAL: PNG data size too large";
    }
  }

  if (a == 'p' || a == 'T') {
    if (a == 'p' && !g->i && !g->I) {
      return "ENOENT: Put command refers to non-existent image with id: 0";
    }
    if (g->U && g->P) {
      return "EINVAL: Put command creating a virtual placement cannot refer "
             "to a parent";
    }
    if (g->P && g->P == g->i && g->Q && g->Q == g->p) {
      return "EINVAL: Put command refers to itself as its own parent";
    }
  }
  return NULL;
}

size_t ctui_kitty_gfx_head(const CTUI_KITTY_GFX *g, int has_payload, char *out,
                           size_t cap) {
  if (cap < 4) {
    return 0;
  }
  memcpy(out, "\x1b_G", 3);
  size_t len = 3;
  size_t k =
      write_keys(gfx_keys, NKEYS(gfx_keys), g, ',', out + len, cap - len);
  if (k == (size_t)-1) {
    return 0;
  }
  len += k;
  if (has_payload) {
    if (len + 1 >= cap) {
      return 0;
    }
    out[len++] = ';';
  }
  out[len] = '\0';
  return len;
}

/* the whole escape: head, then payload base64-encoded (b64) or as is,
 * then ESC \ */
static size_t build(size_t head, const void *payload, size_t n, int b64,
                    char *out, size_t cap, const char **err) {
  size_t body = b64 ? ctui_util_base64_len(n) : n;
  if (body + head + 2 > CTUI_KITTY_MAX_ESCAPE) {
    *err = "E2BIG: longer than kitty's limit for one escape code";
    return 0;
  }
  if (head == 0 || head + body + 3 > cap) {
    *err = "ENOBUFS: the escape doesn't fit the buffer";
    return 0;
  }
  size_t len = head;
  if (n > 0 && b64) {
    len += ctui_util_base64_encode(payload, n, out + len, cap - len);
  } else if (n > 0) {
    memcpy(out + len, payload, n);
    len += n;
  }
  memcpy(out + len, "\x1b\\", 3);
  return len + 2;
}

size_t ctui_kitty_gfx_build(const CTUI_KITTY_GFX *g, const void *payload,
                            size_t n, char *out, size_t cap, const char **err) {
  const char *e = ctui_kitty_gfx_check(g, payload, n);
  const char *ignore;
  err = err ? err : &ignore;
  if (e) {
    *err = e;
    return 0;
  }
  size_t head = ctui_kitty_gfx_head(g, n > 0, out, cap);
  return build(head, payload, n, 1, out, cap, err);
}

const char *ctui_kitty_dnd_check(const CTUI_KITTY_DND *d, const void *payload,
                                 size_t n) {
  const char *err = check_keys(dnd_keys, NKEYS(dnd_keys), d);
  if (err) {
    return err;
  }
  if (!d->t) {
    return "EINVAL: Malformed dnd_command control block, no type";
  }
  /* an OSC payload ends at ESC or BEL */
  if (payload && (memchr(payload, 0x1b, n) || memchr(payload, 0x07, n))) {
    return "EINVAL: a control character in the payload ends the escape";
  }
  if (d->t == 'k' && n > 4096) {
    return "EINVAL: drag source item data chunk too large";
  }
  if (d->t == 'p' && d->x < 0) { /* a drag thumbnail: y format, X x Y */
    if (d->y != 0 && d->y != 24 && d->y != 32 && d->y != 100) {
      return "EINVAL: unknown drag thumbnail format";
    }
    if (d->y != 0 && (d->X < 1 || d->Y < 1)) {
      return "EINVAL: invalid drag thumbnail image dimensions";
    }
  }
  return NULL;
}

size_t ctui_kitty_dnd_build(const CTUI_KITTY_DND *d, const void *payload,
                            size_t n, char *out, size_t cap, const char **err) {
  const char *ignore;
  err = err ? err : &ignore;
  const char *e = ctui_kitty_dnd_check(d, payload, n);
  if (e) {
    *err = e;
    return 0;
  }
  if (cap < 8) {
    *err = "ENOBUFS: the escape doesn't fit the buffer";
    return 0;
  }
  memcpy(out, "\x1b]72;", 5);
  size_t len = 5;
  size_t k =
      write_keys(dnd_keys, NKEYS(dnd_keys), d, ':', out + len, cap - len);
  if (k == (size_t)-1) {
    *err = "ENOBUFS: the escape doesn't fit the buffer";
    return 0;
  }
  len += k;
  if (n > 0) {
    if (len + 1 >= cap) {
      *err = "ENOBUFS: the escape doesn't fit the buffer";
      return 0;
    }
    out[len++] = ';';
  }
  return build(len, payload, n, 0, out, cap, err);
}
