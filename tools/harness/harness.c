#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct CTUI_PTY {
  pid_t pid;
  int fd;
  CTUI_VT *vt;
  char *out;
  size_t nout, out_cap;
  int gone;
  /* reaped already: how it ended */
  int reaped, status;
};

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void set_size(int fd, int rows, int cols) {
  struct winsize ws = {.ws_row = (unsigned short)rows,
                       .ws_col = (unsigned short)cols};
  ioctl(fd, TIOCSWINSZ, &ws);
}

static void child(const CTUI_PTY_SPAWN *how, const char *slave, int rows,
                  int cols) {
  setsid();
  int fd = open(slave, O_RDWR);
  if (fd < 0) {
    _exit(127);
  }
  ioctl(fd, TIOCSCTTY, 0);
  set_size(fd, rows, cols);
  dup2(fd, 0);
  dup2(fd, 1);
  dup2(fd, 2);
  if (fd > 2) {
    close(fd);
  }
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, NULL);
  signal(SIGPIPE, SIG_DFL);
  if (how->cwd && chdir(how->cwd) != 0) {
    dprintf(2, "ctui_pty: chdir %s: %s\n", how->cwd, strerror(errno));
    _exit(127);
  }
  for (const char *const *u = how->unset; u && *u; u++) {
    unsetenv(*u);
  }
  for (const char *const *e = how->env; e && *e; e++) {
    const char *eq = strchr(*e, '=');
    if (eq == NULL) {
      continue;
    }
    char name[256];
    size_t n = (size_t)(eq - *e);
    if (n >= sizeof name) {
      continue;
    }
    memcpy(name, *e, n);
    name[n] = '\0';
    setenv(name, eq + 1, 1);
  }
  execvp(how->argv[0], (char *const *)how->argv);
  dprintf(2, "ctui_pty: %s: %s\n", how->argv[0], strerror(errno));
  _exit(127);
}

CTUI_PTY *ctui_pty_spawn(const CTUI_PTY_SPAWN *how) {
  int rows = how->rows > 0 ? how->rows : 24;
  int cols = how->cols > 0 ? how->cols : 80;
  CTUI_PTY *p = calloc(1, sizeof *p);
  if (p == NULL) {
    return NULL;
  }
  p->vt = ctui_vt_new(rows, cols);
  p->fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (p->vt == NULL || p->fd < 0 || grantpt(p->fd) != 0 ||
      unlockpt(p->fd) != 0 || ptsname(p->fd) == NULL) {
    goto fail;
  }
  ctui_vt_answer_kitty_shm(p->vt, how->kitty_shm);
  char slave[128];
  snprintf(slave, sizeof slave, "%s", ptsname(p->fd));
  set_size(p->fd, rows, cols);
  fflush(NULL);
  p->pid = fork();
  if (p->pid < 0) {
    goto fail;
  }
  if (p->pid == 0) {
    child(how, slave, rows, cols);
  }
  fcntl(p->fd, F_SETFL, fcntl(p->fd, F_GETFL) | O_NONBLOCK);
  return p;
fail:
  if (p->fd >= 0) {
    close(p->fd);
  }
  ctui_vt_free(p->vt);
  free(p);
  return NULL;
}

static void reap(CTUI_PTY *p, int block) {
  if (p->reaped) {
    return;
  }
  if (waitpid(p->pid, &p->status, block ? 0 : WNOHANG) == p->pid) {
    p->reaped = 1;
  }
}

static void keep(CTUI_PTY *p, const char *buf, size_t n) {
  if (p->nout + n > p->out_cap) {
    size_t cap = p->out_cap ? p->out_cap : 65536;
    while (cap < p->nout + n) {
      cap *= 2;
    }
    char *q = realloc(p->out, cap);
    if (q == NULL) {
      return;
    }
    p->out = q;
    p->out_cap = cap;
  }
  memcpy(p->out + p->nout, buf, n);
  p->nout += n;
}

/* reads whatever is ready now: bytes read, -1 once the app's side is gone */
static int drain(CTUI_PTY *p) {
  char buf[65536];
  int total = 0;
  for (;;) {
    ssize_t n = read(p->fd, buf, sizeof buf);
    if (n > 0) {
      keep(p, buf, (size_t)n);
      ctui_vt_feed(p->vt, buf, (size_t)n);
      size_t nr;
      const char *r = ctui_vt_replies(p->vt, &nr);
      if (r) {
        ctui_pty_send(p, r, nr);
      }
      total += (int)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return total;
    }
    /* EIO (Linux) or 0: no one holds the other side any more */
    p->gone = 1;
    reap(p, 0);
    return total ? total : -1;
  }
}

int ctui_pty_pump(CTUI_PTY *p, int ms) {
  if (p->gone) {
    return -1;
  }
  struct pollfd pfd = {.fd = p->fd, .events = POLLIN};
  int r = poll(&pfd, 1, ms < 0 ? 0 : ms);
  if (r <= 0) {
    return 0;
  }
  return drain(p);
}

int ctui_pty_send(CTUI_PTY *p, const char *bytes, size_t n) {
  while (n > 0) {
    ssize_t w = write(p->fd, bytes, n);
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      /* the app isn't reading: read its output meanwhile, it may be
       * blocked writing */
      if (ctui_pty_pump(p, 10) < 0) {
        return -1;
      }
      continue;
    }
    if (w <= 0) {
      return -1;
    }
    bytes += w;
    n -= (size_t)w;
  }
  return 0;
}

void ctui_pty_resize(CTUI_PTY *p, int rows, int cols) {
  ctui_pty_pump(p, 0);
  ctui_vt_resize(p->vt, rows, cols);
  set_size(p->fd, rows, cols);
}

static int until(CTUI_PTY *p, const char *text, int present, int timeout) {
  long end = now_ms() + timeout;
  for (;;) {
    ctui_pty_pump(p, 0);
    if ((ctui_vt_find(p->vt, text) >= 0) == present) {
      return 0;
    }
    long left = end - now_ms();
    if (left <= 0 || p->gone) {
      return -1;
    }
    ctui_pty_pump(p, (int)(left < 50 ? left : 50));
  }
}

int ctui_pty_until_text(CTUI_PTY *p, const char *text, int timeout_ms) {
  return until(p, text, 1, timeout_ms);
}

int ctui_pty_until_gone(CTUI_PTY *p, const char *text, int timeout_ms) {
  return until(p, text, 0, timeout_ms);
}

int ctui_pty_until_quiet(CTUI_PTY *p, int quiet_ms, int timeout_ms) {
  long start = now_ms(), last = start;
  for (;;) {
    long t = now_ms();
    if (t - last >= quiet_ms || p->gone) {
      return 0;
    }
    if (t - start >= timeout_ms) {
      return -1;
    }
    long wait = quiet_ms - (t - last);
    if (wait > timeout_ms - (t - start)) {
      wait = timeout_ms - (t - start);
    }
    if (ctui_pty_pump(p, (int)wait) > 0) {
      last = now_ms();
    }
  }
}

void ctui_pty_wait(CTUI_PTY *p, int ms) {
  long end = now_ms() + ms;
  for (long left = ms; left > 0 && !p->gone; left = end - now_ms()) {
    ctui_pty_pump(p, (int)left);
  }
}

static void rule(int cols, FILE *out) {
  for (int i = 0; i < cols; i++) {
    fputc('-', out);
  }
  fputc('\n', out);
}

void ctui_pty_dump(CTUI_PTY *p, FILE *out, FILE *styles) {
  ctui_pty_pump(p, 0);
  char *text = ctui_vt_text(p->vt);
  fprintf(out, "%s\n", text ? text : "(out of memory)");
  free(text);
  rule(ctui_vt_cols(p->vt), out);
  if (styles) {
    char *st = ctui_vt_styles(p->vt);
    fputs(st ? st : "(out of memory)\n", styles);
    free(st);
    rule(ctui_vt_cols(p->vt), styles);
  }
}

CTUI_VT *ctui_pty_vt(CTUI_PTY *p) { return p->vt; }

const char *ctui_pty_output(const CTUI_PTY *p, size_t *n) {
  *n = p->nout;
  return p->out;
}

int ctui_pty_gone(const CTUI_PTY *p) { return p->gone; }

int ctui_pty_close(CTUI_PTY *p, FILE *out) {
  int ret = 1;
  reap(p, 0);
  if (!p->reaped) {
    kill(p->pid, SIGTERM);
    for (long end = now_ms() + 2000; !p->reaped && now_ms() < end;) {
      if (ctui_pty_pump(p, 20) < 0) {
        /* its side closed: it's ending, or has ended */
        struct timespec ts = {0, 10 * 1000000L};
        nanosleep(&ts, NULL);
      }
      reap(p, 0);
    }
    if (!p->reaped) {
      kill(p->pid, SIGKILL);
      reap(p, 1);
      ret = -1;
    } else {
      /* ended by our SIGTERM, or caught it and exited cleanly */
      ret = (WIFSIGNALED(p->status) && WTERMSIG(p->status) == SIGTERM) ||
                    (WIFEXITED(p->status) && WEXITSTATUS(p->status) == 0)
                ? 0
                : 1;
    }
  }
  if (out && ret == 1) {
    if (WIFEXITED(p->status)) {
      fprintf(out, "-- exited %d --\n", WEXITSTATUS(p->status));
    } else if (WIFSIGNALED(p->status)) {
      fprintf(out, "-- killed by signal %d --\n", WTERMSIG(p->status));
    }
  } else if (out && ret < 0) {
    fprintf(out, "-- would not end: SIGKILL --\n");
  }
  close(p->fd);
  ctui_vt_free(p->vt);
  free(p->out);
  free(p);
  return ret;
}

static void click(CTUI_PTY *p, const CTUI_HARNESS_STEP *s) {
  int button = atoi(s->s);
  char buf[64];
  int n = snprintf(buf, sizeof buf, "\x1b[<%d;%d;%dM", button, s->a, s->b);
  if (button < 64) {
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\x1b[<%d;%d;%dm", button,
                  s->a, s->b);
  }
  ctui_pty_send(p, buf, (size_t)n);
}

static int failed(CTUI_PTY *p, FILE *out, FILE *styles,
                  const CTUI_HARNESS_STEP *s, int ms) {
  if (s->op == CTUI_HS_OP_QUIET) {
    fprintf(out, "!! quiet for %d ms: not within %d ms\n", s->a, ms);
  } else {
    fprintf(out, "!! %s \"%s\": not within %d ms\n",
            s->op == CTUI_HS_OP_UNTIL ? "until" : "gone", s->s, ms);
  }
  ctui_pty_dump(p, out, styles);
  return -1;
}

int ctui_harness_steps(CTUI_PTY *p, const CTUI_HARNESS_STEP *steps, FILE *out,
                       FILE *styles) {
  for (const CTUI_HARNESS_STEP *s = steps; s->op != CTUI_HS_OP_END; s++) {
    int ms = s->a > 0 ? s->a : CTUI_HARNESS_TIMEOUT_MS;
    switch (s->op) {
    case CTUI_HS_OP_KEYS:
      ctui_pty_send(p, s->s, (size_t)s->a);
      if (s->a > 0 && s->s[s->a - 1] == 0x1b &&
          (s->a == 1 || s->s[s->a - 2] != 0x1b)) {
        ctui_pty_wait(p, 80);
      }
      break;
    case CTUI_HS_OP_UNTIL:
      if (ctui_pty_until_text(p, s->s, ms) != 0) {
        return failed(p, out, styles, s, ms);
      }
      break;
    case CTUI_HS_OP_GONE:
      if (ctui_pty_until_gone(p, s->s, ms) != 0) {
        return failed(p, out, styles, s, ms);
      }
      break;
    case CTUI_HS_OP_QUIET: {
      int quiet = s->a > 0 ? s->a : CTUI_HARNESS_QUIET_MS;
      int limit = s->b > 0 ? s->b : CTUI_HARNESS_TIMEOUT_MS;
      if (ctui_pty_until_quiet(p, quiet, limit) != 0) {
        CTUI_HARNESS_STEP shown = *s;
        shown.a = quiet;
        return failed(p, out, styles, &shown, limit);
      }
      break;
    }
    case CTUI_HS_OP_WAIT:
      ctui_pty_wait(p, s->a);
      break;
    case CTUI_HS_OP_RESIZE:
      ctui_pty_resize(p, s->a, s->b);
      break;
    case CTUI_HS_OP_CLICK:
      click(p, s);
      break;
    case CTUI_HS_OP_DUMP:
      ctui_pty_dump(p, out, styles);
      break;
    default:
      break;
    }
  }
  return 0;
}
