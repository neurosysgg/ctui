#ifndef CTUI_LOOK_PNG_H
#define CTUI_LOOK_PNG_H

/* w x h RGBA as a PNG file at path (8-bit RGBA, no filtering, ctui's
 * DEFLATE; past its input cap, ~362x362 px, stored blocks uncompressed),
 * written beside it and renamed in so a reader never sees half a file:
 * how drawn.h hands kitty its images. 0 on success. */
int ctui_look_png_write(const char *path, const unsigned char *rgba, int w,
                        int h);

#endif
