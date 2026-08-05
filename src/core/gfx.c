#include "gfx.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

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
