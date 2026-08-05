#include "ctui.h"
#include "logger.h"
#include "widgets/border.h"
#include "widgets/debug_info.h"
#include "widgets/dump_palette.h"
#include "widgets/label.h"
#include "widgets/menu.h"
#include "widgets/status.h"

#include <stdio.h>
#include <string.h>

/* shared by every layout() below: header ~20%, main ~60%, footer takes
 * whatever's left so the three areas always sum to exactly rows, no matter
 * how rows changes on resize */
static void demo_area_heights(int rows, int *header_h, int *main_h,
                              int *footer_h) {
  *header_h = rows * 20 / 100;
  *main_h = rows * 60 / 100;
  *footer_h = rows - *header_h - *main_h;
}

static void header_border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, main_h, footer_h;
  demo_area_heights(comp->rows, &header_h, &main_h, &footer_h);
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = header_h;
}

static void header_title_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, main_h, footer_h;
  demo_area_heights(comp->rows, &header_h, &main_h, &footer_h);
  self->x = 1;
  self->y = 1;
  self->w = comp->cols - 2;
  self->h = header_h - 2;
}

static void main_border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, main_h, footer_h;
  demo_area_heights(comp->rows, &header_h, &main_h, &footer_h);
  self->x = 0;
  self->y = header_h;
  self->w = comp->cols;
  self->h = main_h;
}

/* the main area now hosts a CTUI_SPLIT (widget_data), not the menu
 * directly: same centering math the old standalone menu_layout() used, but
 * the box is one pane tall (7 rows) until debug info is picked, then two
 * (14, capped to whatever the main area's inner region actually has --
 * see main_inner_h below) once ctui_split's count grows to 2 -- see
 * main_split_handle_debug_toggle(). ctui_split_layout() at the end handles
 * dividing whatever area this computes among the active children and
 * rebinding them; it's re-run automatically by ctui_widget_init() (both at
 * startup and on resize) since this whole function is registered as the
 * split widget's layout(). */
static void main_split_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_SPLIT *split = self->widget_data;
  int header_h, main_h, footer_h;
  demo_area_heights(comp->rows, &header_h, &main_h, &footer_h);
  int main_inner_w = comp->cols - 2, main_inner_h = main_h - 2;
  int wanted_h = 7 * split->count;

  self->w = 48;
  self->h = wanted_h < main_inner_h ? wanted_h : main_inner_h;
  self->x = 1 + (main_inner_w - self->w) / 2;
  self->y = (header_h + 1) + (main_inner_h - self->h) / 2;

  ctui_split_layout(self, comp);
}

/* the split's second pane is a single shared slot that either the "debug
 * info" or "dump palette" menu item can occupy -- these two file-scope
 * pointers are how main_split_handle_panel_toggle() (below) knows which
 * actual widget to swap into split->children[1], since it only gets the
 * split widget itself as `self`, never the menu's or panel widgets'
 * addresses. Assigned once in main(), right after each widget is built. */
static CTUI_WIDGET *debug_info_widget;
static CTUI_WIDGET *dump_palette_widget;

/* listens for ("menu", CTUI_VALUE_CHANGED_EVENT) -- same event status
 * listens for, just a second, independent handler on the same key,
 * demonstrating that the registry supports more than one listener per
 * (source, type). Reacts to either the "debug info" or "dump palette"
 * item, tracking each one's enabled/disabled state (not just the fact that
 * it changed) so its panel opens when toggled on and closes again when
 * toggled off. Both items share the same second-pane slot -- toggling one
 * on swaps it into that slot; toggling one off only closes the pane if
 * it's the one currently showing there. */
static int main_split_handle_panel_toggle(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_SPLIT *split = self->widget_data;
  CTUI_VALUE_CHANGED_EVENT_DATA *changed = ev->event_data;

  CTUI_WIDGET *pane;
  if (strcmp(changed->value, "debug info") == 0) {
    pane = debug_info_widget;
  } else if (strcmp(changed->value, "dump palette") == 0) {
    pane = dump_palette_widget;
  } else {
    return 0;
  }

  if (changed->enabled) {
    split->children[1] = pane;
  } else if (split->children[1] != pane) {
    return 0;
  }

  int want_count = changed->enabled ? 2 : 1;
  if (split->count == want_count) {
    return 0;
  }

  split->count = want_count;
  /* re-run right now rather than waiting for the next resize, so the
   * pane that just appeared/disappeared is (re)bound before the next
   * render */
  main_split_layout(self, split->comp);
  ctui_logf(E_INF,
            "[DEMO:APP] - \"%s\" %s @ tick %d, %s main area\n",
            changed->value, changed->enabled ? "enabled" : "disabled",
            ctui_tick_advance(), changed->enabled ? "splitting" : "closing");
  return 1;
}

static void footer_border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, main_h, footer_h;
  demo_area_heights(comp->rows, &header_h, &main_h, &footer_h);
  self->x = 0;
  self->y = header_h + main_h;
  self->w = comp->cols;
  self->h = footer_h;
}

/* only vertically centered -- x stays a fixed inset from the left border */
static void status_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, main_h, footer_h;
  demo_area_heights(comp->rows, &header_h, &main_h, &footer_h);
  int footer_y = header_h + main_h;
  int footer_inner_h = footer_h - 2;
  self->w = 48;
  self->h = 2;
  self->x = 2;
  self->y = (footer_y + 1) + (footer_inner_h - self->h) / 2;
}

int main(void) {
  /* E_ALL & ~E_DBG: everything except per-primitive putc/puts/byte-read
   * tracing. Pass E_ALL (or E_DBG on its own) to see that level too. */
  /* demo asks for ANSI256, but must still run on a plain terminal --
   * ctui_init() negotiates down instead of failing if the terminal can't
   * do it, rewriting gfx_mode to whatever it actually granted (see
   * ctui_dump_palette_render()'s use of it below, via widget_data). Only
   * the mandatory ANSI16 floor itself can fail this call. */
  CTUI_GFX_MODE gfx_mode = CTUI_GFX_ANSI256;
  if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  ctui_logf(E_INF, "[DEMO:APP] - startup @ tick %d\n", ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  ctui_logf(E_INF, "[DEMO:APP] - terminal size %dx%d @ tick %d\n", cols, rows,
            ctui_tick_advance());
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  static CTUI_MENU_ITEM items[] = {
      {.label = "debug info", .enabled = 0},
      {.label = "dump palette", .enabled = 0},
      {.label = "whatever", .enabled = 0},
      {.label = "1337", .enabled = 0},
      {.label = "quit", .enabled = 0},
  };
  CTUI_MENU data = {.items = items, .count = 5, .selected = 0};
  CTUI_STATUS status_data = {.text = ""};
  CTUI_LABEL title = {
      .text = "ctui - demo", .fg = CTUI_COLOR_CYAN, .bg = CTUI_COLOR_DEFAULT};

  /* shared by all three borders -- ctui_border_render only reads it, so one
   * instance is enough even though three separate widgets point at it */
  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  border_style.corner =
      (CTUI_CELL){.ch = '+', .fg = CTUI_COLOR_YELLOW, .bg = CTUI_COLOR_DEFAULT};

  /* each area's border and content are independent widgets, not a
   * CTUI_GROUP -- content is deliberately inset from its border (never
   * shares a cell with it), so there's no layering to compose and each just
   * gets its own compositor slice via its own (x,y), the normal way.
   *
   * every widget below gets its real x/y/w/h from its layout() callback
   * (see above main()), re-run by ctui_widget_init() -- both the initial
   * one from ctui_app_init() just below, and every one ctui_app_resize()
   * triggers on a terminal resize. The 0,0,0,0 here is just a placeholder,
   * immediately overwritten before anything reads it. */
  CTUI_WIDGET header_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, header_border_layout);
  CTUI_WIDGET header_title = ctui_widget_make(
      0, 0, 0, 0, &title, ctui_label_render, header_title_layout);
  CTUI_WIDGET main_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, main_border_layout);

  /* main area's content: a menu on top, and a second pane revealed below it
   * once either "debug info" or "dump palette" is picked (see
   * main_split_handle_panel_toggle()). Neither child widget's own x/y/w/h
   * matter here -- ctui_split_layout() (called from main_split_layout())
   * overwrites them every time. debug_info/dump_palette are built here but
   * assigned to their file-scope pointers (declared above
   * main_split_handle_panel_toggle()) so that handler can identify them;
   * they stay alive because main()'s stack frame lives for the whole
   * program, same as every other widget below. */
  CTUI_WIDGET menu = ctui_widget_make(0, 0, 0, 0, &data, ctui_menu_render, NULL);
  CTUI_WIDGET debug_info =
      ctui_widget_make(0, 0, 0, 0, NULL, ctui_debug_info_render, NULL);
  CTUI_WIDGET dump_palette = ctui_widget_make(
      0, 0, 0, 0, &gfx_mode, ctui_dump_palette_render, NULL);
  debug_info_widget = &debug_info;
  dump_palette_widget = &dump_palette;
  CTUI_WIDGET *main_split_children[] = {&menu, &debug_info};
  CTUI_SPLIT main_split = {
      .mode = CTUI_SPLIT_V, .children = main_split_children, .count = 1};
  CTUI_WIDGET main_split_widget = ctui_widget_make(
      0, 0, 0, 0, &main_split, ctui_split_render, main_split_layout);

  CTUI_WIDGET footer_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, footer_border_layout);
  CTUI_WIDGET status = ctui_widget_make(0, 0, 0, 0, &status_data,
                                        ctui_status_render, status_layout);

  /* main_split_widget takes the menu's old slot in the top-level widget
   * list -- menu and debug_info are its children now, positioned/bound/
   * rendered entirely through it (see main_split_layout()/
   * ctui_split_render()), so they must NOT also appear here or they'd get
   * bound twice against stale (0,0,0,0) geometry */
  CTUI_WIDGET *widgets[] = {
      &header_border,     &header_title, &main_border,
      &main_split_widget, &footer_border, &status,
  };
  CTUI_APP app;
  ctui_app_init(&app, widgets, 6, rows, cols);

  /* addEventListener()-style wiring: menu reacts to raw keypresses (the
   * "input" source ctui_input_loop() emits under). status and
   * main_split_widget both listen for menu's value-changed events -- two
   * independent handlers on the same (source, type) key -- neither knowing
   * anything about how the other reacts. */
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &menu,
                      ctui_menu_handle_keypress);
  ctui_event_register("menu", CTUI_VALUE_CHANGED_EVENT, &status,
                      ctui_status_handle_value_changed);
  ctui_event_register("menu", CTUI_VALUE_CHANGED_EVENT, &main_split_widget,
                      main_split_handle_panel_toggle);

  ctui_logf(E_INF,
            "[DEMO:APP] - widgets wired (header, main, footer) @ tick %d, "
            "entering event loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 0);
  ctui_logf(E_INF, "[DEMO:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
