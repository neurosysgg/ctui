/* CTUI_DIALOG (widgets/dialog.h): the box (centred, fitted to its parts),
 * buttons by key and by click, cancel, an entry inside, a progress bar,
 * and the events. */
#include "ctui.h"
#include "widgets/dialog.h"

#include "ctui_test.h"

#include <stdio.h>
#include <string.h>

static char got[32];
static int got_index = 99;

static int on_value(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_VALUE_CHANGED_EVENT_DATA *d = ev->event_data;
  snprintf(got, sizeof got, "%s", d->value ? d->value : "(null)");
  got_index = d->enabled;
  return 0;
}

static void click(CTUI_APP *app, CTUI_SCREEN *screen, int row, int col) {
  CTUI_MOUSE_EVENT_DATA m = {
      .action = CTUI_MOUSE_PRESS, .button = 0, .row = row, .col = col};
  CTUI_EVENT ev = {.type = CTUI_MOUSE_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "input",
                   .event_data = &m};
  if (ctui_handle_event(&ev)) {
    ctui_app_render(app, screen);
  }
}

static int find_row(CTUI_SCREEN *screen, const char *s) {
  for (int r = 0; r < screen->rows; r++) {
    if (ctui_test_row_contains(screen, r, s)) {
      return r;
    }
  }
  return -1;
}

static int find_col(CTUI_SCREEN *screen, int row, uint32_t ch) {
  for (int c = 0; c < screen->cols; c++) {
    if (ctui_test_cell(screen, row, c) == ch) {
      return c;
    }
  }
  return -1;
}

/* a hook taking the kinds drawn under text, what it was asked kept */
static CTUI_CONTROL under_asked[8];
static int under_cols[8], under_row[8], under_col[8], under_n;

static int draw_under(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                      int col, int cols, const CTUI_CONTROL *c,
                      unsigned char bg, void *arg) {
  (void)self, (void)comp, (void)bg, (void)arg;
  if (c->kind < CTUI_CONTROL_BUTTON || under_n == 8) {
    return 0;
  }
  under_asked[under_n] = *c;
  under_row[under_n] = row;
  under_col[under_n] = col;
  under_cols[under_n++] = cols;
  return 1;
}

/* a style's control hook: '=' over the bar's cells */
static CTUI_CONTROL progress_asked;

static int draw_progress(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                         int col, int cols, const CTUI_CONTROL *c,
                         unsigned char bg, void *arg) {
  (void)arg;
  if (c->kind != CTUI_CONTROL_PROGRESS) {
    return 0;
  }
  progress_asked = *c;
  for (int i = 0; i < cols; i++) {
    ctui_widget_putc(self, comp, row, col + i, '=', CTUI_COLOR_WHITE, bg);
  }
  return 1;
}

int main(void) {
  ctui_log_init(E_ALL);
  int rows = 16, cols = 40;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  CTUI_DIALOG d = {.title = "Delete",
                   .text = "Delete 3 files for good? This can't be undone.",
                   .buttons = {"Delete", "Cancel"},
                   .button_count = 2,
                   .focus = 1,
                   .progress = -1,
                   .width = 24};
  CTUI_WIDGET w = ctui_widget_make(0, 0, cols, rows, &d, ctui_dialog_render,
                                   NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w,
                      ctui_dialog_handle_keypress);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &w, ctui_dialog_handle_mouse);
  ctui_event_register("dialog", CTUI_VALUE_CHANGED_EVENT, &w, on_value);

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(find_row(screen, "Delete") < 0, "closed: nothing drawn");
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0),
                   "closed: keys aren't taken");

  d.open = 1;
  ctui_app_render(&app, screen);
  /* 24 inside + 4 = 28 wide at (40-28)/2 = 6; text 2 lines + blank +
   * buttons + frame = 6 high at (16-6)/2 = 5 */
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 5, 6) == 0x256d &&
                       ctui_test_cell(screen, 10, 33) == 0x256f,
                   "the frame is centred and fitted");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 5, " Delete "),
                   "the title sits in the top edge");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 6, "Delete 3 files for good?") &&
                       ctui_test_row_contains(screen, 7, "This can't be undone."),
                   "the text wraps inside");
  int br = find_row(screen, " Cancel ");
  CTUI_TEST_ASSERT(br == 9, "buttons on the last inner row (%d)", br);
  int cc = find_col(screen, br, 'C');
  CTUI_TEST_ASSERT(cc >= 0 && screen->cells[br * cols + cc].bg ==
                                  CTUI_COLOR_CYAN,
                   "the focused button is highlighted");

  ctui_test_key(&app, screen, CTUI_KEY_TAB, 0);
  CTUI_TEST_ASSERT(d.focus == 0, "tab moves the focus (wrapping)");
  ctui_test_key(&app, screen, CTUI_KEY_LEFT, 0);
  CTUI_TEST_ASSERT(d.focus == 1, "so does left without an entry");
  CTUI_TEST_ASSERT(ctui_test_key(&app, screen, CTUI_KEY_CHAR, 'x'),
                   "any key counts as handled while open");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!d.open && !strcmp(got, "Cancel") && got_index == 1,
                   "enter picks the focused button and closes");

  d.open = 1;
  ctui_app_render(&app, screen);
  int dc = find_col(screen, br, 'D');
  click(&app, screen, br, dc);
  CTUI_TEST_ASSERT(!d.open && !strcmp(got, "Delete") && got_index == 0,
                   "a click picks a button (%s)", got);

  d.open = 1;
  ctui_test_key(&app, screen, CTUI_KEY_ESC, 0);
  CTUI_TEST_ASSERT(!d.open && !strcmp(got, "(null)") && got_index == -1,
                   "esc cancels");

  char buf[32] = "";
  CTUI_ENTRY e = {.buf = buf, .cap = sizeof buf};
  ctui_entry_set(&e, "old.txt");
  CTUI_DIALOG r = {.open = 1,
                   .title = "Rename",
                   .buttons = {"Rename", "Cancel"},
                   .button_count = 2,
                   .progress = -1,
                   .entry = &e,
                   .width = 20};
  w.widget_data = &r;
  ctui_app_render(&app, screen);
  /* entry + blank + buttons + frame = 5 rows at 5 */
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 6, "old.txt"),
                   "the entry's line");
  ctui_test_key(&app, screen, CTUI_KEY_LEFT, 0);
  CTUI_TEST_ASSERT(r.focus == 0 && e.cursor == 6,
                   "left edits the entry, not the buttons");
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, 'X');
  CTUI_TEST_ASSERT(!strcmp(buf, "old.txXt"), "typing goes to the entry");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!strcmp(got, "Rename"), "enter picks with an entry too");

  CTUI_DIALOG p = {.open = 1, .title = "Copying", .text = "a.iso",
                   .progress = 50, .width = 20};
  w.widget_data = &p;
  ctui_app_render(&app, screen);
  int pr = find_row(screen, "50%");
  CTUI_TEST_ASSERT(pr >= 0, "the bar shows its percentage");
  int full = 0, empty = 0;
  for (int c = 0; c < cols; c++) {
    full += ctui_test_cell(screen, pr, c) == 0x2588;
    empty += ctui_test_cell(screen, pr, c) == 0x2591;
  }
  CTUI_TEST_ASSERT(full == 7 && empty == 8, "half full (%d, %d)", full, empty);
  CTUI_STYLE hooked = ctui_style_default;
  hooked.control = draw_progress;
  p.style = &hooked;
  ctui_app_render(&app, screen);
  int drawn = 0;
  for (int c = 0; c < cols; c++) {
    drawn += ctui_test_cell(screen, pr, c) == '=';
  }
  CTUI_TEST_ASSERT(progress_asked.kind == CTUI_CONTROL_PROGRESS &&
                       progress_asked.value == 50 &&
                       progress_asked.max == 100 && drawn == 15 &&
                       ctui_test_row_contains(screen, pr, "50%"),
                   "a style's control hook draws the bar (%d cells), the "
                   "percentage stays", drawn);
  p.style = NULL;
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0) ||
                       p.open,
                   "no buttons: enter picks nothing");
  ctui_test_key(&app, screen, CTUI_KEY_ESC, 0);
  CTUI_TEST_ASSERT(!p.open, "esc still cancels it");

  /* drawn under the text: the frame, the entry's field, the buttons */
  char nb[32] = "";
  CTUI_ENTRY ne = {.buf = nb, .cap = sizeof nb};
  CTUI_STYLE under = ctui_style_default;
  under.control = draw_under;
  ne.style = &under;
  CTUI_DIALOG u = {.open = 1, .title = "Name", .entry = &ne, .buttons = {"OK", "Cancel"},
                   .button_count = 2, .width = 20, .style = &under};
  w.widget_data = &u;
  ctui_app_render(&app, screen);
  int frames = 0, fields = 0, buttons = 0, focused = 0, glyphs = 0;
  for (int i = 0; i < under_n; i++) {
    const CTUI_CONTROL *c = &under_asked[i];
    frames += c->kind == CTUI_CONTROL_FRAME && c->rows == 7 &&
              under_cols[i] == 24 && c->label == 2 && c->label_cols == 6;
    fields += c->kind == CTUI_CONTROL_FIELD && under_cols[i] == 20;
    buttons += c->kind == CTUI_CONTROL_BUTTON;
    focused += c->kind == CTUI_CONTROL_BUTTON && c->focused;
  }
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      uint32_t ch = ctui_test_cell(screen, r, c);
      glyphs += ch == 0x2500 || ch == 0x2502 || ch == 0x256d;
    }
  }
  int ur = find_row(screen, "Cancel");
  CTUI_TEST_ASSERT(frames == 1 && fields == 1 && buttons == 2 &&
                       focused == 1 && glyphs == 0 &&
                       find_row(screen, " Name ") >= 0 && ur >= 0 &&
                       ctui_test_row_contains(screen, ur, " OK    Cancel "),
                   "a hook taking them: one frame (7 rows, 24 wide, the "
                   "title's opening), the entry's field, two buttons (one "
                   "focused); no box drawing, the labels plain (%d %d %d "
                   "%d %d)",
                   frames, fields, buttons, focused, glyphs);
  ctui_test_key(&app, screen, CTUI_KEY_ESC, 0);

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
