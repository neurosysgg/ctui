/* Exercises core/widget.c directly: the shared bounds-checking in
 * widget_cell_at() (unbound widget, out-of-widget-bounds, out-of-compositor-
 * bounds all reject-and-log rather than write), the three color-mode putc
 * variants tagging a cell correctly, and the gfx dispatch queue
 * (ctui_widget_dispatch_render()/ctui_widget_flush_gfx()) that lets a
 * Phase-4 widget's gfx_render fire only when its mode matches the
 * negotiated one and only after being explicitly flushed. */
#include "ctui.h"

#include "ctui_test.h"

/* g_gfx_mode is intentionally private -- see kitty_protocol_test.c's
 * identical extern for why redeclaring it here is the accepted gray-box
 * trick for a headless test that never calls ctui_init(). */
extern unsigned int g_gfx_mode;

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

  g_gfx_mode = 0;
  ctui_widget_dispatch_render(&text_w, comp);
  ctui_widget_dispatch_render(&gfx_w, comp);
  CTUI_TEST_ASSERT(gfx_fire_count == 0,
                   "dispatch_render() defers to plain render() (not "
                   "gfx_render) when gfx_render_mode doesn't match the "
                   "negotiated g_gfx_mode");

  ctui_widget_flush_gfx(comp);
  CTUI_TEST_ASSERT(gfx_fire_count == 0,
                   "flush_gfx() fires nothing when nothing was queued this "
                   "frame");

  g_gfx_mode = CTUI_GFX_KITTY;
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
  g_gfx_mode = 0;
  ctui_compositor_free(comp);
}

int main(void) {
  ctui_log_init(E_ALL);

  test_putc_bounds();
  test_puts_and_color_modes();
  test_widget_init_out_of_bounds();
  test_gfx_dispatch();

  return ctui_test_summary();
}
