/* Exercises core/widget.c directly: the shared bounds-checking in
 * widget_cell_at() (unbound widget, out-of-widget-bounds, out-of-compositor-
 * bounds all reject-and-log rather than write), the three color-mode putc
 * variants tagging a cell correctly, and the gfx dispatch queue
 * (ctui_widget_dispatch_render()/ctui_widget_flush_gfx()) that lets a
 * Phase-4 widget's gfx_render fire only when its mode matches the
 * negotiated one and only after being explicitly flushed. */
#include "ctui.h"

#include "ctui_test.h"

#include <stdlib.h>
#include <string.h>

/* ctui_g_gfx_mode is intentionally private -- see kitty_protocol_test.c's
 * identical extern for why redeclaring it here is the accepted gray-box
 * trick for a headless test that never calls ctui_init(). */
extern unsigned int ctui_g_gfx_mode;

static void noop_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  (void)comp;
}

static int gfx_fire_count = 0;
static void count_gfx_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  (void)comp;
  gfx_fire_count++;
}

static void test_putc_bounds(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(10, 10);
  CTUI_WIDGET unbound = ctui_widget_make(2, 2, 3, 3, NULL, noop_render, NULL);

  ctui_widget_putc(&unbound, comp, 0, 0, 'X', CTUI_COLOR_RED,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[2 * 10 + 2].ch == ' ',
                   "putc on a widget never bound via ctui_widget_init() "
                   "is rejected (comp cell stays untouched)");

  ctui_widget_init(&unbound, comp);
  CTUI_WIDGET *w = &unbound;

  ctui_widget_putc(w, comp, 5, 5, 'X', CTUI_COLOR_RED, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[2 * 10 + 2].ch == ' ',
                   "putc rejects a (row,col) outside the widget's own w/h "
                   "even though it would land inside the compositor");

  ctui_widget_putc(w, comp, 1, 1, 'X', CTUI_COLOR_RED, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[3 * 10 + 3].ch == 'X',
                   "putc within bounds writes to widget-origin-relative "
                   "(row,col) -> absolute compositor cell");

  CTUI_WIDGET edge = ctui_widget_make(8, 8, 5, 5, NULL, noop_render, NULL);
  ctui_widget_init(&edge, comp);
  /* edge's origin (8,8) is inside the 10x10 compositor, but its declared
   * 5x5 size reaches past it -- (row=3,col=3) is within the widget's own
   * w/h yet lands at absolute (11,11), outside the compositor */
  ctui_widget_putc(&edge, comp, 3, 3, 'Y', CTUI_COLOR_RED, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[9 * 10 + 9].ch != 'Y',
                   "putc rejects a write that's within the widget's "
                   "declared w/h but would fall outside the compositor's "
                   "actual bounds");

  ctui_compositor_free(comp);
}

static void test_puts_cut(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(4, 10);
  CTUI_WIDGET w = ctui_widget_make(0, 0, 10, 4, NULL, noop_render, NULL);
  ctui_widget_init(&w, comp);

  int used = ctui_widget_puts_cut(&w, comp, 0, 0, "abc", 5, CTUI_COLOR_RED,
                                  CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(used == 3 && comp->cells[2].ch == 'c' &&
                       comp->cells[3].ch == ' ',
                   "puts_cut() writes a string that fits as-is");

  used = ctui_widget_puts_cut(&w, comp, 1, 2, "abcdef", 4, CTUI_COLOR_RED,
                              CTUI_COLOR_DEFAULT);
  CTUI_CELL *r1 = &comp->cells[10];
  CTUI_TEST_ASSERT(used == 4 && r1[2].ch == 'a' && r1[4].ch == 'c' &&
                       r1[5].ch == 0x2026 && r1[6].ch == ' ' &&
                       r1[5].fg == CTUI_COLOR_RED,
                   "puts_cut() ends a string that doesn't fit in one "
                   "\"\u2026\" glyph in the last column");

  used = ctui_widget_puts_cut(&w, comp, 2, 0,
                              "a\xe4\xb8\xad"
                              "b",
                              3, CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  CTUI_CELL *r2 = &comp->cells[20];
  CTUI_TEST_ASSERT(used == 2 && r2[0].ch == 'a' && r2[1].ch == 0x2026,
                   "puts_cut() never splits a wide glyph to make room for "
                   "the ellipsis");

  used = ctui_widget_puts_cut(&w, comp, 2, 4, "ab  cdef", 5, CTUI_COLOR_DEFAULT,
                              CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(used == 3 && r2[5].ch == 'b' && r2[6].ch == 0x2026,
                   "puts_cut() drops the spaces before the ellipsis");

  used = ctui_widget_puts_cut(&w, comp, 3, 0, "abcdef", 6, CTUI_COLOR_DEFAULT,
                              CTUI_COLOR_DEFAULT);
  int zero = ctui_widget_puts_cut(&w, comp, 3, 0, "xyz", 0, CTUI_COLOR_DEFAULT,
                                  CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(used == 6 && zero == 0 && comp->cells[30].ch == 'a',
                   "puts_cut() fits an exact-width string whole and writes "
                   "nothing for width 0");

  ctui_compositor_free(comp);
}

static void test_puts_cut_cell(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(1, 10);
  CTUI_WIDGET w = ctui_widget_make(0, 0, 10, 1, NULL, noop_render, NULL);
  ctui_widget_init(&w, comp);
  CTUI_CELL pen = {.color_mode = CTUI_COLOR_MODE_RGB_FG,
                   .fg_r = 9,
                   .bg = CTUI_COLOR_DEFAULT,
                   .attr = CTUI_ATTR_BOLD};
  int used = ctui_widget_puts_cut_cell(&w, comp, 0, 0, "abcdef", 4, &pen);
  CTUI_TEST_ASSERT(used == 4 && comp->cells[3].ch == 0x2026 &&
                       comp->cells[0].color_mode == CTUI_COLOR_MODE_RGB_FG &&
                       comp->cells[0].fg_r == 9 && comp->cells[3].fg_r == 9 &&
                       comp->cells[3].attr == CTUI_ATTR_BOLD,
                   "puts_cut_cell() cuts as puts_cut() does, every cell (the "
                   "ellipsis too) in the pen's colours and attributes");
  ctui_compositor_free(comp);
}

static void test_puts_and_color_modes(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(5, 20);
  CTUI_WIDGET w = ctui_widget_make(0, 0, 20, 5, NULL, noop_render, NULL);
  ctui_widget_init(&w, comp);

  ctui_widget_puts(&w, comp, 0, 0, "hi", CTUI_COLOR_GREEN, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[0].ch == 'h' && comp->cells[1].ch == 'i',
                   "puts() writes each character in sequence");
  CTUI_TEST_ASSERT(comp->cells[0].color_mode == CTUI_COLOR_MODE_BASIC &&
                       comp->cells[0].fg == CTUI_COLOR_GREEN,
                   "plain putc/puts tags cells CTUI_COLOR_MODE_BASIC");

  ctui_widget_putc_256(&w, comp, 1, 0, 'z', 200, 16);
  CTUI_TEST_ASSERT(comp->cells[1 * 20].color_mode == CTUI_COLOR_MODE_256 &&
                       comp->cells[1 * 20].fg == 200 &&
                       comp->cells[1 * 20].bg == 16,
                   "putc_256() tags the cell CTUI_COLOR_MODE_256 with the "
                   "given 256-color indices");

  ctui_widget_putc_rgb(&w, comp, 2, 0, 'r', 10, 20, 30, 40, 50, 60);
  CTUI_CELL *rgb_cell = &comp->cells[2 * 20];
  CTUI_TEST_ASSERT(rgb_cell->color_mode == CTUI_COLOR_MODE_RGB &&
                       rgb_cell->fg_r == 10 && rgb_cell->fg_g == 20 &&
                       rgb_cell->fg_b == 30 && rgb_cell->bg_r == 40 &&
                       rgb_cell->bg_g == 50 && rgb_cell->bg_b == 60,
                   "putc_rgb() tags the cell CTUI_COLOR_MODE_RGB with the "
                   "given 24-bit channels");

  ctui_widget_puts_256(&w, comp, 3, 0, "hi", 201, 17);
  CTUI_TEST_ASSERT(comp->cells[3 * 20].ch == 'h' &&
                       comp->cells[3 * 20 + 1].ch == 'i' &&
                       comp->cells[3 * 20].color_mode == CTUI_COLOR_MODE_256 &&
                       comp->cells[3 * 20].fg == 201,
                   "puts_256() writes each character via putc_256() in "
                   "sequence");

  ctui_widget_puts_rgb(&w, comp, 4, 0, "hi", 1, 2, 3, 4, 5, 6);
  CTUI_TEST_ASSERT(comp->cells[4 * 20].ch == 'h' &&
                       comp->cells[4 * 20 + 1].ch == 'i' &&
                       comp->cells[4 * 20].color_mode == CTUI_COLOR_MODE_RGB &&
                       comp->cells[4 * 20].fg_r == 1,
                   "puts_rgb() writes each character via putc_rgb() in "
                   "sequence");

  ctui_widget_putc_rgb_fg(&w, comp, 0, 5, 'f', 7, 8, 9, CTUI_COLOR_BLUE);
  CTUI_CELL *fg_cell = &comp->cells[5];
  CTUI_TEST_ASSERT(fg_cell->color_mode == CTUI_COLOR_MODE_RGB_FG &&
                       fg_cell->fg_r == 7 && fg_cell->fg_g == 8 &&
                       fg_cell->fg_b == 9 && fg_cell->bg == CTUI_COLOR_BLUE,
                   "putc_rgb_fg() tags the cell CTUI_COLOR_MODE_RGB_FG: "
                   "24-bit fg, basic bg");

  ctui_widget_put_kitty_placeholder(&w, comp, 1, 10, 0x123456, 2, 1,
                                    CTUI_COLOR_DEFAULT);
  CTUI_CELL *p0 = &comp->cells[1 * 20 + 10], *p1 = p0 + 1;
  CTUI_TEST_ASSERT(p0->ch == CTUI_GFX_KITTY_PLACEHOLDER &&
                       p1->ch == CTUI_GFX_KITTY_PLACEHOLDER &&
                       p0->color_mode == CTUI_COLOR_MODE_RGB_FG &&
                       p0->fg_r == 0x12 && p0->fg_g == 0x34 &&
                       p0->fg_b == 0x56 && p1->fg_b == 0x56 &&
                       p0[2].ch != CTUI_GFX_KITTY_PLACEHOLDER,
                   "put_kitty_placeholder() writes cols U+10EEEE cells, the "
                   "image id as their fg");
  CTUI_TEST_ASSERT(p0->kitty_row == 0 && p1->kitty_row == 0,
                   "... a one-row image carries no row diacritic");
  CTUI_TEST_ASSERT(p0->kitty_col == 1 && p1->kitty_col == 0,
                   "... its first cell names its column (0), the next follows "
                   "on from it");

  ctui_widget_put_kitty_placeholder(&w, comp, 2, 0, 9, 3, 2,
                                    CTUI_COLOR_DEFAULT);
  CTUI_CELL *r0 = &comp->cells[2 * 20], *r1 = &comp->cells[3 * 20];
  CTUI_TEST_ASSERT(r0[0].ch == CTUI_GFX_KITTY_PLACEHOLDER &&
                       r0[2].ch == CTUI_GFX_KITTY_PLACEHOLDER &&
                       r1[2].ch == CTUI_GFX_KITTY_PLACEHOLDER &&
                       r1[3].ch != CTUI_GFX_KITTY_PLACEHOLDER &&
                       r0[0].kitty_row == 1 && r0[2].kitty_row == 1 &&
                       r1[0].kitty_row == 2 && r1[2].kitty_row == 2 &&
                       r1[1].fg_b == 9,
                   "a cols x rows block tags each cell with its image row + 1");
  CTUI_TEST_ASSERT(r0[0].kitty_col == 1 && r1[0].kitty_col == 1 &&
                       r0[1].kitty_col == 0 && r1[2].kitty_col == 0,
                   "... and each row's first cell with its column");

  ctui_compositor_clear(comp);
  CTUI_TEST_ASSERT(r1[0].kitty_row == 0 && r1[0].ch == ' ',
                   "compositor_clear() drops kitty_row with the placeholder");

  ctui_compositor_free(comp);
}

static void test_widget_init_out_of_bounds(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(5, 5);

  CTUI_WIDGET off_screen =
      ctui_widget_make(100, 100, 2, 2, NULL, noop_render, NULL);
  ctui_widget_init(&off_screen, comp);
  CTUI_TEST_ASSERT(off_screen.buf == NULL,
                   "widget_init leaves buf NULL when the widget's origin "
                   "falls outside the compositor, instead of binding a "
                   "wild pointer");

  ctui_widget_putc(&off_screen, comp, 0, 0, 'X', CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[0].ch == ' ',
                   "a putc against a widget that failed to bind is "
                   "rejected the same way as one that was simply never "
                   "initialized");

  ctui_compositor_free(comp);
}

static void test_gfx_dispatch(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(5, 5);
  gfx_fire_count = 0;

  CTUI_WIDGET text_w = ctui_widget_make(0, 0, 1, 1, NULL, noop_render, NULL);
  CTUI_WIDGET gfx_w = ctui_widget_make(1, 1, 1, 1, NULL, noop_render, NULL);
  ctui_widget_set_gfx_renderer(&gfx_w, CTUI_GFX_KITTY, count_gfx_render);
  ctui_widget_init(&text_w, comp);
  ctui_widget_init(&gfx_w, comp);

  ctui_g_gfx_mode = 0;
  ctui_widget_dispatch_render(&text_w, comp);
  ctui_widget_dispatch_render(&gfx_w, comp);
  CTUI_TEST_ASSERT(gfx_fire_count == 0,
                   "dispatch_render() defers to plain render() (not "
                   "gfx_render) when gfx_render_mode doesn't match the "
                   "negotiated ctui_g_gfx_mode");

  ctui_widget_flush_gfx(comp);
  CTUI_TEST_ASSERT(gfx_fire_count == 0,
                   "flush_gfx() fires nothing when nothing was queued this "
                   "frame");

  ctui_g_gfx_mode = CTUI_GFX_KITTY;
  ctui_widget_dispatch_render(&gfx_w, comp);
  CTUI_TEST_ASSERT(gfx_fire_count == 0,
                   "dispatch_render() queues a matching gfx widget instead "
                   "of calling gfx_render immediately");

  ctui_widget_flush_gfx(comp);
  CTUI_TEST_ASSERT(gfx_fire_count == 1,
                   "flush_gfx() fires gfx_render exactly once for a widget "
                   "queued this frame");

  ctui_widget_flush_gfx(comp);
  CTUI_TEST_ASSERT(gfx_fire_count == 1,
                   "flush_gfx() clears its queue after draining, so a "
                   "second call without a new dispatch fires nothing more");

  ctui_widget_gfx_reset();
  ctui_g_gfx_mode = 0;
  ctui_compositor_free(comp);
}

static void test_puts_n(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(2, 10);
  CTUI_WIDGET w = ctui_widget_make(0, 0, 10, 2, NULL, noop_render, NULL);
  ctui_widget_init(&w, comp);
  int n = ctui_widget_puts_n(&w, comp, 0, 0, "h\xc3\xa9llo world", 6,
                             CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(n == 5 && comp->cells[1].ch == 0xe9 &&
                       comp->cells[4].ch == 'o' && comp->cells[5].ch == ' ',
                   "puts_n() draws the glyphs of its first n bytes (%d)", n);
  n = ctui_widget_puts_n(&w, comp, 1, 0, "ab", 9, CTUI_COLOR_DEFAULT,
                         CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(n == 2, "puts_n() stops at a NUL before n");
  ctui_compositor_free(comp);
}

static void test_puts_cell(void) {
  CTUI_COMPOSITOR *comp = ctui_compositor_create(2, 10);
  CTUI_WIDGET w = ctui_widget_make(0, 0, 10, 2, NULL, noop_render, NULL);
  ctui_widget_init(&w, comp);
  CTUI_CELL bold = {.fg = CTUI_COLOR_RED, .attr = CTUI_ATTR_BOLD};
  int n =
      ctui_widget_puts_cell(&w, comp, 0, 0, "h\xc3\xa9llo", (size_t)-1, &bold);
  CTUI_TEST_ASSERT(n == 5 && comp->cells[1].ch == 0xe9 &&
                       comp->cells[4].attr == CTUI_ATTR_BOLD &&
                       comp->cells[4].fg == CTUI_COLOR_RED,
                   "puts_cell() draws the whole string with the cell's "
                   "colour and attributes (%d)",
                   n);
  CTUI_CELL rgb = {.color_mode = CTUI_COLOR_MODE_RGB,
                   .fg_r = 9,
                   .attr = CTUI_ATTR_ITALIC | CTUI_ATTR_UNDERLINE};
  n = ctui_widget_puts_cell(&w, comp, 0, 1, "abc", 2, &rgb);
  CTUI_TEST_ASSERT(n == 2 && comp->cells[2].ch == 'b' &&
                       comp->cells[2].color_mode == CTUI_COLOR_MODE_RGB &&
                       comp->cells[2].fg_r == 9 &&
                       comp->cells[2].attr ==
                           (CTUI_ATTR_ITALIC | CTUI_ATTR_UNDERLINE) &&
                       comp->cells[3].ch == 'l',
                   "puts_cell() takes n bytes, any colour mode");
  n = ctui_widget_putc_cell(&w, comp, 1, 0, 0x4e2d, &bold);
  CTUI_TEST_ASSERT(n == 2 && comp->cells[11].ch == CTUI_CELL_CONT &&
                       comp->cells[11].attr == CTUI_ATTR_BOLD,
                   "putc_cell() styles both halves of a wide glyph");
  ctui_widget_puts(&w, comp, 0, 0, "x", CTUI_COLOR_DEFAULT, CTUI_COLOR_DEFAULT);
  CTUI_TEST_ASSERT(comp->cells[0].attr == 0,
                   "plain puts() over a bold cell leaves it plain");
  ctui_compositor_clear(comp);
  CTUI_TEST_ASSERT(comp->cells[4].attr == 0,
                   "compositor_clear drops attributes");
  ctui_compositor_free(comp);
}

/* CTUI_APP's render hooks: begin, each widget drawn (a nested one before
 * the one drawing it), end -- in that order, every frame */
static char g_seen[32];
static int g_nseen;
static CTUI_WIDGET g_inner;

static void outer_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  ctui_widget_dispatch_render(&g_inner, comp);
}

static void plain_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self, (void)comp;
}

static void seen(char c) {
  if (g_nseen < (int)sizeof g_seen - 1) {
    g_seen[g_nseen++] = c;
  }
}

static void hook_begin(void *arg) { seen(*(char *)arg); }

static void hook_rendered(CTUI_WIDGET *w, CTUI_COMPOSITOR *comp, void *arg) {
  (void)comp, (void)arg;
  seen(*(char *)w->widget_data);
}

static void hook_end(CTUI_COMPOSITOR *comp, void *arg) {
  (void)arg;
  seen(comp ? 'e' : '?');
}

static void test_render_hooks(void) {
  char a = 'a', o = 'o', i = 'i', b = 'b';
  CTUI_WIDGET wa = ctui_widget_make(0, 0, 2, 1, &a, plain_render, NULL);
  CTUI_WIDGET wo = ctui_widget_make(0, 1, 2, 1, &o, outer_render, NULL);
  g_inner = ctui_widget_make(0, 1, 2, 1, &i, plain_render, NULL);
  CTUI_WIDGET *ws[] = {&wa, &wo};
  CTUI_SCREEN *screen = ctui_screen_create(3, 4);
  CTUI_APP app;
  ctui_app_init(&app, ws, 2, 3, 4);
  ctui_widget_init(&g_inner, app.comp);
  ctui_app_render(&app, screen);
  CTUI_TEST_ASSERT(g_nseen == 0, "no hooks set: none called");
  app.render_begin = hook_begin;
  app.rendered = hook_rendered;
  app.render_end = hook_end;
  app.render_arg = &b;
  ctui_app_render(&app, screen);
  ctui_app_render(&app, screen);
  g_seen[g_nseen] = 0;
  CTUI_TEST_ASSERT(!strcmp(g_seen, "baioebaioe"),
                   "begin, each widget (the nested one first), end -- each "
                   "frame (%s)",
                   g_seen);
  ctui_app_free(&app);
  ctui_screen_free(screen);
}

/* the reference for a single put: widget.c's general path as it was
 * before the narrow fast path (session 220), from public pieces -- the
 * bounds of widget_cell_at(), then ctui_cell_set_ch() and the style laid
 * over every cell it wrote */
static int ref_putc(CTUI_WIDGET *w, CTUI_COMPOSITOR *comp, int row, int col,
                    uint32_t ch, const CTUI_CELL *style) {
  int abs_row = w->y + row;
  int abs_col = w->x + col;
  if (w->buf == NULL || row < 0 || row >= w->h || col < 0 || col >= w->w ||
      abs_row < 0 || abs_row >= comp->rows || abs_col < 0 ||
      abs_col >= comp->cols) {
    return ctui_cell_width(ch);
  }
  CTUI_CELL *cell = w->buf + (size_t)row * (size_t)comp->cols + (size_t)col;
  int n =
      ctui_cell_set_ch(cell - abs_col, comp->cols, abs_col, w->x + w->w, ch);
  for (int i = 0; i < n; i++) {
    uint32_t keep = cell[i].ch;
    cell[i] = *style;
    cell[i].ch = keep;
  }
  return n;
}

/* the puts family against the reference: the same string put cluster by
 * cluster through ref_putc(), which the ASCII run fast path must match
 * byte for byte -- on rows full of wide glyphs it cuts, at edges and
 * outside the widget and the compositor, for a group member addressing a
 * slice bound elsewhere */
static int ref_puts(CTUI_WIDGET *w, CTUI_COMPOSITOR *comp, int row, int col,
                    const char *str, const char *end, const CTUI_CELL *style) {
  while ((end == NULL || str < end) && *str) {
    uint32_t ch;
    str += ctui_utf8_cluster(str, end ? (size_t)(end - str) : (size_t)-1, &ch,
                             NULL);
    col += ref_putc(w, comp, row, col, ch, style);
  }
  return col;
}

/* random rows of letters and wide glyphs, the same in both */
static void wide_rows(CTUI_COMPOSITOR *a, CTUI_COMPOSITOR *b) {
  for (int y = 0; y < a->rows; y++) {
    CTUI_CELL *line = a->cells + y * a->cols;
    for (int x = 0; x < a->cols; x++) {
      line[x] = (CTUI_CELL){.ch = (uint32_t)('A' + rand() % 26),
                            .fg = (unsigned char)(rand() % 8)};
    }
    for (int x = 0; x < a->cols - 1; x++) {
      if (rand() % 3 == 0) {
        x += ctui_cell_set_ch(line, a->cols, x, a->cols, 0x4F60) - 1;
      }
    }
  }
  memcpy(b->cells, a->cells, sizeof(CTUI_CELL) * a->rows * a->cols);
}

/* the narrow ranges widget_put() knows without the width table are
 * narrow by the table: every codepoint in them */
static void test_known_narrow(void) {
  static const uint32_t ranges[][2] = {
      {0x20, 0x7E}, {0x2500, 0x259F}, {0x2800, 0x28FF}};
  int bad = 0;
  for (size_t r = 0; r < sizeof ranges / sizeof *ranges; r++) {
    for (uint32_t cp = ranges[r][0]; cp <= ranges[r][1]; cp++) {
      bad += ctui_cell_width(cp) != 1;
    }
  }
  CTUI_TEST_ASSERT(bad == 0,
                   "ASCII, box drawing, blocks and braille are all one "
                   "column by the table (%d are not)",
                   bad);
}

/* single puts against the reference: every putc variant, narrow glyphs
 * (the fast path) and the rest -- wide, zero-width, controls, a CONT, a
 * cluster id -- onto rows of wide glyphs, at the edges and outside the
 * widget and the compositor, for a group member too; then with putcs
 * logged (E_DBG), which goes the general way */
static void test_putc_vs_ref(void) {
  ctui_log_shutdown();
  ctui_log_init(E_ERR);
  uint32_t narrow_cluster, wide_cluster;
  ctui_utf8_cluster("e\xcc\x81", (size_t)-1, &narrow_cluster, NULL);
  ctui_utf8_cluster("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", (size_t)-1,
                    &wide_cluster, NULL);
  const uint32_t chars[] = {
      'a',         ' ',    '~',    0x2500, 0x2588,         0x259F,
      0x2800,      0x28FF, 0xE9,   0x2603, 0x4F60,         0x1F44D,
      0x301,       0x200B, '\n',   0x7F,   0x85,           0xFE0F,
      0xFFFD,      0x25A0, 0x25FD, 0x2614, CTUI_CELL_CONT, narrow_cluster,
      wide_cluster};
  enum { ROWS = 5, COLS = 16, RUNS = 40000 };
  CTUI_COMPOSITOR *a = ctui_compositor_create(ROWS, COLS);
  CTUI_COMPOSITOR *b = ctui_compositor_create(ROWS, COLS);
  srand(220);
  int bad = 0;
  for (int r = 0; r < RUNS; r++) {
    if (r == RUNS - 2000) {
      ctui_log_shutdown();
      ctui_log_init(E_ALL);
    }
    wide_rows(a, b);
    CTUI_WIDGET wa = {.render = noop_render,
                      .x = rand() % 6,
                      .y = rand() % 3,
                      .w = 1 + rand() % 14,
                      .h = 1 + rand() % 4};
    CTUI_WIDGET wb = wa;
    ctui_widget_init(&wa, a);
    ctui_widget_init(&wb, b);
    if (rand() % 4 == 0 && wa.buf) {
      int dx = rand() % 3, dy = rand() % 2;
      wa.x += dx;
      wa.y += dy;
      wb.x += dx;
      wb.y += dy;
    }
    int row = rand() % 6 - 1, col = rand() % 18 - 2;
    uint32_t ch = chars[rand() % (int)(sizeof chars / sizeof *chars)];
    unsigned char c[6];
    for (int i = 0; i < 6; i++) {
      c[i] = (unsigned char)rand();
    }
    CTUI_CELL style = {0};
    int got = 0, want = 0, which = rand() % 5;
    if (which == 0) {
      ctui_widget_putc(&wa, a, row, col, ch, c[0] % 8, c[1] % 8);
      style = (CTUI_CELL){.fg = c[0] % 8, .bg = c[1] % 8};
    } else if (which == 1) {
      style = (CTUI_CELL){.fg = c[0],
                          .bg = c[1],
                          .color_mode = (unsigned char)(c[2] % 4),
                          .fg_r = c[3],
                          .bg_b = c[4],
                          .kitty_row = c[5] % 3,
                          .attr = (unsigned char)(c[2] >> 3)};
      got = ctui_widget_putc_cell(&wa, a, row, col, ch, &style);
    } else if (which == 2) {
      ctui_widget_putc_256(&wa, a, row, col, ch, c[0], c[1]);
      style = (CTUI_CELL){
          .fg = c[0], .bg = c[1], .color_mode = CTUI_COLOR_MODE_256};
    } else if (which == 3) {
      ctui_widget_putc_rgb(&wa, a, row, col, ch, c[0], c[1], c[2], c[3], c[4],
                           c[5]);
      style = (CTUI_CELL){.fg_r = c[0],
                          .fg_g = c[1],
                          .fg_b = c[2],
                          .bg_r = c[3],
                          .bg_g = c[4],
                          .bg_b = c[5],
                          .color_mode = CTUI_COLOR_MODE_RGB};
    } else {
      ctui_widget_putc_rgb_fg(&wa, a, row, col, ch, c[0], c[1], c[2], c[3]);
      style = (CTUI_CELL){.fg_r = c[0],
                          .fg_g = c[1],
                          .fg_b = c[2],
                          .bg = c[3],
                          .color_mode = CTUI_COLOR_MODE_RGB_FG};
    }
    want = ref_putc(&wb, b, row, col, ch, &style);
    if ((which == 1 && got != want) ||
        memcmp(a->cells, b->cells, sizeof(CTUI_CELL) * ROWS * COLS) != 0) {
      bad++;
    }
  }
  CTUI_TEST_ASSERT(bad == 0,
                   "putc, putc_cell, _256, _rgb, _rgb_fg: the same cells as "
                   "the reference (%d of %d differ)",
                   bad, RUNS);
  ctui_compositor_free(a);
  ctui_compositor_free(b);
  ctui_log_shutdown();
  ctui_log_init(E_ALL);
}

static void test_puts_vs_ref(void) {
  /* the random runs log little: their out-of-bounds puts are warnings */
  ctui_log_shutdown();
  ctui_log_init(E_ERR);
  static const char *const bits[] = {"a",
                                     "b",
                                     "Z",
                                     " ",
                                     "~",
                                     "\xe4\xbd\xa0" /* wide */,
                                     "\xcc\x81" /* combining */,
                                     "\xf0\x9f\x91\x8d" /* emoji */,
                                     "\xe2\x94\x80" /* box */,
                                     "\n",
                                     "\t",
                                     "\x7f",
                                     "\xc2\x85" /* C1 */,
                                     "#",
                                     "\xef\xb8\x8f" /* VS16 */,
                                     "\xe2\x80\x8b" /* ZWSP */};
  enum { ROWS = 5, COLS = 16 };
  CTUI_COMPOSITOR *a = ctui_compositor_create(ROWS, COLS);
  CTUI_COMPOSITOR *b = ctui_compositor_create(ROWS, COLS);
  srand(219);
  int bad = 0;
  for (int r = 0; r < 30000; r++) {
    wide_rows(a, b);
    CTUI_WIDGET wa = {.render = noop_render,
                      .x = rand() % 6,
                      .y = rand() % 3,
                      .w = 1 + rand() % 14,
                      .h = 1 + rand() % 4};
    CTUI_WIDGET wb = wa;
    ctui_widget_init(&wa, a);
    ctui_widget_init(&wb, b);
    if (rand() % 4 == 0 && wa.buf) {
      /* a group member: the first member's slice, its own place */
      int dx = rand() % 3, dy = rand() % 2;
      wa.x += dx;
      wa.y += dy;
      wb.x += dx;
      wb.y += dy;
    }
    char str[64] = "";
    int nbits = rand() % 12;
    for (int i = 0; i < nbits; i++) {
      strcat(str, bits[rand() % (int)(sizeof bits / sizeof *bits)]);
    }
    CTUI_CELL style = {.fg = (unsigned char)(rand() % 8),
                       .bg = (unsigned char)(rand() % 8),
                       .color_mode = (unsigned char)(rand() % 2),
                       .attr = (unsigned char)(rand() % 4)};
    int row = rand() % 6 - 1, col = rand() % 18 - 2;
    size_t len = strlen(str);
    size_t n = len ? (size_t)rand() % (len + 1) : 0;
    int which = rand() % 3, got = 0, want = 0;
    if (which == 0) {
      got = ctui_widget_puts_cell(&wa, a, row, col, str, (size_t)-1, &style);
      want = ref_puts(&wb, b, row, col, str, NULL, &style) - col;
    } else if (which == 1) {
      got = ctui_widget_puts_cell(&wa, a, row, col, str, n, &style);
      want = ref_puts(&wb, b, row, col, str, str + n, &style) - col;
    } else {
      ctui_widget_puts(&wa, a, row, col, str, style.fg, style.bg);
      CTUI_CELL basic = {.fg = style.fg, .bg = style.bg};
      ref_puts(&wb, b, row, col, str, NULL, &basic);
    }
    if (got != want ||
        memcmp(a->cells, b->cells, sizeof(CTUI_CELL) * ROWS * COLS) != 0) {
      bad++;
    }
  }
  CTUI_TEST_ASSERT(bad == 0,
                   "puts, puts_cell whole and cut: the same cells "
                   "as putting each cluster (%d of 30000 differ)",
                   bad);
  ctui_compositor_free(a);
  ctui_compositor_free(b);
  ctui_log_shutdown();
  ctui_log_init(E_ALL);
}

int main(void) {
  ctui_log_init(E_ALL);

  test_putc_bounds();
  test_puts_and_color_modes();
  test_puts_cut();
  test_puts_cut_cell();
  test_puts_n();
  test_puts_cell();
  test_puts_vs_ref();
  test_known_narrow();
  test_putc_vs_ref();
  test_widget_init_out_of_bounds();
  test_gfx_dispatch();
  test_render_hooks();

  return ctui_test_summary();
}
