#define _GNU_SOURCE /* memfd_create */
#include "wire.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

int ctui_link_send(int sock, uint32_t type, const void *fixed, size_t n,
                   const void *tail, size_t tn, int fd) {
  CTUI_LINK_HDR hdr = {.type = type, .len = (uint32_t)(n + tn)};
  struct iovec iov[3] = {
      {.iov_base = &hdr, .iov_len = sizeof hdr},
      {.iov_base = (void *)fixed, .iov_len = n},
      {.iov_base = (void *)tail, .iov_len = tn},
  };
  union {
    struct cmsghdr c;
    char b[CMSG_SPACE(sizeof(int))];
  } ctl;
  struct msghdr m = {.msg_iov = iov, .msg_iovlen = 3};
  if (fd >= 0) {
    memset(&ctl, 0, sizeof ctl);
    m.msg_control = ctl.b;
    m.msg_controllen = sizeof ctl.b;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
  }
  for (;;) {
    ssize_t r = sendmsg(sock, &m, MSG_NOSIGNAL);
    if (r >= 0) {
      return 0;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
}

long ctui_link_recv(int sock, void *buf, int *fd) {
  *fd = -1;
  union {
    struct cmsghdr c;
    char b[CMSG_SPACE(sizeof(int))];
  } ctl;
  struct iovec iov = {.iov_base = buf, .iov_len = CTUI_LINK_MAX};
  struct msghdr m = {.msg_iov = &iov,
                     .msg_iovlen = 1,
                     .msg_control = ctl.b,
                     .msg_controllen = sizeof ctl.b};
  ssize_t r;
  do {
    r = recvmsg(sock, &m, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
  } while (r < 0 && errno == EINTR);
  if (r < 0) {
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
  }
  if (r == 0) {
    return -1; /* the peer closed (a seqpacket's empty read) */
  }
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
        c->cmsg_len >= CMSG_LEN(sizeof(int))) {
      memcpy(fd, CMSG_DATA(c), sizeof *fd);
    }
  }
  if ((m.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
      (size_t)r < sizeof(CTUI_LINK_HDR)) {
    if (*fd >= 0) {
      close(*fd);
      *fd = -1;
    }
    return -1;
  }
  return (long)r;
}

int ctui_link_memfd(const void *p, size_t n) {
  int fd = memfd_create("ctui-link", MFD_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  const char *b = p;
  while (n) {
    ssize_t w = write(fd, b, n);
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w <= 0) {
      close(fd);
      return -1;
    }
    b += w;
    n -= (size_t)w;
  }
  return fd;
}

const char *ctui_link_dnd_parse(const void *body, size_t len,
                                CTUI_LINK_DND *m) {
  if (len < sizeof *m + 1 || len - sizeof *m > CTUI_LINK_DND_MIMES) {
    return NULL;
  }
  memcpy(m, body, sizeof *m);
  const char *mimes = (const char *)body + sizeof *m;
  /* one string filling the rest exactly */
  if (mimes[len - sizeof *m - 1] != '\0' ||
      strlen(mimes) != len - sizeof *m - 1) {
    return NULL;
  }
  return mimes;
}

const char *ctui_link_osc_parse(const void *body, size_t len, int *code,
                                size_t *n) {
  CTUI_LINK_M_OSC m;
  if (len < sizeof m + 1 || len - sizeof m - 1 > CTUI_OSC_MAX) {
    return NULL;
  }
  memcpy(&m, body, sizeof m);
  const char *text = (const char *)body + sizeof m;
  if (text[len - sizeof m - 1] != '\0') {
    return NULL;
  }
  *code = m.code;
  *n = len - sizeof m - 1;
  return text;
}

int ctui_link_osc_send(int sock, uint32_t type, int code, const char *text,
                       size_t n) {
  if (n > CTUI_OSC_MAX) {
    return -1;
  }
  char *tail = malloc(n + 1);
  if (!tail) {
    return -1;
  }
  memcpy(tail, text, n);
  tail[n] = '\0';
  CTUI_LINK_M_OSC m = {.code = code};
  int r = ctui_link_send(sock, type, &m, sizeof m, tail, n + 1, -1);
  free(tail);
  return r;
}
