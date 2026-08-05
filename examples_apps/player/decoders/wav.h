#ifndef PLAYER_DECODERS_WAV_H
#define PLAYER_DECODERS_WAV_H

#include "../audio/decoder.h"

#include <stdio.h>

/* canonical PCM only (fmt tag 1, 8/16/24/32-bit integer) -- IEEE float
 * (tag 3) and WAVE_FORMAT_EXTENSIBLE are rejected via open() returning -1,
 * not fatal. Caller owns a zeroed WAV_DECODER and passes it to
 * wav_decoder_make(); the CTUI_DECODER handed back is only valid as long
 * as that WAV_DECODER outlives it. */
typedef struct {
  FILE *f;
  long data_start;
  long data_remaining; /* bytes left in the "data" chunk still to decode */
  int channels;
  int bits_per_sample;
  int bytes_per_sample;

  unsigned char *scratch; /* realloc-grown raw-byte staging buffer for
                           * decode(); grows once to the caller's chunk
                           * size and is reused every tick after that,
                           * instead of malloc/free per call. */
  size_t scratch_cap;
} WAV_DECODER;

CTUI_DECODER wav_decoder_make(WAV_DECODER *state);

#endif
