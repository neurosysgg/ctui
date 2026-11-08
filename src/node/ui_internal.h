#ifndef CTUI_NODE_UI_INTERNAL_H
#define CTUI_NODE_UI_INTERNAL_H

/* node/ui.c's state, shared with ui_widgets.c (the widgets as drawn):
 * private to the two */

#include "ui.h"

/* writes waiting for their frame at once; more are refused (EBUSY) */
#define LATER_MAX 64

typedef enum {
  W_NONE,
  W_OTHER,
  W_ENTRY,
  W_TABLE,
  W_LIST,
  W_TABS,
  W_DIALOG,
  W_FORM,
  W_POPMENU
} W_KIND;

/* typed writes a widget holds waiting for their frame */
#define TYPED_MAX 8

/* a typed write's answer waiting: checked against what n reads then */
typedef struct {
  CTUI_NODE_LATER later;
  CTUI_NODE *n;
  char *want; /* what n should read once its keys are handled, or NULL */
  unsigned frame; /* the frames drawn when it was written */
} TYPED;

/* a widget published under ui/w/NAME/: named by the app
 * (ctui_ui_name()), or one it drew without a name as KIND/N */
typedef struct {
  CTUI_UI *ui;
  char *name;
  int named;
  W_KIND kind;  /* W_NONE: a name not drawn yet */
  char *kind_s; /* what the draw called it */
  void *obj;
  unsigned flags;
  CTUI_UI_CHANGED changed;
  void *user;
  int shown, seen;
  int x, y, w, h; /* its cells when last drawn */
  int order;      /* its place among the last frame's draws */
  CTUI_NODE *shown_n, *rect, *text, *cursor, *rows, *selected, *selected_text,
      *top, *label, *title, *buttons, *focus, *progress, *entry, *items, *depth,
      *columns, *find, *press;
  /* a form's fields/LABEL (writable: typed), a table's cells/COLUMN, as
   * published */
  CTUI_NODE **fields;
  char **field_names;
  int nfields;
  /* a table's view/N: the rows on screen */
  CTUI_NODE **view;
  int nview;
  TYPED typed[TYPED_MAX];
  int ntyped;
} UIW;

/* a frame's draws (ctui_widget_drawn()) */
typedef struct {
  const char *kind;
  void *obj;
  int x, y, w, h;
} DRAWN;

struct CTUI_UI {
  CTUI_APP *app;
  CTUI_NODES *ns;
  int own;          /* ns is ours (ctui_ui_auto()) */
  CTUI_WIDGET self; /* what the watch and the timer hand back */
  CTUI_IO_WATCH *watch;
  int watched_fd;
  unsigned watched_events;
  CTUI_TIMER *retry;

  CTUI_NODE *frame, *idle, *busy_n, *size, *key, *text, *mouse, *focus;
  int (*busy)(void *user);
  void *busy_user;
  CTUI_NODE **rows;
  int nrows;
  unsigned frames_seen;
  int first; /* nothing published from the screen yet */

  CTUI_NODE_LATER later[LATER_MAX];
  int nlater;

  UIW **ws;
  int nws;
  CTUI_TIMER *frame_soon; /* a widget written: a frame asked for */

  DRAWN *drawn; /* the frame drawn_frame's draws */
  int ndrawn, drawn_cap;
  unsigned drawn_frame;
  unsigned synced_frame; /* the frame the widgets were last matched to */
};

/* node types both use */
extern const CTUI_NODE_TYPE ctui_ui_t_flag, ctui_ui_t_text;

/* n's answer held for the frame (ui.c): 0, EBUSY when too many wait */
int ctui_ui_hold(CTUI_UI *ui, CTUI_NODE *n);

/* keys posted as input/key's are: 0, EAGAIN (no room for them all: none
 * posted) */
int ctui_ui_post_keys(const CTUI_KEYPRESS_EVENT_DATA *keys, int count);

/* --- ui_widgets.c --- */

/* the widget hook: obj drawn in the frame being drawn */
void ctui_ui_on_drawn(const char *kind, void *obj, int x, int y, int w, int h,
                      void *arg);
/* at a wait: the last frame's draws matched, the widgets' state read */
void ctui_ui_widgets_sync(CTUI_UI *ui);
/* w/NAME (len bytes) 's cell dc, dr into it (-1: its middle): 0, ENOENT
 * (no such widget), ENXIO (not drawn in the last frame), ERANGE (outside
 * it) */
int ctui_ui_widget_cell(CTUI_UI *ui, const char *path, size_t len, int dc,
                        int dr, int *x, int *y);
/* at a wait, after the sync: the typed writes answered (their keys are
 * handled by then, and drawn) */
void ctui_ui_widgets_answer(CTUI_UI *ui);
/* every widget's state freed (its nodes go with ui's) */
void ctui_ui_widgets_free(CTUI_UI *ui);

#endif
