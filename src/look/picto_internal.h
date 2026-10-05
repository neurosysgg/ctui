#ifndef CTUI_LOOK_PICTO_INTERNAL_H
#define CTUI_LOOK_PICTO_INTERNAL_H

#include "picto.h"

/* picto.c's art at the larger grid (picto24.c): CTUI_LOOK_PICTO_GRID_BIG
 * rows of as many role letters, the same templates ('1'-'3', '*'). NULL:
 * that pictogram has none and stays on the 16 px one. */
extern const char *const ctui_look_picto_big[CTUI_LOOK_PICTOS];
extern const char ctui_look_picto_big_cross[];      /* over a muted speaker */
extern const char ctui_look_picto_big_slash[];      /* through a muted mic */
extern const char ctui_look_picto_big_key_broken[]; /* an http page's */

#endif
