#define _POSIX_C_SOURCE 200809L /* fileno(), fstat(), getcwd(), strdup() */

#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int log_entry(CTUI_LOGGER *self, int level, const char *log_str);

/* "e": close-on-exec (glibc, musl), so what an app starts (a terminal's
 * shells, a host's next image) doesn't inherit the log */
static FILE *open_log(const char *path) { return fopen(path, "a+e"); }

static long file_size(FILE *f) {
  struct stat st;
  return fstat(fileno(f), &st) == 0 ? (long)st.st_size : 0;
}

/* the path rotation renames: absolute, so a chdir() after the log opened
 * (a file manager's, a shell's) still renames this file, not one of the
 * same name wherever the process went */
static char *absolute(const char *path) {
  if (path[0] == '/') {
    return strdup(path);
  }
  char *cwd = getcwd(NULL, 0);
  if (cwd == NULL) {
    return strdup(path);
  }
  size_t n = strlen(cwd) + 1 + strlen(path) + 1;
  char *abs = malloc(n);
  if (abs != NULL) {
    snprintf(abs, n, "%s/%s", cwd, path);
  }
  free(cwd);
  return abs;
}

CTUI_LOGGER init_logger(char *path, int verbosity) {
  FILE *f = open_log(path);
  if (f == NULL) {
    f = stderr;
    fprintf(stderr,
            "[CTUI:LOG] - could not open '%s' for logging, falling back to "
            "stderr\n",
            path);
  }
  CTUI_LOGGER logger = {.file = f,
                        .log_entry = log_entry,
                        .path = path,
                        .abs = f != stderr ? absolute(path) : NULL,
                        .verbosity = verbosity,
                        .buffered = 0,
                        .size = f != stderr ? file_size(f) : 0};
  return logger;
}

void shutdown_logger(CTUI_LOGGER *logger) {
  if (logger->file != NULL && logger->file != stderr) {
    fclose(logger->file); /* always flushes, even in buffered mode */
  }
  logger->file = NULL;
  free(logger->abs);
  logger->abs = NULL;
}

void logger_set_buffered(CTUI_LOGGER *logger, int buffered) {
  logger->buffered = buffered;
}

/* the file moves to PATH.1 (an older .1 goes) and PATH starts again. Only
 * if PATH is still the file this logger writes: another process sharing
 * the log may have rotated it already, and then only the new PATH is
 * opened. If nothing can be opened the old file stays and is written on. */
static void rotate(CTUI_LOGGER *self) {
  self->size = 0; /* a failure below waits another max bytes to retry */
  size_t n = strlen(self->abs) + 3;
  char *old = malloc(n);
  if (old == NULL) {
    return;
  }
  snprintf(old, n, "%s.1", self->abs);
  struct stat ours, named;
  int same = fstat(fileno(self->file), &ours) == 0 &&
             stat(self->abs, &named) == 0 && ours.st_dev == named.st_dev &&
             ours.st_ino == named.st_ino;
  FILE *f = NULL;
  if (!same || rename(self->abs, old) == 0) {
    f = open_log(self->abs);
  }
  if (f != NULL) {
    fclose(self->file);
    self->file = f;
    /* straight into the file: not through log_entry(), no level */
    fprintf(f, "[CTUI:LOG] - rotated: the lines before are in %s\n", old);
    fflush(f);
    self->size = file_size(f);
  }
  free(old);
}

/* once self->size (the size at open or the last check, plus what this
 * logger wrote since) reaches max: the file's real size decides -- another
 * process sharing it may have rotated it meanwhile */
static void check_size(CTUI_LOGGER *self) {
  if (!self->max || self->file == NULL || self->file == stderr ||
      self->abs == NULL || self->size < self->max) {
    return;
  }
  fflush(self->file);
  self->size = file_size(self->file);
  if (self->size >= self->max) {
    rotate(self);
  }
}

void logger_set_max(CTUI_LOGGER *logger, long max) {
  logger->max = max > 0 ? max : 0;
  check_size(logger);
}

static int log_entry(CTUI_LOGGER *self, int level, const char *log_str) {
  if (self->file == NULL)
    return -1;

  if (!(self->verbosity & level))
    return 0; /* filtered out by verbosity mask; not an error */

  size_t len = strlen(log_str);
  size_t written = fwrite(log_str, sizeof(char), len, self->file);
  if (!self->buffered) {
    fflush(self->file);
  }
  self->size += (long)written;
  check_size(self);
  if (written != len)
    return -1;
  return (int)written;
}
