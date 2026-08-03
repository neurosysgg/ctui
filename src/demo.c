#include "ctui.h"

#include <stdio.h>

typedef struct {
  const char **items;
  int count;
  int selected;
  char status[64];
} MenuData;

static void menu_render(CTUI_WIDGET *self, CTUI_SCREEN *screen) {
  MenuData *data = self->widget_data;
  ctui_screen_puts(screen, self->y, self->x,
                   "Pick a tool (Enter to select, Esc to quit)",
                   CTUI_COLOR_CYAN, CTUI_COLOR_DEFAULT);

  for (int i = 0; i < data->count; i++) {
    int row = self->y + 2 + i;
    unsigned char fg =
        (i == data->selected) ? CTUI_COLOR_BLACK : CTUI_COLOR_DEFAULT;
    unsigned char bg =
        (i == data->selected) ? CTUI_COLOR_GREEN : CTUI_COLOR_DEFAULT;
    char line[64];
    snprintf(line, sizeof(line), "%c %-20s", (i == data->selected) ? '>' : ' ',
             data->items[i]);
    ctui_screen_puts(screen, row, self->x, line, fg, bg);
  }
}

static int menu_on_event(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  MenuData *data = self->widget_data;
  switch (ev->type) {
  case CTUI_KEYPRESS_EVENT: {
    CTUI_KEYPRESS_EVENT_DATA *ev_data =
        (CTUI_KEYPRESS_EVENT_DATA *)ev->event_data;
    if (ev_data->type == CTUI_KEY_UP) {
      if (data->selected > 0) {
        data->selected--;
        return 1;
      }
    } else if (ev_data->type == CTUI_KEY_DOWN) {
      if (data->selected < data->count - 1) {
        data->selected++;
        return 1;
      }
    } else if (ev_data->type == CTUI_KEY_ENTER) {
      snprintf(data->status, sizeof(data->status), "you picked: %s",
               data->items[data->selected]);
      return 1;
    }
    return 0;
  }
  default:
    return 0;
  }
}

static void status_render(CTUI_WIDGET *self, CTUI_SCREEN *screen) {
  MenuData *data = self->widget_data;
  ctui_screen_puts(screen, self->y, self->x, "Status:", CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
  ctui_screen_puts(screen, self->y + 1, self->x,
                   data->status[0] ? data->status : "(nothing yet)",
                   CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
}

static int status_on_event(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  return 0;
}

int main(void) {
  if (ctui_init() != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  static const char *items[] = {"wallpaper", "vm stats", "whatever", "1337",
                                "quit"};
  MenuData data = {.items = items, .count = 5, .selected = 0, .status = ""};

  CTUI_WIDGET menu = {.x = 2,
                      .y = 1,
                      .w = 48,
                      .h = 6,
                      .widget_data = &data,
                      .render = menu_render,
                      .on_event = menu_on_event};
  CTUI_WIDGET status = {.x = 2,
                        .y = 8,
                        .w = 48,
                        .h = 2,
                        .widget_data = &data,
                        .render = status_render,
                        .on_event = status_on_event};

  CTUI_WIDGET *widgets[] = {&menu, &status};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 2);
  ctui_app_run(&app, screen);

  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
