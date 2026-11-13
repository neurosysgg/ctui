/* An app drawn by a desktop's host: hostwin.h. Sockets, SCM_RIGHTS,
 * dup2: POSIX + Linux */
#define _GNU_SOURCE

#include "link/hostwin.h"

#include "link/link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

struct CTUI_HOSTWIN {
  CTUI_LINK *link;
  int saved_out; /* stdout while the window's open */
  int term;      /* a ctui-term pane's (ctui_hostwin_term()) */
  int aside;     /* stepped aside for a program on the pane's pty */
};

/* v up to its first newline into out (cap bytes), "" for NULL */
static void one_line(char *out, size_t cap, const char *v) {
  size_t len = v ? strcspn(v, "\n") : 0;
  len = len < cap - 1 ? len : cap - 1;
  memcpy(out, v ? v : "", len);
  out[len] = '\0';
}

int ctui_hostwin_msg(char *out, size_t cap, const char *name, const char *title,
                     const char *app_id, int w, int h) {
  char t[256], a[128], kv[2][300], size[2][32];
  one_line(t, sizeof t, title);
  one_line(a, sizeof a, app_id);
  snprintf(kv[0], sizeof kv[0], "title=%s", t);
  snprintf(kv[1], sizeof kv[1], "app-id=%s", a);
  snprintf(size[0], sizeof size[0], "columns=%dpx", w > 0 ? w : 1024);
  snprintf(size[1], sizeof size[1], "lines=%dpx", h > 0 ? h : 640);
  const char *set[4];
  int n = 0;
  if (t[0]) {
    set[n++] = kv[0];
  }
  if (a[0]) {
    set[n++] = kv[1];
  }
  set[n++] = size[0];
  set[n++] = size[1];
  size_t at = 0;
  int len = snprintf(out, cap, "window %s\n", name);
  if (len < 0 || (size_t)len >= cap) {
    return -1;
  }
  at = (size_t)len;
  for (int i = 0; i < n; i++) {
    len = snprintf(out + at, cap - at, "%s\n", set[i]);
    if (len < 0 || (size_t)len >= cap - at) {
      return -1;
    }
    at += (size_t)len;
  }
  return (int)at;
}

/* msg (n bytes) to sock with fd beside it (SCM_RIGHTS): 0, or -1 */
static int send_with_fd(int sock, const char *msg, size_t n, int fd) {
  union {
    char b[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
  } u;
  memset(&u, 0, sizeof u);
  struct iovec iov = {.iov_base = (void *)msg, .iov_len = n};
  struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
  mh.msg_control = u.b;
  mh.msg_controllen = sizeof u.b;
  struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &fd, sizeof fd);
  ssize_t sent;
  do {
    sent = sendmsg(sock, &mh, MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  return sent == (ssize_t)n ? 0 : -1;
}

/* the host's control socket, connected: the fd, or -1 (why in err) */
static int host_connect(char *err, size_t n) {
  const char *env = getenv(CTUI_HOSTWIN_HOST_ENV);
  const char *rt = getenv("XDG_RUNTIME_DIR");
  char path[sizeof((struct sockaddr_un *)0)->sun_path];
  int len;
  if (env && *env) {
    len = snprintf(path, sizeof path, "%s", env);
  } else if (rt && *rt) {
    len = snprintf(path, sizeof path, "%s/ctui-wm/ctui-host.sock", rt);
  } else { /* ctui-wm's runtime dir without one */
    len = snprintf(path, sizeof path, "/tmp/ctui-wm-%u/ctui-host.sock",
                   (unsigned)getuid());
  }
  if (len < 0 || (size_t)len >= sizeof path) {
    snprintf(err, n, "the host's socket path is too long");
    return -1;
  }
  struct sockaddr_un sa = {.sun_family = AF_UNIX};
  memcpy(sa.sun_path, path, (size_t)len + 1);
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    snprintf(err, n, "no ctui-host at %s: %s", path, strerror(errno));
    if (fd >= 0) {
      close(fd);
    }
    return -1;
  }
  return fd;
}

CTUI_HOSTWIN *ctui_hostwin_open(const char *title, const char *app_id, int w,
                                int h, int *rows, int *cols, char *err,
                                size_t n) {
  static int count;
  char name[64], msg[1024];
  snprintf(name, sizeof name, "app/%d.%d", (int)getpid(), ++count);
  int len = ctui_hostwin_msg(msg, sizeof msg, name, title, app_id, w, h);
  if (len < 0) {
    snprintf(err, n, "the window's title doesn't fit");
    return NULL;
  }
  int ctl = host_connect(err, n);
  if (ctl < 0) {
    return NULL;
  }
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) != 0) {
    snprintf(err, n, "socketpair: %s", strerror(errno));
    close(ctl);
    return NULL;
  }
  /* the message stays queued after the close: the host reads it, then
   * the end of this connection (an app's is one message long) */
  int sent = send_with_fd(ctl, msg, (size_t)len, sp[0]);
  close(ctl);
  close(sp[0]);
  if (sent != 0) {
    snprintf(err, n, "ctui-host didn't take the window: %s", strerror(errno));
    close(sp[1]);
    return NULL;
  }
  return ctui_hostwin_adopt(sp[1], rows, cols, err, n);
}

CTUI_HOSTWIN *ctui_hostwin_term(int *rows, int *cols, char *err, size_t n) {
  const char *path = getenv(CTUI_HOSTWIN_TERM_ENV);
  struct sockaddr_un sa = {.sun_family = AF_UNIX};
  if (!path || !*path || strlen(path) >= sizeof sa.sun_path) {
    snprintf(err, n, "no ctui-term link");
    return NULL;
  }
  /* only on the pane's own terminal (a pager, a pipe: the VT path) */
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    snprintf(err, n, "not on a terminal");
    return NULL;
  }
  memcpy(sa.sun_path, path, strlen(path) + 1);
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    snprintf(err, n, "no ctui-term at %s: %s", path, strerror(errno));
    if (fd >= 0) {
      close(fd);
    }
    return NULL;
  }
  CTUI_HOSTWIN *hw = ctui_hostwin_adopt(fd, rows, cols, err, n);
  if (hw) {
    hw->term = 1;
  }
  return hw;
}

int ctui_hostwin_is_term(const CTUI_HOSTWIN *w) { return w && w->term; }

int ctui_hostwin_osc(CTUI_HOSTWIN *w, const char *bytes, size_t n) {
  /* ESC ] CODE ; TEXT (ESC \ | BEL) */
  if (!w || !w->link || n < 4 || bytes[0] != 0x1b || bytes[1] != ']') {
    return -1;
  }
  size_t end = n;
  if (bytes[end - 1] == 0x07) {
    end--;
  } else if (end >= 2 && bytes[end - 2] == 0x1b && bytes[end - 1] == '\\') {
    end -= 2;
  } else {
    return -1;
  }
  size_t at = 2;
  int code = 0, digits = 0;
  while (at < end && bytes[at] >= '0' && bytes[at] <= '9' && digits < 6) {
    code = code * 10 + (bytes[at++] - '0');
    digits++;
  }
  if (!digits || at >= end || bytes[at] != ';') {
    return -1;
  }
  at++;
  return ctui_link_osc(w->link, code, bytes + at, end - at);
}

void ctui_hostwin_aside(CTUI_HOSTWIN *w, int on) {
  if (!w || !w->term || w->aside == on) {
    return;
  }
  w->aside = on;
  fflush(stdout);
  if (on && w->saved_out >= 0) {
    dup2(w->saved_out, STDOUT_FILENO);
  } else if (!on) {
    int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null >= 0) {
      dup2(null, STDOUT_FILENO);
      close(null);
    }
  }
  const char *msg = on ? "term=aside" : "term=back";
  ctui_link_app(w->link, msg, strlen(msg));
}

CTUI_HOSTWIN *ctui_hostwin_adopt(int fd, int *rows, int *cols, char *err,
                                 size_t n) {
  CTUI_HOSTWIN *hw = calloc(1, sizeof *hw);
  if (!hw) {
    snprintf(err, n, "out of memory");
    close(fd);
    return NULL;
  }
  /* what ctui writes for a terminal (mouse modes, a title) reaches no
   * shell, as vk/host.h's */
  fflush(stdout);
  hw->saved_out = dup(STDOUT_FILENO);
  int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (null >= 0) {
    dup2(null, STDOUT_FILENO);
    close(null);
  }
  hw->link = ctui_link_open(fd, CTUI_GFX_KITTY, rows, cols, err, n);
  if (!hw->link) {
    ctui_hostwin_close(hw);
    return NULL;
  }
  return hw;
}

void ctui_hostwin_attach(CTUI_HOSTWIN *w, CTUI_SCREEN *screen) {
  ctui_link_attach(w->link, screen);
}

CTUI_LINK *ctui_hostwin_link(CTUI_HOSTWIN *w) { return w ? w->link : NULL; }

void ctui_hostwin_title(CTUI_HOSTWIN *w, const char *title) {
  char t[256], msg[300];
  one_line(t, sizeof t, title);
  int len = snprintf(msg, sizeof msg, "title=%s", t);
  if (w && w->link && len > 0) {
    ctui_link_app(w->link, msg, (size_t)len);
  }
}

void ctui_hostwin_close(CTUI_HOSTWIN *w) {
  if (!w) {
    return;
  }
  if (w->link) {
    ctui_link_close(w->link);
  }
  if (w->saved_out >= 0) {
    fflush(stdout);
    dup2(w->saved_out, STDOUT_FILENO);
    close(w->saved_out);
  }
  free(w);
}
