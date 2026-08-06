#include "ctui.h"
#include "logger.h"
#include "widgets/border.h"
#include "widgets/debug_info.h"
#include "widgets/dump_palette.h"
#include "widgets/kitty_image.h"
#include "widgets/label.h"
#include "widgets/navbar.h"
#include "widgets/splash.h"
#include "widgets/status.h"

#include <stdio.h>
#include <string.h>

/* small and fixed -- ctui_splash_render() nearest-neighbor resamples this up
 * to whatever the splash pane's actual cell size is every frame (see
 * splash_layout()/main_split_layout() below). ~2.4:1 so the aura reads as
 * roughly circular despite terminal cells being roughly twice as tall as
 * wide. */
#define SPLASH_SRC_W 29
#define SPLASH_SRC_H 12

/* shared by every layout() below: header/navbar/footer get a fixed-feeling
 * share of the screen, main takes whatever's left so the four areas always
 * sum to exactly rows, no matter how rows changes on resize */
static void demo_adv_area_heights(int rows, int *header_h, int *navbar_h,
                                  int *main_h, int *footer_h) {
  *header_h = rows * 15 / 100;
  *navbar_h = 2;
  *footer_h = rows * 18 / 100;
  *main_h = rows - *header_h - *navbar_h - *footer_h;
}

static void header_border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, navbar_h, main_h, footer_h;
  demo_adv_area_heights(comp->rows, &header_h, &navbar_h, &main_h, &footer_h);
  self->x = 0;
  self->y = 0;
  self->w = comp->cols;
  self->h = header_h;
}

/* header_border_widget is laid out first (see widgets[] in main() below),
 * so ctui_util_inset() can read its current x/y/w/h straight off it -- same
 * "outer draws its full box, content gets margin'd inside it" pattern as
 * the original demo's header. */
static CTUI_WIDGET *header_border_widget;

static void header_title_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)comp;
  ctui_util_inset(self, header_border_widget, ctui_margin_uniform(1));
}

static void navbar_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, navbar_h, main_h, footer_h;
  demo_adv_area_heights(comp->rows, &header_h, &navbar_h, &main_h, &footer_h);
  self->x = 0;
  self->y = header_h;
  self->w = comp->cols;
  self->h = navbar_h;
}

static void main_border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, navbar_h, main_h, footer_h;
  demo_adv_area_heights(comp->rows, &header_h, &navbar_h, &main_h, &footer_h);
  self->x = 0;
  self->y = header_h + navbar_h;
  self->w = comp->cols;
  self->h = main_h;
}

static CTUI_WIDGET *main_border_widget;

/* the splash occupies the whole main area by itself (main_split->count ==
 * 1) until at least one of "debug info"/"dump palette"/"kitty image" is
 * checked in the navbar, at which point it shares the area vertically with
 * panels_grid_widget below it (count == 2) -- see
 * main_split_handle_navbar_toggle(). Insets from main_border_widget first
 * (same pattern as header_title_layout()), then hands off to
 * ctui_split_layout() to divide whatever that leaves among the active
 * children and rebind them; re-run automatically by ctui_widget_init() on
 * both startup and resize, since this whole function is main_split_widget's
 * own layout(). */
static void main_split_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_util_inset(self, main_border_widget, ctui_margin_uniform(1));
  ctui_split_layout(self, comp);
}

/* self's current x/y/w/h are already the top (or, once shared, only)
 * split pane main_split's own ctui_split_layout() just assigned -- copied
 * out as the "outer" box so ctui_util_inset() can pad the splash a cell
 * away from its pane's edges on every side, integer margin in, integer
 * margin out, same as every other inset in this app. */
static void splash_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)comp;
  CTUI_WIDGET pane = *self;
  ctui_util_inset(self, &pane, ctui_margin_uniform(1));
}

/* "debug info"/"dump palette"/"kitty image" are independent CTUI_MENU_ITEM
 * checkboxes (see navbar.h) -- any subset of the three can be enabled at
 * once. panels_grid_widget (built in main(), a CTUI_SPLIT_GRID) shows
 * however many of them are currently on, auto-adjusting its own rows x
 * cols as that count changes. These file-scope pointers are how
 * main_split_handle_navbar_toggle() (below) knows which actual widget
 * corresponds to which navbar label, since it only gets main_split_widget
 * itself as `self`, never the navbar's or panel widgets' addresses.
 * Assigned once in main(), right after each widget is built. */
static CTUI_WIDGET *debug_info_widget;
static CTUI_WIDGET *dump_palette_widget;
static CTUI_WIDGET *kitty_image_widget;
static CTUI_WIDGET *panels_grid_widget;

/* set once in main() right after ctui_init() negotiates gfx_mode -- this app
 * requests CTUI_GFX_TRUECOLOR (not CTUI_GFX_KITTY), so ctui_init() never
 * negotiates up to CTUI_GFX_KITTY even on a terminal that supports it (see
 * term.c's ctui_init()); this stays 0 in practice. Kept anyway so
 * kitty_image_widget's cleanup below follows the exact same "only call
 * ctui_gfx_kitty_delete() once CTUI_GFX_KITTY was actually negotiated"
 * contract every other kitty_image consumer has to honor. */
static int kitty_gfx_negotiated;

/* mirrors each panel's CTUI_MENU_ITEM.enabled, indexed the same as
 * panels_children[] passed to ctui_widget_make() for panels_grid in main()
 * -- kept here rather than read back out of the navbar's own widget_data,
 * per "widgets talk through events, never through each other's structs"
 * (see CLAUDE.md); the event payload is the only thing this handler ever
 * looks at. */
static int panels_enabled[3];

/* listens for ("navbar", CTUI_VALUE_CHANGED_EVENT). Reacts to "debug
 * info"/"dump palette"/"kitty image", tracking each one's enabled/disabled
 * state independently and rebuilding panels_grid's active children from
 * scratch every time one changes, in navbar order. Selecting "kitty image"
 * still opens its cell even though this app never negotiates
 * CTUI_GFX_KITTY -- ctui_widget_dispatch_render() falls back to
 * ctui_kitty_image_render()'s plain "[kitty required]" text in that case,
 * exactly like any other widget's ordinary degrade path. */
static int main_split_handle_navbar_toggle(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_SPLIT *main_split = self->widget_data;
  CTUI_SPLIT *grid = panels_grid_widget->widget_data;
  CTUI_VALUE_CHANGED_EVENT_DATA *changed = ev->event_data;

  int *flag;
  if (strcmp(changed->value, "debug info") == 0) {
    flag = &panels_enabled[0];
  } else if (strcmp(changed->value, "dump palette") == 0) {
    flag = &panels_enabled[1];
  } else if (strcmp(changed->value, "kitty image") == 0) {
    flag = &panels_enabled[2];
  } else {
    return 0;
  }

  if (*flag == changed->enabled) {
    return 0;
  }
  *flag = changed->enabled;

  if (flag == &panels_enabled[2] && !changed->enabled && kitty_gfx_negotiated) {
    CTUI_KITTY_IMAGE *img = kitty_image_widget->widget_data;
    ctui_gfx_kitty_delete(img->image_id);
  }

  int n = 0;
  if (panels_enabled[0]) {
    grid->children[n++] = debug_info_widget;
  }
  if (panels_enabled[1]) {
    grid->children[n++] = dump_palette_widget;
  }
  if (panels_enabled[2]) {
    grid->children[n++] = kitty_image_widget;
  }
  grid->count = n;
  main_split->count = n > 0 ? 2 : 1;
  /* re-run right now rather than waiting for the next resize, so whichever
   * panel(s) just appeared/disappeared are (re)bound before the next
   * render */
  main_split_layout(self, main_split->comp);
  ctui_logf(E_INF,
            "[DEMO_ADVANCED:APP] - \"%s\" %s @ tick %d, %d panel(s) now "
            "showing\n",
            changed->value, changed->enabled ? "enabled" : "disabled",
            ctui_tick_advance(), n);
  return 1;
}

static void footer_border_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, navbar_h, main_h, footer_h;
  demo_adv_area_heights(comp->rows, &header_h, &navbar_h, &main_h, &footer_h);
  self->x = 0;
  self->y = header_h + navbar_h + main_h;
  self->w = comp->cols;
  self->h = footer_h;
}

/* only vertically centered -- x stays a fixed inset from the left border */
static void status_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  int header_h, navbar_h, main_h, footer_h;
  demo_adv_area_heights(comp->rows, &header_h, &navbar_h, &main_h, &footer_h);
  int footer_y = header_h + navbar_h + main_h;
  int footer_inner_h = footer_h - 2;
  self->w = 48;
  self->h = 2;
  self->x = 2;
  self->y = (footer_y + 1) + (footer_inner_h - self->h) / 2;
}

int main(void) {
  /* v1: request CTUI_GFX_TRUECOLOR outright rather than CTUI_GFX_KITTY --
   * ctui_init() only ever negotiates *down* from what's requested (see
   * term.c), so this guarantees g_gfx_mode == CTUI_GFX_TRUECOLOR on any
   * truecolor-capable terminal (even a Kitty one) instead of leaving it to
   * whatever the terminal happens to support. Still negotiates down to
   * ANSI256/16 on a terminal that can't do truecolor, same "must still run
   * on a plain terminal" guarantee the original demo makes for
   * CTUI_GFX_KITTY. */
  CTUI_GFX_MODE gfx_mode = CTUI_GFX_TRUECOLOR;
  if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  kitty_gfx_negotiated = (gfx_mode == CTUI_GFX_KITTY);
  ctui_logf(E_INF, "[DEMO_ADVANCED:APP] - startup @ tick %d\n",
            ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  ctui_logf(E_INF, "[DEMO_ADVANCED:APP] - terminal size %dx%d @ tick %d\n",
            cols, rows, ctui_tick_advance());
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  static CTUI_MENU_ITEM items[] = {
      {.label = "debug info", .enabled = 0},
      {.label = "dump palette", .enabled = 0},
      {.label = "kitty image", .enabled = 0},
      {.label = "whatever", .enabled = 0},
      {.label = "1337", .enabled = 0},
      {.label = "quit", .enabled = 0},
  };
  CTUI_NAVBAR navbar_data = {.items = items, .count = 6, .selected = 0};
  CTUI_STATUS status_data = {.text = ""};
  CTUI_LABEL title = {.text = "ctui - demo advanced",
                      .fg = CTUI_COLOR_CYAN,
                      .bg = CTUI_COLOR_DEFAULT};
  CTUI_SPLASH splash_data = ctui_splash_make(SPLASH_SRC_W, SPLASH_SRC_H);

  /* shared by header/main/footer borders -- ctui_border_render only reads
   * it, so one instance is enough even though three separate widgets point
   * at it */
  CTUI_BORDER border_style = ctui_border_make(CTUI_COLOR_WHITE);
  border_style.corner =
      (CTUI_CELL){.ch = '+', .fg = CTUI_COLOR_YELLOW, .bg = CTUI_COLOR_DEFAULT};

  CTUI_WIDGET header_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, header_border_layout);
  header_border_widget = &header_border;
  CTUI_WIDGET header_title = ctui_widget_make(
      0, 0, 0, 0, &title, ctui_label_render, header_title_layout);

  CTUI_WIDGET navbar = ctui_widget_make(0, 0, 0, 0, &navbar_data,
                                        ctui_navbar_render, navbar_layout);

  CTUI_WIDGET main_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, main_border_layout);
  main_border_widget = &main_border;

  /* main area's content: the splash on top, and a grid of however many of
   * "debug info"/"dump palette"/"kitty image" are currently checked in the
   * navbar, revealed below it (see main_split_handle_navbar_toggle()).
   * Neither child widget's own x/y/w/h matter here -- ctui_split_layout()
   * (called from main_split_layout()) overwrites them every time. */
  CTUI_WIDGET splash_widget = ctui_widget_make(
      0, 0, 0, 0, &splash_data, ctui_splash_render, splash_layout);
  CTUI_WIDGET debug_info = ctui_widget_make(0, 0, 0, 0, &gfx_mode,
                                            ctui_debug_info_render, NULL);
  CTUI_WIDGET dump_palette = ctui_widget_make(
      0, 0, 0, 0, &gfx_mode, ctui_dump_palette_render, NULL);
  CTUI_KITTY_IMAGE kitty_image_data = ctui_kitty_image_make(128, 128, 1);
  CTUI_WIDGET kitty_image = ctui_widget_make(
      0, 0, 0, 0, &kitty_image_data, ctui_kitty_image_render, NULL);
  ctui_widget_set_gfx_renderer(&kitty_image, CTUI_GFX_KITTY,
                               ctui_kitty_image_gfx_render);
  debug_info_widget = &debug_info;
  dump_palette_widget = &dump_palette;
  kitty_image_widget = &kitty_image;

  static CTUI_WIDGET *panels_children[3];
  CTUI_SPLIT panels_grid = {
      .mode = CTUI_SPLIT_GRID, .children = panels_children, .count = 0};
  CTUI_WIDGET panels_grid_widget_storage = ctui_widget_make(
      0, 0, 0, 0, &panels_grid, ctui_split_render, ctui_split_layout);
  panels_grid_widget = &panels_grid_widget_storage;

  CTUI_WIDGET *main_split_children[] = {&splash_widget, panels_grid_widget};
  CTUI_SPLIT main_split = {
      .mode = CTUI_SPLIT_V, .children = main_split_children, .count = 1};
  CTUI_WIDGET main_split_widget = ctui_widget_make(
      0, 0, 0, 0, &main_split, ctui_split_render, main_split_layout);

  CTUI_WIDGET footer_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, footer_border_layout);
  CTUI_WIDGET status = ctui_widget_make(0, 0, 0, 0, &status_data,
                                        ctui_status_render, status_layout);

  /* main_split_widget takes the splash's old top-level slot -- splash/
   * debug_info/dump_palette/kitty_image are all reached through it (see
   * main_split_layout()/ctui_split_render()), so none of them appear here
   * directly or they'd get bound twice against stale (0,0,0,0) geometry */
  CTUI_WIDGET *widgets[] = {
      &header_border, &header_title,     &navbar,  &main_border,
      &main_split_widget, &footer_border, &status,
  };
  CTUI_APP app;
  if (ctui_app_init(&app, widgets, 7, rows, cols) != 0) {
    fprintf(stderr, "failed to init ctui app\n");
    return 1;
  }

  /* addEventListener()-style wiring: navbar reacts to raw keypresses (the
   * "input" source ctui_input_loop() emits under). status and
   * main_split_widget both listen for navbar's value-changed events -- two
   * independent handlers on the same (source, type) key -- neither knowing
   * anything about how the other reacts. */
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &navbar,
                      ctui_navbar_handle_keypress);
  ctui_event_register("navbar", CTUI_VALUE_CHANGED_EVENT, &status,
                      ctui_status_handle_value_changed);
  ctui_event_register("navbar", CTUI_VALUE_CHANGED_EVENT, &main_split_widget,
                      main_split_handle_navbar_toggle);

  ctui_logf(E_INF,
            "[DEMO_ADVANCED:APP] - widgets wired (header, navbar, main, "
            "footer) @ tick %d, entering event loop\n",
            ctui_tick_advance());
  ctui_app_run(&app, screen, 0);
  ctui_logf(E_INF, "[DEMO_ADVANCED:APP] - event loop exited @ tick %d\n",
            ctui_tick_advance());

  ctui_kitty_image_free(&kitty_image_data);
  ctui_splash_free(&splash_data);
  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
