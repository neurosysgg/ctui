#ifndef CTUI_LOOK_ICON_H
#define CTUI_LOOK_ICON_H

#include "../ctui.h"

/* Images by path (an app's icons, drawn.h's controls) shown as text cells
 * through kitty's Unicode placeholders (ctui_gfx_kitty_place_file() +
 * ctui_widget_put_kitty_placeholder()). Each path is placed once per
 * process under its own image id; after that an icon is just cells a
 * widget draws in its render(), so it moves and clears like text. Off
 * (every call a no-op answering 0) until ctui_icon_enable(): an app turns
 * it on in kitty only. */

/* an icon covers this many cells of one row (kitty scales the PNG to
 * them; two columns of a usual cell are about square) */
#define CTUI_ICON_COLS 2

/* image ids this process uses for icons (1..this): past it the least
 * recently used one is replaced. Far more than one screen shows. */
#define CTUI_ICON_MAX 256

/* whether icons are shown in this process; 1 by the app when the
 * terminal speaks kitty's graphics protocol */
void ctui_icon_enable(int on);
int ctui_icon_enabled(void);

/* the image id for the PNG at path, placing it the first time (or again
 * once its id went to another path); 0 if icons are off or path is empty.
 * Widgets ask for it every render, which is what keeps it in use. */
unsigned int ctui_icon_id(const char *path);
/* the same for an image placed over cols x rows cells (a drawn control,
 * widgets/drawn.h): a path always comes with the same size */
unsigned int ctui_icon_id_box(const char *path, int cols, int rows);
/* the same; *placed 1 when it was placed just now (what goes with the
 * image -- an animation's frames -- is to be sent again), else 0 */
unsigned int ctui_icon_id_placed(const char *path, int cols, int rows,
                                 int *placed);

/* the same, the file at path placed again under its id even when it was
 * placed before (its pixels changed: drawn.h's live images) */
unsigned int ctui_icon_id_again(const char *path, int cols, int rows);

/* draws the icon (CTUI_ICON_COLS cells) at (row, col) over bg; nothing
 * for id 0 */
void ctui_icon_put(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp, int row, int col,
                   unsigned int id, unsigned char bg);

/* a hook that turns a path into the file to place for cols x rows cells
 * (ctui-wm's: a small theme icon scaled up by whole factors in a look);
 * it writes the file's path into out and answers 1, or 0 to place path
 * as it is. NULL: none. */
typedef int (*CTUI_ICON_PREPARE)(const char *path, int cols, int rows,
                                 char *out, size_t cap);
void ctui_icon_set_prepare(CTUI_ICON_PREPARE fn);

/* forgets every placed path (tests) */
void ctui_icon_reset(void);

#endif
