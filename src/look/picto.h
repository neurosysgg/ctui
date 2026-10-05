#ifndef CTUI_LOOK_PICTO_H
#define CTUI_LOOK_PICTO_H

#include "look.h"
#include "paint.h"

/* Pictograms: what a drawn control can't say with a number (a speaker, a
 * mic, a mouse, ...), as 16x16 (some also 24x24) masks whose pixels are the
 * look's roles (paint.h), so a palette recolours them with the controls.
 * Original art in the 95 manner, not Microsoft's bitmaps. A few follow a state:
 * the speaker's waves, the battery's charge, a cross over a muted speaker or
 * mic. Drawn through the control kind CTUI_LOOK_CTL_PICTO (control.h). */

typedef enum {
  CTUI_LOOK_PICTO_NONE,
  CTUI_LOOK_PICTO_SPEAKER,    /* 🔊 value of max as 0-3 waves; off: muted */
  CTUI_LOOK_PICTO_MIC,        /* 🎤 off: muted */
  CTUI_LOOK_PICTO_BATTERY,    /* 🔋 value of max as its charge */
  CTUI_LOOK_PICTO_BOLT,       /* ⚡ charging */
  CTUI_LOOK_PICTO_BLUETOOTH,  /* ᛒ off: powered off */
  CTUI_LOOK_PICTO_MOUSE,      /* 🖱 */
  CTUI_LOOK_PICTO_KEYBOARD,   /* ⌨ */
  CTUI_LOOK_PICTO_HEADPHONES, /* 🎧 */
  CTUI_LOOK_PICTO_PHONE,      /* 📱 */
  CTUI_LOOK_PICTO_GAMEPAD,    /* 🎮 */
  CTUI_LOOK_PICTO_COMPUTER,   /* 💻 */
  CTUI_LOOK_PICTO_PLUG,       /* 🔌 */
  CTUI_LOOK_PICTO_PEN,        /* ✎ */
  CTUI_LOOK_PICTO_TOUCHPAD,   /* ▭ */
  CTUI_LOOK_PICTO_CUP,        /* ☕ sleep is blocked */
  CTUI_LOOK_PICTO_WIRED,      /* 🖧 a wired network */
  CTUI_LOOK_PICTO_TRANSFER,   /* ⇅ another connection */
  CTUI_LOOK_PICTO_SUN,        /* ☀ a display's brightness */
  CTUI_LOOK_PICTO_LAMP,       /* ● a smart light, its glass in the accent (or
                               * the control's tint: its colour); off: ○ */
  CTUI_LOOK_PICTO_DISK,       /* 💾 a drive */
  CTUI_LOOK_PICTO_DISC,       /* 💿 an optical disc */
  CTUI_LOOK_PICTO_LOCK,       /* 🔒 secured */
  CTUI_LOOK_PICTO_NOTE,       /* ♪ something playing */
  CTUI_LOOK_PICTO_SKY,        /* ☀ ☁ 🌧 ...: value = CTUI_LOOK_SKY_* parts */
  CTUI_LOOK_PICTO_DROP,       /* 💧 humidity */
  CTUI_LOOK_PICTO_LOGO,       /* ctui-wm's: a terminal window, >_ (the
                               * start button); value of max (> 0): a frame
                               * of it busy, lines printed up its screen (a
                               * browser's throbber) */
  CTUI_LOOK_PICTO_BACK,       /* ⇦ a browser's toolbar: back */
  CTUI_LOOK_PICTO_FORWARD,    /* ⇨ forward */
  CTUI_LOOK_PICTO_RELOAD,     /* ↻ reload */
  CTUI_LOOK_PICTO_HOME,       /* ⌂ home */
  CTUI_LOOK_PICTO_STOP,       /* ⛔ stop: a sign in the accent (a button's
                               * tint makes it red) */
  CTUI_LOOK_PICTO_KEY,        /* 🔑 a secure page; off: broken in two (an
                               * insecure one), in colour */
  CTUI_LOOK_PICTO_INFO,       /* ℹ a message box's: information (its i in
                               * the accent: tint it blue) */
  CTUI_LOOK_PICTO_WARNING,    /* ⚠ a warning */
  CTUI_LOOK_PICTO_ERROR,      /* ⛔ an error (a disc in the accent: red) */
  CTUI_LOOK_PICTO_IMAGE,      /* 🖼 a page's picture not (yet) there: a torn
                               * sheet, a sun and a hill (its hill in the
                               * accent: tint it green) */
  CTUI_LOOK_PICTOS,
} CTUI_LOOK_PICTO;

/* the weather pictogram's parts, layered in this order: a sun or moon
 * (PEEK: a small one behind the cloud), the cloud (up high when anything
 * falls from it), a cloudlet, then what falls */
typedef enum {
  CTUI_LOOK_SKY_SUN = 1,
  CTUI_LOOK_SKY_MOON = 2, /* instead of the sun: night */
  CTUI_LOOK_SKY_PEEK = 4, /* the sun / moon small, top left */
  CTUI_LOOK_SKY_CLOUD = 8,
  CTUI_LOOK_SKY_CLOUDLET = 16, /* a small cloud, bottom right */
  CTUI_LOOK_SKY_DRIZZLE = 32,
  CTUI_LOOK_SKY_RAIN = 64,
  CTUI_LOOK_SKY_HEAVY = 128, /* more rain */
  CTUI_LOOK_SKY_SNOW = 256,
  CTUI_LOOK_SKY_BOLT = 512,
  CTUI_LOOK_SKY_FOG = 1024,
  CTUI_LOOK_SKY_ALL = 2047,
} CTUI_LOOK_SKY;

/* the design grid: masks are this many px square, scaled by whole
 * factors into their box. Some (the top bar's) are drawn on a bigger grid
 * too, taken where it fills more of the box: 2 cells of 14x30 hold the
 * small one once (16 px) but the big one too (24 px). */
#define CTUI_LOOK_PICTO_GRID 16
#define CTUI_LOOK_PICTO_GRID_BIG 24

/* rewrites value/max to what p draws of them (the speaker's waves 0-3 of
 * 3, the battery's lit columns, the sky's known parts of 1); 0/0 for one
 * without a state. A muted speaker shows no waves. */
void ctui_look_picto_snap(CTUI_LOOK_PICTO p, int off, int *value, int *max);

/* p into the w x h box at (x, y) showing value of max (snapped or not:
 * the same pixels), in l; off: in the disabled role (a speaker or mic
 * crossed out) */
void ctui_look_picto_paint(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h,
                           CTUI_LOOK_PICTO p, int value, int max, int off,
                           const CTUI_LOOK *l);

#endif
