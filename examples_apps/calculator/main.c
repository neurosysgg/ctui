#include "calc.h"
#include "ctui.h"
#include "widgets/border.h"
#include "widgets/display.h"
#include "widgets/grid.h"

#include <stdio.h>
#include <string.h>

static CALC_STATE g_calc;

/* the CTUI-facing half of the translation layer: turns a CTUI_GRID item's
 * label into the pure-C token calc_apply() understands. Returns 0 (and
 * leaves *out untouched) for a label calc.h has no token for. */
static int calc_token_from_label(const char *key, CALC_TOKEN *out) {
  if (strcmp(key, "C") == 0) {
    *out = (CALC_TOKEN){.type = CALC_CLEAR};
  } else if (strcmp(key, "<-") == 0) {
    *out = (CALC_TOKEN){.type = CALC_BACKSPACE};
  } else if (strcmp(key, "+/-") == 0) {
    *out = (CALC_TOKEN){.type = CALC_NEGATE};
  } else if (strcmp(key, "%") == 0) {
    *out = (CALC_TOKEN){.type = CALC_PERCENT};
  } else if (strcmp(key, ".") == 0) {
    *out = (CALC_TOKEN){.type = CALC_DOT};
  } else if (strcmp(key, "=") == 0) {
    *out = (CALC_TOKEN){.type = CALC_EQUALS};
  } else if (strlen(key) == 1 && strchr("+-*/", key[0])) {
    *out = (CALC_TOKEN){.type = CALC_OP, .value = key[0]};
  } else if (strlen(key) == 1 && key[0] >= '0' && key[0] <= '9') {
    *out = (CALC_TOKEN){.type = CALC_DIGIT, .value = key[0]};
  } else {
    return 0;
  }
  return 1;
}

/* the other half: drives calc_apply() from a grid press and copies the
 * resulting text into self's CTUI_DISPLAY. Register against
 * ("grid", CTUI_VALUE_CHANGED_EVENT) with the display widget as self. */
static int calc_handle_keypad_press(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_DISPLAY *disp = self->widget_data;
  CTUI_VALUE_CHANGED_EVENT_DATA *changed = ev->event_data;
  const char *key = changed->value;

  ctui_logf(E_INF, "[CALC:APP] - key \"%s\" pressed @ tick %d\n", key,
            ctui_tick_advance());

  CALC_TOKEN token;
  if (!calc_token_from_label(key, &token)) {
    ctui_logf(E_WRN, "[CALC:APP] - unrecognized key \"%s\"\n", key);
    return 0;
  }

  CALC_RESULT result = calc_apply(&g_calc, token);
  if (!result.changed) {
    ctui_log(E_INF, "[CALC:APP] - in error state, ignoring until 'C'\n");
    return 0;
  }

  snprintf(disp->text, sizeof(disp->text), "%s", result.text);
  return 1;
}

static void border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = comp->rows;
}

static void display_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 1;
  self->w = comp->cols - 2;
  self->h = 3;
}

static void grid_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 1;
  self->y = 4;
  self->w = comp->cols - 2;
  self->h = comp->rows - 5;
}

int main(void) {
  if (ctui_init(E_INF | E_WRN | E_ERR) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  ctui_logf(E_INF, "[CALC:APP] - startup @ tick %d\n", ctui_tick_advance());
  g_calc = calc_make();

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  border_style.corner =
      (CTUI_CELL){.ch = '+', .fg = CTUI_COLOR_YELLOW, .bg = CTUI_COLOR_DEFAULT};
  CTUI_DISPLAY display_data =
      ctui_display_make(CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);

  static CTUI_GRID_ITEM g_keys[5][4] = {
      {{"C", 'c'}, {"+/-", 0}, {"%", '%'}, {"/", '/'}},
      {{"7", '7'}, {"8", '8'}, {"9", '9'}, {"*", '*'}},
      {{"4", '4'}, {"5", '5'}, {"6", '6'}, {"-", '-'}},
      {{"1", '1'}, {"2", '2'}, {"3", '3'}, {"+", '+'}},
      {{"0", '0'}, {".", '.'}, {"<-", 8}, {"=", '='}},
  };
  CTUI_GRID grid_data = {.items = &g_keys[0][0],
                        .rows = sizeof(g_keys) / sizeof(g_keys[0]),
                        .cols = sizeof(g_keys[0]) / sizeof(g_keys[0][0])};

  CTUI_WIDGET border = ctui_widget_make(0, 0, 0, 0, &border_style,
                                        ctui_border_render, border_layout);
  CTUI_WIDGET display_widget = ctui_widget_make(
      0, 0, 0, 0, &display_data, ctui_display_render, display_layout);
  CTUI_WIDGET grid_widget = ctui_widget_make(0, 0, 0, 0, &grid_data,
                                             ctui_grid_render, grid_layout);

  CTUI_WIDGET *widgets[] = {&border, &display_widget, &grid_widget};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 3, rows, cols);

  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &grid_widget,
                      ctui_grid_handle_keypress);
  ctui_event_register("grid", CTUI_VALUE_CHANGED_EVENT, &display_widget,
                      calc_handle_keypad_press);

  ctui_logf(E_INF,
            "[CALC:APP] - widgets wired @ tick %d, entering event loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 0);
  ctui_logf(E_INF, "[CALC:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
