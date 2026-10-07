#ifndef CTUI_WIDGETS_POPMENU_H
#define CTUI_WIDGETS_POPMENU_H

#include "../ctui.h"
#include "style.h"

/* A popup menu: a context menu opened at a cell (a right-click's), its
 * submenus cascading beside their items, every level kept on screen (one
 * that won't fit right of / below its point opens left of / above it). It
 * draws nothing while closed; make it the app's last widget, full size, so
 * it paints over the rest. "Modal" is the app's part, as for a dialog:
 * while open the menu owns the keys and the mouse (the registry has no
 * stopping one), so the app's other handlers check ctui_popmenu_is_open().
 *
 * Keys (the deepest level's): up / down (wrapping), Home / End, Right or
 * Enter on a submenu opens it, Enter / Space chooses, Left and Esc close a
 * level (Esc the menu at the top), an item's letter chooses it (moves to
 * the next when several share it). Mouse: the pointer selects (and opens a
 * submenu), a release on an item chooses it -- a right-press, drag, release
 * works, and the release of the press that opened it lands on the frame's
 * corner, not an item -- a press outside closes it.
 *
 * The frame is offered to the style's hook as a CTUI_CONTROL_FRAME (no
 * title); drawn there, the cells over it are left blank on st->bg. */

enum {
  CTUI_POPMENU_SEP = 1,     /* a line between groups, not an item */
  CTUI_POPMENU_OFF = 2,     /* greyed: can't be chosen now */
  CTUI_POPMENU_CHECKED = 4, /* a check mark before it */
  CTUI_POPMENU_RADIO = 8,   /* with CHECKED: a dot instead */
};

typedef struct CTUI_POPMENU_ITEM CTUI_POPMENU_ITEM;
struct CTUI_POPMENU_ITEM {
  const char *label; /* '&' before its letter ("&Play"), "&&" an '&' */
  const char *keys;  /* the keys that do it too ("Ctrl+P"), NULL none */
  int act, arg;      /* the caller's: what it does */
  unsigned flags;    /* CTUI_POPMENU_* */
  const CTUI_POPMENU_ITEM *sub; /* a submenu's items (sub_count), or NULL */
  int sub_count;
};

#define CTUI_POPMENU_DEPTH 4

typedef struct {
  const CTUI_POPMENU_ITEM *items; /* caller-owned, kept while open */
  int count;
  int sel;        /* the cursor's item, -1 none */
  int x, y, w, h; /* the screen cells it took when last laid out */
} CTUI_POPMENU_LEVEL;

typedef struct {
  int depth; /* levels open: 0 closed, 1 the menu, 2.. its submenus */
  CTUI_POPMENU_LEVEL level[CTUI_POPMENU_DEPTH];
  int row, col;                    /* where it was opened (screen cells) */
  const CTUI_POPMENU_ITEM *chosen; /* the last chosen, NULL a cancel */
  const CTUI_STYLE *style;
  int rows, cols; /* the screen it was last laid out on */
} CTUI_POPMENU;

/* items (count of them) opened at the screen cell (row, col), the cursor on
 * none; laid out at once on a screen rows x cols (and again by each render,
 * for a resize) */
void ctui_popmenu_open(CTUI_POPMENU *m, const CTUI_POPMENU_ITEM *items,
                       int count, int row, int col, int rows, int cols);
void ctui_popmenu_close(CTUI_POPMENU *m);

static inline int ctui_popmenu_is_open(const CTUI_POPMENU *m) {
  return m->depth > 0;
}

/* the levels placed on a screen rows x cols */
void ctui_popmenu_layout(CTUI_POPMENU *m, int rows, int cols);

/* the label without its '&'s into out (cap bytes), its letter's byte
 * offset there in *letter (-1 none) */
void ctui_popmenu_label(const char *label, char *out, size_t cap, int *letter);

enum {
  CTUI_POPMENU_NONE = 0,
  CTUI_POPMENU_REDRAW, /* the cursor moved, a submenu opened or closed */
  CTUI_POPMENU_CHOSEN, /* m->chosen; closed */
  CTUI_POPMENU_CLOSED, /* cancelled (Esc, a press outside); chosen NULL */
};

/* what a key / a mouse event did while open (NONE while closed) */
int ctui_popmenu_key(CTUI_POPMENU *m, const CTUI_KEYPRESS_EVENT_DATA *kp);
int ctui_popmenu_mouse(CTUI_POPMENU *m, const CTUI_MOUSE_EVENT_DATA *ms);

void ctui_popmenu_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp);

/* ("input", CTUI_KEYPRESS_EVENT / CTUI_MOUSE_EVENT), only while open: a
 * choice or a cancel emits a CTUI_VALUE_CHANGED_EVENT (source "popmenu",
 * origin self), value = the item's label or NULL for a cancel, enabled =
 * its act or -1 (m->chosen has the item). Every key and mouse event counts
 * as handled while open. */
int ctui_popmenu_handle_keypress(CTUI_WIDGET *self, CTUI_EVENT *ev);
int ctui_popmenu_handle_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev);

#endif
