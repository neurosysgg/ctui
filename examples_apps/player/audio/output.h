#ifndef PLAYER_AUDIO_OUTPUT_H
#define PLAYER_AUDIO_OUTPUT_H

#include "format.h"

#include <stddef.h>

/* normalized frames -> device, or nowhere (outputs/null.c). Independent of
 * decoder internals -- write() never blocks, so a slow/full device just
 * accepts fewer frames than offered rather than stalling the caller's
 * timer tick. */
typedef struct {
  void *state;

  int (*open)(void *state, CTUI_AUDIO_FORMAT *fmt);

  /* offers n frames (fmt.channels floats each, interleaved); returns frames
   * actually accepted, which may be less than n. */
  size_t (*write)(void *state, const float *frames, size_t n);

  void (*close)(void *state);
} CTUI_AUDIO_OUTPUT;

#endif
