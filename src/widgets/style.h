#ifndef CTUI_WIDGETS_STYLE_H
#define CTUI_WIDGETS_STYLE_H

#include "../ctui.h"

/* The colours the app widgets (entry, table, dialog, textview, form,
 * tabs) share, so an app themes all of them in one place: each takes a
 * `const CTUI_STYLE *style`, NULL meaning ctui_style_default. Basic
 * CTUI_COLOR_* values. */
typedef struct {
  unsigned char fg, bg;
  unsigned char dim_fg;         /* hints, headers, secondary text */
  unsigned char title_fg;       /* titles, headings */
  unsigned char sel_fg, sel_bg; /* the cursor row, the focused control */
  unsigned char mark_fg;        /* marked rows, a toggle that's on */
  unsigned char error_fg;
} CTUI_STYLE;

extern const CTUI_STYLE ctui_style_default;

/* style, or the default for NULL */
static inline const CTUI_STYLE *ctui_style_of(const CTUI_STYLE *style) {
  return style ? style : &ctui_style_default;
}

#endif
