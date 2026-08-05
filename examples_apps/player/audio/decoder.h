#ifndef PLAYER_AUDIO_DECODER_H
#define PLAYER_AUDIO_DECODER_H

#include "format.h"

#include <stddef.h>

/* file -> normalized frames + metadata. One implementation per format (see
 * decoders/wav.c); mirrors CTUI_WIDGET's render/layout function-pointer
 * dispatch rather than introducing new machinery. */
typedef struct {
  void *state;

  /* opens path, fills fmt/info. Returns 0 on success, -1 on failure
   * (malformed/unsupported file) -- logged by the implementation, not
   * fatal. */
  int (*open)(void *state, const char *path, CTUI_AUDIO_FORMAT *fmt,
             CTUI_TRACK_INFO *info);

  /* decodes up to out_frames frames (fmt.channels floats each, interleaved)
   * into out. Returns frames actually decoded; 0 means EOF. */
  size_t (*decode)(void *state, float *out, size_t out_frames);

  void (*close)(void *state);
} CTUI_DECODER;

#endif
