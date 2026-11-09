#ifndef CTUI_LOOK_PNG_H
#define CTUI_LOOK_PNG_H

#include <stddef.h>

/* w x h RGBA as a PNG file at path (8-bit RGBA, no filtering, ctui's
 * DEFLATE; past its input cap, ~362x362 px, stored blocks uncompressed),
 * written beside it and renamed in so a reader never sees half a file:
 * how drawn.h hands kitty its images. 0 on success. */
int ctui_look_png_write(const char *path, const unsigned char *rgba, int w,
                        int h);

/* the reading side (look/png_read.c, from ctui-wm's icons): every colour
 * type and bit depth, tRNS, Adam7; a bad or truncated file an error, never
 * a read past its end. The largest side and area decoded, bigger ones
 * refused: */
#define CTUI_LOOK_PNG_MAX_SIDE 4096
#define CTUI_LOOK_PNG_MAX_AREA (2048 * 2048)

/* data (len bytes) into a malloc'd w x h RGBA buffer (8 bits a channel,
 * straight alpha) in *rgba; 0, or -1 (nothing allocated) */
int ctui_look_png_decode(const unsigned char *data, size_t len,
                         unsigned char **rgba, int *w, int *h);
/* the same for a file */
int ctui_look_png_load(const char *path, unsigned char **rgba, int *w, int *h);
/* only the size from the header: 0, or -1 if it isn't a PNG */
int ctui_look_png_size(const char *path, int *w, int *h);

#endif
