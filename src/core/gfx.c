#include "gfx.h"

#include "cell.h"
#include "deflate.h"
#include "log.h"
#include "profile.h"
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

/* xterm's standard 16-color palette, normal-intensity half (indices 0-7,
 * matching CTUI_COLOR_BLACK..WHITE) -- see the doc comment on
 * ctui_gfx_ansi16_rgb() in gfx.h for what this table is/isn't claiming. */
static const unsigned char ctui_ansi16_rgb_table[9][3] = {
    [CTUI_COLOR_DEFAULT] = {0, 0, 0},     [CTUI_COLOR_BLACK] = {0, 0, 0},
    [CTUI_COLOR_RED] = {205, 0, 0},       [CTUI_COLOR_GREEN] = {0, 205, 0},
    [CTUI_COLOR_YELLOW] = {205, 205, 0},  [CTUI_COLOR_BLUE] = {0, 0, 238},
    [CTUI_COLOR_MAGENTA] = {205, 0, 205}, [CTUI_COLOR_CYAN] = {0, 205, 205},
    [CTUI_COLOR_WHITE] = {229, 229, 229},
};

void ctui_gfx_ansi16_rgb(unsigned char color, unsigned char *r,
                         unsigned char *g, unsigned char *b) {
  const unsigned char *rgb =
      ctui_ansi16_rgb_table[color <= CTUI_COLOR_WHITE ? color : 0];
  *r = rgb[0];
  *g = rgb[1];
  *b = rgb[2];
}

/* kitty splits the *encoded* payload into chunks, not the raw pixel data
 * -- concatenation happens terminal-side after base64-decoding every
 * chunk in sequence, so any byte offset into the encoded string is a
 * valid split point. 4096 matches the protocol spec's own chunk-size
 * guidance. */
#define CTUI_KITTY_CHUNK 4096

/* Phase 5a: every escape fragment ctui_gfx_kitty_display()/
 * ctui_gfx_kitty_delete() used to write() individually (the CUP escape,
 * every chunk's key-list prefix, every chunk's base64 body, every chunk's
 * terminator) now appends here instead -- one real write() per frame,
 * issued by ctui_gfx_kitty_flush(), instead of dozens. Same growable,
 * high-water-mark, never-shrinks shape as core/widget.c's gfx_pending
 * array: capacity only ever grows, len resets to 0 once flushed. */
static char *g_kitty_batch = NULL;
static size_t g_kitty_batch_len = 0, g_kitty_batch_cap = 0;

static void kitty_batch_append(const void *data, size_t len) {
  if (g_kitty_batch_len + len > g_kitty_batch_cap) {
    g_kitty_batch_cap = g_kitty_batch_cap ? g_kitty_batch_cap * 2 : 8192;
    while (g_kitty_batch_len + len > g_kitty_batch_cap) {
      g_kitty_batch_cap *= 2;
    }
    g_kitty_batch = realloc(g_kitty_batch, g_kitty_batch_cap);
  }
  memcpy(g_kitty_batch + g_kitty_batch_len, data, len);
  g_kitty_batch_len += len;
}

void ctui_gfx_kitty_flush(void) {
  CTUI_PROFILE_SPAN sp = ctui_profile_begin();
  if (g_kitty_batch_len > 0) {
    write(STDOUT_FILENO, g_kitty_batch, g_kitty_batch_len);
  }
  ctui_profile_end(sp, "gfx.kitty_batch_flush");
  g_kitty_batch_len = 0;
}

/* Phase 5b: our own encoder's correctness is the actual risk here (no
 * env var signals a real Kitty terminal's o=z support is broken, and o=z
 * is spec-mandated), not terminal compatibility -- so this is a plain
 * runtime toggle, not a CTUI_GFX_MODE bit. Default on. */
static int g_kitty_compression_enabled = 1;

void ctui_gfx_kitty_set_compression(int enabled) {
  g_kitty_compression_enabled = enabled ? 1 : 0;
  ctui_logf(E_INF,
            "[CTUI:GFX] - kitty compression %s @ tick %d\n",
            g_kitty_compression_enabled ? "enabled" : "disabled",
            ctui_tick_advance());
}

void ctui_gfx_kitty_display(int row, int col, int cell_cols, int cell_rows,
                            const unsigned char *rgba, int width, int height,
                            unsigned int image_id, int z) {
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

  /* Phase 5b: always attempt compression and compare -- never worse than
   * today, since a losing attempt (compressed_len >= raw_len, plausible
   * for small images like loudness_meter.c's baked-in text row given
   * DEFLATE's own block overhead) just falls through to the raw path
   * below unchanged. */
  unsigned char *compressed = NULL;
  const unsigned char *payload = rgba;
  size_t payload_len = raw_len;
  int use_compression = 0;
  if (g_kitty_compression_enabled) {
    CTUI_PROFILE_SPAN compress_span = ctui_profile_begin();
    size_t compressed_len = 0;
    compressed = ctui_deflate_compress(rgba, raw_len, &compressed_len);
    ctui_profile_end(compress_span, "gfx.kitty_compress");
    if (compressed != NULL && compressed_len < raw_len) {
      payload = compressed;
      payload_len = compressed_len;
      use_compression = 1;
    }
  }

  size_t b64_cap = ctui_util_base64_len(payload_len) + 1;
  char *b64 = malloc(b64_cap);
  if (b64 == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_display rejected @ tick %d, malloc(%zu) "
              "failed\n",
              ctui_tick_advance(), b64_cap);
    free(compressed);
    return;
  }
  /* split so a profile run can tell CPU-bound base64 encoding (scales with
   * payload_len) apart from the batch-append loop below (scales with
   * b64_len but also reflects memcpy cost into the batch buffer, not a
   * blocking syscall anymore -- see gfx.kitty_batch_flush for the span
   * that now maps to what gfx.kitty_write used to measure). */
  CTUI_PROFILE_SPAN encode_span = ctui_profile_begin();
  size_t b64_len = ctui_util_base64_encode(payload, payload_len, b64, b64_cap);
  ctui_profile_end(encode_span, "gfx.kitty_encode");

  char out[128];
  int n = snprintf(out, sizeof out, "\x1b[%d;%dH", row, col);
  kitty_batch_append(out, (size_t)n);

  CTUI_PROFILE_SPAN append_span = ctui_profile_begin();
  size_t sent = 0;
  while (sent < b64_len) {
    size_t chunk = b64_len - sent;
    if (chunk > CTUI_KITTY_CHUNK) {
      chunk = CTUI_KITTY_CHUNK;
    }
    int more = (sent + chunk) < b64_len;
    if (sent == 0) {
      /* a=T: transmit + display immediately. f=32: raw RGBA pixels (as
       * opposed to f=100, PNG-encoded). o=z: payload is zlib-wrapped
       * DEFLATE instead of raw bytes (only when use_compression actually
       * won the size comparison above) -- orthogonal to f=32, which still
       * describes the pixel format the far end reconstructs after
       * decompressing. s/v: pixel dimensions, required for raw formats
       * since there's no container header to read them from. c/r: scale
       * the image to cover this many character cells. q=2: suppress both
       * success and error responses -- ctui has no code path that reads
       * stdin for an APC reply. C=1: don't move the cursor after
       * displaying -- the protocol default (C=0) moves it to just past
       * the image, as if it had been printed as text, which for an image
       * tall/low enough to reach the terminal's last row forces the
       * terminal to scroll the whole screen to keep the cursor visible.
       * ctui always repositions the cursor explicitly (the CUP escape
       * right above, and again before every ctui_screen_flush() write)
       * rather than relying on wherever a previous write left it, so a
       * side-effect cursor move here only ever fights that -- this
       * surfaced as the whole screen drifting upward on every redraw once
       * a Kitty image (ctui-mus's footer border) first reached the last
       * row. */
      if (use_compression) {
        n = snprintf(
            out, sizeof out,
            "\x1b_Ga=T,f=32,o=z,s=%d,v=%d,c=%d,r=%d,i=%u,z=%d,q=2,C=1,m=%d;",
            width, height, cell_cols, cell_rows, image_id, z, more);
      } else {
        n = snprintf(
            out, sizeof out,
            "\x1b_Ga=T,f=32,s=%d,v=%d,c=%d,r=%d,i=%u,z=%d,q=2,C=1,m=%d;",
            width, height, cell_cols, cell_rows, image_id, z, more);
      }
    } else {
      /* continuation chunks repeat only m -- every other key was already
       * established by the first chunk */
      n = snprintf(out, sizeof out, "\x1b_Gm=%d;", more);
    }
    kitty_batch_append(out, (size_t)n);
    kitty_batch_append(b64 + sent, chunk);
    kitty_batch_append("\x1b\\", 2);
    sent += chunk;
  }
  ctui_profile_end(append_span, "gfx.kitty_batch_append");

  free(b64);
  free(compressed);
  ctui_logf(E_INF,
            "[CTUI:GFX] - kitty_display @ tick %d (%dx%d px @ row=%d, "
            "col=%d, id=%u, z=%d, %zu b64 bytes, raw=%zu, payload=%zu, "
            "compressed=%s)\n",
            ctui_tick_advance(), width, height, row, col, image_id, z,
            b64_len, raw_len, payload_len, use_compression ? "yes" : "no");
}

void ctui_gfx_kitty_delete(unsigned int image_id) {
  if (!isatty(STDOUT_FILENO)) {
    ctui_logf(E_WRN,
              "[CTUI:GFX] - kitty_delete rejected @ tick %d, stdout isn't a "
              "real terminal\n",
              ctui_tick_advance());
    return;
  }

  char out[32];
  int n = snprintf(out, sizeof out, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", image_id);
  kitty_batch_append(out, (size_t)n);

  ctui_logf(E_INF, "[CTUI:GFX] - kitty_delete @ tick %d (id=%u)\n",
            ctui_tick_advance(), image_id);
}
