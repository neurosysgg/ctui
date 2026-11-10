/* `make test` (src/wl/'s builds; skipped without sway): several surfaces
 * on one connection against a headless sway of the suite's own (a scratch
 * XDG_RUNTIME_DIR, the CPU renderer, no input devices, DISPLAY unset) --
 * the outputs by name, layer surfaces configured as asked (a bar along an
 * edge, a box in the middle), a window beside them, a bar moved, what a
 * layer can't change refused, an output that isn't there. Input routing
 * to a surface needs devices headless sway doesn't have: ctui-wm's
 * screens drive it (a virtual keyboard and pointer). */
#define _GNU_SOURCE

#include "ctui.h"
#include "ctui_test.h"
#include "wl/layer.h"
#include "wl/wl.h"

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
#include <wayland-client.h>

static char root[] = "/tmp/ctui-wl-XXXXXX";

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
  nanosleep(
      &(struct timespec){.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000},
      NULL);
}

static pid_t spawn(char *const *argv, const char *log) {
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (!freopen(log, "w", stdout) || dup2(1, 2) < 0) {
      _exit(127);
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  return pid;
}

/* 1920x1080 at 1.5: 1280x720 logical */
static const CTUI_WL_LAYER k_bar = {
    .output = "HEADLESS-1",
    .ns = "bar",
    .layer = CTUI_WL_LAYER_TOP,
    .anchor = CTUI_WL_EDGE_TOP | CTUI_WL_EDGE_LEFT | CTUI_WL_EDGE_RIGHT,
    .h = 30,
    .exclusive = 30,
};

static void test_surfaces(void) {
  char err[160] = "";
  CTUI_WL_DPY *d = ctui_wl_connect(err, sizeof err);
  CTUI_TEST_ASSERT(d != NULL, "connected (%s)", err);
  if (!d) {
    return;
  }
  const char *names[4];
  int n = ctui_wl_outputs(d, names, 4);
  CTUI_TEST_ASSERT(n == 1 && !strcmp(names[0], "HEADLESS-1"),
                   "one output, by its name (%d: %s)", n, n ? names[0] : "");

  CTUI_WL *bar = ctui_wl_layer(d, &k_bar, err, sizeof err);
  CTUI_TEST_ASSERT(bar != NULL, "a bar along the top (%s)", err);
  int w = 0, h = 0;
  if (bar) {
    ctui_wl_logical(bar, &w, &h);
  }
  CTUI_TEST_ASSERT(w == 1280 && h == 30,
                   "the bar is as wide as the output, as high as asked "
                   "(%dx%d)",
                   w, h);

  CTUI_WL_LAYER box = {.output = "HEADLESS-1",
                       .ns = "popup",
                       .layer = CTUI_WL_LAYER_OVERLAY,
                       .w = 200,
                       .h = 100,
                       .keyboard = CTUI_WL_KEYBOARD_EXCLUSIVE};
  CTUI_WL *pop = ctui_wl_layer(d, &box, err, sizeof err);
  w = h = 0;
  if (pop) {
    ctui_wl_logical(pop, &w, &h);
  }
  CTUI_TEST_ASSERT(pop && w == 200 && h == 100,
                   "a box in the middle, on the same connection (%dx%d, %s)", w,
                   h, err);

  CTUI_WL *win = ctui_wl_window(d, "t", "ctui-test", 300, 200, err, sizeof err);
  CTUI_TEST_ASSERT(win != NULL && ctui_wl_dpy(win) == d,
                   "a window beside them (%s)", err);
  ctui_wl_set_user(win, &box);
  CTUI_TEST_ASSERT(ctui_wl_user(win) == &box, "a surface keeps its user");

  if (bar) {
    CTUI_WL_LAYER taller = k_bar;
    taller.h = taller.exclusive = 50;
    CTUI_TEST_ASSERT(ctui_wl_layer_set(bar, &taller) == 0, "the bar resized");
    wl_display_roundtrip(ctui_wl_dpy_display(d));
    ctui_wl_logical(bar, &w, &h);
    CTUI_TEST_ASSERT(w == 1280 && h == 50, "configured again: %dx%d", w, h);
    CTUI_WL_LAYER moved = taller;
    moved.output = "HEADLESS-2";
    CTUI_TEST_ASSERT(ctui_wl_layer_set(bar, &moved) == -1,
                     "another output: refused (a new surface's)");
    moved = taller;
    moved.ns = "other";
    CTUI_TEST_ASSERT(ctui_wl_layer_set(bar, &moved) == -1,
                     "another namespace: refused");
  }
  if (win) {
    CTUI_TEST_ASSERT(ctui_wl_layer_set(win, &k_bar) == -1,
                     "a window isn't a layer");
  }

  CTUI_WL_LAYER gone = k_bar;
  gone.output = "HEADLESS-9";
  err[0] = '\0';
  CTUI_TEST_ASSERT(!ctui_wl_layer(d, &gone, err, sizeof err) &&
                       strstr(err, "HEADLESS-9"),
                   "an output that isn't there: NULL, named (%s)", err);

  CTUI_EVENT ev;
  CTUI_WL *on = NULL;
  CTUI_TEST_ASSERT(!ctui_wl_dpy_next(d, &ev, &on), "no input, no events");
  CTUI_TEST_ASSERT(!ctui_wl_closed(bar) && !ctui_wl_closed(pop),
                   "nothing closed by the compositor");

  ctui_wl_close(pop);
  ctui_wl_close(win);
  ctui_wl_close(bar);
  ctui_wl_disconnect(d);
}

/* the one-window shorthand still stands alone */
static void test_open(void) {
  char err[160] = "";
  CTUI_WL *wl = ctui_wl_open("t", "ctui-test", 320, 240, err, sizeof err);
  CTUI_TEST_ASSERT(wl != NULL, "ctui_wl_open (%s)", err);
  int pw = 0, ph = 0;
  if (wl) {
    ctui_wl_size(wl, &pw, &ph);
  }
  CTUI_TEST_ASSERT(pw > 0 && ph > 0, "its buffer has a size (%dx%d)", pw, ph);
  ctui_wl_close(wl);
}

int main(void) {
  ctui_log_init(E_ALL);
  if (system("command -v sway >/dev/null") != 0) {
    printf("  -- no sway: skipped\n");
    return ctui_test_summary();
  }
  if (!mkdtemp(root)) {
    perror("mkdtemp");
    return 1;
  }
  chmod(root, 0700);
  setenv("XDG_RUNTIME_DIR", root, 1);
  unsetenv("WAYLAND_DISPLAY");
  unsetenv("DISPLAY");
  unsetenv("SWAYSOCK");
  setenv("WLR_BACKENDS", "headless", 1);
  setenv("WLR_RENDERER", "pixman", 1);
  setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
  char conf[128], log[128];
  snprintf(conf, sizeof conf, "%s/sway.conf", root);
  FILE *f = fopen(conf, "w");
  if (f) {
    fputs("output HEADLESS-1 resolution 1920x1080 scale 1.5\n", f);
    fclose(f);
  }
  snprintf(log, sizeof log, "%s/sway.log", root);
  char *argv[] = {"sway", "--unsupported-gpu", "-c", conf, NULL};
  pid_t sway = spawn(argv, log);
  if (sway <= 0) {
    return 1;
  }
  char wl[64] = "";
  for (long end = now_ms() + 5000; !wl[0] && now_ms() < end;) {
    DIR *dir = opendir(root);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
      if (!strncmp(e->d_name, "wayland-", 8) && !strchr(e->d_name, '.')) {
        snprintf(wl, sizeof wl, "%.60s", e->d_name);
      }
    }
    if (dir) {
      closedir(dir);
    }
    sleep_ms(50);
  }
  CTUI_TEST_ASSERT(wl[0], "a headless sway (%s)", wl);
  /* never a client on another compositor: only with its socket found */
  if (wl[0]) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", root, wl);
    setenv("WAYLAND_DISPLAY", path, 1);
    sleep_ms(200);
    test_surfaces();
    test_open();
  }
  kill(sway, SIGTERM);
  waitpid(sway, NULL, 0);
  char cmd[128];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  if (system(cmd) != 0) {
    printf("  -- couldn't remove %s\n", root);
  }
  return ctui_test_summary();
}
