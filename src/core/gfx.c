#include "gfx.h"

#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned int ctui_gfx_detect_caps(void) {
  unsigned int caps = CTUI_GFX_ANSI16;

  const char *term = getenv("TERM");
  const char *colorterm = getenv("COLORTERM");
  const char *kitty_window = getenv("KITTY_WINDOW_ID");

  if (term && strstr(term, "256color")) {
    caps |= CTUI_GFX_ANSI256;
  }

  if (colorterm &&
      (strcmp(colorterm, "truecolor") == 0 || strcmp(colorterm, "24bit") == 0)) {
    /* a truecolor terminal is a 256-color terminal too */
    caps |= CTUI_GFX_TRUECOLOR | CTUI_GFX_ANSI256;
  }

  if (kitty_window || (term && strcmp(term, "xterm-kitty") == 0)) {
    /* kitty implies both text tiers below it */
    caps |= CTUI_GFX_KITTY | CTUI_GFX_TRUECOLOR | CTUI_GFX_ANSI256;
  }

  ctui_logf(E_INF,
            "[CTUI:GFX] - detected caps 0x%x @ tick %d (TERM=%s, "
            "COLORTERM=%s, KITTY_WINDOW_ID=%s)\n",
            caps, ctui_tick_advance(), term ? term : "(unset)",
            colorterm ? colorterm : "(unset)",
            kitty_window ? kitty_window : "(unset)");
  return caps;
}

/* kitty splits the *encoded* payload into chunks, not the raw pixel data
 * -- concatenation happens terminal-side after base64-decoding every
 * chunk in sequence, so any byte offset into the encoded string is a
 * valid split point. 4096 matches the protocol spec's own chunk-size
 * guidance. */
#define CTUI_KITTY_CHUNK 4096

void ctui_gfx_kitty_display(int row, int col, int cell_cols, int cell_rows,
                            const unsigned char *rgba, int width, int height,
                            unsigned int image_id) {
  if (!isatty(STDOUT_FILENO)) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_display rejected @ tick %d, stdout isn't "
              "a real terminal\n",
              ctui_tick_advance());
    return;
  }
  if (width <= 0 || height <= 0 || rgba == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_display rejected @ tick %d, degenerate "
              "image (%dx%d, rgba=%p)\n",
              ctui_tick_advance(), width, height, (const void *)rgba);
    return;
  }

  size_t raw_len = (size_t)width * (size_t)height * 4;
  size_t b64_cap = ctui_util_base64_len(raw_len) + 1;
  char *b64 = malloc(b64_cap);
  if (b64 == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_display rejected @ tick %d, malloc(%zu) "
              "failed\n",
              ctui_tick_advance(), b64_cap);
    return;
  }
  size_t b64_len = ctui_util_base64_encode(rgba, raw_len, b64, b64_cap);

  char out[128];
  int n = snprintf(out, sizeof out, "\x1b[%d;%dH", row, col);
  write(STDOUT_FILENO, out, (size_t)n);

  size_t sent = 0;
  while (sent < b64_len) {
    size_t chunk = b64_len - sent;
    if (chunk > CTUI_KITTY_CHUNK) {
      chunk = CTUI_KITTY_CHUNK;
    }
    int more = (sent + chunk) < b64_len;
    if (sent == 0) {
      /* a=T: transmit + display immediately. f=32: raw RGBA pixels (as
       * opposed to f=100, PNG-encoded). s/v: pixel dimensions, required
       * for raw formats since there's no container header to read them
       * from. c/r: scale the image to cover this many character cells.
       * q=2: suppress both success and error responses -- ctui has no
       * code path that reads stdin for an APC reply. */
      n = snprintf(out, sizeof out,
                   "\x1b_Ga=T,f=32,s=%d,v=%d,c=%d,r=%d,i=%u,q=2,m=%d;",
                   width, height, cell_cols, cell_rows, image_id, more);
    } else {
      /* continuation chunks repeat only m -- every other key was already
       * established by the first chunk */
      n = snprintf(out, sizeof out, "\x1b_Gm=%d;", more);
    }
    write(STDOUT_FILENO, out, (size_t)n);
    write(STDOUT_FILENO, b64 + sent, chunk);
    write(STDOUT_FILENO, "\x1b\\", 2);
    sent += chunk;
  }

  free(b64);
  ctui_logf(E_INF,
            "[CTUI:GFX] - kitty_display @ tick %d (%dx%d px @ row=%d, "
            "col=%d, id=%u, %zu b64 bytes)\n",
            ctui_tick_advance(), width, height, row, col, image_id, b64_len);
}
