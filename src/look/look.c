#include "look.h"

#include <stdio.h>
#include <string.h>

const char *const ctui_look_names[] = {"win95", "nt-dark", "flat", NULL};

static const char *const ROLE_NAMES[CTUI_LOOK_ROLES] = {
    "face",        "highlight", "light",    "shadow", "dark", "accent",
    "accent_text", "groove",    "disabled", "text",   "warm",
};

/* role colours as 0xRRGGBB, in CTUI_LOOK_ROLE order */
typedef struct {
  const char *name;
  uint32_t color[CTUI_LOOK_ROLES];
  int bevel, outline, thumb, chunk, gap, groove, corner, panel;
  CTUI_LOOK_FILTER filter;
} BUILTIN;

/* 95's small icons: 16 px, its colours, a hard mask */
#define FILTER_95                                                              \
  {1,                                                                          \
   16,                                                                         \
   CTUI_LOOK_FILTER_PALETTE_WIN95,                                             \
   CTUI_LOOK_FILTER_DITHER_ORDERED,                                            \
   128,                                                                        \
   15,                                                                         \
   0,                                                                          \
   0}

// clang-format off: a look per entry, its shape numbers on one line
static const BUILTIN BUILTINS[] = {
    /* Windows 95's system colours on its silver face */
    {"win95",
     {0xc0c0c0, 0xffffff, 0xdfdfdf, 0x808080, 0x000000, 0x000080, 0xffffff,
      0xffffff, 0x808080, 0x000000, 0xffff00},
     2, 0, 0, 6, 2, 2, 0, 1, FILTER_95},
    /* the same bevels in dark greys, for the dark bars as they are */
    {"nt-dark",
     {0x3a3a3a, 0x6e6e6e, 0x4c4c4c, 0x222222, 0x000000, 0x3a6ea5, 0xffffff,
      0x161616, 0x5a5a5a, 0xdcdcdc, 0xe8c547},
     2, 0, 0, 6, 2, 2, 0, 0, FILTER_95},
    /* no bevels: plain shapes */
    {"flat",
     {0x3c3c3c, 0x5a5a5a, 0x4a4a4a, 0x2a2a2a, 0x1a1a1a, 0x5294e2, 0xffffff,
      0x262626, 0x606060, 0xd0d0d0, 0xf0c040},
     0, 0, 0, 0, 1, 0, 0, 0, {0}},
};
// clang-format on

int ctui_look_builtin(const char *name, CTUI_LOOK *out) {
  for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++) {
    const BUILTIN *b = &BUILTINS[i];
    if (strcmp(b->name, name) != 0) {
      continue;
    }
    for (int r = 0; r < CTUI_LOOK_ROLES; r++) {
      out->color[r][0] = (unsigned char)(b->color[r] >> 16);
      out->color[r][1] = (unsigned char)(b->color[r] >> 8);
      out->color[r][2] = (unsigned char)b->color[r];
    }
    out->bevel = b->bevel;
    out->outline = b->outline;
    out->thumb = b->thumb;
    out->chunk = b->chunk;
    out->gap = b->gap;
    out->groove = b->groove;
    out->corner = b->corner;
    out->panel = b->panel;
    out->filter = b->filter;
    snprintf(out->name, sizeof out->name, "%s", b->name);
    return 0;
  }
  return -1;
}

int ctui_look_role(const char *name) {
  for (int r = 0; r < CTUI_LOOK_ROLES; r++) {
    if (strcmp(ROLE_NAMES[r], name) == 0) {
      return r;
    }
  }
  return -1;
}

const char *ctui_look_role_name(int role) {
  return role >= 0 && role < CTUI_LOOK_ROLES ? ROLE_NAMES[role] : "";
}

/* FNV-1a over the fields one by one (the struct has padding) */
static uint64_t mix(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    h = (h ^ ((v >> (8 * i)) & 0xff)) * 0x100000001b3u;
  }
  return h;
}

uint64_t ctui_look_hash(const CTUI_LOOK *l) {
  uint64_t h = 0xcbf29ce484222325u;
  for (int r = 0; r < CTUI_LOOK_ROLES; r++) {
    h = mix(h, (uint32_t)l->color[r][0] << 16 | (uint32_t)l->color[r][1] << 8 |
                   l->color[r][2]);
  }
  const int f[] = {l->bevel, l->outline, l->thumb,  l->chunk,
                   l->gap,   l->groove,  l->corner, l->panel};
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    h = mix(h, (uint32_t)f[i]);
  }
  return h;
}
