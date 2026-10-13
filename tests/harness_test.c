/* tools/harness/: the VT parser (vt.c) on streams, ctui's own renderer's
 * output read back cell for cell, captured app output against
 * tools/pty_harness.py's grid (tests/data/harness/, made by
 * tools/harness/capture.py), and the pty side (harness.c) on /bin/sh. */
#define _POSIX_C_SOURCE 200809L

#include "ctui.h"

#include "ctui_test.h"
#include "harness/harness.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static CTUI_VT *fed(int rows, int cols, const char *s) {
  CTUI_VT *vt = ctui_vt_new(rows, cols);
  ctui_vt_feed(vt, s, strlen(s));
  return vt;
}

static int text_is(CTUI_VT *vt, const char *want) {
  char *t = ctui_vt_text(vt);
  int ok = t && strcmp(t, want) == 0;
  if (!ok) {
    printf("    text: [%s]\n    want: [%s]\n", t ? t : "(null)", want);
  }
  free(t);
  return ok;
}

static void test_text(void) {
  CTUI_VT *vt = fed(3, 10, "ab\x1b[2;4Hcd\x1b[3;1Hx\r\ny");
  CTUI_TEST_ASSERT(text_is(vt, "   cd\nx\ny"),
                   "text and CUP place characters; CR LF at the bottom row "
                   "scrolls it up");
  ctui_vt_free(vt);

  vt = fed(2, 5, "abcdefg");
  CTUI_TEST_ASSERT(text_is(vt, "abcde\nfg"),
                   "the fifth character fills the row, the sixth wraps");
  ctui_vt_free(vt);
  vt = fed(2, 5, "abcde\x1b[1;1Hz");
  CTUI_TEST_ASSERT(text_is(vt, "zbcde\n"),
                   "a full row doesn't wrap until a sixth character comes: "
                   "a CUP after it lands where it says");
  ctui_vt_free(vt);

  vt = fed(2, 6, "abcdef\x1b[1;3H\x1b[K\x1b[2;1Hxyz\x1b[2;2H\x1b[1K");
  CTUI_TEST_ASSERT(text_is(vt, "ab\n  z"), "EL to the end, EL to the start");
  ctui_vt_feed(vt, "\x1b[2J", 4);
  CTUI_TEST_ASSERT(text_is(vt, "\n"), "ED 2 clears it all");
  ctui_vt_free(vt);

  vt = fed(2, 8, "ab\tc\x1b[2;5Hd\x1b[2De\x1b[Af\x1b[3Gg\x1b[1dh\b\bi");
  CTUI_TEST_ASSERT(text_is(vt, "abihf  c\n   ed"),
                   "TAB, CUB, CUU, CHA, VPA, BS move the cursor");
  ctui_vt_free(vt);
}

static void test_swallowed(void) {
  CTUI_VT *vt = fed(1, 20,
                    "a\x1b]0;title\x07"
                    "b\x1b_Gf=32,s=1;AAAA\x1b\\c\x1bP+q\x1b\\d"
                    "\x1b[?1049h\x1b[?25l\x1b(Be\x1b[>1u\x1b"
                    "7f\x1b[38:2::1:2:3mg");
  CTUI_TEST_ASSERT(text_is(vt, "efg"),
                   "OSC, APC, DCS strings, private CSIs, ESC ( B and ESC 7 "
                   "leave no text; entering the alternate screen clears");
  ctui_vt_free(vt);
}

/* fed one byte at a time and whole: the same cells */
static void test_split(void) {
  const char *s = "\x1b[1;1H\x1b[0;1;38;2;10;20;30;48;2;1;2;3mA\xe4\xbd\xa0"
                  "e\xcc\x81\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa\x1b_Gq=2;x\x1b\\"
                  "\x1b[2;3H\x1b[38;5;200;48;5;17mz\x1b[0m";
  CTUI_VT *whole = fed(2, 12, s);
  CTUI_VT *bytes = ctui_vt_new(2, 12);
  for (size_t i = 0; s[i]; i++) {
    ctui_vt_feed(bytes, s + i, 1);
  }
  int same = 1;
  for (int r = 0; r < 2; r++) {
    for (int c = 0; c < 12; c++) {
      same &= memcmp(ctui_vt_cell(whole, r, c), ctui_vt_cell(bytes, r, c),
                     sizeof(CTUI_CELL)) == 0;
    }
  }
  CTUI_TEST_ASSERT(same, "a stream cut anywhere (in a sequence, a UTF-8 "
                         "character, a cluster) reads as the whole");
  CTUI_TEST_ASSERT(text_is(bytes,
                           "A\xe4\xbd\xa0"
                           "e\xcc\x81\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa\n  z"),
                   "a wide glyph, é as e + U+0301 and a flag read back as "
                   "the text sent");
  ctui_vt_free(whole);
  ctui_vt_free(bytes);
}

static void test_cells(void) {
  CTUI_VT *vt = fed(1, 8,
                    "\x1b[31;42ma\x1b[0;1;4;38;5;196;48;5;21mb"
                    "\x1b[22;38;2;1;2;3;44mc\x1b[48;2;4;5;6md"
                    "\x1b[0m\xe4\xbd\xa0");
  const CTUI_CELL *a = ctui_vt_cell(vt, 0, 0), *b = ctui_vt_cell(vt, 0, 1);
  const CTUI_CELL *c = ctui_vt_cell(vt, 0, 2), *d = ctui_vt_cell(vt, 0, 3);
  CTUI_TEST_ASSERT(a->color_mode == CTUI_COLOR_MODE_BASIC &&
                       a->fg == CTUI_COLOR_RED && a->bg == CTUI_COLOR_GREEN &&
                       a->attr == 0,
                   "SGR 31;42: basic red on green");
  CTUI_TEST_ASSERT(b->color_mode == CTUI_COLOR_MODE_256 && b->fg == 196 &&
                       b->bg == 21 &&
                       b->attr == (CTUI_ATTR_BOLD | CTUI_ATTR_UNDERLINE),
                   "38;5 / 48;5: 256 colours, bold and underlined");
  CTUI_TEST_ASSERT(c->color_mode == CTUI_COLOR_MODE_RGB_FG && c->fg_r == 1 &&
                       c->fg_g == 2 && c->fg_b == 3 &&
                       c->bg == CTUI_COLOR_BLUE &&
                       c->attr == CTUI_ATTR_UNDERLINE,
                   "38;2 over a basic bg: RGB_FG; 22 takes bold away");
  CTUI_TEST_ASSERT(d->color_mode == CTUI_COLOR_MODE_RGB && d->bg_r == 4 &&
                       d->bg_g == 5 && d->bg_b == 6 && d->fg_r == 1,
                   "both RGB: RGB");
  CTUI_TEST_ASSERT(ctui_vt_cell(vt, 0, 4)->ch == 0x4F60 &&
                       ctui_vt_cell(vt, 0, 5)->ch == CTUI_CELL_CONT,
                   "a wide glyph takes its cell and a CONT one");
  ctui_vt_feed(vt, "\x1b[1;6Hx", 7);
  CTUI_TEST_ASSERT(ctui_vt_cell(vt, 0, 4)->ch == ' ',
                   "writing over its right half blanks its left");
  ctui_vt_free(vt);

  vt = fed(2, 4, "abc\xe4\xbd\xa0");
  CTUI_TEST_ASSERT(text_is(vt, "abc\n\xe4\xbd\xa0"),
                   "a wide glyph at the last column goes to the next row");
  ctui_vt_resize(vt, 3, 1);
  CTUI_TEST_ASSERT(text_is(vt, "a\n\n"),
                   "a resize keeps what fits; a wide glyph cut in half "
                   "goes");
  ctui_vt_free(vt);
}

static void test_styles(void) {
  CTUI_VT *vt = fed(2, 8,
                    "\x1b[1;37;44mab\x1b[0m c\x1b[4m \x1b[38;2;1;2;3md"
                    "\x1b[0m\x1b[2;1H\x1b[1;31m \x1b[0m\x1b[38;5;9;48;5;0m"
                    "x\x1b[0m");
  char *st = ctui_vt_styles(vt);
  CTUI_TEST_ASSERT(st && strcmp(st, "0: 0 w/b+B 2 d/d 4 d/d+U 5 #010203/d+U 6 "
                                    "d/d\n1: 1 @9/@0 2 d/d\n") == 0,
                   "styles: a column where it changes; a blank's fg and bold "
                   "unseen, its underline kept; a plain row left out");
  if (st &&
      strcmp(
          st,
          "0: 0 w/b+B 2 d/d 4 d/d+U 5 #010203/d+U 6 d/d\n1: 1 @9/@0 2 d/d\n")) {
    printf("    styles: [%s]\n", st);
  }
  free(st);
  ctui_vt_free(vt);
  vt = ctui_vt_new(2, 3);
  st = ctui_vt_styles(vt);
  CTUI_TEST_ASSERT(st && *st == '\0', "all plain: nothing");
  free(st);
  ctui_vt_free(vt);
}

static void test_shm_probe(void) {
  const char *probe = "\x1b_Ga=q,i=31,s=1,v=1,f=32,t=s,q=0;L2N0dWk=\x1b\\";
  CTUI_VT *vt = ctui_vt_new(1, 10);
  size_t n;
  ctui_vt_feed(vt, probe, strlen(probe));
  CTUI_TEST_ASSERT(ctui_vt_replies(vt, &n) == NULL,
                   "no answer to the shm probe unless asked");
  ctui_vt_answer_kitty_shm(vt, 1);
  ctui_vt_feed(vt, probe, strlen(probe));
  const char *r = ctui_vt_replies(vt, &n);
  CTUI_TEST_ASSERT(r && n == 12 && memcmp(r, "\x1b_Gi=31;OK\x1b\\", 12) == 0,
                   "asked to, it answers as kitty does: i=31;OK");
  ctui_vt_feed(vt, "\x1b_Ga=T,i=2,t=s;x\x1b\\", 18);
  CTUI_TEST_ASSERT(ctui_vt_replies(vt, &n) == NULL,
                   "an image sent (a=T) isn't a probe");
  ctui_vt_free(vt);
}

/* ctui_screen_flush()'s bytes, read back, are the cells it drew from */
static size_t capture_flush(CTUI_SCREEN *s, char **out) {
  FILE *f = tmpfile();
  int saved = dup(STDOUT_FILENO);
  fflush(stdout);
  dup2(fileno(f), STDOUT_FILENO);
  ctui_screen_flush(s);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  long n = ftell(f);
  if (n <= 0) {
    n = lseek(fileno(f), 0, SEEK_CUR);
  }
  *out = malloc((size_t)n + 1);
  rewind(f);
  lseek(fileno(f), 0, SEEK_SET);
  size_t got = read(fileno(f), *out, (size_t)n) == n ? (size_t)n : 0;
  fclose(f);
  return got;
}

static int same_cell(const CTUI_CELL *a, const CTUI_CELL *b) {
  uint32_t ach = a->ch ? a->ch : ' ', bch = b->ch ? b->ch : ' ';
  if (ach != bch) {
    return 0;
  }
  if (ach == CTUI_CELL_CONT) {
    return 1;
  }
  if (a->attr != b->attr || a->color_mode != b->color_mode) {
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

static void paint(CTUI_SCREEN *s, unsigned seed) {
  static const char *words[] = {"plain",
                                "\xe4\xbd\xa0\xe5\xa5\xbd",
                                "e\xcc\x81t\xc3\xa9",
                                "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa!",
                                "\xe2\x94\x80\xe2\x94\x82",
                                "x"};
  srand(seed);
  for (int r = 0; r < s->rows; r++) {
    int c = rand() % 4;
    while (c < s->cols - 8) {
      ctui_screen_puts(s, r, c, words[rand() % 6], CTUI_COLOR_DEFAULT,
                       CTUI_COLOR_DEFAULT);
      c += 2 + rand() % 9;
    }
  }
  for (int i = 0; i < s->rows * s->cols; i++) {
    CTUI_CELL *cell = &s->cells[i];
    if (cell->ch == CTUI_CELL_CONT) {
      continue;
    }
    int mode = rand() % 4;
    cell->color_mode = (unsigned char)mode;
    cell->attr = (unsigned char)(rand() % 32);
    cell->fg = (unsigned char)(mode == CTUI_COLOR_MODE_256 ? rand() % 256
                                                           : rand() % 9);
    cell->bg = (unsigned char)(mode == CTUI_COLOR_MODE_256 ? rand() % 256
                                                           : rand() % 9);
    cell->fg_r = (unsigned char)rand(), cell->fg_g = (unsigned char)rand();
    cell->fg_b = (unsigned char)rand(), cell->bg_r = (unsigned char)rand();
    cell->bg_g = (unsigned char)rand(), cell->bg_b = (unsigned char)rand();
    if (ctui_cell_width(cell->ch) == 2) {
      uint32_t cont = cell[1].ch;
      cell[1] = *cell;
      cell[1].ch = cont;
    }
  }
}

static void test_roundtrip(void) {
  CTUI_SCREEN *s = ctui_screen_create(12, 50);
  CTUI_VT *vt = ctui_vt_new(12, 50);
  int frames_same = 1;
  for (unsigned frame = 0; frame < 6; frame++) {
    if (frame % 3 == 0) {
      ctui_screen_clear(s);
    }
    paint(s, 7 + frame);
    char *out;
    size_t n = capture_flush(s, &out);
    ctui_vt_feed(vt, out, n);
    free(out);
    for (int r = 0; r < s->rows; r++) {
      for (int c = 0; c < s->cols; c++) {
        if (!same_cell(ctui_vt_cell(vt, r, c), &s->cells[r * s->cols + c])) {
          if (frames_same) {
            printf("    frame %u differs at %d,%d\n", frame, r, c);
          }
          frames_same = 0;
        }
      }
    }
  }
  CTUI_TEST_ASSERT(frames_same,
                   "six frames of ctui_screen_flush() (wide glyphs, "
                   "clusters, every colour mode and attribute, whole and "
                   "diffed) read back as the cells drawn");
  ctui_vt_free(vt);
  ctui_screen_free(s);
}

static char *slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  rewind(f);
  char *buf = malloc((size_t)len + 1);
  *n = fread(buf, 1, (size_t)len, f);
  buf[*n] = '\0';
  fclose(f);
  return buf;
}

/* tests/data/harness/NAME.out: an app's whole output, NAME.txt the grid
 * pty_harness.py's feed() made of it (its first line: rows cols) */
static void test_captures(void) {
  static const char *names[] = {"gallery", "demo", "calculator"};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    char path[256];
    size_t nout, ntxt;
    snprintf(path, sizeof path, "tests/data/harness/%s.out", names[i]);
    char *out = slurp(path, &nout);
    snprintf(path, sizeof path, "tests/data/harness/%s.txt", names[i]);
    char *txt = slurp(path, &ntxt);
    int rows = 0, cols = 0;
    char *grid = txt ? strchr(txt, '\n') : NULL;
    if (out == NULL || grid == NULL ||
        sscanf(txt, "%d %d", &rows, &cols) != 2) {
      CTUI_TEST_ASSERT(0, "%s: capture missing", names[i]);
      free(out);
      free(txt);
      continue;
    }
    CTUI_VT *vt = ctui_vt_new(rows, cols);
    ctui_vt_feed(vt, out, nout);
    char *t = ctui_vt_text(vt);
    size_t want = strlen(grid + 1);
    if (want && grid[want] == '\n') {
      grid[want] = '\0';
    }
    CTUI_TEST_ASSERT(t && strcmp(t, grid + 1) == 0,
                     "%s: %zu bytes of output make pty_harness.py's grid",
                     names[i], nout);
    free(t);
    ctui_vt_free(vt);
    free(out);
    free(txt);
  }
}

static long ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static CTUI_PTY *sh(const char *script, int rows, int cols) {
  const char *argv[] = {"/bin/sh", "-c", script, NULL};
  return ctui_pty_spawn(
      &(CTUI_PTY_SPAWN){.argv = argv, .rows = rows, .cols = cols});
}

static void test_pty(void) {
  CTUI_PTY *p = sh("printf 'hello\\033[2;3Hworld'; sleep 10", 3, 20);
  CTUI_TEST_ASSERT(p && ctui_pty_until_text(p, "world", 3000) == 0,
                   "until_text sees what the app drew");
  CTUI_TEST_ASSERT(text_is(ctui_pty_vt(p), "hello\n  world\n"),
                   "the grid holds it where the app put it");
  long t0 = ms_now();
  CTUI_TEST_ASSERT(ctui_pty_until_text(p, "nowhere", 200) == -1 &&
                       ms_now() - t0 >= 190 && ms_now() - t0 < 1000,
                   "until_text gives up after its timeout");
  CTUI_TEST_ASSERT(ctui_pty_until_quiet(p, 100, 2000) == 0,
                   "until_quiet: nothing more came for 100 ms");
  CTUI_TEST_ASSERT(ctui_pty_close(p, NULL) == 0,
                   "close ends a running app with SIGTERM: 0");

  const char *env[] = {"CTUI_PTY_TEST=set", NULL};
  const char *unset[] = {"HOME", NULL};
  const char *argv[] = {"/bin/sh", "-c",
                        "echo \"[$CTUI_PTY_TEST][${HOME-none}]\"; pwd; "
                        "stty size; read x; stty size; sleep 10",
                        NULL};
  p = ctui_pty_spawn(&(CTUI_PTY_SPAWN){.argv = argv,
                                       .cwd = "/",
                                       .env = env,
                                       .unset = unset,
                                       .rows = 10,
                                       .cols = 40});
  CTUI_TEST_ASSERT(p && ctui_pty_until_text(p, "10 40", 3000) == 0 &&
                       ctui_vt_find(ctui_pty_vt(p), "[set][none]") == 0 &&
                       ctui_vt_find(ctui_pty_vt(p), "/") == 1,
                   "the child's environment, cwd and pty size are the "
                   "spawn's");
  ctui_pty_resize(p, 12, 50);
  ctui_pty_send(p, "\n", 1);
  CTUI_TEST_ASSERT(ctui_pty_until_text(p, "12 50", 3000) == 0,
                   "a resize reaches the pty");
  ctui_pty_close(p, NULL);

  char buf[256] = "";
  FILE *f = fmemopen(buf, sizeof buf, "w");
  p = sh("exit 3", 2, 10);
  ctui_pty_wait(p, 300);
  CTUI_TEST_ASSERT(ctui_pty_gone(p), "an app that ended is gone");
  CTUI_TEST_ASSERT(ctui_pty_close(p, f) == 1, "close: it ended by itself");
  p = sh("kill -SEGV $$", 2, 10);
  ctui_pty_wait(p, 300);
  ctui_pty_close(p, f);
  p = sh("trap 'exit 0' TERM; echo up; while :; do sleep 0.05; done", 2, 10);
  ctui_pty_until_text(p, "up", 3000);
  CTUI_TEST_ASSERT(ctui_pty_close(p, f) == 0,
                   "an app that catches SIGTERM and exits 0 ended by ours");
  p = sh("trap 'exit 4' TERM; echo up; while :; do sleep 0.05; done", 2, 10);
  ctui_pty_until_text(p, "up", 3000);
  CTUI_TEST_ASSERT(ctui_pty_close(p, f) == 1,
                   "one that exits 4 on it ended otherwise");
  fclose(f);
  CTUI_TEST_ASSERT(strcmp(buf, "-- exited 3 --\n-- killed by signal 11 --\n"
                               "-- exited 4 --\n") == 0,
                   "how it ended goes to the output, a crash too");

  p = sh("printf 'ready'; read x; printf '\\033[2Jgot [%s]' \"$x\"; sleep 10",
         2, 20);
  static const CTUI_HARNESS_STEP ok[] = {CTUI_HS_UNTIL("ready"),
                                         CTUI_HS_KEYS("a\0b\n"),
                                         CTUI_HS_UNTIL("got"),
                                         CTUI_HS_DUMP,
                                         CTUI_HS_UNTIL_MS("never", 100),
                                         CTUI_HS_DUMP,
                                         CTUI_HS_END};
  char dump[512] = "";
  f = fmemopen(dump, sizeof dump, "w");
  int r = ctui_harness_steps(p, ok, f, NULL);
  fclose(f);
  CTUI_TEST_ASSERT(r == -1, "a failed wait fails the run");
  CTUI_TEST_ASSERT(strcmp(dump, "\ngot [ab]\n--------------------\n"
                                "!! until \"never\": not within 100 ms\n"
                                "\ngot [ab]\n--------------------\n") == 0,
                   "steps: keys sent (a NUL among them), a dump, then the "
                   "failed wait's message and screen, and nothing after");
  ctui_pty_close(p, NULL);
}

int main(void) {
  ctui_log_init(E_ERR);
  test_text();
  test_swallowed();
  test_split();
  test_cells();
  test_styles();
  test_shm_probe();
  test_roundtrip();
  test_captures();
  test_pty();
  return ctui_test_summary();
}
