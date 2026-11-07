#ifndef CTUI_NODE_UI_H
#define CTUI_NODE_UI_H

#include "../ctui.h"
#include "../widgets/entry.h"
#include "../widgets/list.h"
#include "../widgets/table.h"
#include "../widgets/tabs.h"
#include "node.h"

/* An app's UI as control nodes, for whatever drives it from outside (a
 * test, a script; ctui-wm's notes/ui-nodes.md): what it shows, and input
 * as if typed into its terminal.
 *
 *   ui/frame         int, ro    frames drawn (CTUI_APP.frames)
 *   ui/idle          bool, ro   1: the loop's last wait had nothing due
 *                               within 50 ms (timers, ticks) -- the loop's
 *                               view only: work on threads is the app's to
 *                               publish
 *   ui/size          string, ro "ROWSxCOLS"
 *   ui/screen/NNN    string, ro row NNN's text (000 first), trailing
 *                               blanks dropped; only changed rows are sent
 *   ui/input/key     trigger    keys, space-separated: "ctrl+f", "down
 *                               down enter" (core/keyname.h's names)
 *   ui/input/text    trigger    UTF-8 typed as characters (a newline
 *                               inside is Enter, a tab Tab)
 *   ui/input/mouse   trigger    "[MODS+]VERB COL ROW [BUTTON]", 0-based
 *                               cells, button 0 left (the default), 1
 *                               middle, 2 right: click, press, release,
 *                               move; "drag C1 R1 C2 R2 [BUTTON]";
 *                               "scroll up|down COL ROW". A widget's
 *                               name for COL ROW aims at it (click,
 *                               press, release, move; below):
 *                               "click w/NAME" its middle, "click w/NAME
 *                               C R" C columns and R rows into it
 *   ui/input/focus   bool       the terminal's focus (0: lost, 1: back)
 *
 * Input is posted (ctui_input_post()) and runs through the run loop as the
 * terminal's would. A write is answered once its events are handled and
 * the frame they made is drawn -- its reply, read back on the same open
 * file, is that frame's number -- so a reader sees the screen as it is
 * after the input, and a script needs no sleeps. Text that isn't the
 * grammar is EINVAL; more than the queue takes, EAGAIN. Resizes stay the
 * terminal's (its pty).
 *
 * Widgets the app names are published under ui/w/NAME/ -- the selectors:
 * what a widget holds rather than where its text happens to be. Their
 * state is read at every wait (a widget is the app's plain struct, with
 * no hook of its own), so a value changes once a frame and only when it
 * did:
 *
 *   rect                 every one: "X Y W H", its cells on the screen
 *   text                 an entry's text (cursor: its byte offset)
 *   rows, selected,      a table's or list's row count, the cursor row
 *   selected_text, top   (-1: none), that row's first column, the first
 *                        row shown
 *   selected, label      tabs: the selected item, its label
 *
 * Read-only unless published CTUI_UI_RDWR, like open(2)'s flags: then
 * text and selected take writes, applied to the widget and handed to the
 * app's changed() (it acts on them as on the user's own: a new selection
 * previewed, a search run), answered after the frame as input is. Driving
 * the widget through input tests its keys too; a write sets things up. */

typedef struct CTUI_UI CTUI_UI;

/* app's ui/ tree published in ns (an app with nodes of its own, whose
 * socket it dispatches already); NULL when out of memory or ui/ is taken */
CTUI_UI *ctui_ui_open(CTUI_NODES *ns, CTUI_APP *app);

/* the same in nodes of its own named name, only when $CTUI_UI_NODES is set
 * (and not "0"): connected, the socket watched in app's loop, a daemon
 * looked for every second while none answers. NULL otherwise. Call after
 * ctui_app_init(). */
CTUI_UI *ctui_ui_auto(CTUI_APP *app, const char *name);

/* its nodes removed (and its own nodes closed); before ctui_app_free(),
 * which drops the watches it holds. NULL: nothing. */
void ctui_ui_close(CTUI_UI *ui);

/* the nodes it publishes in (for an app's own next to ui/) */
CTUI_NODES *ctui_ui_nodes(const CTUI_UI *ui);

enum {
  CTUI_UI_RDONLY = 0, /* published to be read (the default) */
  CTUI_UI_RDWR = 1,   /* writes taken too (changed() is then required) */
};

/* after a write to a CTUI_UI_RDWR widget's node, the widget already
 * changed: 0, or an errno the writer gets (the app refused it) */
typedef int (*CTUI_UI_CHANGED)(CTUI_WIDGET *w, void *user);

/* w (drawn from the widget's own struct, the second argument) published
 * as ui/w/NAME/ (name: a node path of its own, "files" or "dialog/name").
 * NULL ui: nothing (an app whose ui/ is off). 0, or -1 (logged): a name
 * taken or bad, RDWR without changed(). The structs must outlive ui, or
 * be let go of first (ctui_ui_forget()). */
int ctui_ui_entry(CTUI_UI *ui, const char *name, CTUI_WIDGET *w,
                  CTUI_ENTRY *e, unsigned flags, CTUI_UI_CHANGED changed,
                  void *user);
int ctui_ui_table(CTUI_UI *ui, const char *name, CTUI_WIDGET *w,
                  CTUI_TABLE *t, unsigned flags, CTUI_UI_CHANGED changed,
                  void *user);
int ctui_ui_list(CTUI_UI *ui, const char *name, CTUI_WIDGET *w, CTUI_LIST *l,
                 unsigned flags, CTUI_UI_CHANGED changed, void *user);
int ctui_ui_tabs(CTUI_UI *ui, const char *name, CTUI_WIDGET *w, CTUI_TABS *t,
                 unsigned flags, CTUI_UI_CHANGED changed, void *user);
/* name's nodes removed (a dialog's widgets as it closes) */
void ctui_ui_forget(CTUI_UI *ui, const char *name);

#endif
