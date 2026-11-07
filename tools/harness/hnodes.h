#ifndef CTUI_HNODES_H
#define CTUI_HNODES_H

/* The control nodes' daemon end (src/node/node.h's wire), small, for the
 * harness: a socket in a scratch directory of its own that apps under
 * test connect to ($CTUI_NODES_SOCK), the values they publish kept by
 * path, writes sent and their answers collected. No FUSE and no mount:
 * what ctui-nodefs serves as files, this keeps in a table. Every
 * connected app's nodes are looked up together, the newest app's first:
 * one an app runs in its own terminal (a file chooser) is in front of it,
 * and once it's gone the one behind answers again. Nothing blocks: pump
 * when its fds are readable. */

#include <poll.h>

typedef struct CTUI_HNODES CTUI_HNODES;

/* listening; NULL (logged to stderr) when no socket could be made */
CTUI_HNODES *ctui_hnodes_listen(void);
/* closes every connection, removes the socket and its directory */
void ctui_hnodes_free(CTUI_HNODES *h);
/* the socket's path, for the app's $CTUI_NODES_SOCK */
const char *ctui_hnodes_path(const CTUI_HNODES *h);

/* its fds for poll(2), POLLIN each, at most max: how many */
int ctui_hnodes_fds(const CTUI_HNODES *h, struct pollfd *fds, int max);
/* accepts and reads whatever is ready now: 1 if anything came */
int ctui_hnodes_pump(CTUI_HNODES *h);

/* path's value as a read gives it (no newline), NULL while no app has it;
 * a trigger's is "" */
const char *ctui_hnodes_get(const CTUI_HNODES *h, const char *path);
/* how many nodes are published, over every app */
int ctui_hnodes_count(const CTUI_HNODES *h);

/* text written to path: the request's number (> 0), -1 when no app has
 * it. The answer comes with a later pump. */
int ctui_hnodes_write(CTUI_HNODES *h, const char *path, const char *text);
/* 1 once req is answered: *err 0 or the writer's errno, *reply (NULL
 * none) a trigger's reply, valid until the next write; 0 while it waits;
 * -1 when its app went (no answer will come) */
int ctui_hnodes_answer(CTUI_HNODES *h, int req, int *err, const char **reply);

#endif
