/* must precede every #include -- see the identical comment in alsa.h */
#define _GNU_SOURCE

#include "alsa.h"

#include <stdlib.h>

static int alsa_open(void *vstate, CTUI_AUDIO_FORMAT *fmt) {
  ALSA_OUTPUT *state = vstate;

  snd_pcm_t *pcm;
  if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK,
                   SND_PCM_NONBLOCK) < 0) {
    return -1;
  }

  /* 100ms latency: generous enough to tolerate a slow/preempted 20ms timer
   * tick without underrunning, small enough not to add noticeable lag */
  if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned)fmt->channels,
                         (unsigned)fmt->sample_rate, 1, 100000) < 0) {
    snd_pcm_close(pcm);
    return -1;
  }

  state->pcm = pcm;
  state->channels = fmt->channels;
  return 0;
}

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static size_t alsa_write(void *vstate, const float *frames, size_t n) {
  ALSA_OUTPUT *state = vstate;
  size_t need = n * (size_t)state->channels;

  if (need > state->scratch_cap) {
    int16_t *grown = realloc(state->scratch, need * sizeof(int16_t));
    if (!grown) {
      return 0;
    }
    state->scratch = grown;
    state->scratch_cap = need;
  }

  for (size_t i = 0; i < need; i++) {
    state->scratch[i] = (int16_t)(clampf(frames[i], -1.0f, 1.0f) * 32767.0f);
  }

  snd_pcm_sframes_t written =
      snd_pcm_writei(state->pcm, state->scratch, n);
  if (written == -EAGAIN) {
    return 0; /* device buffer full right now -- not an error */
  }
  if (written == -EPIPE) {
    snd_pcm_prepare(state->pcm); /* underrun; recover for the next tick */
    return 0;
  }
  if (written < 0) {
    return 0;
  }
  return (size_t)written;
}

static void alsa_close(void *vstate) {
  ALSA_OUTPUT *state = vstate;
  if (state->pcm) {
    snd_pcm_drain(state->pcm);
    snd_pcm_close(state->pcm);
    state->pcm = NULL;
  }
  free(state->scratch);
  state->scratch = NULL;
  state->scratch_cap = 0;
}

CTUI_AUDIO_OUTPUT alsa_output_make(ALSA_OUTPUT *state) {
  return (CTUI_AUDIO_OUTPUT){
      .state = state, .open = alsa_open, .write = alsa_write, .close = alsa_close};
}
