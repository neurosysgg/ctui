#ifndef CTUI_LOOKCONF_H
#define CTUI_LOOKCONF_H

#include "../json/json.h"
#include "look.h"

#include <stddef.h>

/* The top-level "look" from the config:
 *
 *   "look": "win95"
 *   "look": {"base": "win95", "colors": {"accent": "#000080"}, "bevel": 1}
 *
 * a built-in (look.h) with any field overridden: "colors" by role name,
 * "bevel", "groove" (0-2), "thumb", "chunk", "gap", "corner" (px),
 * "outline", "panel" (booleans), "filter" (the app icons' period
 * filter, filter.h: false, true, or {"size", "palette", "dither",
 * "alpha", "boost", "outline", "shadow"} over the base's). Missing, null
 * or false = off: the widgets draw their glyphs as text. */

/* v into *out: 1 with a look, 0 for none (out untouched), -1 with why in
 * err */
int ctui_look_parse(const CTUI_JSON *v, CTUI_LOOK *out, char *err,
                    size_t err_cap);

#endif
