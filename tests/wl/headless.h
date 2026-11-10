#ifndef CTUI_TEST_HEADLESS_H
#define CTUI_TEST_HEADLESS_H

/* A headless sway of a suite's own, for src/wl/'s and the presenters'
 * suites: a scratch XDG_RUNTIME_DIR (short: sway's socket path has a
 * limit), the CPU renderer, no input devices, DISPLAY and any outer
 * WAYLAND_DISPLAY unset -- so a client can only reach this one, and
 * only once its socket is there (headless_start() 0). Needs _GNU_SOURCE
 * (mkdtemp, setenv) before the first include. */

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char headless_root[] = "/tmp/ctui-wl-XXXXXX";
static pid_t headless_pid = -1;
static int headless_made;

static long headless_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void headless_sleep(long ms) {
  nanosleep(
      &(struct timespec){.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000},
      NULL);
}

/* 1 when there's a sway to run */
static int headless_available(void) {
  return system("command -v sway >/dev/null") == 0;
}

/* sway with conf as its config; WAYLAND_DISPLAY its socket's path. 0,
 * or -1 (nothing to connect to: the suite mustn't try) */
static int headless_start(const char *conf_text) {
  if (!mkdtemp(headless_root)) {
    perror("mkdtemp");
    return -1;
  }
  headless_made = 1;
  chmod(headless_root, 0700);
  setenv("XDG_RUNTIME_DIR", headless_root, 1);
  unsetenv("WAYLAND_DISPLAY");
  unsetenv("DISPLAY");
  unsetenv("SWAYSOCK");
  setenv("WLR_BACKENDS", "headless", 1);
  setenv("WLR_RENDERER", "pixman", 1);
  setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
  char conf[128], log[128];
  snprintf(conf, sizeof conf, "%s/sway.conf", headless_root);
  FILE *f = fopen(conf, "w");
  if (!f) {
    return -1;
  }
  fputs(conf_text, f);
  fclose(f);
  snprintf(log, sizeof log, "%s/sway.log", headless_root);
  fflush(NULL);
  headless_pid = fork();
  if (headless_pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (!freopen(log, "w", stdout) || dup2(1, 2) < 0) {
      _exit(127);
    }
    execlp("sway", "sway", "--unsupported-gpu", "-c", conf, (char *)NULL);
    _exit(127);
  }
  if (headless_pid < 0) {
    return -1;
  }
  char wl[64] = "";
  for (long end = headless_ms() + 5000; !wl[0] && headless_ms() < end;) {
    DIR *dir = opendir(headless_root);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
      if (!strncmp(e->d_name, "wayland-", 8) && !strchr(e->d_name, '.')) {
        snprintf(wl, sizeof wl, "%.60s", e->d_name);
      }
    }
    if (dir) {
      closedir(dir);
    }
    headless_sleep(50);
  }
  if (!wl[0]) {
    return -1;
  }
  char path[160];
  snprintf(path, sizeof path, "%s/%s", headless_root, wl);
  setenv("WAYLAND_DISPLAY", path, 1);
  headless_sleep(200);
  return 0;
}

static void headless_stop(void) {
  if (headless_pid > 0) {
    kill(headless_pid, SIGTERM);
    waitpid(headless_pid, NULL, 0);
    headless_pid = -1;
  }
  if (!headless_made) {
    return;
  }
  char cmd[128];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", headless_root);
  if (system(cmd) != 0) {
    printf("  -- couldn't remove %s\n", headless_root);
  }
}

#endif
