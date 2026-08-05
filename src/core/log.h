#ifndef CTUI_LOG_H
#define CTUI_LOG_H

#include "logger.h" /* for E_DBG/E_WRN/E_INF/E_ERR/E_ALL verbosity flags */

/* the logging half of ctui_init(), split out so headless callers (the test
 * harness in tools/ctui_test.h, which injects events directly instead of
 * reading a real terminal) can get a working ctui_logf() without a tty --
 * ctui_init() calls this itself, so normal apps don't need it. */
void ctui_log_init(int verbosity);

/* the logging half of ctui_shutdown() -- closes the log file opened by
 * ctui_log_init(). Split out for the same reason ctui_log_init() is: kept
 * separate from terminal teardown so a headless caller could shut down
 * logging without ever having touched a tty. ctui_shutdown() calls this
 * itself. */
void ctui_log_shutdown(void);

/* logging; negative return means the write failed, non-negative is the
 * number of bytes written (0 if filtered out by verbosity). level is
 * exactly one of E_DBG/E_WRN/E_INF/E_ERR (see logger.h) */
int ctui_log(int level, const char *log_str);
int ctui_logf(int level, const char *fmt, ...);
int ctui_tick_advance(void);

#endif
