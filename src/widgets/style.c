#include "style.h"

const CTUI_STYLE ctui_style_default = {.fg = CTUI_COLOR_DEFAULT,
                                       .bg = CTUI_COLOR_DEFAULT,
                                       .dim_fg = CTUI_COLOR_BLUE,
                                       .title_fg = CTUI_COLOR_CYAN,
                                       .sel_fg = CTUI_COLOR_BLACK,
                                       .sel_bg = CTUI_COLOR_CYAN,
                                       .mark_fg = CTUI_COLOR_YELLOW,
                                       .error_fg = CTUI_COLOR_RED};

/* a slot's basic colour */
static unsigned char basic(const CTUI_STYLE *st, CTUI_STYLE_SLOT slot,
                           unsigned char given) {
  switch (slot) {
  case CTUI_STYLE_FG:
    return st->fg;
  case CTUI_STYLE_BG:
    return st->bg;
  case CTUI_STYLE_DIM:
    return st->dim_fg;
  case CTUI_STYLE_TITLE:
    return st->title_fg;
  case CTUI_STYLE_SEL_FG:
    return st->sel_fg;
  case CTUI_STYLE_SEL_BG:
    return st->sel_bg;
  case CTUI_STYLE_MARK:
    return st->mark_fg;
  case CTUI_STYLE_ERROR:
    return st->error_fg;
  default:
    return given;
  }
}

/* a slot's 24-bit colour: 0 without one */
static uint32_t rgb(const CTUI_STYLE *st, CTUI_STYLE_SLOT slot) {
  return slot >= 0 && slot < CTUI_STYLE_SLOTS ? st->rgb[slot] & 0x1ffffffu : 0;
}

CTUI_CELL ctui_style_cell(const CTUI_STYLE *st, CTUI_STYLE_SLOT fg, CTUI_STYLE_SLOT bg,
                          unsigned char fg_basic, unsigned char bg_basic) {
  CTUI_CELL c = {.fg = basic(st, fg, fg_basic), .bg = basic(st, bg, bg_basic),
                 .color_mode = CTUI_COLOR_MODE_BASIC};
  uint32_t f = rgb(st, fg), b = rgb(st, bg);
  if (b & CTUI_STYLE_RGB) {
    c.color_mode = CTUI_COLOR_MODE_RGB;
    c.bg_r = (unsigned char)(b >> 16);
    c.bg_g = (unsigned char)(b >> 8);
    c.bg_b = (unsigned char)b;
    if (!(f & CTUI_STYLE_RGB)) {
      ctui_gfx_ansi16_rgb(c.fg, &c.fg_r, &c.fg_g, &c.fg_b);
      return c;
    }
  } else if (f & CTUI_STYLE_RGB) {
    c.color_mode = CTUI_COLOR_MODE_RGB_FG;
  }
  if (f & CTUI_STYLE_RGB) {
    c.fg_r = (unsigned char)(f >> 16);
    c.fg_g = (unsigned char)(f >> 8);
    c.fg_b = (unsigned char)f;
  }
  return c;
}
