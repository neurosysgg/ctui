#include "io.h"

#include "log.h"

#include <stdlib.h>

struct CTUI_IO_WATCH {
  int fd;
  unsigned int events;
  CTUI_WIDGET *widget;
  int (*handler)(CTUI_WIDGET *self, CTUI_EVENT *ev); /* NULL = unwatched,
                                                     * awaiting sweep */
};

/* flat realloc-grown array of pointers (pointers, not structs, so the
 * CTUI_IO_WATCH * handed back to callers stays valid across growth) --
 * same shape as core/timer.c's registries */
static CTUI_IO_WATCH **g_watches;
static int g_watch_count, g_watch_cap;
static int g_dispatching = 0;
static int g_dead = 0;

static void sweep(void) {
  int kept = 0;
  for (int i = 0; i < g_watch_count; i++) {
    if (g_watches[i]->handler) {
      g_watches[kept++] = g_watches[i];
    } else {
      free(g_watches[i]);
    }
  }
  g_watch_count = kept;
  g_dead = 0;
}

CTUI_IO_WATCH *ctui_io_watch(int fd, unsigned int events, CTUI_WIDGET *widget,
                             int (*handler)(CTUI_WIDGET *self,
                                            CTUI_EVENT *ev)) {
  if (fd < 0 || fd >= FD_SETSIZE || events == 0 || handler == NULL) {
    ctui_logf(E_WRN,
              "[CTUI:IO] - watch rejected @ tick %d (fd=%d, events=0x%x, "
              "handler=%s)\n",
              ctui_tick_advance(), fd, events, handler ? "set" : "NULL");
    return NULL;
  }

  if (g_watch_count == g_watch_cap) {
    int new_cap = g_watch_cap == 0 ? 4 : g_watch_cap * 2;
    g_watches = realloc(g_watches, (size_t)new_cap * sizeof(*g_watches));
    g_watch_cap = new_cap;
  }
  CTUI_IO_WATCH *watch = malloc(sizeof(*watch));
  *watch = (CTUI_IO_WATCH){
      .fd = fd, .events = events, .widget = widget, .handler = handler};
  g_watches[g_watch_count++] = watch;

  ctui_logf(E_INF,
            "[CTUI:IO] - watching fd %d @ tick %d (events=0x%x, widget=%p)\n",
            fd, ctui_tick_advance(), events, (void *)widget);
  return watch;
}

void ctui_io_set_events(CTUI_IO_WATCH *watch, unsigned int events) {
  ctui_logf(E_DBG, "[CTUI:IO] - fd %d events 0x%x -> 0x%x @ tick %d\n",
            watch->fd, watch->events, events, ctui_tick_advance());
  watch->events = events;
}

void ctui_io_unwatch(CTUI_IO_WATCH *watch) {
  ctui_logf(E_INF, "[CTUI:IO] - unwatching fd %d @ tick %d\n", watch->fd,
            ctui_tick_advance());
  watch->handler = NULL;
  g_dead++;
  if (!g_dispatching) {
    sweep();
  }
}

void ctui_io_fill(fd_set *rfds, fd_set *wfds, int *maxfd) {
  for (int i = 0; i < g_watch_count; i++) {
    CTUI_IO_WATCH *w = g_watches[i];
    if (!w->handler) {
      continue;
    }
    if (w->events & CTUI_IO_READ) {
      FD_SET(w->fd, rfds);
    }
    if (w->events & CTUI_IO_WRITE) {
      FD_SET(w->fd, wfds);
    }
    if ((w->events & (CTUI_IO_READ | CTUI_IO_WRITE)) && w->fd > *maxfd) {
      *maxfd = w->fd;
    }
  }
}

int ctui_io_ready(fd_set *rfds, fd_set *wfds, CTUI_IO_EVENT_DATA *data) {
  for (int i = 0; i < g_watch_count; i++) {
    CTUI_IO_WATCH *w = g_watches[i];
    if (!w->handler) {
      continue;
    }
    unsigned int ready = 0;
    if ((w->events & CTUI_IO_READ) && FD_ISSET(w->fd, rfds)) {
      ready |= CTUI_IO_READ;
    }
    if ((w->events & CTUI_IO_WRITE) && FD_ISSET(w->fd, wfds)) {
      ready |= CTUI_IO_WRITE;
    }
    if (ready) {
      data->fd = w->fd;
      data->ready = ready;
      return 1;
    }
  }
  return 0;
}

int ctui_io_dispatch(CTUI_EVENT *ev) {
  CTUI_IO_EVENT_DATA *data = ev->event_data;
  int changed = 0;
  g_dispatching = 1;
  /* re-read g_watch_count each pass: a handler may add a watch, and the
   * newcomer simply isn't ready this round (its fd wasn't in the set) */
  for (int i = 0; i < g_watch_count; i++) {
    CTUI_IO_WATCH *w = g_watches[i];
    if (!w->handler || w->fd != data->fd || !(w->events & data->ready)) {
      continue;
    }
    ctui_logf(E_DBG, "[CTUI:IO] - fd %d ready (0x%x) @ tick %d\n", w->fd,
              data->ready, ctui_tick_advance());
    if (w->handler(w->widget, ev)) {
      changed = 1;
    }
  }
  g_dispatching = 0;
  if (g_dead) {
    sweep();
  }
  return changed;
}

void ctui_io_reset(void) {
  for (int i = 0; i < g_watch_count; i++) {
    free(g_watches[i]);
  }
  free(g_watches);
  g_watches = NULL;
  g_watch_count = 0;
  g_watch_cap = 0;
  g_dead = 0;
  ctui_logf(E_INF, "[CTUI:IO] - registry reset @ tick %d\n",
            ctui_tick_advance());
}
