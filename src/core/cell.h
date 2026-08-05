#ifndef CTUI_CELL_H
#define CTUI_CELL_H

typedef struct {
  char ch;
  unsigned char fg;
  unsigned char bg;

  /* how fg/bg (and fg_r../bg_r..) should be interpreted -- one of
   * CTUI_COLOR_MODE_*. BASIC (0) is the default via plain zero-fill, so
   * every existing (CTUI_CELL){.fg = CTUI_COLOR_YELLOW, ...} literal keeps
   * meaning exactly what it always has. */
  unsigned char color_mode;
  unsigned char fg_r, fg_g, fg_b; /* used only when color_mode == RGB */
  unsigned char bg_r, bg_g, bg_b;
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
};

#endif
