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

static struct termios orig_termios, raw_termios;
/* 0 off, else 1 + the tracking level: clicks 1, drag 2, any motion 3 */
static int g_mouse_enabled = 0;
int ctui_g_mouse_pixels = 0;
static int g_focus_enabled = 0;
static int g_kitty_keys_enabled = 0;

volatile sig_atomic_t ctui_g_resize_pending = 0;
unsigned int ctui_g_gfx_mode = 0;

static void handle_sigwinch(int sig) {
  (void)sig;
  ctui_g_resize_pending = 1;
}

/* highest single CTUI_GFX_MODE tier actually present in caps -- used to
 * negotiate down when the requested tier isn't there. Tiers are
 * cumulative in what ctui_gfx_detect_caps() sets (a KITTY terminal's caps
 * also carries TRUECOLOR/ANSI256's bits), so checking top-down and
 * returning the first hit is enough. */
static CTUI_GFX_MODE gfx_max_supported(unsigned int caps) {
  if (caps & CTUI_GFX_KITTY) {
    return CTUI_GFX_KITTY;
  }
  if (caps & CTUI_GFX_TRUECOLOR) {
    return CTUI_GFX_TRUECOLOR;
  }
  if (caps & CTUI_GFX_ANSI256) {
    return CTUI_GFX_ANSI256;
  }
  return CTUI_GFX_ANSI16;
}

int ctui_init(int verbosity, CTUI_GFX_MODE *mode) {
  ctui_log_init(verbosity);

  unsigned int caps = ctui_gfx_detect_caps();
  if (!(caps & CTUI_GFX_ANSI16)) {
    ctui_log(E_ERR,
             "[CTUI:GFX] - ANSI16 floor unsupported by this terminal, "
             "cannot continue\n");
    return -1;
  }
  if (!(caps & (unsigned int)*mode)) {
    CTUI_GFX_MODE negotiated = gfx_max_supported(caps);
    ctui_logf(E_WRN,
              "[CTUI:GFX] - requested mode 0x%x not supported @ tick %d "
              "(caps=0x%x), negotiating down to 0x%x\n",
              (unsigned int)*mode, ctui_tick_advance(), caps,
              (unsigned int)negotiated);
    *mode = negotiated;
  }
  ctui_g_gfx_mode = (unsigned int)*mode;
  ctui_logf(E_INF, "[CTUI:GFX] - negotiated mode 0x%x @ tick %d\n",
            ctui_g_gfx_mode, ctui_tick_advance());

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

  raw_termios = raw;
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

  /* Phase 6: only worth probing t=s support once the negotiated tier is
   * actually CTUI_GFX_KITTY -- for every other tier there's no Kitty
   * transport question to answer at all. Raw mode is already active at
   * this point (byte-level, unbuffered stdin reads, required for the
   * probe's own bounded read of the terminal's APC reply); doing this
   * before the alternate-screen switch just below means a probe that
   * somehow printed anything visible (it shouldn't -- a=q never
   * displays) would land on the normal screen, not linger into the
   * app's own alt-screen frame. */
  if (ctui_g_gfx_mode == CTUI_GFX_KITTY) {
    ctui_gfx_kitty_probe_shm();
  }

  /* alternate screen buffer + hide cursor */
  printf("\x1b[?1049h\x1b[?25l");
  fflush(stdout);
  ctui_logf(E_INF,
            "[CTUI:INIT] - init complete @ tick %d, alternate screen buffer "
            "active\n",
            ctui_tick_advance());
  return 0;
}

static void mouse_on(int level) {
  printf(level == 3   ? "\x1b[?1000h\x1b[?1003h\x1b[?1006h"
         : level == 2 ? "\x1b[?1000h\x1b[?1002h\x1b[?1006h"
                      : "\x1b[?1000h\x1b[?1006h");
  if (ctui_g_mouse_pixels) {
    printf("\x1b[?1016h");
  }
}

/* every mode ctui turned on, off again (the flags stay: for a resume) */
static void modes_off(void) {
  if (g_mouse_enabled) {
    printf("%s\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l",
           ctui_g_mouse_pixels ? "\x1b[?1016l" : "");
  }
  if (g_focus_enabled) {
    printf("\x1b[?1004l");
  }
  if (g_kitty_keys_enabled) {
    printf("\x1b[<u");
  }
  printf("\x1b[?25h\x1b[?1049l");
}

void ctui_suspend(void) {
  ctui_logf(E_INF, "[CTUI:TERM] - suspended @ tick %d\n",
            ctui_tick_advance());
  modes_off();
  fflush(stdout);
  tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios);
}

void ctui_resume(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios);
  printf("\x1b[?1049h\x1b[?25l");
  if (g_mouse_enabled) {
    mouse_on(g_mouse_enabled);
  }
  if (g_focus_enabled) {
    printf("\x1b[?1004h");
  }
  if (g_kitty_keys_enabled) {
    printf("\x1b[>1u");
  }
  fflush(stdout);
  ctui_g_resize_pending = 1; /* a full redraw, at whatever size it is now */
  ctui_logf(E_INF, "[CTUI:TERM] - resumed @ tick %d\n", ctui_tick_advance());
}

void ctui_mouse_enable(int track_motion) {
  int level = track_motion == CTUI_MOUSE_TRACK_ANY    ? 3
              : track_motion == CTUI_MOUSE_TRACK_DRAG ? 2
                                                      : 1;
  if (level <= g_mouse_enabled) {
    return; /* already at least that: setting a lower mode would drop it */
  }
  /* 1000 = press/release, 1002 = motion with a button held, 1003 = any
   * motion (each replaces the one before), 1006 = SGR encoding (no
   * 223-column limit, unambiguous release button) */
  mouse_on(level);
  fflush(stdout);
  g_mouse_enabled = level;
  ctui_logf(E_INF, "[CTUI:TERM] - mouse reporting on @ tick %d (motion=%d)\n",
            ctui_tick_advance(), track_motion);
}

void ctui_mouse_pixels_enable(void) {
  if (ctui_g_mouse_pixels) {
    return;
  }
  /* 1016 = SGR-Pixels: the same reports, x/y in pixels of the window;
   * a terminal without it ignores the mode and keeps sending cells,
   * which the replies can't tell apart -- so only ask where the cell
   * size is known (resolve_mouse() needs it for row/col) */
  int cw, ch;
  if (ctui_cell_px(&cw, &ch) != 0) {
    ctui_logf(E_WRN,
              "[CTUI:TERM] - pixel mouse not turned on: the cell size is "
              "unknown @ tick %d\n",
              ctui_tick_advance());
    return;
  }
  ctui_g_mouse_pixels = 1;
  if (g_mouse_enabled) {
    printf("\x1b[?1016h");
    fflush(stdout);
  }
  ctui_logf(E_INF, "[CTUI:TERM] - pixel mouse reports on @ tick %d\n",
            ctui_tick_advance());
}

static int g_cell_w = 0, g_cell_h = 0;

void ctui_cell_px_set(int cw, int ch) {
  g_cell_w = cw;
  g_cell_h = ch;
}

int ctui_cell_px(int *cw, int *ch) {
  if (g_cell_w > 0 && g_cell_h > 0) {
    *cw = g_cell_w;
    *ch = g_cell_h;
    return 0;
  }
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || !ws.ws_col || !ws.ws_row ||
      !ws.ws_xpixel || !ws.ws_ypixel) {
    return -1;
  }
  *cw = ws.ws_xpixel / ws.ws_col;
  *ch = ws.ws_ypixel / ws.ws_row;
  return *cw > 0 && *ch > 0 ? 0 : -1;
}

void ctui_focus_enable(void) {
  printf("\x1b[?1004h");
  fflush(stdout);
  g_focus_enabled = 1;
  ctui_logf(E_INF, "[CTUI:TERM] - focus reporting on @ tick %d\n",
            ctui_tick_advance());
}

void ctui_kitty_keys_enable(void) {
  /* push flags 1 (disambiguate) onto the terminal's stack; popped at
   * shutdown, so whatever ran before gets its own flags back */
  printf("\x1b[>1u");
  fflush(stdout);
  g_kitty_keys_enabled = 1;
  ctui_logf(E_INF, "[CTUI:TERM] - kitty keyboard protocol on @ tick %d\n",
            ctui_tick_advance());
}

void ctui_shutdown(void) {
  ctui_logf(E_INF, "[CTUI:INIT] - shutting down @ tick %d\n",
            ctui_tick_advance());
  /* before the log closes, so a failure here is still loggable: unlink
   * any Phase 6 t=s segment the terminal never got around to reading.
   * A no-op on every non-Kitty tier (nothing was ever tracked) -- see
   * ctui_gfx_kitty_shm_reap()'s own doc comment. */
  ctui_gfx_kitty_shm_reap();
  ctui_log_shutdown();
  modes_off();
  g_mouse_enabled = g_focus_enabled = g_kitty_keys_enabled = 0;
  ctui_g_mouse_pixels = 0;
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
