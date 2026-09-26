/* CTUI_ENTRY (widgets/entry.h): editing keys, the cursor, scrolling, a
 * secret, the placeholder, Enter's value-changed event. */
#include "ctui.h"
#include "widgets/entry.h"

#include "ctui_test.h"

#include <string.h>

static char got[64];

static int on_value(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_VALUE_CHANGED_EVENT_DATA *d = ev->event_data;
  snprintf(got, sizeof got, "%s", d->value);
  return 0;
}

static int key(CTUI_ENTRY *e, CTUI_KEYTYPE type, uint32_t ch,
               unsigned mods) {
  CTUI_KEYPRESS_EVENT_DATA kp = {.type = type, .ch = ch, .mods = mods};
  return ctui_entry_key(e, &kp);
}

static void type(CTUI_ENTRY *e, const char *s) {
  while (*s) {
    uint32_t cp;
    s += ctui_utf8_decode(s, &cp);
    key(e, CTUI_KEY_CHAR, cp, 0);
  }
}

int main(void) {
  ctui_log_init(E_ALL);

  int rows = 6, cols = 20;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  char buf[16] = "";
  CTUI_ENTRY e = {.buf = buf, .cap = sizeof buf, .placeholder = "name"};
  CTUI_WIDGET w = ctui_widget_make(0, 1, 8, 1, &e, ctui_entry_render, NULL);
  CTUI_WIDGET *widgets[] = {&w};
  CTUI_APP app;
  ctui_app_init(&app, widgets, 1, rows, cols);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &w,
                      ctui_entry_handle_keypress);
  ctui_event_register("entry", CTUI_VALUE_CHANGED_EVENT, &w, on_value);

  ctui_app_render(&app, screen);
  /* --- editing --- */
  type(&e, "hello");
  CTUI_TEST_ASSERT(!strcmp(buf, "hello") && e.cursor == 5, "typing appends");
  key(&e, CTUI_KEY_LEFT, 0, 0);
  key(&e, CTUI_KEY_LEFT, 0, 0);
  type(&e, "X");
  CTUI_TEST_ASSERT(!strcmp(buf, "helXlo"), "typing inserts at the cursor");
  key(&e, CTUI_KEY_CHAR, 0x7f, 0);
  CTUI_TEST_ASSERT(!strcmp(buf, "hello") && e.cursor == 3,
                   "backspace takes the glyph before the cursor");
  key(&e, CTUI_KEY_DELETE, 0, 0);
  CTUI_TEST_ASSERT(!strcmp(buf, "helo"), "delete takes the one under it");
  CTUI_TEST_ASSERT(buf[5] == 0 && buf[6] == 0, "freed bytes are zeroed");
  key(&e, CTUI_KEY_CHAR, 0x0b, 0);
  CTUI_TEST_ASSERT(!strcmp(buf, "hel"), "ctrl+k cuts after the cursor");
  CTUI_TEST_ASSERT(!key(&e, CTUI_KEY_CHAR, 0x0b, 0),
                   "ctrl+k at the end changes nothing");
  ctui_entry_set(&e, "a/b c");
  CTUI_TEST_ASSERT(e.cursor == 5, "set puts the cursor at the end");
  key(&e, CTUI_KEY_CHAR, 0x17, 0);
  CTUI_TEST_ASSERT(!strcmp(buf, "a/b "), "ctrl+w cuts a word: '%s'", buf);
  key(&e, CTUI_KEY_CHAR, 0x7f, CTUI_MOD_CTRL);
  CTUI_TEST_ASSERT(!strcmp(buf, "a/"), "ctrl+backspace too: '%s'", buf);
  key(&e, CTUI_KEY_CHAR, 0x15, 0);
  CTUI_TEST_ASSERT(!strcmp(buf, "") && e.cursor == 0,
                   "ctrl+u cuts before the cursor");
  type(&e, "one two");
  key(&e, CTUI_KEY_LEFT, 0, CTUI_MOD_CTRL);
  CTUI_TEST_ASSERT(e.cursor == 4, "ctrl+left goes a word back");
  key(&e, CTUI_KEY_HOME, 0, 0);
  key(&e, CTUI_KEY_RIGHT, 0, CTUI_MOD_CTRL);
  CTUI_TEST_ASSERT(e.cursor == 3, "ctrl+right goes past a word");
  CTUI_TEST_ASSERT(!key(&e, CTUI_KEY_CHAR, 'x', CTUI_MOD_ALT),
                   "alt+letter isn't text");
  CTUI_TEST_ASSERT(!key(&e, CTUI_KEY_UP, 0, 0), "up is left to the caller");

  ctui_entry_set(&e, "ü");
  type(&e, "0123456789abcdef");
  CTUI_TEST_ASSERT(strlen(buf) == 15, "the buffer's cap holds (%zu)",
                   strlen(buf));
  ctui_entry_set(&e, "0123456789abcdefghij");
  CTUI_TEST_ASSERT(!strcmp(buf, "0123456789abcde"), "set cuts to the cap");
  ctui_entry_set(&e, "0123456789abcdü");
  CTUI_TEST_ASSERT(!strcmp(buf, "0123456789abcd"),
                   "set never splits a glyph: '%s'", buf);

  /* --- drawing --- */
  ctui_entry_set(&e, "abcdefghij");
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "defghij") &&
                       !ctui_test_row_contains(screen, 1, "c"),
                   "scrolled so the cursor at the end shows");
  CTUI_TEST_ASSERT(screen->cells[1 * cols + 7].bg == CTUI_COLOR_CYAN,
                   "the cursor block is the last column");
  ctui_test_key(&app, screen, CTUI_KEY_HOME, 0);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "abcdefgh"),
                   "home scrolls back");
  CTUI_TEST_ASSERT(screen->cells[1 * cols].bg == CTUI_COLOR_CYAN &&
                       ctui_test_cell(screen, 1, 0) == 'a',
                   "the cursor sits on the first glyph");
  ctui_entry_click(&e, 3);
  CTUI_TEST_ASSERT(e.cursor == 3, "a click moves the cursor");
  ctui_entry_click(&e, 40);
  CTUI_TEST_ASSERT(e.cursor == 10, "a click past the end goes to the end");

  ctui_entry_set(&e, "日本語");
  ctui_entry_click(&e, 3);
  CTUI_TEST_ASSERT(e.cursor == 3, "a click inside a wide glyph lands on it");

  e.secret = 1;
  ctui_entry_set(&e, "pässword");
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "•••••••") &&
                       !ctui_test_row_contains(screen, 1, "s"),
                   "a secret shows dots");
  e.secret = 0;

  ctui_entry_clear(&e);
  CTUI_TEST_ASSERT(buf[0] == 0 && buf[9] == 0, "clear zeroes the buffer");
  CTUI_WIDGET other = w;
  ctui_app_render(&app, screen);
  ctui_entry_draw(&other, app.comp, 0, 0, 8, &e, 0);
  ctui_compositor_blit(app.comp, screen);
  CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 1, "name"),
                   "the placeholder shows while empty and unfocused");

  type(&e, "hi");
  ctui_test_key(&app, screen, CTUI_KEY_ENTER, 0);
  CTUI_TEST_ASSERT(!strcmp(got, "hi"), "enter emits the text ('%s')", got);

  ctui_app_free(&app);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
