#ifndef CTUI_LOOK_FILTER_H
#define CTUI_LOOK_FILTER_H

#include "look.h"

#include <stdint.h>

/* The period filter (gui_overhaul.md round 2): a modern full-colour app
 * icon made to sit with the drawn controls -- scaled down to 16 or 32 px
 * (area average over premultiplied colour), contrast and saturation
 * lifted, the alpha cut to a 1-bit mask, the colours put on a period
 * palette (dithered or not), optionally a dark outline and a drop shadow.
 * Every stage is a look parameter (look.h CTUI_LOOK_FILTER) and can be
 * skipped. Pure: RGBA in, RGBA out. ctui-wm runs it once per icon of a
 * theme; what shows it only scales the result up by whole factors. */

/* the names in config order, NULL-terminated */
extern const char *const ctui_look_filter_palette_names[];
extern const char *const ctui_look_filter_dither_names[];

/* src (sw x sh RGBA, straight alpha) through l's filter into out (size x
 * size RGBA, size = l->filter.size), the icon centred with its aspect */
void ctui_look_filter_icon(const unsigned char *src, int sw, int sh,
                           const CTUI_LOOK *l, unsigned char *out);

/* a web page's picture (src, sw x sh RGBA) the way a 90s browser on a
 * 256-colour screen showed it: scaled to w x h into out (by area down,
 * nearest neighbour up), its alpha cut to a 1-bit mask, its colours on
 * the 216 web colours (and 95's 20) with Floyd-Steinberg dithering */
void ctui_look_filter_web(const unsigned char *src, int sw, int sh,
                          unsigned char *out, int w, int h);

/* a hash of everything the filter's output depends on (its settings and
 * the colours it uses): part of a filtered icon's file name */
uint64_t ctui_look_filter_hash(const CTUI_LOOK *l);

/* src scaled by the largest whole factor that fits w x h (nearest
 * neighbour) and centred in it, into out (w x h RGBA, cleared); 0, or -1
 * when src doesn't fit at 1x (out untouched) */
int ctui_look_filter_upscale(const unsigned char *src, int sw, int sh,
                             unsigned char *out, int w, int h);

#endif
