#include "null.h"

static int null_open(void *state, CTUI_AUDIO_FORMAT *fmt) {
  (void)state;
  (void)fmt;
  return 0;
}

static size_t null_write(void *state, const float *frames, size_t n) {
  (void)state;
  (void)frames;
  return n;
}

static void null_close(void *state) {
  (void)state;
}

CTUI_AUDIO_OUTPUT null_output_make(void) {
  return (CTUI_AUDIO_OUTPUT){
      .state = NULL, .open = null_open, .write = null_write, .close = null_close};
}
