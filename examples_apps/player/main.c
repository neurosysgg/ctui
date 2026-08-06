/* must precede every #include -- see the identical comment in
 * outputs/alsa.h/src/ctui.c: dirent.h/chdir()/getcwd() need POSIX, and
 * alsa/asoundlib.h needs _GNU_SOURCE to avoid a struct timespec collision
 * under -std=c11; _GNU_SOURCE implies the POSIX feature set too, so one
 * define covers both. */
#define _GNU_SOURCE

#include "ctui.h"
#include "widgets/border.h"
#include "widgets/label.h"
#include "widgets/list.h"
#include "widgets/meter.h"
#include "widgets/periodic.h"

#include "audio/decoder.h"
#include "audio/format.h"
#include "audio/output.h"
#include "decoders/wav.h"
#include "outputs/alsa.h"
#include "outputs/null.h"

#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TICK_MS 20

static char g_cwd_buf[PATH_MAX] = "";
static char g_track_buf[256] = "select a .wav file";

typedef struct {
  int active;
  int using_null; /* true once ALSA open() failed and null_output took
                   * over -- keeps playback (and the meter) alive so the
                   * app is still exercisable without real audio hardware */

  WAV_DECODER wav_state;
  CTUI_DECODER decoder;

  ALSA_OUTPUT alsa_state;
  CTUI_AUDIO_OUTPUT output;

  CTUI_AUDIO_FORMAT fmt;
  CTUI_TRACK_INFO info;

  float *chunk_buf; /* chunk_frames * fmt.channels floats; sized once at
                     * start_playback(), freed at stop_playback() */
  size_t chunk_frames;
} PLAYER_STATE;

static PLAYER_STATE g_player = {0};
static CTUI_METER *g_meter_data = NULL;

static int has_wav_ext(const char *name) {
  size_t len = strlen(name);
  return len > 4 && strcasecmp(name + len - 4, ".wav") == 0;
}

static void stop_playback(void) {
  if (!g_player.active) {
    return;
  }
  g_player.decoder.close(g_player.decoder.state);
  g_player.output.close(g_player.output.state);
  free(g_player.chunk_buf);
  g_player.chunk_buf = NULL;
  g_player.chunk_frames = 0;
  g_player.active = 0;
  if (g_meter_data) {
    g_meter_data->level = 0.0f;
  }
  ctui_logf(E_INF, "[PLAYER:APP] - playback stopped @ tick %d\n",
           ctui_tick_advance());
}

static void start_playback(const char *path) {
  stop_playback();

  memset(&g_player.wav_state, 0, sizeof(g_player.wav_state));
  g_player.decoder = wav_decoder_make(&g_player.wav_state);
  if (g_player.decoder.open(g_player.decoder.state, path, &g_player.fmt,
                            &g_player.info) != 0) {
    ctui_logf(E_WRN, "[PLAYER:APP] - failed to open \"%s\" as WAV\n", path);
    return;
  }

  memset(&g_player.alsa_state, 0, sizeof(g_player.alsa_state));
  g_player.output = alsa_output_make(&g_player.alsa_state);
  g_player.using_null = 0;
  if (g_player.output.open(g_player.output.state, &g_player.fmt) != 0) {
    ctui_logf(E_WRN,
             "[PLAYER:APP] - ALSA open failed, falling back to null "
             "output (no audio hardware?)\n");
    g_player.output = null_output_make();
    g_player.output.open(g_player.output.state, &g_player.fmt);
    g_player.using_null = 1;
  }

  g_player.chunk_frames = (size_t)g_player.fmt.sample_rate * TICK_MS / 1000;
  if (g_player.chunk_frames == 0) {
    g_player.chunk_frames = 1;
  }
  g_player.chunk_buf =
      malloc(g_player.chunk_frames * (size_t)g_player.fmt.channels * sizeof(float));
  g_player.active = 1;

  snprintf(g_track_buf, sizeof(g_track_buf), "playing: %s (%dHz %dch, %s, %.1fs)%s",
          g_player.info.title, g_player.fmt.sample_rate, g_player.fmt.channels,
          g_player.info.sample_format, g_player.info.duration_sec,
          g_player.using_null ? " [no audio device]" : "");

  ctui_logf(E_INF, "[PLAYER:APP] - playback started @ tick %d: \"%s\"\n",
           ctui_tick_advance(), path);
}

static int playback_tick(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  if (!g_player.active) {
    return 0;
  }
  CTUI_METER *meter = self->widget_data;

  size_t got = g_player.decoder.decode(g_player.decoder.state,
                                      g_player.chunk_buf, g_player.chunk_frames);
  if (got == 0) {
    ctui_logf(E_INF, "[PLAYER:APP] - reached EOF @ tick %d\n",
             ctui_tick_advance());
    snprintf(g_track_buf, sizeof(g_track_buf), "finished: %s", g_player.info.title);
    stop_playback();
    return 1;
  }

  g_player.output.write(g_player.output.state, g_player.chunk_buf, got);

  size_t samples = got * (size_t)g_player.fmt.channels;
  double sumsq = 0.0;
  for (size_t i = 0; i < samples; i++) {
    double v = g_player.chunk_buf[i];
    sumsq += v * v;
  }
  float rms = (float)sqrt(sumsq / (double)samples);
  meter->level = rms * 4.0f; /* typical program material's RMS sits well
                              * under 1.0 -- a flat gain keeps the meter
                              * visually useful instead of barely twitching */

  return 1;
}

static void free_entries(CTUI_LIST *list) {
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].label);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
}

static int cmp_list_item(const void *a, const void *b) {
  const CTUI_LIST_ITEM *ia = a, *ib = b;
  if (ia->is_dir != ib->is_dir) {
    return ib->is_dir - ia->is_dir;
  }
  return strcmp(ia->label, ib->label);
}

static void load_dir(CTUI_LIST *list) {
  DIR *d = opendir(".");
  if (!d) {
    ctui_log(E_WRN, "[PLAYER:APP] - opendir(\".\") failed\n");
    return;
  }

  char *cwd = getcwd(g_cwd_buf, sizeof(g_cwd_buf));
  int has_parent = cwd != NULL && strcmp(cwd, "/") != 0;

  size_t cap = 8, n = 0;
  CTUI_LIST_ITEM *entries = malloc(cap * sizeof(*entries));
  if (has_parent) {
    entries[n++] = (CTUI_LIST_ITEM){.label = strdup(".."), .is_dir = 1};
  }

  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') {
      continue;
    }
    struct stat st;
    if (stat(de->d_name, &st) != 0) {
      continue;
    }
    int is_dir = S_ISDIR(st.st_mode);
    if (!is_dir && !has_wav_ext(de->d_name)) {
      continue; /* filtered to *.wav instead of file_browser's chdir-only rule */
    }
    if (n == cap) {
      cap *= 2;
      entries = realloc(entries, cap * sizeof(*entries));
    }
    entries[n++] = (CTUI_LIST_ITEM){.label = strdup(de->d_name), .is_dir = is_dir};
  }
  closedir(d);

  qsort(entries + has_parent, n - (size_t)has_parent, sizeof(*entries), cmp_list_item);

  free_entries(list);
  list->items = entries;
  list->count = (int)n;
  list->selected = 0;
  list->scroll_offset = 0;

  ctui_logf(E_INF, "[PLAYER:APP] - loaded %zu entries from \"%s\"\n", n, g_cwd_buf);
}

static int list_handle_value_changed(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_LIST *data = self->widget_data;
  CTUI_VALUE_CHANGED_EVENT_DATA *changed = ev->event_data;

  if (changed->enabled) {
    if (chdir(changed->value) != 0) {
      ctui_logf(E_WRN, "[PLAYER:APP] - chdir(\"%s\") failed\n", changed->value);
      return 0;
    }
    load_dir(data);
    return 1;
  }

  start_playback(changed->value);
  return 1;
}

static void header_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = 3;
}

static void path_label_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 1;
  self->w = comp->cols - 2;
  self->h = 1;
}

static void main_border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 3;
  self->w = comp->cols;
  self->h = comp->rows - 3 - 4;
}

static void list_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 4;
  self->w = comp->cols - 2;
  self->h = comp->rows - 3 - 4 - 2;
}

static void footer_border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = comp->rows - 4;
  self->w = comp->cols;
  self->h = 4;
}

static void track_label_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = comp->rows - 3;
  self->w = comp->cols - 2;
  self->h = 1;
}

static void meter_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = comp->rows - 2;
  self->w = comp->cols - 2;
  self->h = 1;
}

int main(void) {
  /* ANSI16 is the mandatory floor -- ctui_init() can only negotiate
   * *down* to it, never below, so this app never needs to inspect
   * gfx_mode again after the call */
  CTUI_GFX_MODE gfx_mode = CTUI_GFX_ANSI16;
  if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  ctui_logf(E_INF, "[PLAYER:APP] - startup @ tick %d\n", ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  border_style.corner =
      (CTUI_CELL){.ch = '+', .fg = CTUI_COLOR_YELLOW, .bg = CTUI_COLOR_DEFAULT};
  CTUI_LABEL path_label = {
      .text = g_cwd_buf, .fg = CTUI_COLOR_CYAN, .bg = CTUI_COLOR_DEFAULT};
  CTUI_LABEL track_label = {
      .text = g_track_buf, .fg = CTUI_COLOR_WHITE, .bg = CTUI_COLOR_DEFAULT};
  CTUI_LIST list_data = {0};
  CTUI_METER meter_data =
      ctui_meter_make(CTUI_COLOR_GREEN, CTUI_COLOR_YELLOW, CTUI_COLOR_RED,
                     CTUI_COLOR_BLACK);
  g_meter_data = &meter_data;

  CTUI_WIDGET header_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, header_layout);
  CTUI_WIDGET path_label_widget = ctui_widget_make(
      0, 0, 0, 0, &path_label, ctui_label_render, path_label_layout);
  CTUI_WIDGET main_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, main_border_layout);
  CTUI_WIDGET list_widget = ctui_widget_make(0, 0, 0, 0, &list_data,
                                             ctui_list_render, list_layout);
  CTUI_WIDGET footer_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, footer_border_layout);
  CTUI_WIDGET track_label_widget = ctui_widget_make(
      0, 0, 0, 0, &track_label, ctui_label_render, track_label_layout);
  CTUI_WIDGET meter_widget = ctui_widget_make(
      0, 0, 0, 0, &meter_data, ctui_meter_render, meter_layout);

  CTUI_WIDGET *widgets[] = {&header_border,     &path_label_widget, &main_border,
                           &list_widget,        &footer_border,     &track_label_widget,
                           &meter_widget};
  CTUI_APP app;
  if (ctui_app_init(&app, widgets, 7, rows, cols) != 0) {
    fprintf(stderr, "failed to init ctui app\n");
    return 1;
  }

  load_dir(&list_data);

  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &list_widget,
                      ctui_list_handle_keypress);
  ctui_event_register("list", CTUI_VALUE_CHANGED_EVENT, &list_widget,
                      list_handle_value_changed);
  ctui_periodic_register(&meter_widget, TICK_MS, 0, playback_tick);

  ctui_logf(E_INF,
           "[PLAYER:APP] - widgets wired @ tick %d, entering event loop\n",
           ctui_tick_advance());
  ctui_app_run(&app, screen, TICK_MS);
  ctui_logf(E_INF, "[PLAYER:APP] - event loop exited @ tick %d\n",
           ctui_tick_advance());

  stop_playback();
  free_entries(&list_data);
  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
