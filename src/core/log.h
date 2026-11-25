#ifndef CTUI_LOG_H
#define CTUI_LOG_H

#include "logger.h" /* for E_DBG/E_WRN/E_INF/E_ERR/E_ALL verbosity flags */

/* the logging half of ctui_init(), split out so headless callers (the test
 * harness in tools/ctui_test.h, which injects events directly instead of
 * reading a real terminal) can get a working ctui_logf() without a tty --
 * ctui_init() calls this itself, so normal apps don't need it. */
void ctui_log_init(int verbosity);

/* the file ctui_log_init() (so ctui_init()) opens: "ctui.log" in the cwd
 * unless set here first. path is kept, not copied; NULL goes back to the
 * default. For an app whose cwd is the user's (a file manager), not a
 * directory of its own. */
void ctui_log_set_path(const char *path);

/* the log's size cap: once the file reaches it, it moves to PATH.1 (an
 * older .1 goes) and PATH starts again -- checked at open and as lines are
 * written, so a log keeps at most ~2 * the cap on disk however long the
 * app runs or however noisy its level. <= 0: no cap. Before or after
 * ctui_log_init(); the CTUI_LOG_MAX environment variable (bytes, K/M/G
 * suffixes, 0 = none) overrides it, for a verbose debugging run. */
#define CTUI_LOG_MAX_DEFAULT (8L << 20)
void ctui_log_set_max_size(long bytes);

/* CTUI_LOG_MAX's syntax: "8M", "512K", "1048576", "0"; -1 if it isn't one */
long ctui_log_parse_max(const char *s);

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
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
int(ctui_logf)(int level, const char *fmt, ...);
int ctui_tick_advance(void);

/* the verbosity ctui_log_init() set (0 before it): what ctui_logf()
 * checks first. Read-only outside core/log.c. */
extern int ctui_g_log_verbosity;

/* ctui_logf() checks the level before anything else is evaluated: a masked
 * call (one E_DBG per putc, per widget render) costs a load and a branch,
 * not a call -- its arguments (ctui_tick_advance(), a strlen(), ...) are
 * never evaluated, so the tick counter numbers logged lines only. Was 4-5
 * % of a text app's instructions (ctui-wm's notes/self-improving.md,
 * session 219). (ctui_logf)(...) calls the function itself. */
#define ctui_logf(level, ...)                                                  \
  ((ctui_g_log_verbosity & (level)) ? (ctui_logf)((level), __VA_ARGS__) : 0)

/* opts into skipping the fflush() ctui_log()/ctui_logf() otherwise issue
 * after every single entry -- see logger.h's CTUI_LOGGER.buffered doc
 * comment for the full tradeoff (log durability across a crash vs. one
 * write() syscall per log line). Worth calling for an app whose logging
 * happens on a tight redraw/tick loop (e.g. ctui-mus's 20ms periodic
 * tick, which was issuing an fflush() per E_INF line per frame -- real,
 * measurable CPU cost from nothing but log I/O). ctui_log_init() leaves
 * the default (unbuffered, immediate-flush) in place; call this
 * afterward to switch. */
void ctui_log_set_buffered(void);

/* reverts ctui_log_set_buffered() -- back to fflush()-per-entry, the
 * default every app gets from ctui_log_init()/ctui_init() until/unless
 * ctui_log_set_buffered() is called. */
void ctui_log_set_unbuffered(void);

#endif
