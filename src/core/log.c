#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static struct CTUI_LOGGER logger;
static int _ctui_ticks;

int ctui_tick_advance(void) { return ++_ctui_ticks; }

int ctui_log(int level, const char *log_str) {
  return logger.log_entry(&logger, level, log_str);
}

int ctui_logf(int level, const char *fmt, ...) {
  /* mirrors log_entry()'s own verbosity check, but ahead of the
   * vsnprintf/malloc/free below -- without this, every call site (many of
   * them per-primitive: one per ctui_widget_putc(), one per event
   * dispatched) pays full formatting cost even when the level is masked
   * out and the string is about to be discarded. */
  if (!(logger.verbosity & level)) {
    return 0;
  }

  int err = -1;
  int f_result = 0;
  size_t size = 0;
  char *p = NULL;
  va_list ap;

  /* Determine required size */

  va_start(ap, fmt);
  f_result = vsnprintf(p, size, fmt, ap);
  va_end(ap);

  if (f_result < 0) {
    ctui_log(E_ERR, "[CTUI:LOG:FORMAT] - could not format string; failed "
                    "determining va_list size\n");
    return err;
  }

  size = (size_t)f_result + 1; /* +1 for \0 */
  p = malloc(size);
  if (p == NULL) {
    ctui_log(E_ERR, "[CTUI:LOG:FORMAT] - could not allocate buffer for "
                    "formatted log string\n");
    return err;
  }

  va_start(ap, fmt);
  f_result = vsnprintf(p, size, fmt, ap);
  va_end(ap);

  if (f_result < 0) {
    ctui_log(E_ERR, "[CTUI:LOG:FORMAT] - could not format string; second "
                    "vsnprintf call failed\n");
    free(p);
    return err;
  }

  int log_result = ctui_log(level, p);
  free(p);
  return log_result;
}

void ctui_log_init(int verbosity) {
  _ctui_ticks = 0;
  logger = init_logger("ctui.log", verbosity);
  ctui_tick_advance();
  ctui_logf(E_INF, "[CTUI:LOG] - logger initialised @ tick %d.\n",
            _ctui_ticks);
}

void ctui_log_shutdown(void) { shutdown_logger(&logger); }
