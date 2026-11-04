/* core/term.c's ctui_term_write(): a frame's bytes reach the terminal
 * whole even when the fd is non-blocking and full (stdin's O_NONBLOCK is
 * stdout's when they're one open file) or a signal interrupts the write.
 * STDOUT is swapped for a pipe a slow child drains, checking the bytes
 * come in order and all of them. */

#define _POSIX_C_SOURCE 200809L

#include "ctui.h"
#include "core/ctui_internal.h"

#include "ctui_test.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { BYTES = 1 << 20 };

static unsigned char at(size_t i) { return (unsigned char)(i * 7 + i / 251); }

/* the child: reads slowly (a pipe's 64 KiB fills behind it), exits 0 when
 * it got exactly BYTES in order */
static int drain(int fd) {
  unsigned char buf[4096];
  size_t got = 0;
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      return 2;
    }
    for (ssize_t k = 0; k < n; k++, got++) {
      if (buf[k] != at(got)) {
        return 3;
      }
    }
    const struct timespec ts = {0, 50000};
    nanosleep(&ts, NULL);
  }
  return got == BYTES ? 0 : 4;
}

static void on_alarm(int sig) { (void)sig; }

/* BYTES through ctui_term_write onto a pipe as stdout; how it went: the
 * write's return and the child's verdict */
static void run(int nonblock, int *wrote, int *child) {
  unsigned char *data = malloc(BYTES);
  int fds[2];
  if (!data || pipe(fds) != 0) {
    free(data);
    *wrote = *child = -1;
    return;
  }
  for (size_t i = 0; i < BYTES; i++) {
    data[i] = at(i);
  }
  fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    close(fds[1]);
    _exit(drain(fds[0]));
  }
  close(fds[0]);
  if (nonblock) {
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
  }
  const int saved = dup(STDOUT_FILENO);
  dup2(fds[1], STDOUT_FILENO);
  close(fds[1]);
  *wrote = ctui_term_write(data, BYTES);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  int st = 0;
  waitpid(pid, &st, 0);
  *child = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  free(data);
}

int main(void) {
  int wrote, child;
  run(1, &wrote, &child);
  CTUI_TEST_ASSERT(wrote == 1 && child == 0,
                   "a non-blocking stdout, full behind a slow reader: every "
                   "byte, in order (wrote %d, reader %d)",
                   wrote, child);

  /* a signal every millisecond, no SA_RESTART: short writes and EINTR */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_alarm;
  sigaction(SIGALRM, &sa, NULL);
  struct itimerval it = {{0, 1000}, {0, 1000}};
  setitimer(ITIMER_REAL, &it, NULL);
  run(0, &wrote, &child);
  struct itimerval off = {{0, 0}, {0, 0}};
  setitimer(ITIMER_REAL, &off, NULL);
  CTUI_TEST_ASSERT(wrote == 1 && child == 0,
                   "a blocking stdout interrupted by signals: every byte, in "
                   "order (wrote %d, reader %d)",
                   wrote, child);
  return ctui_test_summary();
}
