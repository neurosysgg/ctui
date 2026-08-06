/* Exercises core/log.c directly -- ctui_logf()'s own verbosity pre-check
 * (log_entry() in src/logger.c has an identical check, but ctui_logf()'s
 * copy short-circuits before the vsnprintf/malloc work, see its own
 * comment), ctui_log()'s pass-through, ctui_tick_advance()'s counter, and
 * ctui_log_shutdown(). Every other test calls ctui_log_init(E_ALL), so
 * nothing else in the suite ever takes the "filtered out" branch or calls
 * shutdown. */
#include "ctui.h"

#include "ctui_test.h"

int main(void) {
  ctui_log_init(E_ERR); /* only E_ERR passes -- everything else is masked */

  int r = ctui_logf(E_DBG, "this should be filtered out\n");
  CTUI_TEST_ASSERT(r == 0,
                   "logf returns 0 (not an error) when level is masked out "
                   "by verbosity, without touching vsnprintf/malloc");

  r = ctui_logf(E_ERR, "this should be written (%d)\n", 42);
  CTUI_TEST_ASSERT(r > 0,
                   "logf returns the byte count written when level passes "
                   "the verbosity filter");

  r = ctui_log(E_ERR, "a pre-formatted string via ctui_log\n");
  CTUI_TEST_ASSERT(r > 0, "ctui_log writes a pre-formatted string directly");

  int tick1 = ctui_tick_advance();
  int tick2 = ctui_tick_advance();
  CTUI_TEST_ASSERT(tick2 == tick1 + 1,
                   "tick_advance increments a monotonic counter by one "
                   "each call");

  ctui_log_shutdown();

  return ctui_test_summary();
}
