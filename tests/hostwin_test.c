/* link/hostwin.c: the window's message, an app's end against a forked
 * fake host over a real link -- its grid back, its title out --, and a
 * ctui-term pane's link (the host's side of the message is ctui-wm's
 * base/hostmsg.c; its hostwin_test parses this one's) */
#define _GNU_SOURCE

#include "ctui.h"
#include "ctui_test.h"
#include "link/hostwin.h"
#include "link/link.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_message(void) {
  char msg[1024];
  int n = ctui_hostwin_msg(msg, sizeof msg, "app/42.1", "System\nMonitor",
                           "ctui-top", 1280, 800);
  CTUI_TEST_ASSERT(n > 0 && (size_t)n == strlen(msg) &&
                       strcmp(msg, "window app/42.1\ntitle=System\n"
                                   "app-id=ctui-top\ncolumns=1280px\n"
                                   "lines=800px\n") == 0,
                   "the message: the title up to its newline, the app id, "
                   "its size in px (%s)",
                   msg);
  n = ctui_hostwin_msg(msg, sizeof msg, "app/1.1", NULL, NULL, 0, 0);
  CTUI_TEST_ASSERT(n > 0 && strcmp(msg, "window app/1.1\ncolumns=1024px\n"
                                        "lines=640px\n") == 0,
                   "no title, no app id, no size: none sent, 1024 x 640 (%s)",
                   msg);
  CTUI_TEST_ASSERT(ctui_hostwin_msg(msg, 20, "app/1.1", "a title", "x", 1, 1) ==
                       -1,
                   "too small a buffer: -1");
}

/* one message and the fd beside it (the fake host's recvmsg) */
static long recv_fd(int sock, char *buf, size_t cap, int *fd) {
  union {
    char b[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
  } u;
  struct iovec iov = {.iov_base = buf, .iov_len = cap};
  struct msghdr mh = {.msg_iov = &iov,
                      .msg_iovlen = 1,
                      .msg_control = u.b,
                      .msg_controllen = sizeof u.b};
  ssize_t n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
  struct cmsghdr *cm = n > 0 ? CMSG_FIRSTHDR(&mh) : NULL;
  if (cm && cm->cmsg_type == SCM_RIGHTS) {
    memcpy(fd, CMSG_DATA(cm), sizeof *fd);
  }
  return (long)n;
}

/* --- the app's end against a fake host --- */

static void on_app(void *ctx, const void *msg, size_t n) {
  FILE *f = ctx;
  fprintf(f, "app %.*s\n", (int)n, (const char *)msg);
  fflush(f);
}

static int no_frame(void *ctx, const CTUI_SCREEN *s) {
  (void)ctx;
  (void)s;
  return 0;
}

static const CTUI_SCREEN_SINK NO_SINK = {no_frame};

/* the fake host: one window asked for, its message and title written to
 * log, a 30 x 100 grid of 10 x 20 cells, until the app's gone */
static void fake_host(int ls, const char *log) {
  FILE *f = fopen(log, "w");
  int c = accept(ls, NULL, NULL);
  char buf[4097];
  int fd = -1;
  long n = c >= 0 ? recv_fd(c, buf, sizeof buf - 1, &fd) : -1;
  if (!f || n <= 0 || fd < 0) {
    _exit(1);
  }
  for (long i = 0; i < n; i++) {
    buf[i] = buf[i] == '\n' ? '|' : buf[i];
  }
  fprintf(f, "msg %.*s\n", (int)n, buf);
  fflush(f);
  CTUI_LINK_HOST_SINKS sinks = {.screen = &NO_SINK, .app = on_app, .ctx = f};
  CTUI_LINK_HOST *h = ctui_link_host_new(fd, &sinks);
  if (!h || ctui_link_host_grid(h, 30, 100, 10, 20) != 0) {
    _exit(1);
  }
  for (;;) {
    struct pollfd p = {.fd = ctui_link_host_fd(h), .events = POLLIN};
    if (poll(&p, 1, 5000) <= 0 || ctui_link_host_read(h) < 0) {
      break;
    }
  }
  fprintf(f, "gone\n");
  fclose(f);
  ctui_link_host_free(h);
  _exit(0);
}

static void test_window(void) {
  char dir[] = "/tmp/hostwin_test.XXXXXX";
  if (!mkdtemp(dir)) {
    CTUI_TEST_ASSERT(0, "a scratch dir");
    return;
  }
  char path[108], log[128];
  snprintf(path, sizeof path, "%s/h.sock", dir);
  snprintf(log, sizeof log, "%s/log", dir);

  /* no host: refused at once, saying where it looked */
  setenv(CTUI_HOSTWIN_HOST_ENV, path, 1);
  int rows = 0, cols = 0;
  char err[256] = "";
  CTUI_HOSTWIN *w =
      ctui_hostwin_open("T", "t", 0, 0, &rows, &cols, err, sizeof err);
  CTUI_TEST_ASSERT(!w && strstr(err, path) != NULL,
                   "no host listening: NULL, why (%s)", err);

  struct sockaddr_un sa = {.sun_family = AF_UNIX};
  memcpy(sa.sun_path, path, strlen(path) + 1);
  int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 ||
      listen(ls, 4) != 0) {
    CTUI_TEST_ASSERT(0, "the fake host's socket");
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    fake_host(ls, log);
  }
  close(ls);
  w = ctui_hostwin_open("Monitor", "ctui-top", 640, 480, &rows, &cols, err,
                        sizeof err);
  CTUI_TEST_ASSERT(w && rows == 30 && cols == 100,
                   "opened: the host's grid, 30 x 100 (%d x %d; %s)", rows,
                   cols, err);
  int cw = 0, ch = 0;
  ctui_cell_px(&cw, &ch);
  CTUI_TEST_ASSERT(cw == 10 && ch == 20, "the host's cell, 10 x 20 (%d x %d)",
                   cw, ch);
  ctui_hostwin_title(w, "Monitor - busy\nignored");
  ctui_hostwin_close(w);
  int st = -1;
  waitpid(pid, &st, 0);
  char got[1024] = "";
  FILE *f = fopen(log, "r");
  size_t n = f ? fread(got, 1, sizeof got - 1, f) : 0;
  got[n] = '\0';
  if (f) {
    fclose(f);
  }
  char want[256];
  snprintf(want, sizeof want,
           "msg window app/%d.2|title=Monitor|app-id=ctui-top|"
           "columns=640px|lines=480px|\n",
           (int)getpid());
  CTUI_TEST_ASSERT(strncmp(got, want, strlen(want)) == 0,
                   "the host got the window's message, named by pid (%s)", got);
  CTUI_TEST_ASSERT(strstr(got, "app title=Monitor - busy\n") != NULL,
                   "the title over the link, up to its newline");
  CTUI_TEST_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
                       strstr(got, "gone\n") != NULL,
                   "closed: the host saw the app go");
  unlink(log);
  unlink(path);
  rmdir(dir);
  unsetenv(CTUI_HOSTWIN_HOST_ENV);
}

/* a ctui-term pane's link (t4): refused off a terminal; an app's OSCs over
 * a link, after the host said it takes them */
static char osc_got[64];
static int osc_code;
static void t_osc(void *ctx, int code, const char *text, size_t n) {
  (void)ctx;
  osc_code = code;
  snprintf(osc_got, sizeof osc_got, "%.*s", (int)n, text);
}

static void test_term(void) {
  setenv(CTUI_HOSTWIN_TERM_ENV, "/nonexistent/term.link", 1);
  int rows = 0, cols = 0;
  char err[256] = "";
  CTUI_HOSTWIN *w = ctui_hostwin_term(&rows, &cols, err, sizeof err);
  CTUI_TEST_ASSERT(!w && err[0], "no pane's link to be had: NULL, why (%s)",
                   err);
  unsetenv(CTUI_HOSTWIN_TERM_ENV);
  w = ctui_hostwin_term(&rows, &cols, err, sizeof err);
  CTUI_TEST_ASSERT(!w && strstr(err, "no ctui-term link"),
                   "without $CTUI_TERM_LINK: NULL (%s)", err);

  int sp[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) != 0) {
    CTUI_TEST_ASSERT(0, "a socketpair");
    return;
  }
  CTUI_LINK_HOST_SINKS k = {.osc = t_osc};
  CTUI_LINK_HOST *h = ctui_link_host_new(sp[1], &k);
  CTUI_LINK_DND can = {.kind = CTUI_LINK_DND_CAN};
  ctui_link_host_dnd(h, &can, NULL, -1);
  ctui_link_host_grid(h, 10, 40, 10, 20);
  w = ctui_hostwin_adopt(sp[0], &rows, &cols, err, sizeof err);
  /* stdout is /dev/null while it's open: the asserts after the close */
  int adopted = w && rows == 10 && cols == 40;
  int o72 = ctui_hostwin_osc(w, "\x1b]72;t=a;text/uri-list\x1b\\", 24) == 0 &&
            ctui_link_host_read(h) == 0 && osc_code == 72 &&
            !strcmp(osc_got, "t=a;text/uri-list");
  char got72[80];
  snprintf(got72, sizeof got72, "%d %s", osc_code, osc_got);
  int o52 = ctui_hostwin_osc(w, "\x1b]52;c;aGk=\x07", 12) == 0 &&
            ctui_link_host_read(h) == 0 && osc_code == 52 &&
            !strcmp(osc_got, "c;aGk=");
  int bad = ctui_hostwin_osc(w, "\x1b]72;t=a", 9) == -1 &&
            ctui_hostwin_osc(w, "\x1b[1m", 4) == -1 &&
            ctui_hostwin_osc(w, "\x1b];x\x07", 5) == -1;
  int term = ctui_hostwin_is_term(w);
  ctui_hostwin_close(w);
  CTUI_TEST_ASSERT(adopted, "a link adopted (%s)", err);
  CTUI_TEST_ASSERT(o72, "an OSC 72 written whole goes over the link (%s)",
                   got72);
  CTUI_TEST_ASSERT(o52, "BEL ends one too (%s)", osc_got);
  CTUI_TEST_ASSERT(bad, "unterminated, not an OSC, no number: refused");
  CTUI_TEST_ASSERT(!term, "an adopted link isn't a pane's: no stepping aside");
  ctui_link_host_free(h);
}

int main(void) {
  ctui_log_init(E_WRN | E_ERR);
  signal(SIGPIPE, SIG_IGN);
  test_message();
  test_window();
  test_term();
  ctui_log_shutdown();
  return ctui_test_summary();
}
