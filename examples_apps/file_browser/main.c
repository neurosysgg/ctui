/* must precede every #include -- see the identical comment in src/ctui.c;
 * dirent.h/chdir()/getcwd() are POSIX, not C11, and -std=c11 hides them
 * without this */
#define _POSIX_C_SOURCE 200809L

#include "ctui.h"
#include "widgets/border.h"
#include "widgets/label.h"
#include "widgets/list.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_cwd_buf[PATH_MAX] = "";

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
    return ib->is_dir - ia->is_dir; /* directories first */
  }
  return strcmp(ia->label, ib->label);
}

static void load_dir(CTUI_LIST *list) {
  DIR *d = opendir(".");
  if (!d) {
    ctui_log(E_WRN, "[FILE_BROWSER:APP] - opendir(\".\") failed\n");
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
      continue; /* skips hidden entries, "." and ".." */
    }
    struct stat st;
    if (stat(de->d_name, &st) != 0) {
      continue;
    }
    if (n == cap) {
      cap *= 2;
      entries = realloc(entries, cap * sizeof(*entries));
    }
    entries[n++] = (CTUI_LIST_ITEM){.label = strdup(de->d_name),
                                    .is_dir = S_ISDIR(st.st_mode)};
  }
  closedir(d);

  qsort(entries + has_parent, n - (size_t)has_parent, sizeof(*entries),
       cmp_list_item);

  free_entries(list);
  list->items = entries;
  list->count = (int)n;
  list->selected = 0;
  list->scroll_offset = 0;

  ctui_logf(E_INF, "[FILE_BROWSER:APP] - loaded %zu entries from \"%s\"\n", n,
            g_cwd_buf);
}

static int list_handle_value_changed(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_LIST *data = self->widget_data;
  CTUI_VALUE_CHANGED_EVENT_DATA *changed = ev->event_data;

  if (!changed->enabled) {
    ctui_logf(E_INF,
              "[FILE_BROWSER:APP] - \"%s\" is not a directory, ignoring\n",
              changed->value);
    return 0;
  }

  if (chdir(changed->value) != 0) {
    ctui_logf(E_WRN, "[FILE_BROWSER:APP] - chdir(\"%s\") failed\n",
              changed->value);
    return 0;
  }

  load_dir(data);
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
  self->h = comp->rows - 3;
}

static void list_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 4;
  self->w = comp->cols - 2;
  self->h = comp->rows - 5;
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
  ctui_logf(E_INF, "[FILE_BROWSER:APP] - startup @ tick %d\n",
            ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  border_style.corner =
      (CTUI_CELL){.ch = '+', .fg = CTUI_COLOR_YELLOW, .bg = CTUI_COLOR_DEFAULT};
  CTUI_LABEL path_label = {
      .text = g_cwd_buf, .fg = CTUI_COLOR_CYAN, .bg = CTUI_COLOR_DEFAULT};
  CTUI_LIST list_data = {0};

  CTUI_WIDGET header_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, header_layout);
  CTUI_WIDGET path_label_widget = ctui_widget_make(
      0, 0, 0, 0, &path_label, ctui_label_render, path_label_layout);
  CTUI_WIDGET main_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, main_border_layout);
  CTUI_WIDGET list_widget = ctui_widget_make(0, 0, 0, 0, &list_data,
                                             ctui_list_render, list_layout);

  CTUI_WIDGET *widgets[] = {&header_border, &path_label_widget, &main_border,
                            &list_widget};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 4, rows, cols);

  load_dir(&list_data);

  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &list_widget,
                      ctui_list_handle_keypress);
  ctui_event_register("list", CTUI_VALUE_CHANGED_EVENT, &list_widget,
                      list_handle_value_changed);

  ctui_logf(E_INF,
            "[FILE_BROWSER:APP] - widgets wired @ tick %d, entering event "
            "loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 0);
  ctui_logf(E_INF, "[FILE_BROWSER:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  free_entries(&list_data);
  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
