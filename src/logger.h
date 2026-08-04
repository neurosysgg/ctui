#ifndef CTUI_LOGGER_H
#define CTUI_LOGGER_H

#include <stddef.h>
#include <stdio.h>

typedef struct CTUI_LOGGER CTUI_LOGGER;

/* bitmaskable log verbosity levels; OR together to pass as init_logger's
 * verbosity, tag individual ctui_log/ctui_logf calls with exactly one */
enum {
  E_DBG = 1 << 0, /* 1 -- per-primitive tracing (putc, puts, byte reads) */
  E_WRN = 1 << 1, /* 2 -- recoverable anomalies (fallbacks, rejected calls) */
  E_INF = 1 << 2, /* 4 -- routine lifecycle/event-level notices */
  E_ERR = 1 << 3, /* 8 -- unrecoverable failures */
  E_ALL = 0b1111, /* 15 -- E_DBG | E_WRN | E_INF | E_ERR */
};

struct CTUI_LOGGER {
  const char *path;
  FILE *file;
  int verbosity;
  int (*log_entry)(CTUI_LOGGER *self, int level, const char *log_str);
};

CTUI_LOGGER init_logger(char *path, int verbosity);
void shutdown_logger(CTUI_LOGGER *logger);

#endif
