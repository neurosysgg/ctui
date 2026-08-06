#include "ctui.h"
#include "logger.h"
#include "widgets/border.h"
#include "widgets/debug_info.h"
#include "widgets/dump_palette.h"
#include "widgets/kitty_image.h"
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
 * the box is one pane tall (7 rows) until at least one of "debug info"/
 * "dump palette"/"kitty image" is checked, then two (14, capped to
 * whatever the main area's inner region actually has -- see main_inner_h
 * below) once main_split's count grows to 2 -- see
 * main_split_handle_panel_toggle(). That second pane is itself a
 * CTUI_SPLIT_GRID (panels_grid, built in main()) auto-dividing that one
 * 7-row band among however many of the three are actually checked.
 * ctui_split_layout() at the end handles dividing whatever area this
 * computes among the active children and rebinding them; it's re-run
 * automatically by ctui_widget_init() (both at startup and on resize)
 * since this whole function is registered as the split widget's
 * layout(). */
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

/* "debug info"/"dump palette"/"kitty image" are independent CTUI_MENU_ITEM
 * checkboxes (see menu.h) -- any subset of the three can be enabled at
 * once. panels_grid_widget (built in main(), a CTUI_SPLIT_GRID) is what
 * actually shows however many of them are currently on, auto-adjusting
 * its own rows x cols as that count changes (see ctui_split_grid_cols()
 * in core/split.c) -- 1 enabled fills the whole area, 2 go side by side,
 * 3 form a 2x2 grid with one empty cell, etc. These file-scope pointers
 * are how main_split_handle_panel_toggle() (below) knows which actual
 * widget corresponds to which menu label, since it only gets
 * main_split_widget itself as `self`, never the menu's or panel widgets'
 * addresses. Assigned once in main(), right after each widget is built.
 *
 * kitty_image_widget is a genuinely ordinary CTUI_SPLIT_GRID child, same
 * as the other two -- it isn't special-cased in the grid itself. That's
 * the point of ctui_widget_dispatch_render() (core/widget.c):
 * ctui_split_render() routes every child through it instead of calling
 * render() directly, so a Phase 4 widget (ctui_widget_set_gfx_renderer(),
 * see main()) gets its gfx_render dispatched correctly no matter how deep
 * it's nested -- no independent top-level positioning hack needed, unlike
 * an earlier pass at this integration (see docs/protocol.md's
 * "Integrating a non-degradable protocol into a real app" for that
 * history and why the delegation layer replaced it). It does need its own
 * explicit cleanup on the way out, though -- see panels_enabled[] and
 * ctui_gfx_kitty_delete() below. */
static CTUI_WIDGET *debug_info_widget;
static CTUI_WIDGET *dump_palette_widget;
static CTUI_WIDGET *kitty_image_widget;
static CTUI_WIDGET *panels_grid_widget;

/* set once in main() right after ctui_init() negotiates gfx_mode down (or
 * not) -- ctui_gfx_kitty_display() is only ever actually reached via
 * ctui_widget_dispatch_render()'s own CTUI_GFX_KITTY check (see above), so
 * a kitty image was only ever really transmitted if this is true.
 * ctui_gfx_kitty_delete()'s doc comment puts the same "only call this once
 * CTUI_GFX_KITTY was actually negotiated" burden on its caller as
 * ctui_gfx_kitty_display() -- main_split_handle_panel_toggle() is that
 * caller here, so it's the one that has to check. */
static int kitty_gfx_negotiated;

/* mirrors each panel's CTUI_MENU_ITEM.enabled, indexed the same as
 * panels_children[] passed to ctui_widget_make() for panels_grid in
 * main() -- kept here rather than read back out of the menu's own
 * widget_data, per the "widgets talk through events, never through each
 * other's structs" rule (see CLAUDE.md); the event payload is the only
 * thing this handler ever looks at. */
static int panels_enabled[3];

/* listens for ("menu", CTUI_VALUE_CHANGED_EVENT) -- same event status
 * listens for, just a second, independent handler on the same key,
 * demonstrating that the registry supports more than one listener per
 * (source, type). Reacts to "debug info"/"dump palette"/"kitty image",
 * tracking each one's enabled/disabled state independently (not
 * mutually exclusive) and rebuilds panels_grid's active children from
 * scratch every time one changes, in menu order. Selecting "kitty image"
 * on a terminal that didn't negotiate CTUI_GFX_KITTY still opens its
 * cell -- ctui_widget_dispatch_render() falls back to
 * ctui_kitty_image_render()'s plain "[kitty required]" text in that
 * case, exactly like any other widget's ordinary degrade path. */
static int main_split_handle_panel_toggle(CTUI_WIDGET *self, CTUI_EVENT *ev) {
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

  /* kitty_image_widget's content is a raster overlay the terminal keeps
   * showing independent of the text grid -- unlike debug_info/
   * dump_palette, dropping it from the grid doesn't erase it on its own
   * (see ctui_gfx_kitty_delete()'s doc comment), so it has to be told
   * explicitly whenever it's the one being turned off. */
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
  /* re-run right now rather than waiting for the next resize, so
   * whichever panel(s) just appeared/disappeared are (re)bound before
   * the next render -- cascades into panels_grid_widget's own
   * ctui_split_layout() (its .layout callback, see main()), which
   * re-partitions its cells for the new count */
  main_split_layout(self, main_split->comp);
  ctui_logf(E_INF,
            "[DEMO:APP] - \"%s\" %s @ tick %d, %d panel(s) now showing\n",
            changed->value, changed->enabled ? "enabled" : "disabled",
            ctui_tick_advance(), n);
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
  /* demo asks for the richest tier (KITTY, which implies TRUECOLOR/256/16
   * -- see ctui_gfx_detect_caps()), but must still run on a plain
   * terminal -- ctui_init() negotiates down instead of failing if the
   * terminal can't do it, rewriting gfx_mode to whatever it actually
   * granted (see ctui_dump_palette_render()/ctui_debug_info_render()'s
   * use of it below, via widget_data). Only the mandatory ANSI16 floor
   * itself can fail this call; kitty_image below needs no such check at
   * all, since ctui_widget_dispatch_render() degrades it automatically. */
  CTUI_GFX_MODE gfx_mode = CTUI_GFX_KITTY;
  if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  kitty_gfx_negotiated = (gfx_mode == CTUI_GFX_KITTY);
  ctui_logf(E_INF, "[DEMO:APP] - startup @ tick %d\n", ctui_tick_advance());

  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  ctui_logf(E_INF, "[DEMO:APP] - terminal size %dx%d @ tick %d\n", cols, rows,
            ctui_tick_advance());
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  static CTUI_MENU_ITEM items[] = {
      {.label = "debug info", .enabled = 0},
      {.label = "dump palette", .enabled = 0},
      {.label = "kitty image", .enabled = 0},
      {.label = "whatever", .enabled = 0},
      {.label = "1337", .enabled = 0},
      {.label = "quit", .enabled = 0},
  };
  CTUI_MENU data = {.items = items, .count = 6, .selected = 0};
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

  /* main area's content: a menu on top, and a grid of however many of
   * "debug info"/"dump palette"/"kitty image" are currently checked,
   * revealed below it (see main_split_handle_panel_toggle()). Neither
   * child widget's own x/y/w/h matter here -- ctui_split_layout() (called
   * from main_split_layout()) overwrites them every time. debug_info/
   * dump_palette/kitty_image are built here but assigned to their
   * file-scope pointers (declared above main_split_handle_panel_toggle())
   * so that handler can identify them; they stay alive because main()'s
   * stack frame lives for the whole program, same as every other widget
   * below. */
  CTUI_WIDGET menu = ctui_widget_make(0, 0, 0, 0, &data, ctui_menu_render, NULL);
  CTUI_WIDGET debug_info = ctui_widget_make(0, 0, 0, 0, &gfx_mode,
                                            ctui_debug_info_render, NULL);
  CTUI_WIDGET dump_palette = ctui_widget_make(
      0, 0, 0, 0, &gfx_mode, ctui_dump_palette_render, NULL);
  /* built unconditionally, same as debug_info/dump_palette above -- no
   * "was CTUI_GFX_KITTY actually negotiated?" check needed here at all.
   * It's never in the top-level widgets[] array below (only reachable
   * via panels_grid's children, exactly like debug_info/dump_palette),
   * so ctui_app_init()'s Phase 4 validation never sees it and has
   * nothing to hard-fail; on a terminal that didn't negotiate
   * CTUI_GFX_KITTY, ctui_widget_dispatch_render() just calls its
   * ordinary ctui_kitty_image_render() instead, same as any other
   * widget's plain degrade path. */
  CTUI_KITTY_IMAGE kitty_image_data = ctui_kitty_image_make(128, 128, 1);
  CTUI_WIDGET kitty_image = ctui_widget_make(
      0, 0, 0, 0, &kitty_image_data, ctui_kitty_image_render, NULL);
  ctui_widget_set_gfx_renderer(&kitty_image, CTUI_GFX_KITTY,
                               ctui_kitty_image_gfx_render);
  debug_info_widget = &debug_info;
  dump_palette_widget = &dump_palette;
  kitty_image_widget = &kitty_image;

  /* holds whichever of debug_info/dump_palette/kitty_image are currently
   * enabled, packed into children[0..count-1] by
   * main_split_handle_panel_toggle() -- capacity 3 covers all of them at
   * once, the grid's actual shape auto-adjusting to count (see
   * CTUI_SPLIT_GRID, core/split.h). .layout = ctui_split_layout (rather
   * than NULL, like every other main_split child) is what makes this a
   * composable nested split: ctui_widget_init() (called on this widget by
   * main_split's own ctui_split_layout(), inside main_split_layout())
   * runs it automatically once this widget's own x/y/w/h are set, which
   * in turn re-partitions and (re)binds panels_children[0..count-1]. */
  static CTUI_WIDGET *panels_children[3];
  CTUI_SPLIT panels_grid = {
      .mode = CTUI_SPLIT_GRID, .children = panels_children, .count = 0};
  CTUI_WIDGET panels_grid_widget_storage = ctui_widget_make(
      0, 0, 0, 0, &panels_grid, ctui_split_render, ctui_split_layout);
  panels_grid_widget = &panels_grid_widget_storage;

  CTUI_WIDGET *main_split_children[] = {&menu, panels_grid_widget};
  CTUI_SPLIT main_split = {
      .mode = CTUI_SPLIT_V, .children = main_split_children, .count = 1};
  CTUI_WIDGET main_split_widget = ctui_widget_make(
      0, 0, 0, 0, &main_split, ctui_split_render, main_split_layout);

  CTUI_WIDGET footer_border = ctui_widget_make(
      0, 0, 0, 0, &border_style, ctui_border_render, footer_border_layout);
  CTUI_WIDGET status = ctui_widget_make(0, 0, 0, 0, &status_data,
                                        ctui_status_render, status_layout);

  /* main_split_widget takes the menu's old slot in the top-level widget
   * list -- menu/debug_info/dump_palette/kitty_image are all reached
   * through it (see main_split_layout()/ctui_split_render()), so none of
   * them appear here directly or they'd get bound twice against stale
   * (0,0,0,0) geometry */
  CTUI_WIDGET *widgets[] = {
      &header_border,     &header_title, &main_border,
      &main_split_widget, &footer_border, &status,
  };
  CTUI_APP app;
  if (ctui_app_init(&app, widgets, 6, rows, cols) != 0) {
    fprintf(stderr, "failed to init ctui app\n");
    return 1;
  }

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

  ctui_kitty_image_free(&kitty_image_data);
  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
