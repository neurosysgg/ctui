#ifndef CTUI_TEST_H
#define CTUI_TEST_H

/* Thin C test driver: hooks straight into the real event registry instead
 * of a pty/terminal, so a test looks like a normal ctui app's main() minus
 * ctui_app_run() -- inject a key or resize, the widgets react through their
 * real handlers exactly as they would live, then assert directly against
 * screen->cells. No process spawning, no ANSI parsing.
 *
 * A test file:
 *   ctui_log_init(E_ALL);                          instead of ctui_init()
 *   ... build widgets/events exactly like a real app's main() ...
 *   ctui_app_render(&app, screen);
 *   CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 4, "> alpha"), "...");
 *   ctui_test_key(&app, screen, CTUI_KEY_DOWN, 0);
 *   CTUI_TEST_ASSERT(ctui_test_row_contains(screen, 5, "> beta"), "...");
 *   return ctui_test_summary();
 *
 * For real-terminal concerns this can't reach -- raw-mode byte/ESC-sequence
 * decoding in ctui_input_loop(), actual SIGWINCH delivery, the ANSI bytes
 * ctui_screen_flush() emits -- use tools/pty_harness.py instead; the two
 * cover different layers. */

#include "ctui.h"

#include <stdio.h>
#include <string.h>

static int ctui_test_pass_count = 0;
static int ctui_test_fail_count = 0;

#define CTUI_TEST_ASSERT(cond, ...)                                          \
  do {                                                                       \
    if (cond) {                                                             \
      ctui_test_pass_count++;                                                \
      printf("  ok  " __VA_ARGS__);                                         \
      printf("\n");                                                         \
    } else {                                                                 \
      ctui_test_fail_count++;                                                \
      printf("FAIL  " __VA_ARGS__);                                         \
      printf("  (%s:%d)\n", __FILE__, __LINE__);                            \
    }                                                                        \
  } while (0)

/* prints the pass/fail tally and returns a process exit code (0 all
 * passed, 1 otherwise) so a test binary can `return ctui_test_summary();`
 * and `make test` can just check each binary's exit status. */
static inline int ctui_test_summary(void) {
  printf("%d passed, %d failed\n", ctui_test_pass_count,
        ctui_test_fail_count);
  return ctui_test_fail_count == 0 ? 0 : 1;
}

/* builds the same CTUI_EVENT ctui_input_loop() would have for this key
 * (source "input", CTUI_KEYPRESS_EVENT), dispatches it through the real
 * registry, and re-renders if any handler reported a change -- so
 * screen->cells reflects the keypress immediately. `ch` only matters when
 * type is CTUI_KEY_CHAR. */
static inline int ctui_test_key(CTUI_APP *app, CTUI_SCREEN *screen,
                                CTUI_KEYTYPE type, char ch) {
  CTUI_KEYPRESS_EVENT_DATA kp = {.type = type, .ch = ch};
  CTUI_EVENT ev = {.type = CTUI_KEYPRESS_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "input",
                   .event_data = &kp};
  int changed = ctui_handle_event(&ev);
  if (changed) {
    ctui_app_render(app, screen);
  }
  return changed;
}

/* same real path ctui_app_resize() takes on a real SIGWINCH (reallocate,
 * re-run every widget's layout(), dispatch CTUI_RESIZE_EVENT), then
 * re-renders so screen->cells is ready to assert against at the new size. */
static inline void ctui_test_resize(CTUI_APP *app, CTUI_SCREEN *screen,
                                    int rows, int cols) {
  ctui_app_resize(app, screen, rows, cols);
  ctui_app_render(app, screen);
}

/* reads a rendered character straight out of the screen's backing buffer --
 * the same array ctui_screen_flush() diffs against, so this is exactly
 * what would have hit the real terminal. '\0' if out of bounds. */
static inline char ctui_test_cell(CTUI_SCREEN *screen, int row, int col) {
  if (row < 0 || row >= screen->rows || col < 0 || col >= screen->cols) {
    return '\0';
  }
  return screen->cells[row * screen->cols + col].ch;
}

/* true if `needle` appears verbatim anywhere in `row` (cell-by-cell -- a
 * screen row isn't a NUL-terminated string). The common-case assertion:
 * "does this row show the text I expect", without caring about exact
 * column or trailing padding. */
static inline int ctui_test_row_contains(CTUI_SCREEN *screen, int row,
                                          const char *needle) {
  if (row < 0 || row >= screen->rows) {
    return 0;
  }
  size_t needle_len = strlen(needle);
  if (needle_len == 0 || needle_len > (size_t)screen->cols) {
    return 0;
  }
  for (int col = 0; col <= screen->cols - (int)needle_len; col++) {
    size_t i = 0;
    for (; i < needle_len; i++) {
      if (screen->cells[row * screen->cols + col + i].ch != needle[i]) {
        break;
      }
    }
    if (i == needle_len) {
      return 1;
    }
  }
  return 0;
}

#endif
