#include "util.h"

#include "log.h"
#include "utf8.h"

#include <string.h>

int ctui_util_center_h(char *center_str, char *line, CTUI_CELL fill) {
  size_t str_len = strlen(center_str);
  size_t line_len = strlen(line);
  size_t str_w = (size_t)ctui_utf8_width(center_str);

  if (fill.ch >= 0x80) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - center_h rejected @ tick %d, fill U+%04X isn't "
              "ASCII\n",
              ctui_tick_advance(), fill.ch);
    return -1;
  }
  if (str_w > line_len || str_len > line_len) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - center_h rejected @ tick %d, center_str (%zu "
              "cols, %zu bytes) doesn't fit line (%zu)\n",
              ctui_tick_advance(), str_w, str_len, line_len);
    return -1;
  }

  /* pad by columns, but never write past strlen(line) bytes: multi-byte
   * glyphs take more bytes than columns, so drop padding from the right
   * (then the left) until the result fits the caller's buffer */
  size_t total_pad = line_len - str_w;
  if (str_len + total_pad > line_len) {
    total_pad = line_len - str_len;
  }
  size_t left_pad = total_pad / 2;
  size_t right_pad = total_pad - left_pad;

  memset(line, (int)fill.ch, left_pad);
  memcpy(line + left_pad, center_str, str_len);
  memset(line + left_pad + str_len, (int)fill.ch, right_pad);
  line[left_pad + str_len + right_pad] = '\0';

  ctui_logf(E_DBG,
            "[CTUI:UTIL] - center_h @ tick %d (\"%s\" in %zu-wide line, "
            "left_pad=%zu, right_pad=%zu)\n",
            ctui_tick_advance(), center_str, line_len, left_pad, right_pad);
  return 0;
}

int ctui_util_center_col(const char *str, int width) {
  int w = ctui_utf8_width(str);
  return w >= width ? 0 : (width - w) / 2;
}

int ctui_util_truncate_str(char *str, size_t desired, char *trunc) {
  size_t str_len = strlen(str);
  size_t trunc_len = strlen(trunc);
  size_t str_w = (size_t)ctui_utf8_width(str);
  size_t trunc_w = (size_t)ctui_utf8_width(trunc);

  if (str_w <= desired) {
    return 0;
  }

  if (trunc_w > desired) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - truncate_str rejected @ tick %d, trunc (%zu "
              "cols) wider than desired (%zu cols)\n",
              ctui_tick_advance(), trunc_w, desired);
    return -1;
  }

  size_t keep = ctui_utf8_prefix(str, (int)(desired - trunc_w), NULL);
  /* result must fit in str's existing bytes -- only possible to violate
   * with a trunc whose byte length outgrows the tail it replaces */
  if (keep + trunc_len > str_len) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - truncate_str rejected @ tick %d, trunc (%zu "
              "bytes) doesn't fit in the %zu bytes it would replace\n",
              ctui_tick_advance(), trunc_len, str_len - keep);
    return -1;
  }
  memcpy(str + keep, trunc, trunc_len);
  str[keep + trunc_len] = '\0';

  ctui_logf(E_DBG,
            "[CTUI:UTIL] - truncate_str @ tick %d (%zu cols -> %zu cols)\n",
            ctui_tick_advance(), str_w, desired);
  return 0;
}

int ctui_util_rescale_i(int value, int in_min, int in_max, int out_min,
                        int out_max) {
  if (in_min == in_max) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - rescale_i rejected @ tick %d, in_min == in_max "
              "(%d)\n",
              ctui_tick_advance(), in_min);
    return out_min;
  }

  if (value < in_min) {
    value = in_min;
  } else if (value > in_max) {
    value = in_max;
  }

  return out_min + (value - in_min) * (out_max - out_min) / (in_max - in_min);
}

CTUI_MARGIN ctui_margin_uniform(int n) {
  return (CTUI_MARGIN){.top = n, .right = n, .bottom = n, .left = n};
}

int ctui_util_inset(CTUI_WIDGET *content, CTUI_WIDGET *outer,
                    CTUI_MARGIN margin) {
  int w = outer->w - margin.left - margin.right;
  int h = outer->h - margin.top - margin.bottom;

  if (w <= 0 || h <= 0) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - inset rejected @ tick %d, margin "
              "(top=%d,right=%d,bottom=%d,left=%d) leaves no room in "
              "outer's %dx%d box\n",
              ctui_tick_advance(), margin.top, margin.right, margin.bottom,
              margin.left, outer->w, outer->h);
    return -1;
  }

  content->x = outer->x + margin.left;
  content->y = outer->y + margin.top;
  content->w = w;
  content->h = h;

  ctui_logf(E_DBG,
            "[CTUI:UTIL] - inset @ tick %d (outer %dx%d @ %d,%d -> content "
            "%dx%d @ %d,%d)\n",
            ctui_tick_advance(), outer->w, outer->h, outer->x, outer->y, w,
            h, content->x, content->y);
  return 0;
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ctui_util_base64_len(size_t len) { return ((len + 2) / 3) * 4; }

size_t ctui_util_base64_encode(const unsigned char *src, size_t len,
                               char *dst, size_t dst_cap) {
  size_t need = ctui_util_base64_len(len);
  if (dst_cap < need + 1) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - base64_encode rejected @ tick %d, dst_cap %zu "
              "too small for %zu input bytes (need %zu)\n",
              ctui_tick_advance(), dst_cap, len, need + 1);
    return 0;
  }

  size_t di = 0, i;
  for (i = 0; i + 3 <= len; i += 3) {
    unsigned int n = ((unsigned int)src[i] << 16) |
                     ((unsigned int)src[i + 1] << 8) | src[i + 2];
    dst[di++] = base64_alphabet[(n >> 18) & 0x3f];
    dst[di++] = base64_alphabet[(n >> 12) & 0x3f];
    dst[di++] = base64_alphabet[(n >> 6) & 0x3f];
    dst[di++] = base64_alphabet[n & 0x3f];
  }
  size_t rem = len - i;
  if (rem == 1) {
    unsigned int n = (unsigned int)src[i] << 16;
    dst[di++] = base64_alphabet[(n >> 18) & 0x3f];
    dst[di++] = base64_alphabet[(n >> 12) & 0x3f];
    dst[di++] = '=';
    dst[di++] = '=';
  } else if (rem == 2) {
    unsigned int n =
        ((unsigned int)src[i] << 16) | ((unsigned int)src[i + 1] << 8);
    dst[di++] = base64_alphabet[(n >> 18) & 0x3f];
    dst[di++] = base64_alphabet[(n >> 12) & 0x3f];
    dst[di++] = base64_alphabet[(n >> 6) & 0x3f];
    dst[di++] = '=';
  }
  dst[di] = '\0';

  ctui_logf(E_DBG,
            "[CTUI:UTIL] - base64_encode @ tick %d (%zu bytes -> %zu "
            "chars)\n",
            ctui_tick_advance(), len, di);
  return di;
}
