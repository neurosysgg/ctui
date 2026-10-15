#ifndef CTUI_HARNESS_H
#define CTUI_HARNESS_H

/* An app under a pty, driven from C: spawned with its environment, keys
 * and resizes sent, its output replayed into a CTUI_VT (vt.h), waits on
 * what the screen shows instead of fixed sleeps, the screen dumped as
 * tools/pty_harness.py dumps it. Cases are data: an array of
 * CTUI_HARNESS_STEPs run by ctui_harness_steps().
 *
 *   static const CTUI_HARNESS_STEP log_case[] = {
 *       CTUI_HS_UNTIL("alpha"), CTUI_HS_KEYS(CTUI_HK_ENTER), CTUI_HS_QUIET,
 *       CTUI_HS_DUMP, CTUI_HS_KEYS("/number 3\r"), CTUI_HS_QUIET,
 *       CTUI_HS_DUMP, CTUI_HS_END};
 *   CTUI_PTY *p = ctui_pty_spawn(&(CTUI_PTY_SPAWN){.argv = argv,
 *                                .rows = 32, .cols = 110});
 *   int bad = ctui_harness_steps(p, log_case, stdout, NULL);
 *   ctui_pty_close(p, stdout);
 *
 * $CTUI_PTY_RECORD=DIR records each app's output as DIR/NAME-PID.vt, its
 * sizes in it as "\e_ctui-record;size=ROWS;COLS\e\\" (APC strings: a
 * terminal swallows them), for tools/flushbench.c's replays.
 *
 * POSIX only (posix_openpt, no -lutil): glibc and musl alike. Not part of
 * any app: what links it is the tests and the screens runners. */

#include "vt.h"

#include <stddef.h>
#include <stdio.h>

typedef struct CTUI_PTY CTUI_PTY;

typedef struct {
  const char *const *argv;  /* argv[0] looked up in PATH (execvp) */
  const char *cwd;          /* NULL: the caller's */
  const char *const *env;   /* "NAME=value" set in the child, NULL-ended */
  const char *const *unset; /* names removed from the child's, NULL-ended */
  int rows, cols;           /* 0: 24 x 80 */
  int kitty_shm;            /* answer ctui's kitty shm probe (vt.h) */
} CTUI_PTY_SPAWN;

/* the app running on a new pty of rows x cols; NULL when no pty or no
 * fork (an exec failure shows on the pty as the child's own message,
 * then the child exits 127) */
CTUI_PTY *ctui_pty_spawn(const CTUI_PTY_SPAWN *how);

/* stops the app if it still runs (SIGTERM, SIGKILL 2 s later) and frees
 * it all. How it ended goes to out (NULL: nowhere) unless that was our
 * SIGTERM (killed by it, or exit 0 after it): "-- exited N --" or
 * "-- killed by signal N --", so a crash shows in a dump's diff. Returns
 * 0 for our SIGTERM, 1 when it ended otherwise, -1 when it would not end
 * (SIGKILL). */
int ctui_pty_close(CTUI_PTY *p, FILE *out);

CTUI_VT *ctui_pty_vt(CTUI_PTY *p);

/* everything the app wrote so far, *n its length */
const char *ctui_pty_output(const CTUI_PTY *p, size_t *n);

/* 1 once the app's side of the pty is gone (exited, or closed it) */
int ctui_pty_gone(const CTUI_PTY *p);

int ctui_pty_send(CTUI_PTY *p, const char *bytes, size_t n);

/* a window resize: the pty's size (SIGWINCH to the app) and the grid's */
void ctui_pty_resize(CTUI_PTY *p, int rows, int cols);

/* reads what the app writes for up to ms (returning early once something
 * came and nothing more is ready): the bytes read, 0 none, -1 gone */
int ctui_pty_pump(CTUI_PTY *p, int ms);

/* the waits: 0 once true, -1 after timeout_ms (or the app gone first) */
int ctui_pty_until_text(CTUI_PTY *p, const char *text, int timeout_ms);
int ctui_pty_until_gone(CTUI_PTY *p, const char *text, int timeout_ms);
/* no output for quiet_ms in a row */
int ctui_pty_until_quiet(CTUI_PTY *p, int quiet_ms, int timeout_ms);
/* the app ended (by itself: ESC quit it, ...), so ctui_pty_close() reports
 * how without racing it with a SIGTERM. Its screen going isn't that: it
 * leaves the alternate screen before it exits */
int ctui_pty_until_ended(CTUI_PTY *p, int timeout_ms);
/* reads for ms whatever happens: a fixed sleep, for what no condition
 * says (a timer that has to tick) */
void ctui_pty_wait(CTUI_PTY *p, int ms);

/* the grid's text, then a line of '-' as wide as the grid (the dump
 * pty_harness.py prints); its styles (ctui_vt_styles()) and the same line
 * to styles unless NULL */
void ctui_pty_dump(CTUI_PTY *p, FILE *out, FILE *styles);

/* what a wait gives a condition when the step leaves it 0 */
#define CTUI_HARNESS_TIMEOUT_MS 5000
#define CTUI_HARNESS_QUIET_MS 120

enum {
  CTUI_HS_OP_END = 0,
  CTUI_HS_OP_KEYS,   /* s: n = a bytes written at once */
  CTUI_HS_OP_UNTIL,  /* s on screen, within a ms */
  CTUI_HS_OP_GONE,   /* s off the screen, within a ms */
  CTUI_HS_OP_QUIET,  /* no output for a ms, within b ms */
  CTUI_HS_OP_WAIT,   /* a ms, whatever happens */
  CTUI_HS_OP_RESIZE, /* a rows, b cols */
  CTUI_HS_OP_CLICK,  /* button atoi(s) pressed at column a, row b
                      * (1-based), as SGR mouse reports; released too
                      * unless a wheel (64, 65) */
  CTUI_HS_OP_DUMP,
  CTUI_HS_OP_ENDED, /* the app ended, within a ms */
};

typedef struct {
  int op;
  const char *s;
  int a, b;
} CTUI_HARNESS_STEP;

/* a string literal's bytes, NULs included (ctrl+space is one) */
#define CTUI_HS_KEYS(lit) {CTUI_HS_OP_KEYS, (lit), (int)sizeof(lit) - 1, 0}
#define CTUI_HS_UNTIL(text) {CTUI_HS_OP_UNTIL, (text), 0, 0}
#define CTUI_HS_UNTIL_MS(text, ms) {CTUI_HS_OP_UNTIL, (text), (ms), 0}
#define CTUI_HS_GONE(text) {CTUI_HS_OP_GONE, (text), 0, 0}
#define CTUI_HS_QUIET {CTUI_HS_OP_QUIET, NULL, 0, 0}
#define CTUI_HS_QUIET_MS(ms) {CTUI_HS_OP_QUIET, NULL, (ms), 0}
#define CTUI_HS_WAIT(ms) {CTUI_HS_OP_WAIT, NULL, (ms), 0}
#define CTUI_HS_RESIZE(rows, cols) {CTUI_HS_OP_RESIZE, NULL, (rows), (cols)}
#define CTUI_HS_CLICK(col, row) {CTUI_HS_OP_CLICK, "0", (col), (row)}
#define CTUI_HS_RCLICK(col, row) {CTUI_HS_OP_CLICK, "2", (col), (row)}
#define CTUI_HS_WHEEL_UP(col, row) {CTUI_HS_OP_CLICK, "64", (col), (row)}
#define CTUI_HS_WHEEL_DOWN(col, row) {CTUI_HS_OP_CLICK, "65", (col), (row)}
#define CTUI_HS_DUMP {CTUI_HS_OP_DUMP, NULL, 0, 0}
#define CTUI_HS_ENDED {CTUI_HS_OP_ENDED, NULL, 0, 0}
#define CTUI_HS_END {CTUI_HS_OP_END, NULL, 0, 0}

/* keys as a terminal sends them (kitty's legacy encoding), to put in
 * CTUI_HS_KEYS(); literals, so they concatenate */
#define CTUI_HK_UP "\x1b[A"
#define CTUI_HK_DOWN "\x1b[B"
#define CTUI_HK_RIGHT "\x1b[C"
#define CTUI_HK_LEFT "\x1b[D"
#define CTUI_HK_HOME "\x1b[H"
#define CTUI_HK_END "\x1b[F"
#define CTUI_HK_PGUP "\x1b[5~"
#define CTUI_HK_PGDN "\x1b[6~"
#define CTUI_HK_INSERT "\x1b[2~"
#define CTUI_HK_DELETE "\x1b[3~"
#define CTUI_HK_ENTER "\r"
#define CTUI_HK_ESC "\x1b"
#define CTUI_HK_TAB "\t"
#define CTUI_HK_BTAB "\x1b[Z"
#define CTUI_HK_BACKSPACE "\x7f"

/* runs steps against p, dumps to out (and their styles to styles unless
 * NULL: ctui_pty_dump()). A wait that fails prints why
 * ("!! until "X": not within N ms") and the screen, and ends the run:
 * what follows depended on it. Returns 0, or -1 after a failed wait. A
 * KEYS step that ends in a lone ESC is followed by 80 ms of reading, so
 * the app takes it as the Esc key, not the start of the next key's
 * sequence (ctui waits 50 ms). */
int ctui_harness_steps(CTUI_PTY *p, const CTUI_HARNESS_STEP *steps, FILE *out,
                       FILE *styles);

#endif
