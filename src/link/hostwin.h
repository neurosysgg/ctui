#ifndef CTUI_HOSTWIN_H
#define CTUI_HOSTWIN_H

/* An app drawn by a desktop's host over a link (link.h) instead of by a
 * terminal: a window of its own on the host, or the ctui-term pane it
 * runs in. ctui-wm's ctui-host and ctui-term are the hosts (its
 * notes/kittyless.md z8, notes/terminal.md t4); an app on any other
 * desktop finds neither and takes its terminal (ctui_init()) as before.
 *
 * Either way the app opens no Vulkan device of its own: the link is its
 * input source, screen sink and graphics sink (ctui_app_run() as on a
 * terminal, the host closing ending the loop), kitty's image calls go to
 * the host's GPU at exactly the kitty tier, and stdout points at
 * /dev/null while it's open (what ctui writes for a terminal reaches no
 * shell).
 *
 * The window: one message to the host's control socket (SOCK_SEQPACKET,
 * $CTUI_WM_HOST_SOCKET, else ctui-wm/ctui-host.sock in $XDG_RUNTIME_DIR),
 * text -- "window app/PID.N", then "title=", "app-id=", "columns=Wpx",
 * "lines=Hpx" lines -- with the link's other end passed beside it
 * (SCM_RIGHTS). The host never answers; one that can't make the window
 * closes the link. ctui-wm's base/hostmsg.h parses it (its tests hold the
 * two together). The pane: $CTUI_TERM_LINK, a socket the pane's
 * terminal listens on, taken only by its foreground program.
 *
 * Linux only (SCM_RIGHTS, memfd through the link). */

#include "ctui.h"

#include <stddef.h>

typedef struct CTUI_HOSTWIN CTUI_HOSTWIN;

/* the environment naming the host's control socket, and a ctui-term
 * pane's link (set by the pane for its programs) */
#define CTUI_HOSTWIN_HOST_ENV "CTUI_WM_HOST_SOCKET"
#define CTUI_HOSTWIN_TERM_ENV "CTUI_TERM_LINK"

/* a window w x h logical px (<= 0: 1024 x 640), titled title (app_id
 * xdg's: the compositor's rules match it), its grid into *rows / *cols
 * for ctui_screen_create(): no ctui_init(), the gfx tier CTUI_GFX_KITTY.
 * NULL when no host answers (why in err) */
CTUI_HOSTWIN *ctui_hostwin_open(const char *title, const char *app_id, int w,
                                int h, int *rows, int *cols, char *err,
                                size_t n);

/* the same over a link's end someone else asked the host for (a zone's
 * surface its supervisor made). NULL when the link doesn't start (why in
 * err; fd closed) */
CTUI_HOSTWIN *ctui_hostwin_adopt(int fd, int *rows, int *cols, char *err,
                                 size_t n);

/* the same over the link of the ctui-term pane the app runs in
 * ($CTUI_TERM_LINK): its cells drawn by the terminal as they are, input
 * as events, the pty only for stdio. NULL when there's none, stdin /
 * stdout aren't a terminal, or the terminal refused (it isn't the pane's
 * foreground program): the caller takes the terminal (ctui_init()) */
CTUI_HOSTWIN *ctui_hostwin_term(int *rows, int *cols, char *err, size_t n);
int ctui_hostwin_is_term(const CTUI_HOSTWIN *w);

/* an OSC escape (ESC ] CODE ; ... ST or BEL, whole) the app would write
 * to its terminal, over the link instead: 0, or -1 (not one, a host that
 * doesn't take them) */
int ctui_hostwin_osc(CTUI_HOSTWIN *w, const char *bytes, size_t n);
/* a pane's app stepping aside for a program on its pty (an editor) and
 * back: stdout the pty meanwhile, the pane showing its terminal */
void ctui_hostwin_aside(CTUI_HOSTWIN *w, int on);
/* screen's frames to the host, its input to ctui_input_loop(), before
 * ctui_app_run() */
void ctui_hostwin_attach(CTUI_HOSTWIN *w, CTUI_SCREEN *screen);

/* its link's app end (link.h): drag and drop, default colours */
struct CTUI_LINK;
struct CTUI_LINK *ctui_hostwin_link(CTUI_HOSTWIN *w);
/* the window's title now (a newline ends it) */
void ctui_hostwin_title(CTUI_HOSTWIN *w, const char *title);

/* after ctui_app_run() and before the screen's freed (its sink given
 * back): the link closed (the host drops the window), stdout as it was.
 * NULL: nothing */
void ctui_hostwin_close(CTUI_HOSTWIN *w);

/* the control message asking for a window named name ("app/..."; the
 * title up to its first newline, NULL: not said) into out (cap bytes,
 * NUL-terminated): its length, or -1 when it doesn't fit */
int ctui_hostwin_msg(char *out, size_t cap, const char *name, const char *title,
                     const char *app_id, int w, int h);

#endif
