#ifndef CTUI_LOOK_CONTROL_H
#define CTUI_LOOK_CONTROL_H

#include "look.h"
#include "picto.h"

#include <stdint.h>

/* The drawn controls: each a pure function of its state, a box in device
 * pixels and a look -- no stored art (ctui-wm's gui_overhaul.md). Widgets
 * draw them over the cells their glyphs took (drawn.h caches the images);
 * horizontal ones read left to right. */

typedef enum {
  CTUI_LOOK_CTL_SLIDER, /* a thumb on a groove at value of max, accent up to
                         * it (volume, brightness) */
  CTUI_LOOK_CTL_LEVEL,  /* value of max as a fill in a sunken field, in the
                         * look's chunks (battery, a progress bar) */
  CTUI_LOOK_CTL_SIGNAL, /* max bars rising left to right, value of them lit */
  CTUI_LOOK_CTL_CHECK,  /* a sunken square, ticked when CHECKED */
  CTUI_LOOK_CTL_TOGGLE, /* a groove with the thumb at the left, at the right
                         * (on the accent) when CHECKED */
  CTUI_LOOK_CTL_BUTTON, /* raised, sunken while PRESSED, its glyph (or its
                         * pictogram: the start button's logo) inside; with
                         * a pictogram and max > 0 a toolbar's: the
                         * pictogram in its top value of max, the rest left
                         * for a label (text over it), the pictogram's
                         * accent in the tint if one is given; CHECKED:
                         * latched down, the face inside checkered with
                         * the highlight (95's taskbar button of the
                         * active window); span: px left clear above and
                         * below it (off a bar's edge), up to a quarter of
                         * its height */
  CTUI_LOOK_CTL_CHIP,   /* the glyph alone: raised on HOVER, sunken while
                         * PRESSED (a 95 toolbar button); the glyph in the
                         * tint if one is given (a menu's ▶ on its
                         * selection) */
  CTUI_LOOK_CTL_SCROLL, /* a scrollbar, vertical when taller than wide:
                         * value = the first shown of max, span = how many
                         * show */
  CTUI_LOOK_CTL_PICTO,  /* a pictogram (picto.h) with its state in value of
                         * max; OFF greys it (a muted speaker is crossed) */
  CTUI_LOOK_CTL_RADIO,  /* a sunken round field, a dot in it when CHECKED */
  CTUI_LOOK_CTL_FIELD,  /* a sunken text field; with a glyph a button at its
                         * right end holds it (a drop-down's ▾) */
  CTUI_LOOK_CTL_FRAME,  /* an etched outline (95's group box; a flat look's
                         * a 1 px line), clear inside: a page's bordered
                         * badge or tag */
  CTUI_LOOK_CTL_PANEL,  /* a status bar's panel: a 1 px sunken edge, the
                         * face inside (text over it) */
  CTUI_LOOK_CTL_BOX,    /* a row of a page's frame (markup.h's: a bordered
                         * box, a table, a code block, a rule, a message
                         * box's icon) as box, sides and place say; span =
                         * the cells it is wide (where their middles are);
                         * the tint inks a flat style (solid, double,
                         * dashed, dotted: else the shadow) or an icon's
                         * accent */
  CTUI_LOOK_CTL_SPLITTER, /* the bar between two panes that resizes them,
                           * vertical when taller than wide: the face with
                           * a grip of raised bumps in its middle, the
                           * shadow while PRESSED (being dragged) */
  /* a bar's surface (a taskbar, a top bar): the face, raised along the
   * sides set (1 << top, right, bottom, left: the box's order) -- the edges
   * facing the screen; none: the face alone */
  CTUI_LOOK_CTL_BAR,
  /* a level meter's LEDs in a black well: value of max lit, green, the
   * top 30 % amber, the top 5 % red, in every look; span: a held peak
   * (one more lit, 0 none); CHECKED: clipped (the last one lit red).
   * Taller than wide: NT Task Manager's usage meter instead, its LEDs
   * rows lit from the bottom, all green */
  CTUI_LOOK_CTL_METER,
  /* NT Task Manager's history graph: a black well, a dim green grid, the
   * samples (0-100, the newest at the right) as a bright green line;
   * samples2 (as many) a red one over it (its kernel time); a point every
   * tenth of its height in px, or with span > 0 span points across it
   * (a big graph holding minutes) */
  CTUI_LOOK_CTL_GRAPH,
  CTUI_LOOK_CTL_KINDS,
} CTUI_LOOK_CTL_KIND;

typedef enum {
  CTUI_LOOK_CTL_OFF = 1,     /* muted, disabled: marks in the disabled role */
  CTUI_LOOK_CTL_HOVER = 2,   /* the pointer is over it */
  CTUI_LOOK_CTL_PRESSED = 4, /* held down */
  CTUI_LOOK_CTL_CHECKED = 8, /* a check or toggle that is on, a button
                              * latched down */
  /* a check box or radio button with the keys' focus and no label to
   * carry it: 95's dotted rectangle round it; a button's inside its
   * bevel */
  CTUI_LOOK_CTL_FOCUSED = 16,
  /* nothing behind it where the look would put its panel (a pictogram in
   * an app's list, on the list's own background) */
  CTUI_LOOK_CTL_BARE = 32,
} CTUI_LOOK_CTL_FLAG;

typedef enum {
  CTUI_LOOK_GLYPH_NONE,
  CTUI_LOOK_GLYPH_CLOSE,   /* ✕ */
  CTUI_LOOK_GLYPH_EJECT,   /* ⏏ */
  CTUI_LOOK_GLYPH_PLAY,    /* ▶ */
  CTUI_LOOK_GLYPH_PAUSE,   /* ⏸ */
  CTUI_LOOK_GLYPH_PREV,    /* ⏮ */
  CTUI_LOOK_GLYPH_NEXT,    /* ⏭ */
  CTUI_LOOK_GLYPH_STOP,    /* ⏹ */
  CTUI_LOOK_GLYPH_REFRESH, /* ↻ */
  CTUI_LOOK_GLYPH_BUSY,    /* … */
  CTUI_LOOK_GLYPH_BACK,    /* ◀ (forward is PLAY's ▶) */
  CTUI_LOOK_GLYPH_DOWN,    /* ▾ */
  /* a window's caption buttons, 95's: a bar on the baseline, a window,
   * two windows one behind the other */
  CTUI_LOOK_GLYPH_MINIMIZE, /* _ */
  CTUI_LOOK_GLYPH_MAXIMIZE, /* □ */
  CTUI_LOOK_GLYPH_RESTORE,  /* ❐ */
  CTUI_LOOK_GLYPHS,
} CTUI_LOOK_GLYPH;

typedef struct {
  CTUI_LOOK_CTL_KIND kind;
  int value, max, span;
  unsigned flags; /* CTUI_LOOK_CTL_FLAG */
  CTUI_LOOK_GLYPH glyph;
  CTUI_LOOK_PICTO picto; /* CTUI_LOOK_CTL_PICTO's */
  /* a pictogram's own colour for its accent part, as 0xRRGGBB with
   * CTUI_LOOK_CTL_TINT set (a smart light's); 0 = the look's accent */
  uint32_t tint;
  /* painted where the control leaves the box clear, as 0xRRGGBB with
   * CTUI_LOOK_CTL_TINT set (the paper of a page it sits in: an image under
   * text covers the cells' background); 0 = left clear, or the look's
   * panel */
  uint32_t under;
  /* CTUI_LOOK_CTL_BOX's: its CTUI_LOOK_BOX_STYLE, the sides drawn (1 <<
   * top, right, bottom, left: CSS's order; a CTUI_LOOK_CTL_BAR's raised
   * ones too) and its place (CTUI_LOOK_BOX_TOP, ...) */
  uint8_t box, sides, place;
  /* CTUI_LOOK_CTL_BOX's: painted inside its edges (a page's background),
   * as 0xRRGGBB with CTUI_LOOK_CTL_TINT set; 0 = what under paints */
  uint32_t fill;
  /* CTUI_LOOK_CTL_BOX's top row: its edge left open over gap_cols cells
   * from cell gap (a group box's heading, a window's title on it) */
  uint16_t gap, gap_cols;
  /* CTUI_LOOK_CTL_GRAPH's: count samples, oldest first (the caller's;
   * only read while the image is drawn or its key made) */
  const unsigned char *samples;
  const unsigned char *samples2; /* NULL: one line */
  int count;
} CTUI_LOOK_CONTROL;

#define CTUI_LOOK_CTL_TINT 0x1000000u

/* a CTUI_LOOK_CTL_BOX's box: CSS's border styles, then a code block's
 * field and a message box's icons (the numbers markup.h's frames have) */
typedef enum {
  CTUI_LOOK_BOX_SOLID = 1,
  CTUI_LOOK_BOX_DOUBLE,
  CTUI_LOOK_BOX_DASHED,
  CTUI_LOOK_BOX_DOTTED,
  CTUI_LOOK_BOX_GROOVE, /* 95's etched edge (a rule: shadow over highlight) */
  CTUI_LOOK_BOX_RIDGE,
  CTUI_LOOK_BOX_INSET,  /* sunken */
  CTUI_LOOK_BOX_OUTSET, /* raised */
  CTUI_LOOK_BOX_FIELD,  /* sunken 2 px, the groove inside (a text field) */
  CTUI_LOOK_BOX_INFO,   /* the pictogram alone over the box (2 rows high
                         * when place has only TOP or only BOTTOM) */
  CTUI_LOOK_BOX_WARNING,
  CTUI_LOOK_BOX_ERROR,
  CTUI_LOOK_BOXES,
} CTUI_LOOK_BOX_STYLE;

/* a CTUI_LOOK_CTL_BOX's place */
enum {
  CTUI_LOOK_BOX_TOP = 1,      /* the frame's first row: its top edge here */
  CTUI_LOOK_BOX_BOTTOM = 2,   /* its last row (both: a one-row frame) */
  CTUI_LOOK_BOX_FLUSH = 4,    /* top / bottom edges along the cells' outer
                               * side; else through the middle of the first /
                               * last row (where box drawing runs) */
  CTUI_LOOK_BOX_THICK = 8,    /* 2 px edges (else 1) */
  CTUI_LOOK_BOX_FLUSH_X = 16, /* the same for the left / right edges and the
                               * first / last column */
};

/* a CTUI_LOOK_CTL_BOX's place: FLUSH for the top edge alone, the bottom
 * one through the middle (a one-row frame; more rows say it per row) */
#define CTUI_LOOK_BOX_FLUSH_TOP 32
/* FLUSH_X for one side alone (a box bordered on the other only: its
 * background to the cells' outer side where there's no border) */
#define CTUI_LOOK_BOX_FLUSH_LEFT 64
#define CTUI_LOOK_BOX_FLUSH_RIGHT 128

/* row r (of rows) of a framed box drawn as one CTUI_LOOK_CTL_BOX per row
 * (the middle rows one picture): box a CTUI_LOOK_BOX_STYLE over cols
 * cells, all four sides; the top row's edge left open over gap_cols
 * cells from gap (a group box's heading, a dialog's title; gap_cols 0:
 * none). OUTSET (a dialog's frame) is 2 px; the others sit flush with the
 * cells' left / right and the bottom row's bottom, so a box filling a
 * pane reaches its edges. */
CTUI_LOOK_CONTROL ctui_look_box_row(int box, int cols, int r, int rows, int gap,
                                    int gap_cols);

/* rewrites c's numbers to the pixels they land on in a w x h box (a
 * slider's value = its thumb's x, max = the thumb's travel, ...), so
 * every state that looks the same is the same; painting the snapped
 * control gives the same pixels. Out-of-range numbers are clamped. */
void ctui_look_control_snap(CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l, int w,
                            int h);

/* the cache key of c drawn w x h in l: equal for any two states that
 * snap alike */
uint64_t ctui_look_control_key(const CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l,
                               int w, int h);

/* the sample of GRAPH c drawn w x h px whose point is nearest px column
 * x (from the box's left): its index in c->samples, or -1 on the frame,
 * left of the oldest, or too small to draw (what a pointer over the
 * graph is over) */
int ctui_look_graph_at(const CTUI_LOOK_CONTROL *c, int w, int h, int x);

/* paints c into rgba (w x h, cleared first: what the control doesn't
 * cover stays transparent, or the face with the look's panel) */
void ctui_look_control_paint(const CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l,
                             unsigned char *rgba, int w, int h);

#endif
