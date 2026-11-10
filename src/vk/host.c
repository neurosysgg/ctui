#define _POSIX_C_SOURCE 200809L
#include "host.h"

#include "present.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

struct CTUI_HOST {
  CTUI_WL *wl;
  CTUI_VK_GPU *gpu;
  CTUI_VK_WIN *win;
  CTUI_SCREEN *screen;
  CTUI_INPUT_SOURCE src;
  /* the source's fd: the connection, the key repeat, and the kick (an
   * eventfd written when events were queued with nobody waiting to see
   * them: Vulkan's present may read the connection itself) */
  int ep, kick;
  /* armed while something moves (ctui_vk_at()): when its next column is
   * due, so that frame is presented then and none in between */
  int anim;
  int resized;
  CTUI_RESIZE_EVENT_DATA resize;
  int saved_stdout;
};

/* the loop's about to wait: the frame it drew goes out, the anim timer
 * set for the next column while something moves */
static void on_wait(void *ctx, long timeout_ms) {
  (void)timeout_ms;
  CTUI_HOST *h = ctx;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
  uint64_t next = 0;
  ctui_vk_win_present(h->win, now, &next);
  struct itimerspec it = {0};
  it.it_value.tv_sec = (time_t)(next / 1000000000u);
  it.it_value.tv_nsec = (long)(next % 1000000000u);
  if (timerfd_settime(h->anim, 0, &it, NULL) != 0) {
    ctui_logf(E_WRN, "[CTUI:HOST] - the animation timer couldn't be set\n");
  }
  ctui_wl_flush(h->wl);
  int pw, ph;
  /* a configure, or input, the present's own reading of the connection
   * left queued: wake the wait at once */
  if (ctui_wl_size(h->wl, &pw, &ph) || h->resized) {
    h->resized = 1;
  }
  if (h->resized || ctui_wl_closed(h->wl) || ctui_wl_queued(h->wl)) {
    uint64_t one = 1;
    if (write(h->kick, &one, sizeof one) < 0) {
      ctui_logf(E_WRN, "[CTUI:HOST] - the kick couldn't be written\n");
    }
  }
}

static int src_next(void *ctx, CTUI_EVENT *ev, int readable) {
  CTUI_HOST *h = ctx;
  if (readable) {
    struct epoll_event e[4];
    int n = epoll_wait(h->ep, e, 4, 0);
    for (int i = 0; i < n; i++) {
      if (e[i].data.fd == ctui_wl_fd(h->wl)) {
        if (ctui_wl_dispatch(h->wl) < 0) {
          ctui_logf(E_ERR, "[CTUI:HOST] - the compositor's gone\n");
          return -1;
        }
      } else if (e[i].data.fd == ctui_wl_repeat_fd(h->wl)) {
        ctui_wl_repeat(h->wl);
      } else {
        /* the anim timer or the kick: the next on_wait() presents what's
         * due */
        uint64_t k;
        if (read(e[i].data.fd, &k, sizeof k) < 0) {
          k = 0;
        }
      }
    }
  }
  if (ctui_wl_closed(h->wl)) {
    ctui_logf(E_INF, "[CTUI:HOST] - window closed @ tick %d\n",
              ctui_tick_advance());
    return -1;
  }
  int pw, ph;
  if (ctui_wl_size(h->wl, &pw, &ph) || h->resized) {
    h->resized = 0;
    if (ctui_vk_win_resize(h->win, &h->resize.rows, &h->resize.cols) == 0) {
      ev->type = CTUI_RESIZE_EVENT;
      ev->scope = CTUI_EVENT_SCOPE_GLOBAL;
      ev->ev_source = "terminal";
      ev->event_data = &h->resize;
      ev->origin = NULL;
      return 1;
    }
  }
  return ctui_wl_next(h->wl, ev);
}

void ctui_host_close(CTUI_HOST *h) {
  if (!h) {
    return;
  }
  ctui_input_set_source(NULL);
  ctui_input_on_wait(NULL, NULL);
  ctui_gfx_set_sink(NULL, NULL);
  unsigned warnings = h->gpu ? ctui_vk_gpu_warnings(h->gpu) : 0;
  ctui_vk_win_free(h->win);
  ctui_vk_gpu_close(h->gpu);
  ctui_wl_close(h->wl);
  if (h->ep >= 0) {
    close(h->ep);
  }
  if (h->kick >= 0) {
    close(h->kick);
  }
  if (h->anim >= 0) {
    close(h->anim);
  }
  if (h->saved_stdout >= 0) {
    fflush(stdout);
    dup2(h->saved_stdout, STDOUT_FILENO);
    close(h->saved_stdout);
  }
  ctui_logf(E_INF, "[CTUI:HOST] - closed (%u validation warnings) @ tick %d\n",
            warnings, ctui_tick_advance());
  free(h);
}

CTUI_HOST *ctui_host_open(const char *title, const char *app_id, int w, int h,
                          int *rows, int *cols, char *err, size_t n) {
  CTUI_HOST *host = calloc(1, sizeof *host);
  if (!host) {
    snprintf(err, n, "out of memory");
    return NULL;
  }
  host->ep = host->kick = host->anim = host->saved_stdout = -1;
  host->wl = ctui_wl_open(title, app_id, w, h, err, (unsigned)n);
  if (!host->wl) {
    free(host);
    return NULL;
  }
  host->gpu = ctui_vk_gpu_open(ctui_wl_dpy(host->wl), err, (unsigned)n);
  if (host->gpu) {
    host->win = ctui_vk_win_new(host->gpu, host->wl, &host->resize.rows,
                                &host->resize.cols, err, (unsigned)n);
  }
  if (!host->win) {
    ctui_host_close(host);
    return NULL;
  }
  host->ep = epoll_create1(EPOLL_CLOEXEC);
  host->kick = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  host->anim = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  int fds[4] = {ctui_wl_fd(host->wl), ctui_wl_repeat_fd(host->wl), host->kick,
                host->anim};
  for (int i = 0; i < 4; i++) {
    struct epoll_event e = {.events = EPOLLIN, .data.fd = fds[i]};
    if (host->ep < 0 || fds[i] < 0 ||
        epoll_ctl(host->ep, EPOLL_CTL_ADD, fds[i], &e) != 0) {
      snprintf(err, n, "no epoll for the window's fds");
      ctui_host_close(host);
      return NULL;
    }
  }
  *rows = host->resize.rows;
  *cols = host->resize.cols;
  /* before any widget makes a CTUI_GFX_KITTY_IMAGE: it keeps the path it
   * was made on */
  ctui_gfx_set_sink(ctui_vk_gfx_sink(), ctui_vk_win_vk(host->win));
  host->src =
      (CTUI_INPUT_SOURCE){.fd = host->ep, .next = src_next, .ctx = host};
  /* no terminal: what's written for one goes nowhere */
  fflush(stdout);
  host->saved_stdout = dup(STDOUT_FILENO);
  int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (null >= 0) {
    dup2(null, STDOUT_FILENO);
    close(null);
  }
  return host;
}

void ctui_host_attach(CTUI_HOST *host, CTUI_SCREEN *screen) {
  host->screen = screen;
  ctui_screen_set_sink(screen, ctui_vk_screen_sink(),
                       ctui_vk_win_vk(host->win));
  ctui_input_set_source(&host->src);
  ctui_input_on_wait(on_wait, host);
}

void ctui_host_set_default(CTUI_HOST *host, uint32_t fg, uint32_t bg) {
  ctui_vk_set_default(ctui_vk_win_vk(host->win), fg, bg);
}

void ctui_host_title(CTUI_HOST *host, const char *title) {
  ctui_wl_title(host->wl, title);
}
