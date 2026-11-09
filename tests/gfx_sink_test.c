/* the graphics sink (gfx.h): a kitty widget's image calls arrive at a
 * sink as calls, in order, after the screen sink's frame, with nothing
 * written to stdout -- no ctui_init(), the tier set by
 * ctui_gfx_set_mode(), as a window host would */
#define _POSIX_C_SOURCE 200809L

#include "ctui.h"
#include "ctui_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char g_rec[512];
static unsigned char g_px[2 * 2 * 4];

static void rec(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rec(const char *fmt, ...) {
  size_t n = strlen(g_rec);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_rec + n, sizeof g_rec - n, fmt, ap);
  va_end(ap);
}

static void s_display(void *ctx, int row, int col, int cell_cols,
                      int cell_rows, const unsigned char *rgba, int width,
                      int height, unsigned int image_id, int z) {
  (void)ctx;
  rec("display(%u %d,%d %dx%d %dx%dpx z%d px%d) ", image_id, row, col,
      cell_cols, cell_rows, width, height, z, rgba[0]);
}
static unsigned char *s_begin(void *ctx, unsigned int image_id, int width,
                              int height, int keep) {
  (void)ctx;
  rec("begin(%u %dx%d keep%d) ", image_id, width, height, keep);
  return g_px;
}
static void s_commit(void *ctx, unsigned int image_id, int row, int col,
                     int cell_cols, int cell_rows, int z) {
  (void)ctx;
  rec("commit(%u %d,%d %dx%d z%d px%d) ", image_id, row, col, cell_cols,
      cell_rows, z, g_px[0]);
}
static void s_free(void *ctx, unsigned int image_id) {
  (void)ctx;
  rec("free(%u) ", image_id);
}
static void s_delete(void *ctx, unsigned int image_id) {
  (void)ctx;
  rec("delete(%u) ", image_id);
}
static void s_place(void *ctx, unsigned int image_id, const char *path,
                    int cols, int rows) {
  (void)ctx;
  rec("place(%u %s %dx%d) ", image_id, path, cols, rows);
}
static void s_put(void *ctx, unsigned int image_id, unsigned int placement_id,
                  int row, int col, int cols, int rows, int z) {
  (void)ctx;
  rec("put(%u p%u %d,%d %dx%d z%d) ", image_id, placement_id, row, col, cols,
      rows, z);
}
static void s_unput(void *ctx, unsigned int image_id,
                    unsigned int placement_id) {
  (void)ctx;
  rec("unput(%u p%u) ", image_id, placement_id);
}
static void s_flush(void *ctx) {
  (void)ctx;
  rec("flush ");
}

static const CTUI_GFX_SINK k_sink = {s_display, s_begin, s_commit,
                                     s_free,    s_delete, s_place,
                                     s_put,     s_unput,  s_flush};

static int s_frame(void *ctx, const CTUI_SCREEN *s) {
  (void)ctx;
  rec("frame(%c) ", (char)s->cells[0].ch);
  return 0;
}
static const CTUI_SCREEN_SINK k_screen_sink = {s_frame};

static CTUI_GFX_KITTY_IMAGE *g_img;

static void text_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_widget_putc(self, comp, 0, 0, 't', CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
}

static void pics_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)comp;
  static const unsigned char rgba[4] = {7, 0, 0, 255};
  ctui_gfx_kitty_display(self->y + 1, self->x + 1, 2, 1, rgba, 1, 1, 5, -1);
  unsigned char *px = ctui_gfx_kitty_image_begin(g_img, 2, 2, 1);
  if (px) {
    px[0] = 9;
    ctui_gfx_kitty_image_commit(g_img, 2, 1, 2, 1, 0);
  }
  ctui_gfx_kitty_place_file(7, "/icons/a.png", 2, 1);
  ctui_gfx_kitty_put(7, 1, 1, 1, 2, 1, -2);
  ctui_gfx_kitty_unput(7, 1);
  ctui_gfx_kitty_delete(5);
}

/* one run_frame()'s trio, stdout caught: what reached it */
static size_t frame(CTUI_APP *app, CTUI_SCREEN *screen) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return (size_t)-1;
  }
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  dup2(pipefd[1], STDOUT_FILENO);
  close(pipefd[1]);
  ctui_app_render(app, screen);
  ctui_screen_flush(screen);
  ctui_widget_flush_gfx(app->comp);
  fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  char buf[256];
  ssize_t n = read(pipefd[0], buf, sizeof buf);
  close(pipefd[0]);
  return n < 0 ? 0 : (size_t)n;
}

int main(void) {
  ctui_log_init(E_ALL);
  ctui_gfx_set_sink(&k_sink, NULL);
  ctui_gfx_set_mode(CTUI_GFX_KITTY);
  g_img = ctui_gfx_kitty_image_new(6);

  CTUI_WIDGET text = ctui_widget_make(0, 0, 4, 1, NULL, text_render, NULL);
  CTUI_WIDGET pics = ctui_widget_make(0, 1, 4, 1, NULL, text_render, NULL);
  ctui_widget_set_gfx_renderer(&pics, CTUI_GFX_KITTY, pics_render);
  CTUI_WIDGET *ws[] = {&text, &pics};
  CTUI_SCREEN *screen = ctui_screen_create(2, 4);
  ctui_screen_set_sink(screen, &k_screen_sink, NULL);
  CTUI_APP app;
  ctui_app_init(&app, ws, 2, 2, 4);

  size_t out = frame(&app, screen);
  CTUI_TEST_ASSERT(!strcmp(g_rec, "frame(t) display(5 2,1 2x1 1x1px z-1 px7) "
                                  "begin(6 2x2 keep1) commit(6 2,1 2x1 z0 "
                                  "px9) place(7 /icons/a.png 2x1) put(7 p1 1,1 2x1 z-2) "
                                  "unput(7 p1) delete(5) "
                                  "flush "),
                   "the frame's text first, then the kitty widget's calls "
                   "at the sink in order, flush last (%s)",
                   g_rec);
  CTUI_TEST_ASSERT(out == 0, "nothing reached stdout (%zu bytes)", out);

  g_rec[0] = '\0';
  frame(&app, screen);
  CTUI_TEST_ASSERT(!strncmp(g_rec, "display(", 8) &&
                       strstr(g_rec, "flush ") != NULL,
                   "a frame whose cells didn't change has no frame(), but "
                   "its graphics and their flush still come (%s)",
                   g_rec);

  g_rec[0] = '\0';
  ctui_gfx_kitty_image_free(g_img);
  CTUI_TEST_ASSERT(!strcmp(g_rec, "free(6) "), "image_free forwards (%s)",
                   g_rec);

  ctui_gfx_set_sink(NULL, NULL);
  g_rec[0] = '\0';
  out = frame(&app, screen);
  CTUI_TEST_ASSERT(g_rec[0] == '\0' || !strncmp(g_rec, "frame(", 6),
                   "with the sink cleared no image call reaches it (%s)",
                   g_rec);
  CTUI_TEST_ASSERT(out == 0,
                   "and, stdout not being a terminal, the kitty path rejects "
                   "them as before (%zu bytes)",
                   out);

  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_widget_gfx_reset();
  return ctui_test_summary();
}
