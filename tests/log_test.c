/* Exercises core/log.c directly -- ctui_logf()'s own verbosity pre-check
 * (log_entry() in src/logger.c has an identical check, but ctui_logf()'s
 * copy short-circuits before the vsnprintf/malloc work, see its own
 * comment), ctui_log()'s pass-through, ctui_tick_advance()'s counter, and
 * ctui_log_shutdown(). Every other test calls ctui_log_init(E_ALL), so
 * nothing else in the suite ever takes the "filtered out" branch or calls
 * shutdown. */
#define _POSIX_C_SOURCE 200809L /* mkstemp(), mkdtemp(), setenv() */

#include "ctui.h"

#include "ctui_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int slurp(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    return 0;
  }
  size_t n = fread(buf, 1, cap - 1, f);
  buf[n] = '\0';
  fclose(f);
  return 1;
}

int main(void) {
  /* before ctui_log_init(): dropped, not a call through a NULL logger */
  CTUI_TEST_ASSERT(ctui_log(E_ERR, "before init\n") == 0,
                   "ctui_log before ctui_log_init drops the line");
  CTUI_TEST_ASSERT(ctui_logf(E_ERR, "before init %d\n", 1) == 0,
                   "ctui_logf before ctui_log_init drops the line");

  ctui_log_init(E_ERR); /* only E_ERR passes -- everything else is masked */

  int r = ctui_logf(E_DBG, "this should be filtered out\n");
  CTUI_TEST_ASSERT(r == 0,
                   "logf returns 0 (not an error) when level is masked out "
                   "by verbosity, without touching vsnprintf/malloc");

  r = ctui_logf(E_ERR, "this should be written (%d)\n", 42);
  CTUI_TEST_ASSERT(r > 0,
                   "logf returns the byte count written when level passes "
                   "the verbosity filter");

  r = ctui_log(E_ERR, "a pre-formatted string via ctui_log\n");
  CTUI_TEST_ASSERT(r > 0, "ctui_log writes a pre-formatted string directly");

  int tick1 = ctui_tick_advance();
  int tick2 = ctui_tick_advance();
  CTUI_TEST_ASSERT(tick2 == tick1 + 1,
                   "tick_advance increments a monotonic counter by one "
                   "each call");

  ctui_log_shutdown();

  /* a path of the app's own */
  char path[] = "/tmp/ctui-log-test-XXXXXX";
  int fd = mkstemp(path);
  CTUI_TEST_ASSERT(fd >= 0, "a scratch log file");
  close(fd);
  ctui_log_set_path(path);
  ctui_log_init(E_ALL);
  ctui_log(E_INF, "into the app's log\n");
  ctui_log_shutdown();
  ctui_log_set_path(NULL);
  FILE *f = fopen(path, "r");
  char buf[4096] = "";
  size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
  buf[n] = '\0';
  if (f) {
    fclose(f);
  }
  CTUI_TEST_ASSERT(strstr(buf, "into the app's log") != NULL,
                   "ctui_log_set_path() moves the log");
  remove(path);

  /* rotation: a scratch directory, a 1000-byte cap */
  char dir[] = "/tmp/ctui-log-rot-XXXXXX";
  CTUI_TEST_ASSERT(mkdtemp(dir) != NULL, "a scratch log directory");
  char lp[256], old[256];
  snprintf(lp, sizeof lp, "%s/app.log", dir);
  snprintf(old, sizeof old, "%s/app.log.1", dir);
  struct stat st;
  ctui_log_set_path(lp);
  ctui_log_set_max_size(1000);
  ctui_log_init(E_ALL);
  for (int i = 0; i < 30; i++) { /* 30 * 40 = 1200 bytes */
    ctui_logf(E_INF, "line %02d: ...............................\n", i);
  }
  CTUI_TEST_ASSERT(stat(old, &st) == 0 && st.st_size >= 1000,
                   "past the cap the log moved to PATH.1");
  CTUI_TEST_ASSERT(stat(lp, &st) == 0 && st.st_size < 1000,
                   "and PATH started again");
  CTUI_TEST_ASSERT(slurp(lp, buf, sizeof buf) &&
                       strstr(buf, "rotated: the lines before are in") &&
                       strstr(buf, "line 29"),
                   "the new file says where the old lines went, then goes on");
  for (int i = 0; i < 60; i++) {
    ctui_logf(E_INF, "more %02d: ...............................\n", i);
  }
  CTUI_TEST_ASSERT(slurp(old, buf, sizeof buf) && !strstr(buf, "line 00") &&
                       strstr(buf, "more "),
                   "a second rotation replaces .1: one old generation");
  ctui_log_shutdown();

  /* a file already past the cap rotates at open */
  remove(old);
  FILE *big = fopen(lp, "w");
  for (int i = 0; big && i < 100; i++) {
    fputs("left over from an earlier run.........\n", big);
  }
  if (big) {
    fclose(big);
  }
  ctui_log_init(E_ALL);
  CTUI_TEST_ASSERT(slurp(old, buf, sizeof buf) &&
                       strstr(buf, "left over from an earlier run"),
                   "a big log from before moves aside at open");
  CTUI_TEST_ASSERT(slurp(lp, buf, sizeof buf) && !strstr(buf, "left over") &&
                       strstr(buf, "logger initialised"),
                   "and this run starts a fresh one");

  /* someone else rotated it (another process on the same log): this one
   * reopens PATH, renaming nothing */
  remove(old);
  char moved[256];
  snprintf(moved, sizeof moved, "%s/elsewhere", dir);
  rename(lp, moved);
  FILE *other = fopen(lp, "w");
  if (other) {
    fputs("the other process's line\n", other);
    fclose(other);
  }
  for (int i = 0; i < 30; i++) {
    ctui_logf(E_INF, "late %02d: ...............................\n", i);
  }
  CTUI_TEST_ASSERT(stat(old, &st) != 0,
                   "a log rotated by another process isn't rotated again");
  CTUI_TEST_ASSERT(slurp(lp, buf, sizeof buf) &&
                       strstr(buf, "the other process's line") &&
                       strstr(buf, "rotated:"),
                   "its lines go on in the new PATH");
  ctui_log_shutdown();

  /* no cap */
  remove(lp);
  ctui_log_set_max_size(0);
  ctui_log_init(E_ALL);
  for (int i = 0; i < 60; i++) {
    ctui_logf(E_INF, "uncapped %02d: ...........................\n", i);
  }
  CTUI_TEST_ASSERT(stat(old, &st) != 0 && stat(lp, &st) == 0 &&
                       st.st_size > 2000,
                   "a cap of 0 never rotates");
  ctui_log_shutdown();

  /* CTUI_LOG_MAX overrides the app's cap */
  remove(lp);
  ctui_log_set_max_size(1L << 30);
  setenv("CTUI_LOG_MAX", "1K", 1);
  ctui_log_init(E_ALL);
  for (int i = 0; i < 30; i++) {
    ctui_logf(E_INF, "env %02d: ................................\n", i);
  }
  CTUI_TEST_ASSERT(stat(old, &st) == 0, "CTUI_LOG_MAX=1K rotates at 1024");
  ctui_log_shutdown();
  unsetenv("CTUI_LOG_MAX");
  ctui_log_set_max_size(CTUI_LOG_MAX_DEFAULT);
  ctui_log_set_path(NULL);
  remove(lp);
  remove(old);
  remove(moved);
  rmdir(dir);

  CTUI_TEST_ASSERT(ctui_log_parse_max("8M") == 8L << 20 &&
                       ctui_log_parse_max("512k") == 512L << 10 &&
                       ctui_log_parse_max("1G") == 1L << 30 &&
                       ctui_log_parse_max("1234") == 1234 &&
                       ctui_log_parse_max("0") == 0,
                   "CTUI_LOG_MAX: bytes, K/M/G, 0");
  CTUI_TEST_ASSERT(
      ctui_log_parse_max("") == -1 && ctui_log_parse_max("-5") == -1 &&
          ctui_log_parse_max("8MB") == -1 && ctui_log_parse_max("big") == -1 &&
          ctui_log_parse_max(NULL) == -1,
      "anything else is ignored");

  return ctui_test_summary();
}
