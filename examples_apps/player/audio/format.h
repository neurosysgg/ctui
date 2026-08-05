#ifndef PLAYER_AUDIO_FORMAT_H
#define PLAYER_AUDIO_FORMAT_H

/* shared between every decoder/output stage -- see DESIGN.md's "three
 * independent stages, tee'd". Internal representation between all stages
 * is always interleaved float32, regardless of the source file's on-disk
 * bit depth. */
typedef struct {
  int sample_rate;
  int channels;
} CTUI_AUDIO_FORMAT;

typedef struct {
  char title[128];
  int bitrate;
  double duration_sec;
  char sample_format[32]; /* e.g. "16-bit PCM" */
} CTUI_TRACK_INFO;

#endif
