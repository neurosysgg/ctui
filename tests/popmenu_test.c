/* CTUI_POPMENU (widgets/popmenu.h): labels and their letters, the level
 * placed at its point and kept on screen, submenus cascading, the keys,
 * the mouse (hover, release, a press outside), the frame hook, and the
 * events. */
#include "ctui.h"
#include "widgets/popmenu.h"

#include "ctui_test.h"

#include <stdio.h>
#include <string.h>

static char got[32];
static int got_act = 99, got_n;

static int on_value(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_VALUE_CHANGED_EVENT_DATA *d = ev->event_data;
  snprintf(got, sizeof got, "%s", d->value ? d->value : "(null)");
  got_act = d->enabled;
  got_n++;
  return 0;
}

static int mouse(CTUI_APP *app, CTUI_SCREEN *screen, CTUI_MOUSE_ACTION a,
                 int row, int col) {
  CTUI_MOUSE_EVENT_DATA m = {.action = a,
                             .button = a == CTUI_MOUSE_MOTION ? -1 : 2,
                             .row = row,
                             .col = col};
  CTUI_EVENT ev = {.type = CTUI_MOUSE_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "input",
                   .event_data = &m};
  int changed = ctui_handle_event(&ev);
  ctui_app_render(app, screen);
  return changed;
}

static int find_row(CTUI_SCREEN *screen, const char *s) {
  for (int r = 0; r < screen->rows; r++) {
    if (ctui_test_row_contains(screen, r, s)) {
      return r;
    }
  }
  return -1;
}

static const CTUI_CELL *cell(CTUI_SCREEN *screen, int row, int col) {
  return &screen->cells[row * screen->cols + col];
}

/* a hook drawing the frame (what it was asked kept) */
static CTUI_CONTROL frame_asked;
static int frame_cols, frames;

static int draw_frame(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row,
                      int col, int cols, const CTUI_CONTROL *c,
                      unsigned char bg, void *arg) {
  (void)self, (void)comp, (void)row, (void)col, (void)bg, (void)arg;
  if (c->kind != CTUI_CONTROL_FRAME) {
    return 0;
  }
  frame_asked = *c;
  frame_cols = cols;
  frames++;
  return 1;
}

static void test_label(void) {
  char s[32];
  int letter;
  ctui_popmenu_label("&Play", s, sizeof s, &letter);
  CTUI_TEST_ASSERT(strcmp(s, "Play") == 0 && letter == 0,
                   "'&Play': Play, its P");
  ctui_popmenu_label("Send &to", s, sizeof s, &letter);
  CTUI_TEST_ASSERT(strcmp(s, "Send to") == 0 && letter == 5,
                   "'Send &to': its t at 5");
  ctui_popmenu_label("R&&B", s, sizeof s, &letter);
  CTUI_TEST_ASSERT(strcmp(s, "R&B") == 0 && letter == -1,
                   "'&&' an '&', no letter");
  ctui_popmenu_label("trailing&", s, sizeof s, &letter);
  CTUI_TEST_ASSERT(strcmp(s, "trailing&") == 0 && letter == -1,
                   "a last '&' kept as it is");
  ctui_popmenu_label(NULL, s, sizeof s, &letter);
  CTUI_TEST_ASSERT(s[0] == '\0' && letter == -1, "no label: empty");
}

static const CTUI_POPMENU_ITEM SUB[] = {
    {.label = "&Rock", .act = 10, .arg = 0},
    {.label = "&Jazz",
     .act = 10,
     .arg = 1,
     .flags = CTUI_POPMENU_CHECKED | CTUI_POPMENU_RADIO},
};

static const CTUI_POPMENU_ITEM ITEMS[] = {
    {.label = "&Play", .keys = "Enter", .act = 1},
    {.label = "&Queue", .keys = "q", .act = 2, .flags = CTUI_POPMENU_CHECKED},
    {.flags = CTUI_POPMENU_SEP},
    {.label = "Send &to", .act = 3, .sub = SUB, .sub_count = 2},
    {.label = "&Remove", .act = 4, .flags = CTUI_POPMENU_OFF},
    {.label = "C&rop", .act = 5},
    {.label = "Sc&an", .act = 6},
    {.label = "Sh&ow", .act = 7},
};
#define N_ITEMS ((int)(sizeof ITEMS / sizeof ITEMS[0]))

int main(void) {
  ctui_log_init(E_ALL);
  test_label();

  int rows = 20, cols = 60;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  CTUI_POPMENU m = {0};
  CTUI_WIDGET w =
      ctui_widget_make(0, 0, cols, rows, &m, ctui_popmenu_render, NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w,
                      ctui_popmenu_handle_keypress);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &w, ctui_popmenu_handle_mouse);
  ctui_event_register("popmenu", CTUI_VALUE_CHANGED_EVENT, &w, on_value);

  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(find_row(screen, "Play") < 0, "closed: nothing drawn");
  CTUI_TEST_ASSERT(!ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0),
                   "closed: keys aren't taken");

  /* the frame, room, "✓ ", the widest label (7), a gap of 3, the keys
   * (5), the arrow's 2: 4 + 2 + 7 + 3 + 5 + 2 = 23 wide, 8 + 2 high */
  ctui_popmenu_open(&m, ITEMS, N_ITEMS, 2, 3, rows, cols);
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(m.level[0].w == 23 && m.level[0].h == 10, "its size (%dx%d)",
                   m.level[0].w, m.level[0].h);
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 2, 3) == 0x250c &&
                       ctui_test_cell(screen, 11, 25) == 0x2518,
                   "its corner at the point, the rest right and below");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 3, "  Play") &&
                       ctui_test_row_contains(screen, 3, "Enter"),
                   "an item: its label after the mark's room, its keys");
  CTUI_TEST_ASSERT(cell(screen, 3, 7)->attr & CTUI_ATTR_UNDERLINE,
                   "its letter underlined");
  CTUI_TEST_ASSERT(!(cell(screen, 3, 8)->attr & CTUI_ATTR_UNDERLINE),
                   "only its letter");
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 4, 5) == 0x2713,
                   "a checked item's mark");
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 5, 4) == 0x2500 &&
                       ctui_test_cell(screen, 5, 24) == 0x2500,
                   "a separator across");
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 6, 23) == 0x25b8,
                   "a submenu's arrow");
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 3, 24) == ' ',
                   "nothing past the keys but room");

  /* keys */
  CTUI_TEST_ASSERT(m.level[0].sel == -1, "opened with the cursor on none");
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(m.level[0].sel == 0, "down: the first");
  CTUI_TEST_ASSERT(cell(screen, 3, 6)->bg == ctui_style_default.sel_bg,
                   "the cursor's row");
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(m.level[0].sel == 3, "down skips the separator (%d)",
                   m.level[0].sel);
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(m.level[0].sel == 5, "and an item that's off (%d)",
                   m.level[0].sel);
  ctui_test_key(&app, screen, CTUI_KEY_END, 0);
  ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
  CTUI_TEST_ASSERT(m.level[0].sel == 0, "down from the last wraps");
  ctui_test_key(&app, screen, CTUI_KEY_UP, 0);
  CTUI_TEST_ASSERT(m.level[0].sel == N_ITEMS - 1, "up from the first wraps");

  /* a submenu by keys */
  m.level[0].sel = 3;
  ctui_test_key(&app, screen, CTUI_KEY_RIGHT, 0);
  CTUI_TEST_ASSERT(m.depth == 2 && m.level[1].sel == 0,
                   "right opens the submenu, its first");
  CTUI_TEST_ASSERT(m.level[1].x == 26 && m.level[1].y == 5,
                   "beside its item, its first item level with it (%d,%d)",
                   m.level[1].x, m.level[1].y);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 6, "Rock") &&
                       ctui_test_cell(screen, 7, 28) == 0x2022,
                   "drawn there, a radio's dot");
  ctui_test_key(&app, screen, CTUI_KEY_LEFT, 0);
  CTUI_TEST_ASSERT(m.depth == 1 && find_row(screen, "Rock") < 0,
                   "left closes it");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(m.depth == 2, "enter on it opens it");
  got_n = 0;
  ctui_test_key(&app, screen, CTUI_KEY_ESC, 0);
  CTUI_TEST_ASSERT(m.depth == 1 && got_n == 0, "esc closes a level, no event");
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, 't');
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, 'J');
  CTUI_TEST_ASSERT(m.depth == 0 && m.chosen == &SUB[1] && got_act == 10 &&
                       strcmp(got, "&Jazz") == 0,
                   "letters: t opens Send to, J chooses Jazz (%s %d)", got,
                   got_act);
  CTUI_TEST_ASSERT(find_row(screen, "Play") < 0, "closed after a choice");

  ctui_popmenu_open(&m, ITEMS, N_ITEMS, 2, 3, rows, cols);
  ctui_app_render(&app, screen);
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, 'r');
  CTUI_TEST_ASSERT(
      m.depth == 0 && got_act == 5,
      "a letter two share, one off (Remove): the other chosen (%d)", got_act);

  /* letters shared by two that can be chosen: the cursor goes between */
  static const CTUI_POPMENU_ITEM TWO[] = {{.label = "&Save", .act = 1},
                                          {.label = "&Send", .act = 2}};
  ctui_popmenu_open(&m, TWO, 2, 0, 0, rows, cols);
  ctui_app_render(&app, screen);
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, 's');
  CTUI_TEST_ASSERT(m.depth == 1 && m.level[0].sel == 0,
                   "a shared letter: the first");
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, 'S');
  CTUI_TEST_ASSERT(m.level[0].sel == 1, "again: the next");
  got_n = 0;
  ctui_test_key(&app, screen, CTUI_KEY_CHAR, ' ');
  CTUI_TEST_ASSERT(m.depth == 0 && got_act == 2 && got_n == 1,
                   "space chooses the cursor's");

  /* off items aren't chosen */
  ctui_popmenu_open(&m, ITEMS, N_ITEMS, 2, 3, rows, cols);
  ctui_app_render(&app, screen);
  m.level[0].sel = 4;
  CTUI_TEST_ASSERT(
      !ctui_popmenu_key(&m,
                        &(CTUI_KEYPRESS_EVENT_DATA){.type = CTUI_KEY_ENTER}) &&
          m.depth == 1,
      "enter on an item that's off: nothing");
  got_n = 0;
  ctui_test_key(&app, screen, CTUI_KEY_ESC, 0);
  CTUI_TEST_ASSERT(m.depth == 0 && got_n == 1 && got_act == -1 &&
                       strcmp(got, "(null)") == 0,
                   "esc at the top cancels: value NULL, -1");

  /* kept on screen: opened at the bottom right corner it goes up and left,
   * the point its corner */
  ctui_popmenu_open(&m, ITEMS, N_ITEMS, rows - 1, cols - 1, rows, cols);
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(m.level[0].x + m.level[0].w == cols &&
                       m.level[0].y + m.level[0].h == rows,
                   "flipped above and left of the point (%d,%d)", m.level[0].x,
                   m.level[0].y);
  CTUI_TEST_ASSERT(ctui_test_cell(screen, rows - 1, cols - 1) == 0x2518,
                   "the point its corner");
  m.level[0].sel = 3;
  ctui_test_key(&app, screen, CTUI_KEY_RIGHT, 0);
  CTUI_TEST_ASSERT(m.level[1].x + m.level[1].w == m.level[0].x,
                   "a submenu with no room right: left of it (%d)",
                   m.level[1].x);
  CTUI_TEST_ASSERT(m.level[1].y + m.level[1].h <= rows,
                   "and up, to stay on screen");
  ctui_popmenu_close(&m);

  /* the mouse */
  ctui_popmenu_open(&m, ITEMS, N_ITEMS, 2, 3, rows, cols);
  ctui_app_render(&app, screen);
  got_n = 0;
  mouse(&app, screen, CTUI_MOUSE_RELEASE, 2, 3);
  CTUI_TEST_ASSERT(m.depth == 1 && got_n == 0,
                   "the opening press's release (the corner): nothing");
  mouse(&app, screen, CTUI_MOUSE_MOTION, 4, 10);
  CTUI_TEST_ASSERT(m.level[0].sel == 1, "the pointer selects");
  mouse(&app, screen, CTUI_MOUSE_MOTION, 5, 10);
  CTUI_TEST_ASSERT(m.level[0].sel == 1, "a separator doesn't");
  mouse(&app, screen, CTUI_MOUSE_MOTION, 6, 10);
  CTUI_TEST_ASSERT(m.depth == 2 && m.level[1].sel == -1,
                   "a submenu opens under the pointer");
  mouse(&app, screen, CTUI_MOUSE_MOTION, 6, 30);
  CTUI_TEST_ASSERT(m.depth == 2 && m.level[1].sel == 0 && m.level[0].sel == 3,
                   "into it: its item, the parent kept");
  mouse(&app, screen, CTUI_MOUSE_MOTION, 3, 10);
  CTUI_TEST_ASSERT(m.depth == 1 && m.level[0].sel == 0,
                   "back on another item: the submenu closed");
  mouse(&app, screen, CTUI_MOUSE_RELEASE, 7, 10);
  CTUI_TEST_ASSERT(m.depth == 1 && got_n == 0,
                   "a release on an item that's off: nothing");
  mouse(&app, screen, CTUI_MOUSE_RELEASE, 8, 10);
  CTUI_TEST_ASSERT(m.depth == 0 && got_act == 5 && m.chosen == &ITEMS[5],
                   "a release on an item chooses it");

  ctui_popmenu_open(&m, ITEMS, N_ITEMS, 2, 3, rows, cols);
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(mouse(&app, screen, CTUI_MOUSE_SCROLL_UP, 0, 0) &&
                       m.depth == 1,
                   "the wheel: taken, nothing");
  got_n = 0;
  CTUI_TEST_ASSERT(mouse(&app, screen, CTUI_MOUSE_PRESS, 15, 50) &&
                       m.depth == 0 && got_act == -1 && got_n == 1,
                   "a press outside: taken, cancelled");
  CTUI_TEST_ASSERT(!mouse(&app, screen, CTUI_MOUSE_PRESS, 15, 50),
                   "closed: the mouse isn't taken");

  /* the style's hook draws the frame: the cells blank on its bg */
  CTUI_STYLE st = ctui_style_default;
  st.control = draw_frame;
  m.style = &st;
  ctui_popmenu_open(&m, ITEMS, N_ITEMS, 2, 3, rows, cols);
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(frames == 1 && frame_asked.rows == 10 && frame_cols == 23 &&
                       frame_asked.label_cols == 0,
                   "the hook asked for the frame, its size, no title");
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 2, 3) == ' ' &&
                       ctui_test_cell(screen, 4, 3) == ' ',
                   "no box drawing over it");
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 3, "Play"),
                   "the items still text");

  /* resized smaller: kept on screen at the next render */
  ctui_test_resize(&app, screen, 8, 20);
  CTUI_TEST_ASSERT(m.level[0].x + m.level[0].w <= 20 &&
                       m.level[0].y + m.level[0].h <= 8,
                   "a resize keeps it on screen");

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
