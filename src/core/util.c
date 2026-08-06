#include "util.h"

#include "log.h"

#include <string.h>

int ctui_util_center_h(char *center_str, char *line, CTUI_CELL fill) {
  size_t str_len = strlen(center_str);
  size_t line_len = strlen(line);

  if (str_len > line_len) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - center_h rejected @ tick %d, center_str (%zu "
              "chars) longer than line (%zu chars)\n",
              ctui_tick_advance(), str_len, line_len);
    return -1;
  }

  size_t total_pad = line_len - str_len;
  size_t left_pad = total_pad / 2;
  size_t right_pad = total_pad - left_pad;

  memset(line, fill.ch, left_pad);
  memcpy(line + left_pad, center_str, str_len);
  memset(line + left_pad + str_len, fill.ch, right_pad);

  ctui_logf(E_DBG,
            "[CTUI:UTIL] - center_h @ tick %d (\"%s\" in %zu-wide line, "
            "left_pad=%zu, right_pad=%zu)\n",
            ctui_tick_advance(), center_str, line_len, left_pad, right_pad);
  return 0;
}

int ctui_util_truncate_str(char *str, size_t desired, char *trunc) {
  size_t str_len = strlen(str);
  size_t trunc_len = strlen(trunc);

  if (str_len <= desired) {
    return 0;
  }

  if (trunc_len > desired) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - truncate_str rejected @ tick %d, trunc (%zu "
              "chars) longer than desired (%zu chars)\n",
              ctui_tick_advance(), trunc_len, desired);
    return -1;
  }

  size_t keep = desired - trunc_len;
  memcpy(str + keep, trunc, trunc_len);
  str[desired] = '\0';

  ctui_logf(E_DBG,
            "[CTUI:UTIL] - truncate_str @ tick %d (%zu chars -> %zu chars)\n",
            ctui_tick_advance(), str_len, desired);
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
