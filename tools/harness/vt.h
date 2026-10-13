#ifndef CTUI_VT_H
#define CTUI_VT_H

/* The terminal side of a pty, as far as the screens tests need it: a byte
 * stream a ctui app wrote, replayed into a grid of CTUI_CELLs. Fed in
 * chunks split anywhere (a sequence or a UTF-8 character cut between two
 * reads is finished by the next feed). Understood: printable text placed
 * cluster by cluster at ctui's widths (core/utf8.h: kitty's), CR LF BS
 * TAB, cursor moves (CUP/HVP, CUU/CUD/CUF/CUB, CHA, VPA), ED and EL, SGR
 * into the cells' colours and attributes, the alternate screen (entering
 * it clears). Swallowed: every other CSI, OSC/APC/DCS/PM/SOS strings
 * (kitty graphics, titles), ESC + intermediates + final (charsets, DECSC,
 * ...). No scrolling regions, no insert/delete: ctui's renderer never
 * sends them. Not part of any app: tools/harness/ links it into the
 * tests' pty harness (harness.h). */

#include "ctui.h"

#include <stddef.h>

typedef struct CTUI_VT CTUI_VT;

/* a grid of rows x cols blank cells, the cursor at the top left */
CTUI_VT *ctui_vt_new(int rows, int cols);
void ctui_vt_free(CTUI_VT *vt);

/* replays n bytes of terminal output */
void ctui_vt_feed(CTUI_VT *vt, const char *buf, size_t n);

/* a new size, the way a terminal takes a window resize: what fits stays,
 * the rest is cut or filled blank, the cursor clamped */
void ctui_vt_resize(CTUI_VT *vt, int rows, int cols);

int ctui_vt_rows(const CTUI_VT *vt);
int ctui_vt_cols(const CTUI_VT *vt);

/* the cell at (row, col), NULL outside the grid. Text not yet followed by
 * anything else is placed first (a cluster can still be growing at the
 * end of a feed). */
const CTUI_CELL *ctui_vt_cell(CTUI_VT *vt, int row, int col);

/* the grid as UTF-8 text: rows joined by \n, each without its trailing
 * blanks, a wide glyph's right half nothing (as tools/pty_harness.py
 * prints it). Malloc'd, the caller frees; NULL out of memory. */
char *ctui_vt_text(CTUI_VT *vt);

/* the grid's colours and attributes beside ctui_vt_text(): a line per
 * row that isn't all plain, "ROW: COL STYLE COL STYLE ..." with a column
 * wherever the style changes (from plain at 0). STYLE is FG/BG then
 * "+BDIUS" for bold, dim, italic, underline, strike: a colour is d
 * (default), k r g y b m c w (basic), @N (256) or #rrggbb. A blank cell's
 * fg and its B/D/I can't be seen and are left out. Malloc'd ("" when all
 * is plain), the caller frees; NULL out of memory. */
char *ctui_vt_styles(CTUI_VT *vt);

/* the first row whose text holds needle, -1 if none */
int ctui_vt_find(CTUI_VT *vt, const char *needle);

/* answer a kitty shm probe (a=q,...,t=s) with "OK", as kitty does: an
 * app built on ctui then takes the shm transport (pty_harness.py's
 * --emulate-kitty-shm). Off by default. */
void ctui_vt_answer_kitty_shm(CTUI_VT *vt, int on);

/* what the terminal has to say back since the last call (replies to
 * queries), *n its length; NULL when nothing. Valid until the next
 * feed. */
const char *ctui_vt_replies(CTUI_VT *vt, size_t *n);

#endif
