/* strdup() is POSIX, not C11 */
#define _POSIX_C_SOURCE 200809L

#include "icon.h"

#include <stdlib.h>
#include <string.h>

/* the paths placed so far; slot i is image id i + 1. Tray pixmaps and
 * notification images (ctui-wm's) are files named by their content, so an
 * animating tray icon brings a new path every frame: past CTUI_ICON_MAX the
 * least recently asked-for slot takes the new path, and placing it under
 * that id replaces the old image in kitty. */
typedef struct {
  char *path;
  unsigned long hash;
  unsigned long used; /* g_clock when last asked for */
} SLOT;

static SLOT *g_slots;
static int g_count;
static unsigned long g_clock;
static int g_enabled;
static CTUI_ICON_PREPARE g_prepare;

void ctui_icon_set_prepare(CTUI_ICON_PREPARE fn) { g_prepare = fn; }

void ctui_icon_enable(int on) { g_enabled = on; }

int ctui_icon_enabled(void) { return g_enabled; }

/* FNV-1a */
static unsigned long hash_path(const char *s) {
  unsigned long h = 2166136261u;
  for (; *s; s++) {
    h = (h ^ (unsigned char)*s) * 16777619u;
  }
  return h;
}

unsigned int ctui_icon_id(const char *path) {
  return ctui_icon_id_box(path, CTUI_ICON_COLS, 1);
}

unsigned int ctui_icon_id_box(const char *path, int cols, int rows) {
  int placed;
  return ctui_icon_id_placed(path, cols, rows, &placed);
}

unsigned int ctui_icon_id_placed(const char *path, int cols, int rows,
                                 int *placed) {
  *placed = 0;
  if (!g_enabled || !path || !path[0]) {
    return 0;
  }
  unsigned long h = hash_path(path);
  int lru = 0;
  for (int i = 0; i < g_count; i++) {
    if (g_slots[i].hash == h && strcmp(g_slots[i].path, path) == 0) {
      g_slots[i].used = ++g_clock;
      return (unsigned int)i + 1;
    }
    if (g_slots[i].used < g_slots[lru].used) {
      lru = i;
    }
  }
  int i = lru;
  if (g_count < CTUI_ICON_MAX) {
    if (!g_slots) {
      g_slots = malloc(CTUI_ICON_MAX * sizeof *g_slots);
    }
    i = g_count++;
  } else {
    free(g_slots[i].path);
  }
  g_slots[i] = (SLOT){.path = strdup(path), .hash = h, .used = ++g_clock};
  unsigned int id = (unsigned int)i + 1;
  char prepared[4096];
  ctui_gfx_kitty_place_file(
      id,
      g_prepare && g_prepare(path, cols, rows, prepared, sizeof prepared)
          ? prepared
          : path,
      cols, rows);
  *placed = 1;
  return id;
}

void ctui_icon_put(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                   unsigned int id, unsigned char bg) {
  if (id) {
    ctui_widget_put_kitty_placeholder(self, comp, row, col, id, CTUI_ICON_COLS,
                                      1, bg);
  }
}

void ctui_icon_reset(void) {
  for (int i = 0; i < g_count; i++) {
    free(g_slots[i].path);
  }
  free(g_slots);
  g_slots = NULL;
  g_count = 0;
  g_clock = 0;
}
