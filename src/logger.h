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
  /* 0 (default, set by init_logger()) = fflush() after every entry, so the
   * file on disk is always current -- what CLAUDE.md's "grep the log,
   * don't guess" workflow assumes, and safe even if the process later
   * crashes without a clean shutdown. 1 = skip that fflush(), relying on
   * stdio's own buffering (flushed at shutdown_logger()'s fclose(), or
   * whenever the buffer fills) -- an opt-in for a caller whose logging
   * happens on a tight redraw/tick loop, where a write()-syscall-per-
   * log-line cost is measurable; a crash before a clean shutdown loses
   * whatever's still sitting in the unflushed buffer. See
   * logger_set_buffered() below. */
  int buffered;
  int (*log_entry)(CTUI_LOGGER *self, int level, const char *log_str);
};

CTUI_LOGGER init_logger(char *path, int verbosity);
void shutdown_logger(CTUI_LOGGER *logger);

/* toggles the fflush()-per-entry behavior documented on CTUI_LOGGER.buffered
 * above. Exposed as a plain field-setter, not through ctui_log_init()'s
 * argument list, since it's an opt-in performance tradeoff most callers
 * never need -- see log.h's ctui_log_set_buffered()/
 * ctui_log_set_unbuffered() for the public, no-CTUI_LOGGER*-needed wrapper
 * every app actually calls. */
void logger_set_buffered(CTUI_LOGGER *logger, int buffered);

#endif
