#ifndef CTUI_CELL_H
#define CTUI_CELL_H

#include <stdint.h>

/* marks the right half of a width-2 glyph: the cell immediately left of
 * it holds the glyph itself. Never a valid codepoint (> U+10FFFF), and
 * outside 0xFFFFFF80-0xFFFFFFFF too, which is where a sign-extended
 * (char) byte passed to a putc lands -- so it can't collide with content. ctui_screen_flush() emits nothing for
 * it, since the terminal already advanced past it drawing the lead cell;
 * core/utf8.c's ctui_cell_set_ch() keeps lead and CONT paired. */
#define CTUI_CELL_CONT ((uint32_t)0x80000000u)

typedef struct {
  uint32_t ch; /* one Unicode codepoint, or CTUI_CELL_CONT */
  unsigned char fg;
  unsigned char bg;

  /* how fg/bg (and fg_r../bg_r..) should be interpreted -- one of
   * CTUI_COLOR_MODE_*. BASIC (0) is the default via plain zero-fill, so
   * every existing (CTUI_CELL){.fg = CTUI_COLOR_YELLOW, ...} literal keeps
   * meaning exactly what it always has. */
  unsigned char color_mode;
  unsigned char fg_r, fg_g, fg_b; /* used only when color_mode == RGB(_FG) */
  unsigned char bg_r, bg_g, bg_b;

  /* a CTUI_GFX_KITTY_PLACEHOLDER cell's image row + 1, sent as kitty's row
   * diacritic (the column follows from the placeholder to its left); 0 =
   * no diacritic, i.e. row 0 */
  unsigned char kitty_row;
} CTUI_CELL;

/* basic ANSI colors */
enum {
  CTUI_COLOR_DEFAULT = 0,
  CTUI_COLOR_BLACK,
  CTUI_COLOR_RED,
  CTUI_COLOR_GREEN,
  CTUI_COLOR_YELLOW,
  CTUI_COLOR_BLUE,
  CTUI_COLOR_MAGENTA,
  CTUI_COLOR_CYAN,
  CTUI_COLOR_WHITE,
};

/* how a cell's fg/bg should be read; independent of CTUI_GFX_MODE
 * (core/gfx.h), which describes what the terminal session negotiated, not
 * how any one cell is encoded */
enum {
  CTUI_COLOR_MODE_BASIC = 0, /* fg/bg are CTUI_COLOR_* indices, as above */
  CTUI_COLOR_MODE_256,       /* fg/bg are a 0-255 ANSI 256-color index */
  CTUI_COLOR_MODE_RGB,       /* fg_r/g/b, bg_r/g/b are used instead */
  CTUI_COLOR_MODE_RGB_FG,    /* fg_r/g/b over a basic bg: a truecolor fg
                              * that keeps the widget's own background
                              * (e.g. a Kitty image placeholder, whose fg
                              * *is* the image id -- gfx.h) */
};

#endif
