#ifndef CTUI_LOOK_DRAWN_H
#define CTUI_LOOK_DRAWN_H

#include "../ctui.h"
#include "control.h"

/* Drawn controls (control.h) in a widget: each state painted once at its
 * cells' exact device pixels into a PNG named by its key (DIR/look-KEY.png,
 * DIR ctui_drawn_dir(): a cache several processes can share), then placed
 * and shown like an icon (icon.h: its ids, its LRU). A state that comes
 * back is just its id again. In kitty with a look; else the widgets draw
 * their glyphs as text. */

/* where the PNGs go: dir (made when needed), or NULL / "" for the
 * default, $XDG_RUNTIME_DIR/ctui (/tmp/ctui-UID without one). ctui-wm
 * sets its own runtime dir, shared by its zones. */
void ctui_drawn_set_dir(const char *dir);
const char *ctui_drawn_dir(void);

/* the image id for c over cols x rows cells in l, painting and placing it
 * the first time; 0 when icons are off (text instead) or the cell size is
 * unknown */
unsigned int ctui_drawn_id(const CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l,
                           int cols, int rows);

/* the look controls are drawn in (lookconf.h: a config's "look"), copied;
 * NULL = none: the widgets draw their glyphs as text */
void ctui_drawn_set_look(const CTUI_LOOK *l);
const CTUI_LOOK *ctui_drawn_look(void);
/* whether controls are drawn here: a look, in kitty */
int ctui_drawn_on(void);

/* a pictogram's cells: two columns of a usual cell are about square */
#define CTUI_DRAWN_PICTO_COLS 2
/* an on/off switch's cells, where its "on" / "off" was */
#define CTUI_DRAWN_TOGGLE_COLS 4

/* c over cols cells of row at col, in the look: cols when drawn
 * (a NULL c: always the text);
 * else (no look, no kitty, no room) text as ctui_widget_puts_cut() puts
 * it (up to max_w columns) and its width, or 0 for a NULL text (the
 * caller draws its own). What a widget calls where a glyph was. */
int ctui_drawn_or_text(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                       int col, const CTUI_LOOK_CONTROL *c, int cols,
                       const char *text, int max_w, unsigned char fg,
                       unsigned char bg);

/* the same for pictogram p (value of max its state, off greyed) over
 * CTUI_DRAWN_PICTO_COLS cells, glyph the text */
int ctui_drawn_picto(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                     CTUI_LOOK_PICTO p, int value, int max, int off,
                     const char *glyph, int max_w, unsigned char fg,
                     unsigned char bg);

/* a chip (glyph g, flags CTUI_LOOK_CTL_*) over CTUI_DRAWN_PICTO_COLS
 * cells, else text: a row's ✕ / ⏏ / ↻ / … */
int ctui_drawn_chip(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                    CTUI_LOOK_GLYPH g, unsigned flags, const char *text,
                    int max_w, unsigned char fg, unsigned char bg);

/* a row's end mark (✕ ⏏ ↻ …): text in the last column, in a look a chip
 * over the last CTUI_DRAWN_PICTO_COLS (a hit there covers both) */
void ctui_drawn_end_chip(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                         CTUI_LOOK_GLYPH g, const char *text, unsigned char fg,
                         unsigned char bg);

/* shows it at (row, col) over bg; nothing for id 0 */
void ctui_drawn_put(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                    unsigned int id, int cols, int rows, unsigned char bg);

/* Controls under text: a labelled control (a page's button, its text
 * field) drawn as an image below the text layer -- kitty's own placement
 * (ctui_gfx_kitty_put()) under the cells' backgrounds -- so its label
 * stays text. The cells over it are left on the terminal's background,
 * which shows it; anything painted over them (a dialog) hides it. A
 * placement isn't a cell: what was put last time and isn't this time is
 * taken away, so a widget puts all of its every render, between begin()
 * and end(). */
typedef struct {
  unsigned int id; /* the image's */
  int row, col, cols, rows, z;
} CTUI_DRAWN_PUT;

typedef struct {
  CTUI_DRAWN_PUT *put, *next; /* shown, being put */
  int nput, nnext, cap_put, cap_next;
  unsigned int base; /* its placement ids: base + 1.. */
} CTUI_DRAWN_UNDER;

void ctui_drawn_under_begin(CTUI_DRAWN_UNDER *u);
/* c under cols cells of self's row at col (one row): 1 if it will be, 0
 * when controls aren't drawn here (or it doesn't fit) -- the text alone */
int ctui_drawn_under(CTUI_DRAWN_UNDER *u, const CTUI_WIDGET *self, int row,
                     int col, const CTUI_LOOK_CONTROL *c, int cols);
/* the same over rows rows from row (a toolbar's button: its pictogram
 * drawn in its top part, its label text on the row below) */
int ctui_drawn_under_rows(CTUI_DRAWN_UNDER *u, const CTUI_WIDGET *self, int row,
                          int col, const CTUI_LOOK_CONTROL *c, int cols,
                          int rows);
/* the same at z (CTUI_DRAWN_UNDER_Z and below: a page's frames under
 * its controls, nested ones over those they are in) */
int ctui_drawn_under_z(CTUI_DRAWN_UNDER *u, const CTUI_WIDGET *self, int row,
                       int col, const CTUI_LOOK_CONTROL *c, int cols, int rows,
                       int z);
/* the frame's placements made, the rest taken away */
void ctui_drawn_under_end(CTUI_DRAWN_UNDER *u);
/* every one taken away, u's memory freed (it can begin again) */
void ctui_drawn_under_clear(CTUI_DRAWN_UNDER *u);

/* the z-index they're put at: under every cell with a background */
#define CTUI_DRAWN_UNDER_Z (CTUI_GFX_KITTY_Z_UNDER_BG - 16)

/* a cell's size in device px (the terminal's, or what tests set): 0, or
 * -1 when it isn't known */
int ctui_drawn_cell_px(int *cw, int *ch);

/* a cell's size in device px to paint at instead of asking the terminal
 * (TIOCGWINSZ): tests; 0, 0 asks again */
void ctui_drawn_cell(int cw, int ch);

/* where the PNG of c drawn w x h px goes, into out (cap bytes) */
void ctui_drawn_path(const CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l, int w,
                     int h, char *out, size_t cap);

#endif
