#ifndef CTUI_H
#define CTUI_H

#include "logger.h" /* for E_DBG/E_WRN/E_INF/E_ERR/E_ALL verbosity flags */

#include <stddef.h>

typedef struct {
  char ch;
  unsigned char fg;
  unsigned char bg;
} CTUI_CELL;

typedef struct {
  int rows, cols;
  CTUI_CELL *cells;  /* frame being built */
  CTUI_CELL *buffer; /* buffer currently displayed on screen */
} CTUI_SCREEN;

typedef enum {
  CTUI_KEY_NONE = 0,
  CTUI_KEY_UP,
  CTUI_KEY_DOWN,
  CTUI_KEY_LEFT,
  CTUI_KEY_RIGHT,
  CTUI_KEY_ENTER,
  CTUI_KEY_ESC,
  CTUI_KEY_TAB,
  CTUI_KEY_CHAR,
} CTUI_KEYTYPE;

typedef struct {
  CTUI_KEYTYPE type;
  char ch;
} CTUI_KEYPRESS_EVENT_DATA;

typedef enum {
  CTUI_KEYPRESS_EVENT,
  CTUI_FOCUS_EVENT,
  CTUI_WIDGET_REDRAW,
  CTUI_DUMMY_EVENT,
} CTUI_EVENTTYPE;

typedef enum {
  CTUI_EVENT_SCOPE_GLOBAL,
} CTUI_EVENT_SCOPE;

typedef struct {
  CTUI_EVENTTYPE type;
  CTUI_EVENT_SCOPE scope;
  void *event_data;
} CTUI_EVENT;

typedef struct CTUI_WIDGET CTUI_WIDGET;

struct CTUI_WIDGET {
  int x, y; /* top left position on screen in cells */
  int w, h; /* fixed size in cols/rows */
  void *widget_data;

  /* callback to run when a widget is sent an event; return 0 to reject event or
   * signal an error, and 1 to signal success. NULL is a valid no-op: callers
   * are guarded (see ctui_process_event in ctui.c), so a widget that never
   * handles events can pass NULL instead of a stub function. */
  int (*on_event)(CTUI_WIDGET *self, CTUI_EVENT *ev);
  // int (*init)(void);
  /* render callback, called unconditionally per frame -- unlike on_event,
   * this one must never be NULL. */
  void (*render)(CTUI_WIDGET *self, CTUI_SCREEN *screen);
};

/* Trips if a field is appended to CTUI_WIDGET without updating
 * ctui_widget_make() (ctui.c) to initialize it; a designated initializer
 * silently zero-fills an omitted field instead of erroring, so this struct
 * should always be built via ctui_widget_make() rather than a literal. Does
 * NOT catch a field inserted before `render` -- review ctui_widget_make()
 * by hand if you do that. */
_Static_assert(offsetof(CTUI_WIDGET, render) +
                       sizeof(((CTUI_WIDGET *)0)->render) ==
                   sizeof(CTUI_WIDGET),
               "CTUI_WIDGET changed shape; update ctui_widget_make()");

CTUI_WIDGET ctui_widget_make(int x, int y, int w, int h, void *widget_data,
                             int (*on_event)(CTUI_WIDGET *self,
                                             CTUI_EVENT *ev),
                             void (*render)(CTUI_WIDGET *self,
                                            CTUI_SCREEN *screen));

typedef struct {
  CTUI_WIDGET **widgets;
  int count;
} CTUI_APP;

/* basic ANSI colors */
enum {
  CTUI_COLOR_DEFAULT = 0,
  CTUI_COLOR_BLACK,
  CTUI_COLOR_RED,
  CTUI_COLOR_GREEN,
  CTUI_COLOR_YELLOW,
  CTUI_COLOR_BLUE,
  CTUI_COLOR_MAGENTA,
  CTUI_COLOR_CYAN,
  CTUI_COLOR_WHITE,
};

/* terminal lifecycle; verbosity is a bitmask of E_DBG/E_WRN/E_INF/E_ERR
 * (see logger.h) controlling which ctui_log/ctui_logf calls actually get
 * written */
int ctui_init(int verbosity);
void ctui_shutdown(void);
void ctui_get_termsize(int *rows, int *cols);

/* logging; negative return means the write failed, non-negative is the
 * number of bytes written (0 if filtered out by verbosity). level is
 * exactly one of E_DBG/E_WRN/E_INF/E_ERR (see logger.h) */
int ctui_log(int level, const char *log_str);
int ctui_logf(int level, const char *fmt, ...);
int ctui_tick_advance(void);
const char *ctui_keytype_name(CTUI_KEYTYPE type);
const char *ctui_eventtype_name(CTUI_EVENTTYPE type);

/* screen buffer */
CTUI_SCREEN *ctui_screen_create(int rows, int cols);
void ctui_screen_free(CTUI_SCREEN *s);
void ctui_screen_clear(CTUI_SCREEN *s);
void ctui_screen_putc(CTUI_SCREEN *s, int row, int col, char ch,
                      unsigned char fg, unsigned char bg);
void ctui_screen_puts(CTUI_SCREEN *s, int row, int col, const char *str,
                      unsigned char fg, unsigned char bg);
void ctui_screen_flush(
    CTUI_SCREEN *s); /* diff against prev frame, write only the changes */

/* app / event loop */
void ctui_app_init(CTUI_APP *app, CTUI_WIDGET **widgets, int count);
void ctui_app_render(CTUI_APP *app, CTUI_SCREEN *screen);
int ctui_input_loop(CTUI_EVENT *ev); /* blocking; returns 0 on EOF/error */
void ctui_app_run(CTUI_APP *app, CTUI_SCREEN *screen); /* blocks until ESC */
int ctui_process_event(CTUI_EVENT *ev);
#endif
