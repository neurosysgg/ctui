#include "wav.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd_u16le(const unsigned char *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32le(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static float convert_sample(const unsigned char *p, int bytes_per_sample) {
  switch (bytes_per_sample) {
  case 1: /* WAV 8-bit is unsigned, centered at 128 */
    return ((float)p[0] - 128.0f) / 128.0f;
  case 2: {
    int16_t v = (int16_t)rd_u16le(p);
    return (float)v / 32768.0f;
  }
  case 3: {
    int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x800000) {
      v |= (int32_t)0xFF000000; /* sign-extend 24 -> 32 */
    }
    return (float)v / 8388608.0f;
  }
  case 4: {
    int32_t v = (int32_t)rd_u32le(p);
    return (float)v / 2147483648.0f;
  }
  default:
    return 0.0f;
  }
}

static int wav_open(void *vstate, const char *path, CTUI_AUDIO_FORMAT *fmt,
                    CTUI_TRACK_INFO *info) {
  WAV_DECODER *state = vstate;
  state->f = fopen(path, "rb");
  if (!state->f) {
    return -1;
  }

  unsigned char hdr[12];
  if (fread(hdr, 1, 12, state->f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
      memcmp(hdr + 8, "WAVE", 4) != 0) {
    fclose(state->f);
    state->f = NULL;
    return -1;
  }

  int have_fmt = 0;
  int audio_format = 0, channels = 0, bits_per_sample = 0;
  long sample_rate = 0, byte_rate = 0;
  long data_size = 0;

  for (;;) {
    unsigned char chdr[8];
    if (fread(chdr, 1, 8, state->f) != 8) {
      break; /* EOF before a "data" chunk turned up */
    }
    unsigned long csize = rd_u32le(chdr + 4);

    if (memcmp(chdr, "fmt ", 4) == 0) {
      unsigned char fmtbuf[16];
      if (csize < 16 || fread(fmtbuf, 1, 16, state->f) != 16) {
        fclose(state->f);
        state->f = NULL;
        return -1;
      }
      audio_format = rd_u16le(fmtbuf + 0);
      channels = rd_u16le(fmtbuf + 2);
      sample_rate = (long)rd_u32le(fmtbuf + 4);
      byte_rate = (long)rd_u32le(fmtbuf + 8);
      bits_per_sample = rd_u16le(fmtbuf + 14);
      have_fmt = 1;

      long skip = (long)csize - 16 + (long)(csize & 1);
      if (skip > 0) {
        fseek(state->f, skip, SEEK_CUR);
      }
    } else if (memcmp(chdr, "data", 4) == 0) {
      if (!have_fmt) {
        fclose(state->f);
        state->f = NULL;
        return -1; /* "data" before "fmt " -- not canonical */
      }
      state->data_start = ftell(state->f);
      data_size = (long)csize;
      break;
    } else {
      long skip = (long)csize + (long)(csize & 1);
      if (fseek(state->f, skip, SEEK_CUR) != 0) {
        break;
      }
    }
  }

  int ok = have_fmt && data_size > 0 && audio_format == 1 && channels > 0 &&
           sample_rate > 0 &&
           (bits_per_sample == 8 || bits_per_sample == 16 ||
            bits_per_sample == 24 || bits_per_sample == 32);
  if (!ok) {
    fclose(state->f);
    state->f = NULL;
    return -1;
  }

  state->data_remaining = data_size;
  state->channels = channels;
  state->bits_per_sample = bits_per_sample;
  state->bytes_per_sample = bits_per_sample / 8;
  state->scratch = NULL;
  state->scratch_cap = 0;

  fmt->sample_rate = (int)sample_rate;
  fmt->channels = channels;

  if (info) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(info->title, sizeof(info->title), "%s", base);
    info->bitrate = (int)(sample_rate * channels * bits_per_sample);
    info->duration_sec =
        byte_rate > 0 ? (double)data_size / (double)byte_rate : 0.0;
    snprintf(info->sample_format, sizeof(info->sample_format), "%d-bit PCM",
             bits_per_sample);
  }

  return 0;
}

static size_t wav_decode(void *vstate, float *out, size_t out_frames) {
  WAV_DECODER *state = vstate;
  if (state->data_remaining <= 0) {
    return 0;
  }

  size_t bytes_per_frame = (size_t)state->channels * (size_t)state->bytes_per_sample;
  size_t want_bytes = out_frames * bytes_per_frame;
  size_t avail_bytes = (size_t)state->data_remaining;
  size_t read_bytes = want_bytes < avail_bytes ? want_bytes : avail_bytes;
  read_bytes -= read_bytes % bytes_per_frame;
  if (read_bytes == 0) {
    return 0;
  }

  if (read_bytes > state->scratch_cap) {
    unsigned char *grown = realloc(state->scratch, read_bytes);
    if (!grown) {
      return 0;
    }
    state->scratch = grown;
    state->scratch_cap = read_bytes;
  }

  size_t got = fread(state->scratch, 1, read_bytes, state->f);
  size_t frames = got / bytes_per_frame;
  state->data_remaining -= (long)(frames * bytes_per_frame);

  for (size_t i = 0; i < frames * (size_t)state->channels; i++) {
    out[i] = convert_sample(state->scratch + i * (size_t)state->bytes_per_sample,
                            state->bytes_per_sample);
  }

  return frames;
}

static void wav_close(void *vstate) {
  WAV_DECODER *state = vstate;
  if (state->f) {
    fclose(state->f);
  }
  free(state->scratch);
  state->f = NULL;
  state->scratch = NULL;
  state->scratch_cap = 0;
}

CTUI_DECODER wav_decoder_make(WAV_DECODER *state) {
  return (CTUI_DECODER){
      .state = state, .open = wav_open, .decode = wav_decode, .close = wav_close};
}
