/* must precede every #include: struct sigaction/sigaction()/sigemptyset()
 * are POSIX, not C11, and -std=c11 sets __STRICT_ANSI__, which suppresses
 * glibc's usual default of exposing them without an explicit feature-test
 * macro. Has to be set before the first system header (even a transitive
 * one, e.g. via "ctui.h" -> "logger.h" -> <stdio.h>) or it's too late --
 * glibc's <features.h> only evaluates it once. */
#define _POSIX_C_SOURCE 200809L

#include "ctui.h"
#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig_termios;
static struct CTUI_LOGGER logger;
static int _ctui_ticks;

/* set (async-signal-safe: just a flag) by handle_sigwinch(); polled by
 * ctui_input_loop() */
static volatile sig_atomic_t g_resize_pending = 0;

static void handle_sigwinch(int sig) {
  (void)sig;
  g_resize_pending = 1;
}

int ctui_tick_advance(void) { return ++_ctui_ticks; }

int ctui_log(int level, const char *log_str) {
  return logger.log_entry(&logger, level, log_str);
}

int ctui_logf(int level, const char *fmt, ...) {
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

const char *ctui_keytype_name(CTUI_KEYTYPE type) {
  switch (type) {
  case CTUI_KEY_NONE:
    return "NONE";
  case CTUI_KEY_UP:
    return "UP";
  case CTUI_KEY_DOWN:
    return "DOWN";
  case CTUI_KEY_LEFT:
    return "LEFT";
  case CTUI_KEY_RIGHT:
    return "RIGHT";
  case CTUI_KEY_ENTER:
    return "ENTER";
  case CTUI_KEY_ESC:
    return "ESC";
  case CTUI_KEY_TAB:
    return "TAB";
  case CTUI_KEY_CHAR:
    return "CHAR";
  }
  return "UNKNOWN";
}

const char *ctui_eventtype_name(CTUI_EVENTTYPE type) {
  switch (type) {
  case CTUI_KEYPRESS_EVENT:
    return "KEYPRESS";
  case CTUI_FOCUS_EVENT:
    return "FOCUS";
  case CTUI_WIDGET_REDRAW:
    return "WIDGET_REDRAW";
  case CTUI_RESIZE_EVENT:
    return "RESIZE";
  case CTUI_VALUE_CHANGED_EVENT:
    return "VALUE_CHANGED";
  case CTUI_DUMMY_EVENT:
    return "DUMMY";
  }
  return "UNKNOWN";
}

int ctui_init(int verbosity) {
  _ctui_ticks = 0;
  logger = init_logger("ctui.log", verbosity);
  ctui_tick_advance();
  ctui_logf(E_INF, "[CTUI:LOG] - logger initialised @ tick %d.\n",
            _ctui_ticks);

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
  shutdown_logger(&logger);
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

static void screen_alloc(CTUI_SCREEN *s, int rows, int cols) {
  s->rows = rows;
  s->cols = cols;
  s->cells = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));
  s->buffer = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));

  for (int i = 0; i < rows * cols; i++) {
    s->cells[i].ch = ' ';
    s->buffer[i].ch = '\0'; /* force buffer flush on next draw */
  }
}

CTUI_SCREEN *ctui_screen_create(int rows, int cols) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - creating %dx%d screen @ tick %d\n", cols,
            rows, ctui_tick_advance());
  CTUI_SCREEN *screen = malloc(sizeof(CTUI_SCREEN));
  screen_alloc(screen, rows, cols);
  return screen;
}

void ctui_screen_free(CTUI_SCREEN *s) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - freeing %dx%d screen @ tick %d\n",
            s->cols, s->rows, ctui_tick_advance());
  free(s->cells);
  free(s->buffer);
  free(s);
}

void ctui_screen_resize(CTUI_SCREEN *s, int rows, int cols) {
  ctui_logf(E_INF,
            "[CTUI:SCREEN] - resizing %dx%d -> %dx%d @ tick %d\n", s->cols,
            s->rows, cols, rows, ctui_tick_advance());
  free(s->cells);
  free(s->buffer);
  screen_alloc(s, rows, cols);

  /* clear the real terminal too -- a shrink could otherwise leave stale
   * content from the old (larger) frame outside the new bounds */
  printf("\x1b[2J");
  fflush(stdout);
}

void ctui_screen_clear(CTUI_SCREEN *s) {
  ctui_logf(E_DBG, "[CTUI:SCREEN] - clearing %dx%d screen @ tick %d\n",
            s->cols, s->rows, ctui_tick_advance());
  for (int i = 0; i < s->rows * s->cols; i++) {
    s->cells[i].ch = ' ';
    s->cells[i].fg = CTUI_COLOR_DEFAULT;
    s->cells[i].bg = CTUI_COLOR_DEFAULT;
  }
}

void ctui_screen_putc(CTUI_SCREEN *s, int row, int col, char ch,
                      unsigned char fg, unsigned char bg) {
  if (row < 0 || row >= s->rows || col < 0 || col >= s->cols) {
    ctui_logf(E_WRN,
              "[CTUI:SCREEN] - putc out of bounds @ tick %d (row=%d, col=%d, "
              "size=%dx%d)\n",
              ctui_tick_advance(), row, col, s->cols, s->rows);
    return;
  }
  ctui_logf(E_DBG,
            "[CTUI:SCREEN] - putc @ tick %d (row=%d, col=%d, ch='%c')\n",
            ctui_tick_advance(), row, col, ch);
  CTUI_CELL *cell = &s->cells[row * s->cols + col];
  cell->ch = ch;
  cell->bg = bg;
  cell->fg = fg;
}

void ctui_screen_puts(CTUI_SCREEN *s, int row, int col, const char *str,
                      unsigned char fg, unsigned char bg) {
  ctui_logf(E_DBG,
            "[CTUI:SCREEN] - puts @ tick %d (row=%d, col=%d, len=%zu): "
            "\"%s\"\n",
            ctui_tick_advance(), row, col, strlen(str), str);
  for (int i = 0; str[i] != '\0'; i++) {
    ctui_screen_putc(s, row, col + i, str[i], fg, bg);
  }
}

static int ansi_fg_code(unsigned char c) {
  return c == CTUI_COLOR_DEFAULT ? 39 : 30 + (c - 1);
}
static int ansi_bg_code(unsigned char c) {
  return c == CTUI_COLOR_DEFAULT ? 49 : 40 + (c - 1);
}

static int ctui_compare_ctuicell(CTUI_CELL *lhs, CTUI_CELL *rhs) {
  return (lhs->ch == rhs->ch && lhs->bg == rhs->bg && lhs->fg == rhs->fg);
}

void ctui_screen_flush(CTUI_SCREEN *s) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - flush starting @ tick %d\n",
            ctui_tick_advance());
  size_t cap = (size_t)s->rows * (size_t)s->cols * 24 + 64;
  char *out = malloc(cap);
  size_t len = 0;

  int last_row = -1, last_col = -1;
  int last_fg = -1, last_bg = -1;

  // iterate over cells, compare to our buffer and rewrite accordingly
  for (int r = 0; r < s->rows; r++) {
    for (int c = 0; c < s->cols; c++) {
      CTUI_CELL *cur = &s->cells[r * s->cols + c];
      CTUI_CELL *buf = &s->buffer[r * s->cols + c];

      if (ctui_compare_ctuicell(cur, buf) == 1)
        continue;

      if (r != last_row || c != last_col) {
        len +=
            (size_t)snprintf(out + len, cap - len, "\x1b[%d;%dH", r + 1, c + 1);
      }

      if (cur->fg != last_fg || cur->bg != last_bg) {
        len += (size_t)snprintf(out + len, cap - len, "\x1b[%d;%dm",
                                ansi_fg_code(cur->fg), ansi_bg_code(cur->bg));
        last_fg = cur->fg;
        last_bg = cur->bg;
      }

      out[len++] = cur->ch;
      last_row = r;
      last_col = c + 1;
    }
  }

  len += (size_t)snprintf(out + len, cap - len, "\x1b[0m");

  write(STDOUT_FILENO, out, len);
  ctui_logf(E_INF, "[CTUI:SCREEN] - flush wrote %zu bytes @ tick %d\n", len,
            ctui_tick_advance());
  free(out);
  memcpy(s->buffer, s->cells,
         sizeof(CTUI_CELL) * (size_t)s->rows * (size_t)s->cols);
}

static void compositor_alloc(CTUI_COMPOSITOR *comp, int rows, int cols) {
  comp->rows = rows;
  comp->cols = cols;
  comp->cells = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));

  for (int i = 0; i < rows * cols; i++) {
    comp->cells[i].ch = ' ';
  }
}

CTUI_COMPOSITOR *ctui_compositor_create(int rows, int cols) {
  ctui_logf(E_INF, "[CTUI:COMPOSITOR] - creating %dx%d compositor @ tick %d\n",
            cols, rows, ctui_tick_advance());
  CTUI_COMPOSITOR *comp = malloc(sizeof(CTUI_COMPOSITOR));
  compositor_alloc(comp, rows, cols);
  return comp;
}

void ctui_compositor_free(CTUI_COMPOSITOR *comp) {
  ctui_logf(E_INF, "[CTUI:COMPOSITOR] - freeing %dx%d compositor @ tick %d\n",
            comp->cols, comp->rows, ctui_tick_advance());
  free(comp->cells);
  free(comp);
}

void ctui_compositor_clear(CTUI_COMPOSITOR *comp) {
  ctui_logf(E_DBG, "[CTUI:COMPOSITOR] - clearing %dx%d compositor @ tick %d\n",
            comp->cols, comp->rows, ctui_tick_advance());
  for (int i = 0; i < comp->rows * comp->cols; i++) {
    comp->cells[i].ch = ' ';
    comp->cells[i].fg = CTUI_COLOR_DEFAULT;
    comp->cells[i].bg = CTUI_COLOR_DEFAULT;
  }
}

void ctui_compositor_resize(CTUI_COMPOSITOR *comp, int rows, int cols) {
  ctui_logf(E_INF,
            "[CTUI:COMPOSITOR] - resizing %dx%d -> %dx%d @ tick %d\n",
            comp->cols, comp->rows, cols, rows, ctui_tick_advance());
  free(comp->cells);
  compositor_alloc(comp, rows, cols);
}

void ctui_compositor_blit(CTUI_COMPOSITOR *comp, CTUI_SCREEN *screen) {
  if (comp->rows != screen->rows || comp->cols != screen->cols) {
    ctui_logf(E_WRN,
              "[CTUI:COMPOSITOR] - blit size mismatch @ tick %d (compositor "
              "%dx%d, screen %dx%d)\n",
              ctui_tick_advance(), comp->cols, comp->rows, screen->cols,
              screen->rows);
    return;
  }
  ctui_logf(E_DBG, "[CTUI:COMPOSITOR] - blit @ tick %d\n",
            ctui_tick_advance());
  memcpy(screen->cells, comp->cells,
         sizeof(CTUI_CELL) * (size_t)comp->rows * (size_t)comp->cols);
}

static int read_byte_timeout(char *c, int timeout_ms) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  struct timeval tv = {.tv_sec = 0, .tv_usec = timeout_ms * 1000};
  int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
  if (r <= 0) {
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout(%dms) timed out @ tick %d\n",
              timeout_ms, ctui_tick_advance());
    return 0;
  }
  int ok = read(STDIN_FILENO, c, 1) == 1;
  if (ok) {
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout got byte 0x%02x @ tick %d\n",
              (unsigned char)*c, ctui_tick_advance());
  } else {
    ctui_logf(E_DBG,
              "[CTUI:INPUT] - read_byte_timeout read failed @ tick %d\n",
              ctui_tick_advance());
  }
  return ok;
}

int ctui_input_loop(CTUI_EVENT *ev) {
  /* owns its own event_data storage rather than relying on the caller to
   * pre-populate ev->event_data, since which struct shape is needed depends
   * on which event type this call ends up producing */
  static CTUI_KEYPRESS_EVENT_DATA kp_data;
  static CTUI_RESIZE_EVENT_DATA resize_data;
  char c;

  for (;;) {
    if (g_resize_pending) {
      g_resize_pending = 0;
      ctui_get_termsize(&resize_data.rows, &resize_data.cols);
      ev->type = CTUI_RESIZE_EVENT;
      ev->ev_source = "terminal";
      ev->event_data = &resize_data;
      ctui_logf(E_INF, "[CTUI:INPUT] - resize detected @ tick %d (%dx%d)\n",
                ctui_tick_advance(), resize_data.cols, resize_data.rows);
      return 1;
    }

    ctui_logf(E_DBG, "[CTUI:INPUT] - waiting for input @ tick %d\n",
              ctui_tick_advance());

    if (read(STDIN_FILENO, &c, 1) == 1) {
      break;
    }
    if (errno == EINTR) {
      /* almost certainly SIGWINCH; loop back to the pending-resize check */
      continue;
    }
    ctui_logf(E_WRN, "[CTUI:INPUT] - read failed/EOF @ tick %d\n",
              ctui_tick_advance());
    return 0;
  }

  ev->type = CTUI_KEYPRESS_EVENT;
  ev->ev_source = "input";
  ev->event_data = &kp_data;
  CTUI_KEYPRESS_EVENT_DATA *ev_data = &kp_data;

  if (c == '\x1b') {
    char seq0, seq1;
    /* single ESC produces no follow-up; arrow key does */
    if (!read_byte_timeout(&seq0, 50)) {
      ev_data->type = CTUI_KEY_ESC;
      ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                ctui_keytype_name(ev_data->type), ctui_tick_advance());
      return 1;
    }

    if (!read_byte_timeout(&seq1, 50)) {
      ev_data->type = CTUI_KEY_ESC;
      ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                ctui_keytype_name(ev_data->type), ctui_tick_advance());
      return 1;
    }

    if (seq0 == '[') {
      switch (seq1) {
      case 'A':
        ev_data->type = CTUI_KEY_UP;
        ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                  ctui_keytype_name(ev_data->type), ctui_tick_advance());
        return 1;
      case 'B':
        ev_data->type = CTUI_KEY_DOWN;
        ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                  ctui_keytype_name(ev_data->type), ctui_tick_advance());
        return 1;
      case 'C':
        ev_data->type = CTUI_KEY_RIGHT;
        ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                  ctui_keytype_name(ev_data->type), ctui_tick_advance());
        return 1;
      case 'D':
        ev_data->type = CTUI_KEY_LEFT;
        ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
                  ctui_keytype_name(ev_data->type), ctui_tick_advance());
        return 1;
      default:
        break;
      }
    }

    ev_data->type = CTUI_KEY_ESC;
    ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
              ctui_keytype_name(ev_data->type), ctui_tick_advance());
    return 1;
  }

  if (c == '\r' || c == '\n') {
    ev_data->type = CTUI_KEY_ENTER;
    ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
              ctui_keytype_name(ev_data->type), ctui_tick_advance());
    return 1;
  }

  if (c == '\t') {
    ev_data->type = CTUI_KEY_TAB;
    ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s @ tick %d\n",
              ctui_keytype_name(ev_data->type), ctui_tick_advance());
    return 1;
  }

  ev_data->type = CTUI_KEY_CHAR;
  ev_data->ch = c;
  ctui_logf(E_INF, "[CTUI:INPUT] - resolved key %s ('%c') @ tick %d\n",
            ctui_keytype_name(ev_data->type), c, ctui_tick_advance());
  return 1;
}

CTUI_WIDGET ctui_widget_make(int x, int y, int w, int h, void *widget_data,
                             void (*render)(CTUI_WIDGET *self,
                                            CTUI_COMPOSITOR *comp),
                             void (*layout)(CTUI_WIDGET *self,
                                           CTUI_COMPOSITOR *comp)) {
  ctui_logf(E_INF,
            "[CTUI:WIDGET] - creating widget @ tick %d (x=%d, y=%d, w=%d, "
            "h=%d)\n",
            ctui_tick_advance(), x, y, w, h);
  return (CTUI_WIDGET){.x = x,
                       .y = y,
                       .w = w,
                       .h = h,
                       .widget_data = widget_data,
                       .ticks = 0,
                       .buf = NULL,
                       .layout = layout,
                       .render = render};
}

void ctui_widget_tick_advance(CTUI_WIDGET *widget) { ++widget->ticks; }

void ctui_widget_init(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp) {
  if (widget->layout) {
    widget->layout(widget, comp);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p layout() re-run @ tick %d (x=%d, "
              "y=%d, w=%d, h=%d)\n",
              (void *)widget, ctui_tick_advance(), widget->x, widget->y,
              widget->w, widget->h);
  }

  if (widget->x < 0 || widget->x >= comp->cols || widget->y < 0 ||
      widget->y >= comp->rows) {
    ctui_logf(E_WRN,
              "[CTUI:WIDGET] - widget %p origin out of compositor bounds @ "
              "tick %d (x=%d, y=%d, compositor=%dx%d); leaving buf NULL\n",
              (void *)widget, ctui_tick_advance(), widget->x, widget->y,
              comp->cols, comp->rows);
    widget->buf = NULL;
    return;
  }
  widget->buf = comp->cells + (size_t)widget->y * (size_t)comp->cols +
               (size_t)widget->x;
  ctui_logf(E_INF,
            "[CTUI:WIDGET] - widget %p bound to compositor slice @ tick %d "
            "(x=%d, y=%d, w=%d, h=%d)\n",
            (void *)widget, ctui_tick_advance(), widget->x, widget->y,
            widget->w, widget->h);
}

void ctui_widget_putc(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, char ch, unsigned char fg, unsigned char bg) {
  if (widget->buf == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:WIDGET] - putc rejected @ tick %d, widget %p not bound "
              "to a compositor slice (call ctui_widget_init() first)\n",
              ctui_tick_advance(), (void *)widget);
    return;
  }
  if (row < 0 || row >= widget->h || col < 0 || col >= widget->w) {
    ctui_logf(E_WRN,
              "[CTUI:WIDGET] - putc out of widget bounds @ tick %d (row=%d, "
              "col=%d, size=%dx%d)\n",
              ctui_tick_advance(), row, col, widget->w, widget->h);
    return;
  }
  int abs_row = widget->y + row;
  int abs_col = widget->x + col;
  if (abs_row < 0 || abs_row >= comp->rows || abs_col < 0 ||
      abs_col >= comp->cols) {
    ctui_logf(E_WRN,
              "[CTUI:WIDGET] - putc out of compositor bounds @ tick %d "
              "(row=%d, col=%d, compositor=%dx%d)\n",
              ctui_tick_advance(), abs_row, abs_col, comp->cols, comp->rows);
    return;
  }
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - putc @ tick %d (row=%d, col=%d, ch='%c')\n",
            ctui_tick_advance(), row, col, ch);
  CTUI_CELL *cell = widget->buf + (size_t)row * (size_t)comp->cols + (size_t)col;
  cell->ch = ch;
  cell->bg = bg;
  cell->fg = fg;
}

void ctui_widget_puts(CTUI_WIDGET *widget, CTUI_COMPOSITOR *comp, int row,
                      int col, const char *str, unsigned char fg,
                      unsigned char bg) {
  ctui_logf(E_DBG,
            "[CTUI:WIDGET] - puts @ tick %d (row=%d, col=%d, len=%zu): "
            "\"%s\"\n",
            ctui_tick_advance(), row, col, strlen(str), str);
  for (int i = 0; str[i] != '\0'; i++) {
    ctui_widget_putc(widget, comp, row, col + i, str[i], fg, bg);
  }
}

int ctui_util_center_h(char *center_str, char *line, CTUI_CELL fill) {
  size_t str_len = strlen(center_str);
  size_t line_len = strlen(line);

  if (str_len > line_len) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - center_h rejected @ tick %d, center_str (%zu "
              "chars) longer than line (%zu chars)\n",
              ctui_tick_advance(), str_len, line_len);
    return -1;
  }

  size_t total_pad = line_len - str_len;
  size_t left_pad = total_pad / 2;
  size_t right_pad = total_pad - left_pad;

  memset(line, fill.ch, left_pad);
  memcpy(line + left_pad, center_str, str_len);
  memset(line + left_pad + str_len, fill.ch, right_pad);

  ctui_logf(E_DBG,
            "[CTUI:UTIL] - center_h @ tick %d (\"%s\" in %zu-wide line, "
            "left_pad=%zu, right_pad=%zu)\n",
            ctui_tick_advance(), center_str, line_len, left_pad, right_pad);
  return 0;
}

int ctui_util_truncate_str(char *str, size_t desired, char *trunc) {
  size_t str_len = strlen(str);
  size_t trunc_len = strlen(trunc);

  if (str_len <= desired) {
    return 0;
  }

  if (trunc_len > desired) {
    ctui_logf(E_WRN,
              "[CTUI:UTIL] - truncate_str rejected @ tick %d, trunc (%zu "
              "chars) longer than desired (%zu chars)\n",
              ctui_tick_advance(), trunc_len, desired);
    return -1;
  }

  size_t keep = desired - trunc_len;
  memcpy(str + keep, trunc, trunc_len);
  str[desired] = '\0';

  ctui_logf(E_DBG,
            "[CTUI:UTIL] - truncate_str @ tick %d (%zu chars -> %zu chars)\n",
            ctui_tick_advance(), str_len, desired);
  return 0;
}

CTUI_GROUP ctui_group_make(char *group_id, CTUI_WIDGET *members, size_t size) {
  ctui_logf(E_INF,
            "[CTUI:GROUP] - creating group \"%s\" @ tick %d (%zu members)\n",
            group_id, ctui_tick_advance(), size);
  return (CTUI_GROUP){.size = size, .group_id = group_id, .members = members};
}

void ctui_group_init(CTUI_GROUP *group, CTUI_COMPOSITOR *comp) {
  if (group->size == 0) {
    ctui_logf(E_WRN,
              "[CTUI:GROUP] - group \"%s\" has no members @ tick %d, nothing "
              "to bind\n",
              group->group_id, ctui_tick_advance());
    return;
  }

  ctui_widget_init(&group->members[0], comp);
  CTUI_CELL *shared_buf = group->members[0].buf;

  for (size_t i = 1; i < group->size; i++) {
    group->members[i].buf = shared_buf;
  }

  ctui_logf(E_INF,
            "[CTUI:GROUP] - group \"%s\" bound to shared compositor slice @ "
            "tick %d (%zu members)\n",
            group->group_id, ctui_tick_advance(), group->size);
}

void ctui_group_render(CTUI_GROUP *group, CTUI_COMPOSITOR *comp) {
  ctui_logf(E_INF,
            "[CTUI:GROUP] - render pass @ tick %d (group \"%s\", %zu "
            "members)\n",
            ctui_tick_advance(), group->group_id, group->size);
  for (size_t i = 0; i < group->size; i++) {
    CTUI_WIDGET *w = &group->members[i];

    ctui_widget_tick_advance(w);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) pre-render @ "
              "widget-tick %d\n",
              (void *)w, w->x, w->y, w->ticks);

    w->render(w, comp);

    ctui_widget_tick_advance(w);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) post-render @ "
              "widget-tick %d\n",
              (void *)w, w->x, w->y, w->ticks);
  }
}

void ctui_split_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_SPLIT *split = self->widget_data;
  split->comp = comp;

  if (split->count <= 0) {
    ctui_logf(E_WRN,
              "[CTUI:SPLIT] - widget %p has no active children @ tick %d, "
              "nothing to lay out\n",
              (void *)self, ctui_tick_advance());
    return;
  }

  if (split->mode == CTUI_SPLIT_V) {
    int base = self->h / split->count;
    int remainder = self->h - base * split->count;
    int y = self->y;
    for (int i = 0; i < split->count; i++) {
      CTUI_WIDGET *child = split->children[i];
      child->x = self->x;
      child->w = self->w;
      child->y = y;
      child->h = base + (i == split->count - 1 ? remainder : 0);
      y += child->h;
      ctui_widget_init(child, comp);
    }
  } else {
    int base = self->w / split->count;
    int remainder = self->w - base * split->count;
    int x = self->x;
    for (int i = 0; i < split->count; i++) {
      CTUI_WIDGET *child = split->children[i];
      child->y = self->y;
      child->h = self->h;
      child->x = x;
      child->w = base + (i == split->count - 1 ? remainder : 0);
      x += child->w;
      ctui_widget_init(child, comp);
    }
  }

  ctui_logf(E_INF,
            "[CTUI:SPLIT] - widget %p laid out @ tick %d (%s, %d active "
            "children, self=%dx%d @ %d,%d)\n",
            (void *)self, ctui_tick_advance(),
            split->mode == CTUI_SPLIT_V ? "V" : "H", split->count, self->w,
            self->h, self->x, self->y);
}

void ctui_split_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  CTUI_SPLIT *split = self->widget_data;
  ctui_logf(E_INF,
            "[CTUI:SPLIT] - render pass @ tick %d (widget %p, %d active "
            "children)\n",
            ctui_tick_advance(), (void *)self, split->count);
  for (int i = 0; i < split->count; i++) {
    CTUI_WIDGET *child = split->children[i];

    ctui_widget_tick_advance(child);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) pre-render @ "
              "widget-tick %d\n",
              (void *)child, child->x, child->y, child->ticks);

    child->render(child, comp);

    ctui_widget_tick_advance(child);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) post-render @ "
              "widget-tick %d\n",
              (void *)child, child->x, child->y, child->ticks);
  }
}

struct CTUI_EVENT_HANDLER {
  const char *source;
  CTUI_EVENTTYPE type;
  CTUI_WIDGET *widget;
  int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev);
};

static CTUI_APP *g_app = NULL;

void ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count, int rows,
                   int cols) {
  app->widgets = widgets;
  app->count = count;
  app->comp = ctui_compositor_create(rows, cols);
  app->handlers = NULL;
  app->handler_count = 0;
  app->handler_cap = 0;
  g_app = app;
  ctui_logf(E_INF,
            "[CTUI:APP] - app initialised @ tick %d (%d widgets, %dx%d "
            "compositor)\n",
            ctui_tick_advance(), count, cols, rows);

  for (int i = 0; i < count; i++) {
    ctui_widget_init(widgets[i], app->comp);
  }
}

void ctui_app_free(CTUI_APP *app) {
  ctui_logf(E_INF, "[CTUI:APP] - freeing app @ tick %d\n",
            ctui_tick_advance());
  free(app->handlers);
  ctui_compositor_free(app->comp);
}

void ctui_app_render(CTUI_APP *app, CTUI_SCREEN *screen) {
  ctui_logf(E_INF, "[CTUI:APP] - render pass @ tick %d (%d widgets)\n",
            ctui_tick_advance(), app->count);
  ctui_compositor_clear(app->comp);
  for (int i = 0; i < app->count; i++) {
    CTUI_WIDGET *w = app->widgets[i];

    ctui_widget_tick_advance(w);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) pre-render @ "
              "widget-tick %d\n",
              (void *)w, w->x, w->y, w->ticks);

    w->render(w, app->comp);

    ctui_widget_tick_advance(w);
    ctui_logf(E_DBG,
              "[CTUI:WIDGET] - widget %p (x=%d, y=%d) post-render @ "
              "widget-tick %d\n",
              (void *)w, w->x, w->y, w->ticks);
  }
  ctui_compositor_blit(app->comp, screen);
}

void ctui_event_register(const char *source, CTUI_EVENTTYPE type,
                         CTUI_WIDGET *widget,
                         int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev)) {
  if (!g_app) {
    ctui_log(E_WRN,
             "[CTUI:EVENT] - no app registered, dropping registration\n");
    return;
  }

  if (g_app->handler_count == g_app->handler_cap) {
    int new_cap = g_app->handler_cap == 0 ? 4 : g_app->handler_cap * 2;
    g_app->handlers = realloc(g_app->handlers,
                              (size_t)new_cap * sizeof(CTUI_EVENT_HANDLER));
    g_app->handler_cap = new_cap;
  }

  g_app->handlers[g_app->handler_count++] = (CTUI_EVENT_HANDLER){
      .source = source, .type = type, .widget = widget, .handler = handler};

  ctui_logf(E_INF,
            "[CTUI:EVENT] - registered handler @ tick %d (source=\"%s\", "
            "type=%s, widget=%p)\n",
            ctui_tick_advance(), source, ctui_eventtype_name(type),
            (void *)widget);
}

int ctui_handle_event(CTUI_EVENT *ev) {
  int changed = 0;
  ctui_logf(E_INF,
            "[CTUI:EVENT] - dispatching %s event @ tick %d (source=\"%s\")\n",
            ctui_eventtype_name(ev->type), ctui_tick_advance(),
            ev->ev_source ? ev->ev_source : "(null)");
  if (!g_app) {
    ctui_log(E_WRN, "[CTUI:EVENT] - no app registered, dropping event\n");
    return 0;
  }
  for (int i = 0; i < g_app->handler_count; i++) {
    CTUI_EVENT_HANDLER *h = &g_app->handlers[i];
    if (h->type != ev->type)
      continue;
    if (h->source == NULL || ev->ev_source == NULL ||
        strcmp(h->source, ev->ev_source) != 0)
      continue;
    if (h->handler(h->widget, ev))
      changed = 1;
  }
  ctui_logf(E_INF, "[CTUI:EVENT] - event dispatched @ tick %d (changed=%d)\n",
            ctui_tick_advance(), changed);
  return changed;
}

void ctui_app_resize(CTUI_APP *app, CTUI_SCREEN *screen, int rows, int cols) {
  ctui_logf(E_INF, "[CTUI:APP] - resize @ tick %d (%dx%d -> %dx%d)\n",
            ctui_tick_advance(), app->comp->cols, app->comp->rows, cols,
            rows);

  ctui_compositor_resize(app->comp, rows, cols);
  ctui_screen_resize(screen, rows, cols);

  /* rebind every widget (and re-run each one's optional layout() first, so
   * dynamic geometry reflows against the new size) before anything else
   * touches app->comp -- widgets with stale buf pointers from the freed
   * compositor would otherwise write into freed memory */
  for (int i = 0; i < app->count; i++) {
    ctui_widget_init(app->widgets[i], app->comp);
  }

  CTUI_RESIZE_EVENT_DATA resize_data = {.rows = rows, .cols = cols};
  CTUI_EVENT ev = {.type = CTUI_RESIZE_EVENT,
                   .scope = CTUI_EVENT_SCOPE_GLOBAL,
                   .ev_source = "terminal",
                   .event_data = &resize_data};
  ctui_handle_event(&ev);
}

void ctui_app_run(CTUI_APP *app, CTUI_SCREEN *screen) {
  ctui_logf(E_INF, "[CTUI:APP] - run loop starting @ tick %d\n",
            ctui_tick_advance());
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);

  CTUI_EVENT ev;
  ev.scope = CTUI_EVENT_SCOPE_GLOBAL;

  while (ctui_input_loop(&ev)) {
    if (ev.type == CTUI_RESIZE_EVENT) {
      CTUI_RESIZE_EVENT_DATA *resize_data = ev.event_data;
      ctui_app_resize(app, screen, resize_data->rows, resize_data->cols);
      ctui_app_render(app, screen);
      ctui_screen_flush(screen);
      continue;
    }

    if (ev.type == CTUI_KEYPRESS_EVENT) {
      CTUI_KEYPRESS_EVENT_DATA *kp_data = ev.event_data;
      if (kp_data->type == CTUI_KEY_ESC) {
        ctui_logf(E_INF,
                  "[CTUI:APP] - ESC received @ tick %d, breaking run loop\n",
                  ctui_tick_advance());
        break;
      }
    }

    if (ctui_handle_event(&ev)) {
      ctui_app_render(app, screen);
      ctui_screen_flush(screen);
    }
  }
  ctui_logf(E_INF, "[CTUI:APP] - run loop exited @ tick %d\n",
            ctui_tick_advance());
}
