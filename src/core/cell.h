#ifndef CTUI_CELL_H
#define CTUI_CELL_H

typedef struct {
  char ch;
  unsigned char fg;
  unsigned char bg;
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

#endif
