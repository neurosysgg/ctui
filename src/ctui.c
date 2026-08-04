#include "ctui.h"
#include "logger.h"

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

CTUI_SCREEN *ctui_screen_create(int rows, int cols) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - creating %dx%d screen @ tick %d\n", cols,
            rows, ctui_tick_advance());
  CTUI_SCREEN *screen = malloc(sizeof(CTUI_SCREEN));
  screen->rows = rows;
  screen->cols = cols;
  screen->cells = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));
  screen->buffer = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));

  for (int i = 0; i < rows * cols; i++) {
    screen->cells[i].ch = ' ';
    screen->buffer[i].ch = '\0'; /* force buffer flush on initial draw */
  }

  return screen;
}

void ctui_screen_free(CTUI_SCREEN *s) {
  ctui_logf(E_INF, "[CTUI:SCREEN] - freeing %dx%d screen @ tick %d\n",
            s->cols, s->rows, ctui_tick_advance());
  free(s->cells);
  free(s->buffer);
  free(s);
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
  char c;
  ev->type = CTUI_KEYPRESS_EVENT;
  CTUI_KEYPRESS_EVENT_DATA *ev_data =
      (CTUI_KEYPRESS_EVENT_DATA *)ev->event_data;

  ctui_logf(E_DBG, "[CTUI:INPUT] - waiting for input @ tick %d\n",
            ctui_tick_advance());

  if (read(STDIN_FILENO, &c, 1) != 1) {
    ctui_logf(E_WRN, "[CTUI:INPUT] - read failed/EOF @ tick %d\n",
              ctui_tick_advance());
    return 0;
  }

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
                             int (*on_event)(CTUI_WIDGET *self, CTUI_EVENT *ev),
                             void (*render)(CTUI_WIDGET *self,
                                            CTUI_SCREEN *screen)) {
  ctui_logf(E_INF,
            "[CTUI:WIDGET] - creating widget @ tick %d (x=%d, y=%d, w=%d, "
            "h=%d)\n",
            ctui_tick_advance(), x, y, w, h);
  return (CTUI_WIDGET){.x = x,
                       .y = y,
                       .w = w,
                       .h = h,
                       .widget_data = widget_data,
                       .on_event = on_event,
                       .render = render};
}

static CTUI_APP *g_app = NULL;

void ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count) {
  app->widgets = widgets;
  app->count = count;
  g_app = app;
  ctui_logf(E_INF, "[CTUI:APP] - app initialised @ tick %d (%d widgets)\n",
            ctui_tick_advance(), count);
}

void ctui_app_render(CTUI_APP *app, CTUI_SCREEN *screen) {
  ctui_logf(E_INF, "[CTUI:APP] - render pass @ tick %d (%d widgets)\n",
            ctui_tick_advance(), app->count);
  for (int i = 0; i < app->count; i++) {
    CTUI_WIDGET *w = app->widgets[i];
    w->render(w, screen);
  }
}

int ctui_process_event(CTUI_EVENT *ev) {
  int changed = 0;
  ctui_logf(E_INF, "[CTUI:APP] - dispatching %s event @ tick %d\n",
            ctui_eventtype_name(ev->type), ctui_tick_advance());
  if (!g_app) {
    ctui_log(E_WRN, "[CTUI:APP] - no app registered, dropping event\n");
    return 0;
  }
  for (int i = 0; i < g_app->count; i++) {
    CTUI_WIDGET *w = g_app->widgets[i];
    if (w->on_event && w->on_event(w, ev))
      changed = 1;
  }
  ctui_logf(E_INF, "[CTUI:APP] - event dispatched @ tick %d (changed=%d)\n",
            ctui_tick_advance(), changed);
  return changed;
}

void ctui_app_run(CTUI_APP *app, CTUI_SCREEN *screen) {
  ctui_logf(E_INF, "[CTUI:APP] - run loop starting @ tick %d\n",
            ctui_tick_advance());
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);

  CTUI_KEYPRESS_EVENT_DATA kp_data;
  CTUI_EVENT ev;
  ev.type = CTUI_KEYPRESS_EVENT;
  ev.scope = CTUI_EVENT_SCOPE_GLOBAL;
  ev.event_data = &kp_data;

  while (ctui_input_loop(&ev)) {
    if (kp_data.type == CTUI_KEY_ESC) {
      ctui_logf(E_INF,
                "[CTUI:APP] - ESC received @ tick %d, breaking run loop\n",
                ctui_tick_advance());
      break;
    }

    if (ctui_process_event(&ev)) {
      ctui_app_render(app, screen);
      ctui_screen_flush(screen);
    }
  }
  ctui_logf(E_INF, "[CTUI:APP] - run loop exited @ tick %d\n",
            ctui_tick_advance());
}
