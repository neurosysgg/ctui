/* Exercises core/group.c: every member of a CTUI_GROUP binds to the SAME
 * compositor slice (derived from members[0]'s origin, per ctui_group_init()'s
 * contract), and ctui_group_render() draws them in order so later members
 * layer over earlier ones in the same cells -- proving the "members share
 * the same (x,y) and paint over each other" semantics documented in
 * CLAUDE.md's Groups-vs-splits-vs-independent section. */
#include "ctui.h"

#include "ctui_test.h"

typedef struct {
  char ch;
} FILLER;

static void filler_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  FILLER *f = self->widget_data;
  for (int r = 0; r < self->h; r++) {
    for (int c = 0; c < self->w; c++) {
      ctui_widget_putc(self, comp, r, c, f->ch, CTUI_COLOR_DEFAULT,
                       CTUI_COLOR_DEFAULT);
    }
  }
}

static void punch_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_widget_putc(self, comp, 0, 0, 'X', CTUI_COLOR_DEFAULT,
                   CTUI_COLOR_DEFAULT);
}

int main(void) {
  ctui_log_init(E_ALL);

  int rows = 10, cols = 10;
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);
  CTUI_COMPOSITOR *comp = ctui_compositor_create(rows, cols);

  FILLER bg = {'.'};
  /* deliberately given a DIFFERENT (x,y) than members[0] -- per
   * ctui_group_init()'s contract, this is ignored: it still binds to
   * members[0]'s slice, not its own */
  CTUI_WIDGET bg_w = ctui_widget_make(2, 2, 4, 3, &bg, filler_render, NULL);
  CTUI_WIDGET fg_w = ctui_widget_make(9, 9, 4, 3, NULL, punch_render, NULL);

  CTUI_WIDGET members[] = {bg_w, fg_w};
  CTUI_GROUP group = ctui_group_make("test-group", members, 2);

  ctui_group_init(&group, comp);
  CTUI_TEST_ASSERT(group.members[0].buf != NULL,
                   "group_init binds members[0] to a real compositor slice");
  CTUI_TEST_ASSERT(group.members[1].buf == group.members[0].buf,
                   "group_init binds every later member to the SAME buf as "
                   "members[0], regardless of the later member's own (x,y)");

  ctui_group_render(&group, comp);
  ctui_compositor_blit(comp, screen);

  /* members[0]'s origin is (2,2) -- both members painted into that slice */
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 2, 3) == '.',
                   "the background member fills its cells with '.'");
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 2, 2) == 'X',
                   "the foreground member, rendered second, overwrites the "
                   "background at the same cell -- layering within a "
                   "group happens because both share one buf, not because "
                   "of their own distinct (x,y)");
  CTUI_TEST_ASSERT(ctui_test_cell(screen, 9, 9) == ' ',
                   "the foreground member's own (9,9) origin is never "
                   "actually used for addressing -- nothing was drawn "
                   "there");

  ctui_compositor_free(comp);
  ctui_screen_free(screen);
  return ctui_test_summary();
}
