#ifndef CTUI_NODE_UI_H
#define CTUI_NODE_UI_H

#include "../ctui.h"
#include "node.h"

/* An app's UI as control nodes, for whatever drives it from outside (a
 * test, a script; ctui-wm's notes/ui-nodes.md): what it shows, and input
 * as if typed into its terminal.
 *
 *   ui/frame         int, ro    frames drawn (CTUI_APP.frames)
 *   ui/idle          bool, ro   1: the loop's last wait had nothing due
 *                               within 50 ms (timers, ticks) and the app
 *                               wasn't busy (ctui_ui_busy())
 *   ui/busy          bool, ro   the app's busy() at that wait: work on a
 *                               thread or in a child the loop can't see
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
 * Widgets are found as they're drawn: ctui's widgets tell what they draw
 * (core/widget.h's ctui_widget_drawn(): a table inside another widget's
 * render, an entry in a dialog), and each one is published under ui/w/ --
 * the selectors: what a widget holds rather than where its text happens to
 * be. The app names the ones a test should find by name
 * (ctui_ui_name(): ui/w/NAME/); the others are ui/w/KIND/N, the Nth
 * unnamed KIND drawn in the last frame ("table/0", "entry/2"), there
 * while they're drawn -- usable, but not stable across versions of the
 * app. Their state is read at the wait after a frame that drew them (a
 * widget is the app's plain struct, with no hook of its own; what its draw
 * read is there then), so a value changes once a frame and only when it
 * did, and a widget not drawn keeps its last values:
 *
 *   shown                every one: 1 while drawn in the last frame (a
 *                        named one not drawn: 0, and its other nodes
 *                        only once it has been)
 *   rect                 "X Y W H", its cells on the screen when last
 *                        drawn (a dialog's: its box)
 *   text, cursor         an entry's text, the cursor's byte offset
 *   rows, selected,      a table's or list's row count, the cursor row
 *   selected_text, top   (-1: none), that row's first column, the first
 *                        row shown
 *   rows, selected,      tabs: the items, the selected one, its label
 *   label
 *
 * A table's by column (asked of its cell() as its render does, for the
 * rows on screen only: a 100 000-row table costs what drawing it does):
 *
 *   columns              its titles "Name|Size|Modified"
 *   cells/NAME           the cursor row's cell in each column, named by
 *                        title like a form's fields (cells/name; "col2"
 *                        for an untitled one)
 *   view/N               the Nth row on screen (N from 0; row top + N), its
 *                        cells joined by tabs; republished as the page
 *                        grows or shrinks
 *   find                 named CTUI_UI_RDWR only: "COLUMN=TEXT" (a cells/
 *                        name or a column's number) selects the first row
 *                        whose cell there is TEXT, through changed() as
 *                        selected's writes go; ENOENT for no such row or
 *                        column, EINVAL without '='
 *
 * A dialog's and a form's own:
 *
 *   title, text, buttons a dialog's title, text, its buttons' labels
 *                        "Rename|Cancel"
 *   focus                the focused button's label; a form's focused
 *                        row's field name (a button's label); a menu's
 *                        item under the cursor ("" none)
 *   progress, entry      a dialog's bar (-1: none), what its line holds
 *   fields/NAME          a form's rows by label, letters and digits in
 *                        lower case and the rest '_' ("Repeat rate":
 *                        fields/repeat_rate; a repeated one _2, ...):
 *                        an entry's text, a toggle's 0 / 1, a choice's
 *                        option, a slider's value; republished when the
 *                        rows change. Headings and buttons aren't fields.
 *   items, depth         a popmenu's open level's labels "Open|Copy" (no
 *                        separators, no '&'s), the levels open
 *
 * An entry inside a drawn form or dialog is theirs (fields/NAME, entry),
 * not an unnamed one too.
 *
 * A dialog's entry and press and a form's fields/NAME and press take
 * writes whatever the flags: they're typed as keys, as input/key's are,
 * so the app's own handling of the keys runs (most apps act on what
 * ctui_form_key() / ctui_dialog_key() return). A field's value as it
 * reads: an entry's text (ctrl+e ctrl+u, then the text), a toggle's 0 / 1
 * (space if it differs), a choice's option (left / right), a slider's
 * value (its steps, home / end at the ends); the focus walked to the row
 * first (up / down). press: a button by its label (the focus put on it,
 * Enter). Refused: ENXIO not shown (a dialog: not open), ENOENT no such
 * button / option, EINVAL a value it can't take (a toggle's 2, a slider
 * off its steps), ERANGE a slider outside its range, EPERM a disabled
 * row, EBUSY a form under an open dialog (drawn over it in the last
 * frame: the keys would be the dialog's) or with a choice's list down,
 * EAGAIN more keys than the queue takes. The keys go where the app sends
 * keys, so a test puts the focus in the form first (a pane beside it may
 * have them): answered after their frame, a write that didn't take is
 * EIO -- the field not reading what was written then, or nothing drawn
 * since (a press: nothing drawn). Other kinds (textview, an app's own) have shown
 * and rect. "click w/NAME" refuses a widget not shown (ENXIO), a cell
 * outside it (ERANGE).
 *
 * Read-only unless named CTUI_UI_RDWR, like open(2)'s flags: then text,
 * selected and find take writes, applied to the widget and handed to the
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

/* busy(user), asked at every wait: nonzero while the app works where the
 * loop can't see it (a search in a child, a copy on a thread); ui/busy,
 * and ui/idle stays 0 meanwhile, so a test waiting on idle waits for it.
 * NULL ui: nothing. */
void ctui_ui_busy(CTUI_UI *ui, int (*busy)(void *user), void *user);

/* the nodes it publishes in (for an app's own next to ui/) */
CTUI_NODES *ctui_ui_nodes(const CTUI_UI *ui);

enum {
  CTUI_UI_RDONLY = 0, /* published to be read (the default) */
  CTUI_UI_RDWR = 1,   /* writes taken too (changed() is then required) */
};

/* after a write to a CTUI_UI_RDWR widget's node, the widget (obj, as
 * named) already changed: 0, or an errno the writer gets (the app refused
 * it) */
typedef int (*CTUI_UI_CHANGED)(void *obj, void *user);

/* the widget whose struct is obj (a CTUI_TABLE, CTUI_ENTRY, ...: what its
 * draw is given) published as ui/w/NAME/ (name: a node path of its own,
 * "files" or "dialog/name", its last part not a number); its kind and
 * place come from its draws. NULL ui: nothing (an app whose ui/ is off). 0,
 * or -1 (logged): a name taken or bad, obj named already, RDWR without
 * changed(). obj must outlive ui, or be let go of first
 * (ctui_ui_forget()). */
int ctui_ui_name(CTUI_UI *ui, void *obj, const char *name, unsigned flags,
                 CTUI_UI_CHANGED changed, void *user);
/* label as a node name, the way a form's fields are named: letters and
 * digits kept, lower case, the rest one '_' ("Repeat rate" ->
 * "repeat_rate"), at most 40 bytes; "" for none */
void ctui_ui_slug(const char *label, char *out, size_t cap);

/* name's nodes removed (a dialog's widgets as it's freed) */
void ctui_ui_forget(CTUI_UI *ui, const char *name);

#endif
