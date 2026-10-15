/* ctui_screen_flush() on real frames: recordings of apps' output
 * ($CTUI_PTY_RECORD, tools/harness/harness.h) replayed into a VT, the grid
 * taken at every frame's end (the "\e[0m" flush closes it with), each
 * frame flushed again by this build's flush -- timed against the frame
 * before it, its bytes counted, and replayed into a second VT whose cells
 * must come out the same.
 *
 *   make flushbench
 *   CTUI_PTY_RECORD=DIR <run the screens>
 *   ./flushbench [--reps N] [--out DIR] DIR/(star).vt
 *
 * --out DIR writes NAME.vt (the recording as replayed) and NAME.new.vt
 * (this flush's bytes) with "\e_ctui-record;frame\e\\" after each chunk
 * taken as a frame (in both, so they stay in step),
 * for a terminal to compare them frame by frame (tools/check_flush_kitty.py).
 * Exit 1 when a frame's cells differ. Not part of any app. */
#define _POSIX_C_SOURCE 200809L

#include "harness/vt.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  long frames, cells, bad;
  size_t in_bytes, out_bytes;
  /* ns: the frames flushed; idle_ns: each flushed again unchanged (the
   * compare alone, nothing written) */
  double ns, idle_ns;
} TOTALS;

static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static char *slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return NULL;
  }
  size_t cap = 1 << 20, len = 0;
  char *buf = malloc(cap);
  size_t got;
  while (buf && (got = fread(buf + len, 1, cap - len, f)) > 0) {
    len += got;
    if (len == cap) {
      char *p = realloc(buf, cap *= 2);
      if (p == NULL) {
        free(buf);
        buf = NULL;
      }
      buf = p;
    }
  }
  fclose(f);
  *n = len;
  return buf;
}

/* the cells a terminal shows the same: per colour mode, as flush compares */
static int same_cell(const CTUI_CELL *a, const CTUI_CELL *b) {
  if (a->ch != b->ch || a->color_mode != b->color_mode || a->attr != b->attr) {
    return 0;
  }
  switch (a->color_mode) {
  case CTUI_COLOR_MODE_RGB:
    return a->fg_r == b->fg_r && a->fg_g == b->fg_g && a->fg_b == b->fg_b &&
           a->bg_r == b->bg_r && a->bg_g == b->bg_g && a->bg_b == b->bg_b;
  case CTUI_COLOR_MODE_RGB_FG:
    return a->fg_r == b->fg_r && a->fg_g == b->fg_g && a->fg_b == b->fg_b &&
           a->bg == b->bg;
  default:
    return a->fg == b->fg && a->bg == b->bg;
  }
}

/* flush's bytes, through a temporary file (a frame can pass a pipe's) */
static size_t capture(CTUI_SCREEN *s, FILE *tmp, char **buf, size_t *cap) {
  ftruncate(fileno(tmp), 0);
  lseek(fileno(tmp), 0, SEEK_SET);
  int saved = dup(STDOUT_FILENO);
  dup2(fileno(tmp), STDOUT_FILENO);
  ctui_screen_flush(s);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  off_t n = lseek(fileno(tmp), 0, SEEK_CUR);
  if ((size_t)n > *cap) {
    *cap = (size_t)n * 2;
    *buf = realloc(*buf, *cap);
  }
  pread(fileno(tmp), *buf, (size_t)n, 0);
  return (size_t)n;
}

/* the best of reps flushes of the frame against prev (the frame before,
 * or s->buffer: against itself) */
static double time_flush(CTUI_SCREEN *s, const CTUI_CELL *prev, int reps,
                         int devnull) {
  size_t n = (size_t)s->rows * (size_t)s->cols * sizeof(CTUI_CELL);
  CTUI_CELL *done = malloc(n);
  memcpy(done, s->buffer, n);
  if (prev == s->buffer) {
    prev = done; /* the frame against itself */
  }
  int saved = dup(STDOUT_FILENO);
  dup2(devnull, STDOUT_FILENO);
  double best = 1e18;
  for (int k = 0; k < reps; k++) {
    memcpy(s->buffer, prev, n);
    double t0 = now_ns();
    ctui_screen_flush(s);
    double dt = now_ns() - t0;
    best = dt < best ? dt : best;
  }
  dup2(saved, STDOUT_FILENO);
  close(saved);
  memcpy(s->buffer, done, n);
  free(done);
  return best;
}

static const char SIZE_MARK[] = "\x1b_ctui-record;size=";
static const char FRAME_MARK[] = "\x1b_ctui-record;frame\x1b\\";

static void replay(const char *path, int reps, const char *outdir,
                   TOTALS *all) {
  size_t n;
  char *in = slurp(path, &n);
  if (in == NULL) {
    fprintf(stderr, "%s: can't read\n", path);
    return;
  }
  FILE *old_out = NULL, *new_out = NULL;
  if (outdir) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char p[4096];
    snprintf(p, sizeof p, "%s/%.*s.vt", outdir, (int)(strlen(base) - 3), base);
    old_out = fopen(p, "wb");
    snprintf(p, sizeof p, "%s/%.*s.new.vt", outdir, (int)(strlen(base) - 3),
             base);
    new_out = fopen(p, "wb");
  }
  CTUI_VT *a = NULL, *b = NULL;
  CTUI_SCREEN *s = NULL;
  CTUI_CELL *prev = NULL;
  FILE *tmp = tmpfile();
  int devnull = open("/dev/null", O_WRONLY);
  char *buf = NULL;
  size_t cap = 0;
  TOTALS t = {0};
  t.in_bytes = n;
  size_t i = 0;
  while (i < n) {
    if (n - i >= sizeof SIZE_MARK - 1 &&
        memcmp(in + i, SIZE_MARK, sizeof SIZE_MARK - 1) == 0) {
      int rows = 0, cols = 0;
      sscanf(in + i + sizeof SIZE_MARK - 1, "%d;%d", &rows, &cols);
      const char *end = memchr(in + i, '\\', n - i);
      size_t len = end ? (size_t)(end - (in + i)) + 1 : n - i;
      if (old_out) {
        fwrite(in + i, 1, len, old_out);
        fwrite(in + i, 1, len, new_out);
      }
      i += len;
      if (a == NULL) {
        a = ctui_vt_new(rows, cols);
        b = ctui_vt_new(rows, cols);
        s = ctui_screen_create(rows, cols);
      } else {
        ctui_vt_resize(a, rows, cols);
        ctui_vt_resize(b, rows, cols);
        ctui_screen_resize(s, rows, cols);
      }
      prev = realloc(prev, (size_t)rows * (size_t)cols * sizeof *prev);
      continue;
    }
    if (a == NULL) {
      fprintf(stderr, "%s: no size first: not a recording\n", path);
      break;
    }
    /* up to the next frame's end, or the next size */
    size_t len = n - i;
    for (size_t j = i; j + 3 < n; j++) {
      if (memcmp(in + j, "\x1b[0m", 4) == 0) {
        len = j + 4 - i;
        break;
      }
      if (n - j >= sizeof SIZE_MARK - 1 &&
          memcmp(in + j, SIZE_MARK, sizeof SIZE_MARK - 1) == 0) {
        len = j - i;
        break;
      }
    }
    ctui_vt_feed(a, in + i, len);
    if (old_out) {
      fwrite(in + i, 1, len, old_out);
      fwrite(FRAME_MARK, 1, sizeof FRAME_MARK - 1, old_out);
    }
    i += len;

    int rows = ctui_vt_rows(a), cols = ctui_vt_cols(a);
    for (int r = 0; r < rows; r++) {
      for (int c = 0; c < cols; c++) {
        s->cells[r * cols + c] = *ctui_vt_cell(a, r, c);
      }
    }
    memcpy(prev, s->buffer, (size_t)rows * (size_t)cols * sizeof *prev);
    for (int k = 0; k < rows * cols; k++) {
      t.cells += !same_cell(&s->cells[k], &prev[k]);
    }
    size_t got = capture(s, tmp, &buf, &cap);
    if (new_out) {
      fwrite(buf, 1, got, new_out);
      fwrite(FRAME_MARK, 1, sizeof FRAME_MARK - 1, new_out);
    }
    if (got == 0) {
      continue; /* nothing changed: not a frame */
    }
    t.frames++;
    t.out_bytes += got;
    ctui_vt_feed(b, buf, got);
    for (int r = 0; r < rows; r++) {
      for (int c = 0; c < cols; c++) {
        if (!same_cell(ctui_vt_cell(a, r, c), ctui_vt_cell(b, r, c))) {
          if (t.bad < 3) {
            fprintf(stderr, "%s: frame %ld differs at %d,%d\n", path,
                    t.frames, r, c);
          }
          t.bad++;
          r = rows;
          break;
        }
      }
    }
    t.ns += time_flush(s, prev, reps, devnull);
    t.idle_ns += time_flush(s, s->buffer, reps, devnull);
  }
  printf("%-34s %5ld frames %8ld cells %9zu B in %9zu B out %8.1f us%s\n",
         strrchr(path, '/') ? strrchr(path, '/') + 1 : path, t.frames,
         t.cells, t.in_bytes, t.out_bytes, t.ns / 1e3,
         t.bad ? "  DIFFERS" : "");
  all->frames += t.frames;
  all->cells += t.cells;
  all->bad += t.bad;
  all->in_bytes += t.in_bytes;
  all->out_bytes += t.out_bytes;
  all->ns += t.ns;
  all->idle_ns += t.idle_ns;
  if (old_out) {
    fclose(old_out);
    fclose(new_out);
  }
  close(devnull);
  fclose(tmp);
  free(buf);
  free(prev);
  if (s) {
    ctui_screen_free(s);
    ctui_vt_free(a);
    ctui_vt_free(b);
  }
  free(in);
}

int main(int argc, char **argv) {
  int reps = 20;
  const char *outdir = NULL;
  TOTALS all = {0};
  int files = 0;
  ctui_log_init(0);
  for (int k = 1; k < argc; k++) {
    if (strcmp(argv[k], "--reps") == 0 && k + 1 < argc) {
      reps = atoi(argv[++k]);
    } else if (strcmp(argv[k], "--out") == 0 && k + 1 < argc) {
      outdir = argv[++k];
    } else {
      replay(argv[k], reps, outdir, &all);
      files++;
    }
  }
  if (files == 0) {
    fprintf(stderr, "usage: flushbench [--reps N] [--out DIR] FILE.vt...\n");
    return 2;
  }
  printf("total: %ld frames, %ld cells changed, %zu B recorded, %zu B "
         "flushed (%.1f per changed cell), flush %.1f us (%.1f ns per "
         "changed cell), the same frames unchanged %.1f us%s\n",
         all.frames, all.cells, all.in_bytes, all.out_bytes,
         all.cells ? (double)all.out_bytes / (double)all.cells : 0.0,
         all.ns / 1e3, all.cells ? all.ns / (double)all.cells : 0.0,
         all.idle_ns / 1e3, all.bad ? ", FRAMES DIFFER" : "");
  return all.bad ? 1 : 0;
}
