#ifndef CTUI_KITTY_H
#define CTUI_KITTY_H

#include <stddef.h>
#include <stdint.h>

/* kitty's escape codes, built so they parse: a typed command (one field
 * per key) and an encoder that checks what kitty checks and names kitty's
 * own refusal instead of emitting something it would reject. Written from
 * the published protocol; tools/check_kitty_protocol.py diffs the key
 * tables below against kitty's parser generator (gen/apc_parsers.py) and
 * runs what these build through kitty's own parser (dev-time only).
 *
 * The refusals are strings in kitty's reply form, code first ("EINVAL:
 * Filename too long"). What only the terminal's state decides (an image id
 * kitty doesn't have, a full cache, a file it may not read) passes here and
 * fails there. */

/* --- the graphics protocol (APC G) --- */

/* every key of kitty's graphics command; 0 = not sent (kitty's default for
 * every key is 0, so an omitted key and a 0 are the same command). The
 * one-character flags take one of the characters listed; the rest are
 * numbers. */
typedef struct {
  char a; /* action: t T q p d f a c (default t) */
  char d; /* what a=d deletes: a A i I c C f F n N p P q Q r R x X y Y z Z */
  char t; /* transmission: d (direct, default) f (file) t (temp file) s (shm) */
  char o; /* compression: z (zlib) */
  uint32_t f;          /* format: 24 RGB, 32 RGBA (default), 100 PNG */
  uint32_t m;          /* more chunks follow */
  uint32_t i, I, p;    /* image id, image number, placement id */
  uint32_t q;          /* quiet: 1 no OK replies, 2 no replies */
  uint32_t w, h, x, y; /* source rectangle in pixels */
  uint32_t v, s;       /* image height, width in pixels (raw formats) */
  uint32_t S, O;       /* data size, offset (files, shm) */
  uint32_t c, r;       /* columns, rows to scale the image to */
  uint32_t X, Y;       /* pixel offset within the first cell */
  uint32_t C;          /* 1: don't move the cursor */
  uint32_t U;          /* 1: a virtual placement (unicode placeholders) */
  uint32_t P, Q;       /* parent image id, parent placement id */
  uint32_t N;          /* usage hints */
  int32_t z;           /* z-index */
  int32_t H, V;        /* offset from the parent, in cells */
} CTUI_KITTY_GFX;

/* kitty's parser's limit for one escape code (vt-parser.c) */
#define CTUI_KITTY_MAX_ESCAPE (256u * 1024u)
/* the largest image side kitty takes (graphics.c) */
#define CTUI_KITTY_MAX_SIDE 10000u
/* the longest file / shm name kitty takes for t=f, t=t, t=s */
#define CTUI_KITTY_MAX_NAME 2048u

/* NULL when kitty accepts g as the command it starts, with payload[0..n)
 * its whole payload before base64: the pixel (or compressed) data for
 * t=d, the file or shm name for t=f/t/s. payload may be NULL where only n
 * matters (a chunked t=d transmission: its total size). Otherwise kitty's
 * refusal, "CODE: message". */
const char *ctui_kitty_gfx_check(const CTUI_KITTY_GFX *g, const void *payload,
                                 size_t n);

/* writes ESC _ G and g's nonzero keys into out, then ';' when has_payload:
 * the caller appends the base64 payload and ESC \ (a chunked transmission
 * slices one base64 string that way: first chunk with every key and m=1,
 * the rest only m). No checks -- run ctui_kitty_gfx_check() on the
 * command first. Returns the bytes written, 0 when cap is too small. */
size_t ctui_kitty_gfx_head(const CTUI_KITTY_GFX *g, int has_payload, char *out,
                           size_t cap);

/* the whole escape into out: checked, keys, payload[0..n) base64-encoded
 * (none when n is 0), ESC \. Returns its length, or 0 with *err (if
 * non-NULL) set to kitty's refusal, or to "ENOBUFS: ..." when cap is too
 * small / "E2BIG: ..." when the escape would pass kitty's length limit. */
size_t ctui_kitty_gfx_build(const CTUI_KITTY_GFX *g, const void *payload,
                            size_t n, char *out, size_t cap, const char **err);

/* --- drag and drop (OSC 72) --- */

typedef struct {
  char t;             /* type: a A m M r R o p P q e E k (required) */
  uint32_t m, i, o;   /* more, client id, operation */
  int32_t x, y, X, Y; /* cell / pixel positions, indices (per type) */
} CTUI_KITTY_DND;

/* NULL when kitty's parser takes d with payload[0..n) (sent as is: the
 * caller base64-encodes where the type wants it); else "CODE: message".
 * The drag and drop state machine's own refusals are kitty's to make. */
const char *ctui_kitty_dnd_check(const CTUI_KITTY_DND *d, const void *payload,
                                 size_t n);

/* ESC ] 72 ; keys [; payload] ESC \ into out, checked like
 * ctui_kitty_gfx_build(). */
size_t ctui_kitty_dnd_build(const CTUI_KITTY_DND *d, const void *payload,
                            size_t n, char *out, size_t cap, const char **err);

#endif
