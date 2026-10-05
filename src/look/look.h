#ifndef CTUI_LOOK_H
#define CTUI_LOOK_H

#include <stdint.h>

/* A look: the colour roles and shape parameters every drawn control
 * (control.h) reads, and nothing else does -- tuning one is a config edit
 * (lookconf.h), no art is stored. Plain data; came from ctui-wm (its
 * gui_overhaul.md) once ctui-mus wanted it too. */

typedef enum {
  CTUI_LOOK_FACE,        /* a raised thing's surface (95: silver) */
  CTUI_LOOK_HIGHLIGHT,   /* a bevel's lit outer edge (white) */
  CTUI_LOOK_LIGHT,       /* its lit inner edge */
  CTUI_LOOK_SHADOW,      /* its shaded inner edge (grey) */
  CTUI_LOOK_DARK,        /* its shaded outer edge, the outline (black) */
  CTUI_LOOK_ACCENT,      /* a fill, a selection (navy) */
  CTUI_LOOK_ACCENT_TEXT, /* marks on the accent */
  CTUI_LOOK_GROOVE,      /* inside a sunken track or field */
  CTUI_LOOK_DISABLED,    /* marks of something off or muted */
  CTUI_LOOK_TEXT,        /* marks on the face: a tick, a glyph */
  CTUI_LOOK_WARM,        /* a sun, a moon, a bolt (95's yellow) */
  CTUI_LOOK_ROLES,
} CTUI_LOOK_ROLE;

/* the period filter app icons go through (filter.h; gui_overhaul.md round
 * 2): not part of the controls' hash */
typedef enum {
  CTUI_LOOK_FILTER_PALETTE_NONE,  /* the colours as they are */
  CTUI_LOOK_FILTER_PALETTE_WIN95, /* 95's 20 system colours */
  CTUI_LOOK_FILTER_PALETTE_VGA16,
  CTUI_LOOK_FILTER_PALETTE_HALFTONE, /* 216 web colours + the 20 */
  CTUI_LOOK_FILTER_PALETTE_GREY,     /* black, grey, silver, white (NT 3.x) */
  CTUI_LOOK_FILTER_PALETTE_ROLES,    /* the look's own role colours */
  CTUI_LOOK_FILTER_PALETTES,
} CTUI_LOOK_FILTER_PALETTE;

typedef enum {
  CTUI_LOOK_FILTER_DITHER_NONE,
  CTUI_LOOK_FILTER_DITHER_ORDERED, /* 4x4 Bayer */
  CTUI_LOOK_FILTER_DITHER_FS,      /* Floyd-Steinberg */
  CTUI_LOOK_FILTER_DITHERS,
} CTUI_LOOK_FILTER_DITHER;

typedef struct {
  int on;
  int size;    /* the icon's side after it: 16 or 32 px */
  int palette; /* CTUI_LOOK_FILTER_PALETTE */
  int dither;  /* CTUI_LOOK_FILTER_DITHER */
  int alpha;   /* opaque from this alpha up, clear below (95's 1-bit
                * mask); 0 keeps soft edges */
  int boost;   /* % more contrast and saturation (small icons lose it) */
  int outline; /* a 1 px ring in the dark role around the mask */
  int shadow;  /* a 1 px drop shadow, down and right, in shadow */
} CTUI_LOOK_FILTER;

typedef struct {
  unsigned char color[CTUI_LOOK_ROLES][3];
  int bevel;   /* a raised/sunken edge's depth: 0 (flat), 1 or 2 px */
  int outline; /* a 1 px dark line around thumbs, buttons and chips */
  int thumb;   /* a slider thumb's width in px; 0 = from the box's height */
  int chunk;   /* a fill drawn as chunks this wide (the 95 progress
                * bar); 0 = one solid block */
  int gap;     /* px between chunks, and between signal bars */
  int groove;  /* a track's sunken edge depth, 0-2 px */
  int corner;  /* px cut off each corner of a raised thing (0 for 95) */
  int panel;   /* paint the face behind every control (silver zones); 0 =
                * transparent, the zone's background shows */
  CTUI_LOOK_FILTER filter;
  char name[16]; /* the built-in it came from: the hand-made icons' folder
                  * (filter.h) */
} CTUI_LOOK;

/* the built-in looks' names, NULL-terminated; the first is the default */
extern const char *const ctui_look_names[];

/* the built-in look called name into *out; 0, or -1 for no such look */
int ctui_look_builtin(const char *name, CTUI_LOOK *out);

/* the role called name ("face", "accent_text", ...): its index, or -1 */
int ctui_look_role(const char *name);
const char *ctui_look_role_name(int role);

/* a hash of every field: part of each drawn image's cache key, so a
 * changed look draws everything anew */
uint64_t ctui_look_hash(const CTUI_LOOK *l);

#endif
