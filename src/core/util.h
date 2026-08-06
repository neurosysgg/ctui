#ifndef CTUI_UTIL_H
#define CTUI_UTIL_H

#include "cell.h"
#include "widget.h"

#include <stddef.h>

/* string-layout utilities for widget render() callbacks; operate on plain
 * char buffers rather than compositor cells -- callers still push the
 * result through ctui_widget_puts()/ctui_screen_puts() to get it on screen
 * with color. Both return 0 on success, -1 on invalid input (logged via
 * E_WRN). */

/* centers center_str within line in place, using line's current strlen() as
 * the target width (so line is expected to already be padded/allocated to
 * that width by the caller). Pads both sides with fill.ch; fill.fg/fill.bg
 * are accepted for symmetry with other CTUI_CELL-based APIs but unused
 * here, since line is a plain string, not a cell buffer. Fails if
 * center_str is longer than line -- truncate it first with
 * ctui_util_truncate_str() if it might not fit. */
int ctui_util_center_h(char *center_str, char *line, CTUI_CELL fill);

/* truncates str in place to desired chars total, replacing its tail with
 * trunc (e.g. "...", ">>") so the result is exactly desired chars long.
 * No-op if str already fits within desired. Fails if trunc itself is longer
 * than desired. */
int ctui_util_truncate_str(char *str, size_t desired, char *trunc);

/* linearly rescales value from [in_min, in_max] to [out_min, out_max],
 * clamping value to [in_min, in_max] first so out-of-range input can't
 * produce an out-of-range result. Integer math throughout (truncates
 * toward zero, like a normal C division) -- fine at the scale this gets
 * used at (cell/pixel coordinates, 0-255 color channels), not a
 * general-purpose fixed-point rescaler. in_min == in_max returns out_min
 * (would otherwise divide by zero) and logs E_WRN. */
int ctui_util_rescale_i(int value, int in_min, int in_max, int out_min,
                        int out_max);

/* margin on each of the four sides independently -- a border's edge
 * thickness, typically, but nothing here assumes border specifically */
typedef struct {
  int top, right, bottom, left;
} CTUI_MARGIN;

/* same margin on all four sides -- the common case (a uniform-thickness
 * border) */
CTUI_MARGIN ctui_margin_uniform(int n);

/* sets content->x/y/w/h to outer's CURRENT x/y/w/h, inset by margin on
 * each side -- the "outer draws its full box, content gets margin'd
 * inside it" pattern (e.g. a CTUI_BORDER and its separately-positioned
 * inset content, see src/widgets/border.h). Call from content's own
 * layout(), listing outer before content in the widgets[] passed to
 * ctui_app_init()/present in the same CTUI_SPLIT/CTUI_GROUP earlier, so
 * outer's own layout() has already run and outer->x/y/w/h are current --
 * same ordering ctui_split_layout()'s callers already rely on. Fails
 * (logs E_WRN, leaves content->x/y/w/h untouched) if
 * margin.left+margin.right >= outer->w or margin.top+margin.bottom >=
 * outer->h, rather than producing a zero/negative-size widget. */
int ctui_util_inset(CTUI_WIDGET *content, CTUI_WIDGET *outer,
                    CTUI_MARGIN margin);

/* generic byte -> base64 (RFC 4648, '=' padded) encoding -- not
 * ctui-specific, but the Kitty graphics protocol (core/gfx.c) is the
 * first consumer, since its transmission payload has to be base64. */

/* encoded length of a `len`-byte input, excluding the NUL terminator
 * ctui_util_base64_encode() also writes -- size dst as this + 1. */
size_t ctui_util_base64_len(size_t len);

/* encodes src (len bytes) into dst (must be at least
 * ctui_util_base64_len(len) + 1 bytes -- the encoded chars plus a NUL
 * ctui_util_base64_encode() appends for convenience). Returns the
 * encoded length (excluding the NUL), or 0 if dst_cap is too small
 * (logged via E_WRN). */
size_t ctui_util_base64_encode(const unsigned char *src, size_t len,
                               char *dst, size_t dst_cap);

#endif
