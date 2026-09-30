/* CTUI_FORM (widgets/form.h): the rows and their controls, focus moving
 * past headings and disabled rows, each control's keys and clicks, a
 * slider drag, scrolling, and the events. */
#include "ctui.h"
#include "widgets/form.h"

#include "ctui_test.h"

#include <stdio.h>
#include <string.h>

static char got[32];
static int got_row = -1;

static int on_value(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_VALUE_CHANGED_EVENT_DATA *d = ev->event_data;
  snprintf(got, sizeof got, "%s", d->value);
  got_row = d->enabled;
  return 0;
}

static void mouse(CTUI_APP *app, CTUI_SCREEN *screen, CTUI_MOUSE_ACTION a,
                  int button, int row, int col) {
  CTUI_MOUSE_EVENT_DATA m = {.action = a, .button = button, .row = row,
                             .col = col};
  CTUI_EVENT ev = {.type = CTUI_MOUSE_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "input",
                   .event_data = &m};
  if (ctui_handle_event(&ev)) {
    ctui_app_render(app, screen);
  }
}


/* a style's control hook: '#' over the cells, what was asked kept; it
 * declines what `declines` names */
static CTUI_CONTROL asked[8];
static int asked_cols[8], asked_n, declines = -1;

static int draw_control(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                        int col, int cols, const CTUI_CONTROL *c,
                        unsigned char bg, void *arg) {
  if ((int)c->kind == declines) {
    return 0;
  }
  if (asked_n < 8) {
    asked[asked_n] = *c;
    asked_cols[asked_n++] = cols;
  }
  for (int i = 0; i < cols; i++) {
    ctui_widget_putc(self, comp, row, col + i, '#', *(unsigned char *)arg, bg);
  }
  return 1;
}

int main(void) {
  ctui_log_init(E_ALL);
  int rows = 10, cols = 50;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  char layout[16] = "";
  CTUI_ENTRY entry = {.buf = layout, .cap = sizeof layout};
  ctui_entry_set(&entry, "de");
  static const char *const profiles[] = {"flat", "adaptive"};
  CTUI_FORM_ROW frows[] = {
      {.kind = CTUI_FORM_HEADING, .label = "Keyboard"},
      {.kind = CTUI_FORM_ENTRY, .label = "Layout", .entry = &entry},
      {.kind = CTUI_FORM_SLIDER, .label = "Repeat rate", .value = 40,
       .min = 0, .max = 100, .step = 5, .hint = "keys/s"},
      {.kind = CTUI_FORM_TOGGLE, .label = "Locked", .disabled = 1},
      {.kind = CTUI_FORM_TOGGLE, .label = "Natural scroll"},
      {.kind = CTUI_FORM_CHOICE, .label = "Accel profile",
       .options = profiles, .option_count = 2},
      {.kind = CTUI_FORM_BUTTON, .label = "Apply"},
  };
  CTUI_FORM f = {.rows = frows, .count = 7, .drag = -1};
  /* at (0, 1), 40 x 7: labels 14 wide -> controls at 2 + 14 + 2 = 18 */
  CTUI_WIDGET w = ctui_widget_make(0, 1, 40, 7, &f, ctui_form_render, NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w,
                      ctui_form_handle_keypress);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &w, ctui_form_handle_mouse);
  ctui_event_register("form", CTUI_VALUE_CHANGED_EVENT, &w, on_value);

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(f.focus == 1, "the first row taking focus has it");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "Keyboard") &&
                       ctui_test_row_contains(screen, 2, "› Layout") &&
                       ctui_test_cell(screen, 2, 18) == 'd',
                   "heading, focused label, the entry at the control column");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 3, "━━●───── 40  keys/s"),
                   "a slider's value and hint");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 5, "[ ]") &&
                       ctui_test_row_contains(screen, 6, "‹ flat ›") &&
                       ctui_test_row_contains(screen, 7, "[ Apply ]"),
                   "toggle, choice, button");

  ctui_test_key(&app, screen, CTUI_KEY_CHAR, 'x');
  CTUI_TEST_ASSERT(!strcmp(layout, "dex") && !strcmp(got, "changed") &&
                       got_row == 1,
                   "typing edits the entry, reported as changed");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!strcmp(got, "pressed"), "enter in an entry presses");

  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  ctui_test_key(&app, screen, CTUI_KEY_RIGHT, 0);
  CTUI_TEST_ASSERT(frows[2].value == 45 && !strcmp(got, "changed") &&
                       got_row == 2,
                   "right moves a slider a step");
  ctui_test_key(&app, screen, CTUI_KEY_END, 0);
  CTUI_TEST_ASSERT(frows[2].value == 100, "end: its maximum");
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_RIGHT, 0),
                   "no further");

  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(f.focus == 4, "down skips the disabled row (%d)", f.focus);
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, ' ');
  CTUI_TEST_ASSERT(frows[4].value == 1 &&
                       ctui_test_row_contains(screen, 5, "[x]"),
                   "space flips a toggle");
  ctui_test_key(&app, screen, CTUI_KEY_TAB, 0);
  ctui_test_key(&app, screen, CTUI_KEY_LEFT, 0);
  CTUI_TEST_ASSERT(frows[5].value == 1 &&
                       ctui_test_row_contains(screen, 6, "‹ adaptive ›"),
                   "left steps a choice (wrapping)");
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!strcmp(got, "pressed") && got_row == 6,
                   "enter presses a button");
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(f.focus == 1, "down from the last wraps to the first");
  ctui_test_key(&app, screen, CTUI_KEY_UP, 0);
  CTUI_TEST_ASSERT(f.focus == 6, "up from the first wraps to the last");

  mouse(&app, screen, CTUI_MOUSE_PRESS, 0, 5, 19);
  CTUI_TEST_ASSERT(f.focus == 4 && frows[4].value == 0 &&
                       !strcmp(got, "changed"),
                   "a click on a toggle focuses and flips it");
  mouse(&app, screen, CTUI_MOUSE_PRESS, 0, 6, 25);
  CTUI_TEST_ASSERT(frows[5].value == 0, "a click on a choice steps it");
  mouse(&app, screen, CTUI_MOUSE_PRESS, 0, 3, 18);
  CTUI_TEST_ASSERT(frows[2].value == 0 && f.drag == 2,
                   "a click on a slider's start: its minimum, dragging");
  /* bar = 40 - 18 - 6 - 8 (the hint) = 8 columns */
  mouse(&app, screen, CTUI_MOUSE_MOTION, 0, 8, 45);
  CTUI_TEST_ASSERT(frows[2].value == 100,
                   "a drag follows past the end (%d)", frows[2].value);
  mouse(&app, screen, CTUI_MOUSE_RELEASE, 0, 8, 45);
  mouse(&app, screen, CTUI_MOUSE_MOTION, 0, 3, 18);
  CTUI_TEST_ASSERT(frows[2].value == 100 && f.drag == -1,
                   "the release ends the drag");
  mouse(&app, screen, CTUI_MOUSE_PRESS, 0, 2, 19);
  CTUI_TEST_ASSERT(f.focus == 1 && entry.cursor == 1,
                   "a click in an entry places its cursor");
  mouse(&app, screen, CTUI_MOUSE_PRESS, 0, 4, 19);
  CTUI_TEST_ASSERT(f.focus == 1, "a disabled row takes no click");
  mouse(&app, screen, CTUI_MOUSE_PRESS, 0, 7, 20);
  CTUI_TEST_ASSERT(!strcmp(got, "pressed") && got_row == 6,
                   "a click presses a button");

  w.h = 3; /* rows 1-3 of 7 */
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(f.scroll == 4 && ctui_test_row_contains(screen, 3, "Apply"),
                   "short: scrolled to the focus (%d)", f.scroll);
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(f.scroll == 0 &&
                       ctui_test_row_contains(screen, 1, "Keyboard"),
                   "back at the top, its heading comes along");

  /* the style's control hook: toggles and sliders go to it first */
  unsigned char ink = CTUI_COLOR_WHITE;
  CTUI_STYLE hooked = ctui_style_default;
  hooked.control = draw_control;
  hooked.control_arg = &ink;
  f.style = &hooked;
  w.h = 8;
  frows[2].value = 45;
  frows[4].value = 1;
  ctui_form_focus(&f, 4);
  f.scroll = 0; /* the page was 3 rows until the next render */
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(asked_n == 3 && asked[0].kind == CTUI_CONTROL_SLIDER &&
                       asked[0].value == 45 && asked[0].max == 100 &&
                       asked_cols[0] == 8 && !asked[0].focused &&
                       asked[1].kind == CTUI_CONTROL_TOGGLE &&
                       asked[1].disabled && asked[1].value == 0 &&
                       asked_cols[1] == 3 && asked[2].value == 1 &&
                       asked[2].max == 1 && asked[2].focused &&
                       asked[2].kind == CTUI_CONTROL_TOGGLE,
                   "a hook: asked for the slider (45 of 100, 8 cells) and "
                   "each toggle (3 cells; disabled, focused) (%d)", asked_n);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 3, "######## 45  keys/s") &&
                       ctui_test_row_contains(screen, 5, "###") &&
                       !ctui_test_row_contains(screen, 5, "[x]") &&
                       ctui_test_row_contains(screen, 6, "‹ flat ›"),
                   "... it drew them; the number, the hint and the other "
                   "controls as before");
  mouse(&app, screen, CTUI_MOUSE_PRESS, 0, 3, 18 + 7);
  mouse(&app, screen, CTUI_MOUSE_RELEASE, 0, 3, 18 + 7);
  CTUI_TEST_ASSERT(frows[2].value == 100,
                   "a click on a drawn slider lands as on the glyphs (%d)",
                   frows[2].value);
  declines = CTUI_CONTROL_TOGGLE;
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 5, "[x]") &&
                       ctui_test_row_contains(screen, 3, "########"),
                   "a hook that declines a control leaves it to the glyphs");

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
