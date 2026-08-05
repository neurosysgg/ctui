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
