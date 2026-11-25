#include "log.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static struct CTUI_LOGGER logger;
static int _ctui_ticks;
int ctui_g_log_verbosity;

int ctui_tick_advance(void) { return ++_ctui_ticks; }

int ctui_log(int level, const char *log_str) {
  /* before ctui_log_init() there is no logger yet: dropped, as
   * ctui_logf() drops it (its verbosity reads 0 then) */
  if (logger.log_entry == NULL) {
    return 0;
  }
  return logger.log_entry(&logger, level, log_str);
}

int(ctui_logf)(int level, const char *fmt, ...) {
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

static const char *log_path = "ctui.log";
static long log_max = CTUI_LOG_MAX_DEFAULT;

/* CTUI_LOG_MAX: bytes, a K/M/G suffix (1024s), 0 = no cap; anything else
 * is ignored (-1) */
long ctui_log_parse_max(const char *s) {
  if (s == NULL || *s < '0' || *s > '9') {
    return -1;
  }
  char *end;
  long v = strtol(s, &end, 10);
  long unit = 1;
  switch (*end) {
  case 'k':
  case 'K':
    unit = 1L << 10;
    end++;
    break;
  case 'm':
  case 'M':
    unit = 1L << 20;
    end++;
    break;
  case 'g':
  case 'G':
    unit = 1L << 30;
    end++;
    break;
  default:
    break;
  }
  if (*end != '\0' || v < 0 || v > LONG_MAX / unit) {
    return -1;
  }
  return v * unit;
}

static long effective_max(void) {
  long env = ctui_log_parse_max(getenv("CTUI_LOG_MAX"));
  return env >= 0 ? env : log_max;
}

void ctui_log_set_max_size(long bytes) {
  log_max = bytes > 0 ? bytes : 0;
  if (logger.log_entry != NULL) {
    logger_set_max(&logger, effective_max());
  }
}

void ctui_log_set_path(const char *path) {
  log_path = path ? path : "ctui.log";
}

void ctui_log_init(int verbosity) {
  _ctui_ticks = 0;
  logger = init_logger((char *)log_path, verbosity);
  logger_set_max(&logger, effective_max());
  ctui_g_log_verbosity = verbosity;
  ctui_tick_advance();
  ctui_logf(E_INF, "[CTUI:LOG] - logger initialised @ tick %d.\n", _ctui_ticks);
}

void ctui_log_shutdown(void) { shutdown_logger(&logger); }

void ctui_log_set_buffered(void) { logger_set_buffered(&logger, 1); }
void ctui_log_set_unbuffered(void) { logger_set_buffered(&logger, 0); }
