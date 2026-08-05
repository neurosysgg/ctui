/* must precede every #include: struct sigaction/sigaction()/sigemptyset()
 * are POSIX, not C11, and -std=c11 sets __STRICT_ANSI__, which suppresses
 * glibc's usual default of exposing them without an explicit feature-test
 * macro. Has to be set before the first system header (even a transitive
 * one, e.g. via "term.h" -> "log.h" -> <stdio.h>) or it's too late --
 * glibc's <features.h> only evaluates it once. */
#define _POSIX_C_SOURCE 200809L

#include "term.h"

#include "ctui_internal.h"
#include "log.h"

#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig_termios;

volatile sig_atomic_t g_resize_pending = 0;

static void handle_sigwinch(int sig) {
  (void)sig;
  g_resize_pending = 1;
}

int ctui_init(int verbosity) {
  ctui_log_init(verbosity);

  ctui_tick_advance();
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
    ctui_log(E_ERR, "[CTUI:TERM] - error retrieving termios struct\n");
    return -1;
  }
  ctui_tick_advance();

  struct termios raw = orig_termios;
  raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(unsigned)(OPOST);
  raw.c_cflag |= CS8;
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  ctui_tick_advance();

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    ctui_log(E_ERR, "[CTUI:TERM] - TCSAFLUSH error\n");
    return -1;
  }
  ctui_tick_advance();

  /* no SA_RESTART: we want blocking read() in ctui_input_loop() to return
   * EINTR on SIGWINCH so the event loop can react to the resize promptly
   * instead of waiting for the next keypress */
  struct sigaction sa = {0};
  sa.sa_handler = handle_sigwinch;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGWINCH, &sa, NULL);
  ctui_logf(E_INF, "[CTUI:TERM] - SIGWINCH handler installed @ tick %d\n",
            ctui_tick_advance());

  /* alternate screen buffer + hide cursor */
  printf("\x1b[?1049h\x1b[?25l");
  fflush(stdout);
  ctui_logf(E_INF,
            "[CTUI:INIT] - init complete @ tick %d, alternate screen buffer "
            "active\n",
            ctui_tick_advance());
  return 0;
}

void ctui_shutdown(void) {
  ctui_logf(E_INF, "[CTUI:INIT] - shutting down @ tick %d\n",
            ctui_tick_advance());
  ctui_log_shutdown();
  printf("\x1b[?25h\x1b[?1049l");
  fflush(stdout);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void ctui_get_termsize(/*ref*/ int *rows, /*ref*/ int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    *rows = 24;
    *cols = 60;
    ctui_logf(E_WRN,
              "[CTUI:TERM] - TIOCGWINSZ failed @ tick %d, falling back to "
              "%dx%d\n",
              ctui_tick_advance(), *cols, *rows);
  } else {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    ctui_logf(E_INF, "[CTUI:TERM] - terminal size %dx%d @ tick %d\n", *cols,
              *rows, ctui_tick_advance());
  }
}
