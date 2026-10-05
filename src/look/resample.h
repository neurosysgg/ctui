#ifndef CTUI_LOOK_RESAMPLE_H
#define CTUI_LOOK_RESAMPLE_H

/* A picture made smaller by the area each source pixel covers (a box
 * filter over premultiplied colour), in integers and in two passes --
 * along each row, then down the columns -- with each output column's
 * span and edge weights worked out once: what plugins/img's shrink (a
 * picture kept at most 1600 px) and the look's filter (a picture fitted
 * to its box, an icon) both do (notes/image-perf.md, p2). */

/* src (sw x sh RGBA) into the dw x dh pixels at dst, a row every stride
 * pixels apart (dw <= sw, dh <= sh: only smaller). A pixel nothing opaque
 * covers is left as it is. 0, or -1 without memory (dst untouched) */
int ctui_look_resample_area(const unsigned char *src, int sw, int sh,
                            unsigned char *dst, int stride, int dw, int dh);

#endif
