#include "ctui.h"
#include "logger.h"

#include <stdio.h>

typedef struct {
  const char **items;
  int count;
  int selected;
  char status[64];
} MenuData;

/*typedef struct {
  CTUI_SCREEN *screen
} DebugData;*/

static void menu_render(CTUI_WIDGET *self, CTUI_SCREEN *screen) {
  MenuData *data = self->widget_data;
  ctui_logf(E_INF, "[DEMO:MENU] - rendering @ tick %d (selected=%d)\n",
            ctui_tick_advance(), data->selected);

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

  ctui_logf(E_INF, "[DEMO:MENU] - event received @ tick %d (type=%s)\n",
            ctui_tick_advance(), ctui_eventtype_name(ev->type));

  switch (ev->type) {
  case CTUI_KEYPRESS_EVENT: {
    CTUI_KEYPRESS_EVENT_DATA *ev_data =
        (CTUI_KEYPRESS_EVENT_DATA *)ev->event_data;
    ctui_logf(E_INF, "[DEMO:MENU] - keypress %s\n",
              ctui_keytype_name(ev_data->type));

    if (ev_data->type == CTUI_KEY_UP) {
      if (data->selected > 0) {
        data->selected--;
        ctui_logf(E_INF, "[DEMO:MENU] - selection moved up to %d ('%s')\n",
                  data->selected, data->items[data->selected]);
        return 1;
      }
      ctui_log(E_INF, "[DEMO:MENU] - already at top, ignoring UP\n");
    } else if (ev_data->type == CTUI_KEY_DOWN) {
      if (data->selected < data->count - 1) {
        data->selected++;
        ctui_logf(E_INF, "[DEMO:MENU] - selection moved down to %d ('%s')\n",
                  data->selected, data->items[data->selected]);
        return 1;
      }
      ctui_log(E_INF, "[DEMO:MENU] - already at bottom, ignoring DOWN\n");
    } else if (ev_data->type == CTUI_KEY_ENTER) {
      snprintf(data->status, sizeof(data->status), "you picked: %s",
               data->items[data->selected]);
      ctui_logf(E_INF, "[DEMO:MENU] - picked '%s'\n",
                data->items[data->selected]);
      return 1;
    }
    ctui_log(E_INF, "[DEMO:MENU] - event not handled\n");
    return 0;
  }
  default:
    ctui_logf(E_INF, "[DEMO:MENU] - ignoring non-keypress event type %s\n",
              ctui_eventtype_name(ev->type));
    return 0;
  }
}

static void status_render(CTUI_WIDGET *self, CTUI_SCREEN *screen) {
  MenuData *data = self->widget_data;
  ctui_logf(E_INF, "[DEMO:STATUS] - rendering @ tick %d (status=\"%s\")\n",
            ctui_tick_advance(), data->status);

  ctui_screen_puts(screen, self->y, self->x, "Status:", CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
  ctui_screen_puts(screen, self->y + 1, self->x,
                   data->status[0] ? data->status : "(nothing yet)",
                   CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
}

static int status_on_event(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  ctui_logf(
      E_INF,
      "[DEMO:STATUS] - event received @ tick %d (type=%s), ignoring (no-op "
      "widget)\n",
      ctui_tick_advance(), ctui_eventtype_name(ev->type));
  return 0;
}

static void debug_info_render(CTUI_WIDGET *self, CTUI_SCREEN *screen) {
  char buf[screen->cols * screen->rows];
  ctui_logf(E_INF, "[DEMO:DEBUG] - rendering @ tick %d (screen=%dx%d)\n",
            ctui_tick_advance(), screen->cols, screen->rows);

  // row 1/6
  ctui_screen_puts(screen, self->y, self->x, "__debug_info__",
                   CTUI_COLOR_YELLOW, CTUI_COLOR_DEFAULT);
  // row 2/6
  snprintf(buf, sizeof(buf), "width: %d chars/columns", screen->cols);
  ctui_screen_puts(screen, self->y + 1, self->x, buf, CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
  // row 3/6; etc
  snprintf(buf, sizeof(buf), "height: %d chars/rows", screen->rows);
  ctui_screen_puts(screen, self->y + 2, self->x, buf, CTUI_COLOR_YELLOW,
                   CTUI_COLOR_DEFAULT);
}

static int debug_info_on_event(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  ctui_logf(
      E_INF,
      "[DEMO:DEBUG] - event received @ tick %d (type=%s), ignoring (no-op "
      "widget)\n",
      ctui_tick_advance(), ctui_eventtype_name(ev->type));
  return 0;
}

int main(void) {
  /* E_ALL & ~E_DBG: everything except per-primitive putc/puts/byte-read
   * tracing. Pass E_ALL (or E_DBG on its own) to see that level too. */
  if (ctui_init(E_INF | E_WRN | E_ERR) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  ctui_logf(E_INF, "[DEMO:APP] - startup @ tick %d\n", ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  ctui_logf(E_INF, "[DEMO:APP] - terminal size %dx%d @ tick %d\n", cols, rows,
            ctui_tick_advance());
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  static const char *items[] = {"debug info", "vm stats", "whatever", "1337",
                                "quit"};
  MenuData data = {.items = items, .count = 5, .selected = 0, .status = ""};
  // DebugData debug_data = {.screen = screen};

  CTUI_WIDGET menu =
      ctui_widget_make(2, 1, 48, 6, &data, menu_on_event, menu_render);

  CTUI_WIDGET debug_info = ctui_widget_make(
      2, 8, 48, 6, NULL, debug_info_on_event, debug_info_render);

  CTUI_WIDGET status = ctui_widget_make(2, rows - 8, 48, 2, &data,
                                        status_on_event, status_render);

  CTUI_WIDGET *widgets[] = {&menu, &debug_info, &status};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 3);
  ctui_logf(E_INF,
            "[DEMO:APP] - widgets wired (menu, debug_info, status) @ tick "
            "%d, entering event loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen);
  ctui_logf(E_INF, "[DEMO:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
