#ifndef PLAYER_OUTPUTS_ALSA_H
#define PLAYER_OUTPUTS_ALSA_H

/* every .c file that includes this header must `#define _GNU_SOURCE` as
 * its own first line, before any #include (same convention as the
 * `_POSIX_C_SOURCE` defines in core/term.c, core/input.c, core/timer.c,
 * and file_browser's main.c) -- alsa/asoundlib.h's own struct timespec
 * fallback collides with glibc's under plain -std=c11 otherwise, and the
 * fix only takes effect if no earlier include in the TU has already
 * pulled in <time.h> without it. */

#include "../audio/output.h"

#include <alsa/asoundlib.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  snd_pcm_t *pcm;
  int channels;

  int16_t *scratch; /* realloc-grown float->S16_LE staging buffer, same
                     * grow-once-reuse pattern as WAV_DECODER's scratch */
  size_t scratch_cap;
} ALSA_OUTPUT;

/* opens the default ALSA PCM device, S16_LE / SND_PCM_NONBLOCK -- write()
 * never blocks, so a full device buffer just accepts fewer frames than
 * offered, matching CTUI_AUDIO_OUTPUT's contract. */
CTUI_AUDIO_OUTPUT alsa_output_make(ALSA_OUTPUT *state);

#endif
