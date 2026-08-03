#include "ctui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig_termios;

int ctui_init(void) {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    return -1;

  struct termios raw = orig_termios;
  raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(unsigned)(OPOST);
  raw.c_cflag |= CS8;
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    return -1;

  /* alternate screen buffer + hide cursor */
  printf("\x1b[?1049h\x1b[?25l");
  fflush(stdout);
  return 0;
}

void ctui_shutdown(void) {
  printf("\x1b[?25h\x1b[?1049l");
  fflush(stdout);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void ctui_get_termsize(/*ref*/ int *rows, /*ref*/ int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    *rows = 24;
    *cols = 60;
  } else {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
  }
}

CTUI_SCREEN *ctui_screen_create(int rows, int cols) {
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
  free(s->cells);
  free(s->buffer);
  free(s);
}

void ctui_screen_clear(CTUI_SCREEN *s) {
  for (int i = 0; i < s->rows * s->cols; i++) {
    s->cells[i].ch = ' ';
    s->cells[i].fg = CTUI_COLOR_DEFAULT;
    s->cells[i].bg = CTUI_COLOR_DEFAULT;
  }
}

void ctui_screen_putc(CTUI_SCREEN *s, int row, int col, char ch,
                      unsigned char fg, unsigned char bg) {
  if (row < 0 || row >= s->rows || col < 0 || col >= s->cols)
    return;
  CTUI_CELL *cell = &s->cells[row * s->cols + col];
  cell->ch = ch;
  cell->bg = bg;
  cell->fg = fg;
}

void ctui_screen_puts(CTUI_SCREEN *s, int row, int col, const char *str,
                      unsigned char fg, unsigned char bg) {
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
  if (r <= 0)
    return 0;
  return read(STDIN_FILENO, c, 1) == 1;
}

int ctui_input_loop(CTUI_EVENT *ev) {
  char c;
  ev->type = CTUI_KEYPRESS_EVENT;
  CTUI_KEYPRESS_EVENT_DATA *ev_data =
      (CTUI_KEYPRESS_EVENT_DATA *)ev->event_data;

  if (read(STDIN_FILENO, &c, 1) != 1)
    return 0;

  if (c == '\x1b') {
    char seq0, seq1;
    /* single ESC produces no follow-up; arrow key does */
    if (!read_byte_timeout(&seq0, 50)) {
      ev_data->type = CTUI_KEY_ESC;
      return 1;
    }

    if (!read_byte_timeout(&seq1, 50)) {
      ev_data->type = CTUI_KEY_ESC;
      return 1;
    }

    if (seq0 == '[') {
      switch (seq1) {
      case 'A':
        ev_data->type = CTUI_KEY_UP;
        return 1;
      case 'B':
        ev_data->type = CTUI_KEY_DOWN;
        return 1;
      case 'C':
        ev_data->type = CTUI_KEY_RIGHT;
        return 1;
      case 'D':
        ev_data->type = CTUI_KEY_LEFT;
        return 1;
      default:
        break;
      }
    }

    ev_data->type = CTUI_KEY_ESC;
    return 1;
  }

  if (c == '\r' || c == '\n') {
    ev_data->type = CTUI_KEY_ENTER;
    return 1;
  }

  if (c == '\t') {
    ev_data->type = CTUI_KEY_TAB;
    return 1;
  }

  ev_data->type = CTUI_KEY_CHAR;
  ev_data->ch = c;
  return 1;
}

static CTUI_APP *g_app = NULL;

void ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count) {
  app->widgets = widgets;
  app->count = count;
  g_app = app;
}

void ctui_app_render(CTUI_APP *app, CTUI_SCREEN *screen) {
  for (int i = 0; i < app->count; i++) {
    CTUI_WIDGET *w = app->widgets[i];
    w->render(w, screen);
  }
}

int ctui_process_event(CTUI_EVENT *ev) {
  int changed = 0;
  if (!g_app)
    return 0;
  for (int i = 0; i < g_app->count; i++) {
    CTUI_WIDGET *w = g_app->widgets[i];
    if (w->on_event && w->on_event(w, ev))
      changed = 1;
  }
  return changed;
}

void ctui_app_run(CTUI_APP *app, CTUI_SCREEN *screen) {
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);

  CTUI_KEYPRESS_EVENT_DATA kp_data;
  CTUI_EVENT ev;
  ev.type = CTUI_KEYPRESS_EVENT;
  ev.scope = CTUI_EVENT_SCOPE_GLOBAL;
  ev.event_data = &kp_data;

  while (ctui_input_loop(&ev)) {
    if (kp_data.type == CTUI_KEY_ESC)
      break;

    if (ctui_process_event(&ev)) {
      ctui_app_render(app, screen);
      ctui_screen_flush(screen);
    }
  }
}
