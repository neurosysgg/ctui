#include "logger.h"

#include <stdio.h>
#include <string.h>

static int log_entry(CTUI_LOGGER *self, int level, const char *log_str);

CTUI_LOGGER init_logger(char *path, int verbosity) {
  FILE *f = fopen(path, "a+");
  if (f == NULL) {
    f = stderr;
    fprintf(stderr,
            "[CTUI:LOG] - could not open '%s' for logging, falling back to "
            "stderr\n",
            path);
  }
  CTUI_LOGGER logger = {
      .file = f, .log_entry = log_entry, .path = path, .verbosity = verbosity};
  return logger;
}

void shutdown_logger(CTUI_LOGGER *logger) {
  if (logger->file != NULL && logger->file != stderr) {
    fclose(logger->file);
  }
  logger->file = NULL;
}

static int log_entry(CTUI_LOGGER *self, int level, const char *log_str) {
  if (self->file == NULL)
    return -1;

  if (!(self->verbosity & level))
    return 0; /* filtered out by verbosity mask; not an error */

  size_t len = strlen(log_str);
  size_t written = fwrite(log_str, sizeof(char), len, self->file);
  fflush(self->file);
  if (written != len)
    return -1;
  return (int)written;
}
