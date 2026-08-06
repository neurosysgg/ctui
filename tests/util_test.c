/* Exercises core/util.c's geometry/string helpers -- ctui_util_center_h(),
 * ctui_util_truncate_str(), ctui_util_rescale_i(), and
 * ctui_util_inset()/ctui_margin_uniform() -- directly against plain values
 * and CTUI_WIDGET structs, no app/screen needed since none of them touch
 * the compositor. */
#include "ctui.h"

#include "ctui_test.h"

#include <string.h>

int main(void) {
  ctui_log_init(E_ALL);

  char line[9];
  strcpy(line, "        ");
  CTUI_CELL fill = {.ch = '-'};
  int rc = ctui_util_center_h((char[]){"hi"}, line, fill);
  CTUI_TEST_ASSERT(rc == 0, "center_h succeeds when center_str fits");
  CTUI_TEST_ASSERT(strcmp(line, "---hi---") == 0,
                   "center_h centers with an even split when total_pad is "
                   "even (8-2=6 pad -> 3/3)");

  strcpy(line, "       "); /* 7 wide, odd total_pad */
  rc = ctui_util_center_h((char[]){"hi"}, line, fill);
  CTUI_TEST_ASSERT(rc == 0 && strcmp(line, "--hi---") == 0,
                   "center_h puts the extra pad cell on the right when "
                   "total_pad is odd");

  strcpy(line, "ab");
  rc = ctui_util_center_h((char[]){"abc"}, line, fill);
  CTUI_TEST_ASSERT(rc == -1,
                   "center_h rejects a center_str longer than line");

  char buf[32];
  strcpy(buf, "short");
  rc = ctui_util_truncate_str(buf, 10, "...");
  CTUI_TEST_ASSERT(rc == 0 && strcmp(buf, "short") == 0,
                   "truncate_str no-ops when str already fits within "
                   "desired");

  strcpy(buf, "a very long string");
  rc = ctui_util_truncate_str(buf, 10, "...");
  CTUI_TEST_ASSERT(rc == 0 && strcmp(buf, "a very ...") == 0 &&
                       strlen(buf) == 10,
                   "truncate_str cuts to exactly desired chars, replacing "
                   "the tail with trunc");

  strcpy(buf, "hi");
  rc = ctui_util_truncate_str(buf, 1, "...");
  CTUI_TEST_ASSERT(rc == -1,
                   "truncate_str rejects a trunc string longer than "
                   "desired");

  CTUI_TEST_ASSERT(ctui_util_rescale_i(0, 0, 10, 0, 100) == 0,
                   "rescale_i maps in_min to out_min");
  CTUI_TEST_ASSERT(ctui_util_rescale_i(10, 0, 10, 0, 100) == 100,
                   "rescale_i maps in_max to out_max");
  CTUI_TEST_ASSERT(ctui_util_rescale_i(5, 0, 10, 0, 100) == 50,
                   "rescale_i maps the midpoint proportionally");
  CTUI_TEST_ASSERT(ctui_util_rescale_i(-5, 0, 10, 0, 100) == 0,
                   "rescale_i clamps below in_min");
  CTUI_TEST_ASSERT(ctui_util_rescale_i(50, 0, 10, 0, 100) == 100,
                   "rescale_i clamps above in_max");
  CTUI_TEST_ASSERT(ctui_util_rescale_i(3, 0, 10, 16, 231) == 16 + 3 * 215 / 10,
                   "rescale_i works with a non-zero out_min (e.g. the "
                   "256-color cube's 16..231 range)");
  CTUI_TEST_ASSERT(ctui_util_rescale_i(5, 5, 5, 7, 20) == 7,
                   "rescale_i returns out_min and doesn't crash when "
                   "in_min == in_max");

  CTUI_WIDGET outer = ctui_widget_make(2, 3, 20, 10, NULL, NULL, NULL);
  CTUI_WIDGET content = ctui_widget_make(0, 0, 0, 0, NULL, NULL, NULL);

  rc = ctui_util_inset(&content, &outer, ctui_margin_uniform(1));
  CTUI_TEST_ASSERT(rc == 0, "inset with a 1-cell margin succeeds");
  CTUI_TEST_ASSERT(content.x == 3 && content.y == 4,
                   "inset offsets content's origin by the margin");
  CTUI_TEST_ASSERT(content.w == 18 && content.h == 8,
                   "inset shrinks content's size by the margin on both "
                   "sides");

  CTUI_MARGIN asym = {.top = 1, .right = 2, .bottom = 3, .left = 4};
  rc = ctui_util_inset(&content, &outer, asym);
  CTUI_TEST_ASSERT(rc == 0, "inset with an asymmetric margin succeeds");
  CTUI_TEST_ASSERT(content.x == 6 && content.y == 4,
                   "inset honors left/top independently");
  CTUI_TEST_ASSERT(content.w == 14 && content.h == 6,
                   "inset honors right/bottom independently");

  CTUI_WIDGET stale = {
      .x = -1, .y = -1, .w = -1, .h = -1, .widget_data = NULL};
  CTUI_WIDGET small_outer = ctui_widget_make(0, 0, 2, 10, NULL, NULL, NULL);
  rc = ctui_util_inset(&stale, &small_outer, ctui_margin_uniform(1));
  CTUI_TEST_ASSERT(rc == -1,
                   "inset rejects a margin that consumes the whole width");
  CTUI_TEST_ASSERT(stale.x == -1 && stale.w == -1,
                   "a rejected inset leaves content's geometry untouched");

  return ctui_test_summary();
}
