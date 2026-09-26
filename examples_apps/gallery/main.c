/* The app widgets side by side: a sidebar (CTUI_TABS) switching between a
 * settings form (CTUI_FORM), a file table (CTUI_TABLE) and a text pane
 * (CTUI_TEXTVIEW), with dialogs (CTUI_DIALOG, one holding a CTUI_ENTRY)
 * over them. One router handler sends each key to whatever has the focus,
 * the way an app with several panes does it.
 *
 * Keys: up/down pick a page, Enter or right goes into it, Esc back out;
 * in the table d asks to delete, r renames, p copies (a progress box);
 * in the text w toggles wrapping; q or ctrl+c quits from the sidebar. */
#include "ctui.h"
#include "widgets/dialog.h"
#include "widgets/entry.h"
#include "widgets/form.h"
#include "widgets/table.h"
#include "widgets/tabs.h"
#include "widgets/textview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIDEBAR 16
#define FILES 200

enum { PAGE_FORM, PAGE_TABLE, PAGE_TEXT };

static const CTUI_TABS_ITEM side_items[] = {
    {.label = "Widgets", .heading = 1},
    {.label = "Form"},
    {.label = "Table"},
    {.label = "Text"},
};
static CTUI_TABS side = {.items = side_items, .count = 4, .vertical = 1};
static int in_page; /* the focus: 0 the sidebar, 1 the page */

/* --- the form page --- */
static char layout_buf[32];
static CTUI_ENTRY layout_entry = {.buf = layout_buf,
                                  .cap = sizeof layout_buf,
                                  .placeholder = "us"};
static const char *const profiles[] = {"flat", "adaptive", "custom"};
static CTUI_FORM_ROW form_rows[] = {
    {.kind = CTUI_FORM_HEADING, .label = "Keyboard"},
    {.kind = CTUI_FORM_ENTRY, .label = "Layout", .entry = &layout_entry},
    {.kind = CTUI_FORM_SLIDER, .label = "Repeat rate", .value = 40,
     .min = 10, .max = 100, .step = 5, .hint = "keys/s"},
    {.kind = CTUI_FORM_SLIDER, .label = "Repeat delay", .value = 250,
     .min = 100, .max = 1000, .step = 50, .hint = "ms"},
    {.kind = CTUI_FORM_HEADING, .label = ""},
    {.kind = CTUI_FORM_HEADING, .label = "Pointer"},
    {.kind = CTUI_FORM_TOGGLE, .label = "Natural scroll"},
    {.kind = CTUI_FORM_TOGGLE, .label = "Tap to click", .value = 1},
    {.kind = CTUI_FORM_CHOICE, .label = "Accel profile", .options = profiles,
     .option_count = 3},
    {.kind = CTUI_FORM_TOGGLE, .label = "Middle emulation", .disabled = 1,
     .hint = "(not on this device)"},
    {.kind = CTUI_FORM_HEADING, .label = ""},
    {.kind = CTUI_FORM_BUTTON, .label = "Apply"},
};
static CTUI_FORM form = {.rows = form_rows,
                         .count = sizeof form_rows / sizeof *form_rows,
                         .drag = -1};

/* --- the table page --- */
typedef struct {
  char name[32];
  long size;
  int day;
} FILE_ROW;
static FILE_ROW files[FILES];
static int order[FILES];
static unsigned char marks[FILES];
static const CTUI_TABLE_COLUMN columns[] = {
    {.title = "Name"},
    {.title = "Size", .width = 8, .align_right = 1},
    {.title = "Modified", .width = 10},
};

static const char *file_cell(void *ctx, int row, int col, char *scratch,
                             size_t cap) {
  (void)ctx;
  const FILE_ROW *f = &files[order[row]];
  if (col == 0) {
    return f->name;
  }
  if (col == 1) {
    if (f->size < 1024) {
      snprintf(scratch, cap, "%ld B", f->size);
    } else {
      snprintf(scratch, cap, "%.1f K", (double)f->size / 1024);
    }
  } else {
    snprintf(scratch, cap, "2026-09-%02d", f->day);
  }
  return scratch;
}

static unsigned char file_fg(void *ctx, int row) {
  (void)ctx;
  return strchr(files[order[row]].name, '/') ? CTUI_COLOR_BLUE : 0;
}

static CTUI_TABLE table = {.columns = columns,
                           .column_count = 3,
                           .count = FILES,
                           .header = 1,
                           .sort_col = 0,
                           .marks = marks,
                           .cell = file_cell,
                           .row_fg = file_fg};

static int sort_desc;
static int sort_col;
static int cmp_files(const void *a, const void *b) {
  const FILE_ROW *x = &files[*(const int *)a], *y = &files[*(const int *)b];
  long d = sort_col == 1   ? (x->size > y->size) - (x->size < y->size)
           : sort_col == 2 ? x->day - y->day
                           : strcmp(x->name, y->name);
  return sort_desc ? (int)-d : (int)d;
}

static void sort_files(void) {
  int cur = order[table.selected];
  sort_col = table.sort_col;
  sort_desc = table.sort_desc;
  qsort(order, FILES, sizeof *order, cmp_files);
  for (int i = 0; i < FILES; i++) {
    if (order[i] == cur) {
      ctui_table_select(&table, i);
    }
  }
}

/* --- the text page --- */
static const char text[] =
    "CTUI_TEXTVIEW\n"
    "=============\n"
    "\n"
    "A read-only text pane: the app's text, indexed by line once, shown "
    "either wrapped at the width (a long line takes several rows, broken "
    "between words) "
    "or unwrapped with a sideways scroll (left/right, 8 columns).\n"
    "\n"
    "\tTabs\tgo\tto\tthe\tnext\tmultiple\tof\t8.\n"
    "Control bytes show as ?: \x01\x02\x7f, a CR before a line end goes.\r\n"
    "Wide glyphs count two columns: 日本語のテキスト, and emoji too.\n"
    "\n"
    "Press w to toggle wrapping.\n";
static CTUI_TEXTVIEW textview = {.wrap = 1};

/* --- dialogs --- */
static char rename_buf[64];
static CTUI_ENTRY rename_entry = {.buf = rename_buf, .cap = sizeof rename_buf};
static CTUI_DIALOG dialog = {.progress = -1};
static CTUI_TIMER *copy_timer;
static char dialog_text[128];
static char status[128] = "up/down: page · enter: in · esc: out · q: quit";

/* --- layout --- */
static void side_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = 0;
  self->w = SIDEBAR;
  self->h = comp->rows - 1;
}

static void page_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = SIDEBAR + 1;
  self->y = 0;
  self->w = comp->cols - SIDEBAR - 1;
  self->h = comp->rows - 1;
}

static void status_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = 0;
  self->y = comp->rows - 1;
  self->w = comp->cols;
  self->h = 1;
}

static void full_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = self->y = 0;
  self->w = comp->cols;
  self->h = comp->rows;
}

static CTUI_WIDGET form_w, table_w, text_w;

static CTUI_WIDGET *page_widget(void) {
  return side.selected == 1 + PAGE_FORM    ? &form_w
         : side.selected == 1 + PAGE_TABLE ? &table_w
                                           : &text_w;
}

static void page_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  (void)self;
  CTUI_WIDGET *p = page_widget();
  ctui_widget_init(p, comp);
  ctui_widget_dispatch_render(p, comp);
}

static void status_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  ctui_widget_puts_cut(self, comp, 0, 0, status, self->w, CTUI_COLOR_BLUE,
                       CTUI_COLOR_DEFAULT);
}

static void rule_render(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  for (int r = 0; r < self->h; r++) {
    ctui_widget_putc(self, comp, r, 0, 0x2502, in_page ? CTUI_COLOR_BLUE
                                                       : CTUI_COLOR_CYAN,
                     CTUI_COLOR_DEFAULT);
  }
}

static void rule_layout(CTUI_WIDGET *self, CTUI_COMPOSITOR *comp) {
  self->x = SIDEBAR;
  self->y = 0;
  self->w = 1;
  self->h = comp->rows - 1;
}

/* --- input --- */
static int copy_tick(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  (void)ev;
  dialog.progress += 4;
  if (dialog.progress >= 100 || !dialog.open) {
    dialog.open = 0;
    snprintf(status, sizeof status, "copy done");
    ctui_timer_cancel(copy_timer);
    copy_timer = NULL;
  }
  return 1;
}

static void open_dialog(int c, CTUI_WIDGET *owner) {
  int n = 0;
  for (int i = 0; i < FILES; i++) {
    n += marks[i];
  }
  const char *name = files[order[table.selected]].name;
  dialog = (CTUI_DIALOG){.open = 1, .progress = -1};
  if (c == 'd') {
    if (n) {
      snprintf(dialog_text, sizeof dialog_text,
               "Delete the %d marked files for good? This can't be undone.",
               n);
    } else {
      snprintf(dialog_text, sizeof dialog_text,
               "Delete %s for good? This can't be undone.", name);
    }
    dialog.title = "Delete";
    dialog.text = dialog_text;
    dialog.buttons[0] = "Delete";
    dialog.buttons[1] = "Cancel";
    dialog.button_count = 2;
    dialog.focus = 1;
  } else if (c == 'r') {
    ctui_entry_set(&rename_entry, name);
    dialog.title = "Rename";
    dialog.entry = &rename_entry;
    dialog.buttons[0] = "Rename";
    dialog.buttons[1] = "Cancel";
    dialog.button_count = 2;
  } else {
    snprintf(dialog_text, sizeof dialog_text, "Copying %s to /tmp", name);
    dialog.title = "Copy";
    dialog.text = dialog_text;
    dialog.progress = 0;
    dialog.width = 36;
    copy_timer = ctui_timer_register(100, owner, copy_tick);
  }
}

static void dialog_done(int what) {
  if (what == CTUI_DIALOG_CANCEL) {
    snprintf(status, sizeof status, "%s: cancelled", dialog.title);
  } else if (what == CTUI_DIALOG_CHOSEN) {
    snprintf(status, sizeof status, "%s: %s%s%s", dialog.title,
             dialog.buttons[dialog.focus], dialog.entry ? " -> " : "",
             dialog.entry ? rename_buf : "");
  }
}

static int page_key(CTUI_WIDGET *self, const CTUI_KEYPRESS_EVENT_DATA *kp) {
  if (side.selected == 1 + PAGE_FORM) {
    int what = ctui_form_key(&form, kp);
    if (what == CTUI_FORM_CHANGED || what == CTUI_FORM_PRESSED) {
      snprintf(status, sizeof status, "form: '%s' %s",
               form.rows[form.focus].label,
               what == CTUI_FORM_CHANGED ? "changed" : "pressed");
    }
    return what != CTUI_FORM_NONE;
  }
  if (side.selected == 1 + PAGE_TABLE) {
    if (kp->type == CTUI_KEY_CHAR &&
        (kp->ch == 'd' || kp->ch == 'r' || kp->ch == 'p')) {
      open_dialog((int)kp->ch, self);
      return 1;
    }
    int what = ctui_table_key(&table, kp);
    if (what == CTUI_TABLE_ACTIVATE) {
      snprintf(status, sizeof status, "table: open %s",
               files[order[table.selected]].name);
    }
    return what != CTUI_TABLE_NONE;
  }
  if (kp->type == CTUI_KEY_CHAR && kp->ch == 'w') {
    textview.wrap = !textview.wrap;
    textview.hscroll = 0;
    return 1;
  }
  return ctui_textview_key(&textview, kp);
}

static int on_key(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  CTUI_KEYPRESS_EVENT_DATA *kp = ev->event_data;
  if (dialog.open) {
    int what = ctui_dialog_key(&dialog, kp);
    dialog_done(what);
    return 1;
  }
  if (kp->type == CTUI_KEY_CHAR && kp->ch == 0x03) {
    ctui_app_quit();
    return 0;
  }
  if (!in_page) {
    if (kp->type == CTUI_KEY_CHAR && kp->ch == 'q') {
      ctui_app_quit();
      return 0;
    }
    if (kp->type == CTUI_KEY_RIGHT) {
      in_page = 1;
      return 1;
    }
    int what = ctui_tabs_key(&side, kp);
    if (what == CTUI_TABS_ACTIVATE) {
      in_page = 1;
    }
    return what != CTUI_TABS_NONE;
  }
  if (kp->type == CTUI_KEY_ESC) {
    in_page = 0;
    return 1;
  }
  return page_key(self, kp);
}

static int on_mouse(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)self;
  CTUI_MOUSE_EVENT_DATA *m = ev->event_data;
  if (dialog.open) {
    int what = ctui_dialog_mouse(&dialog, m);
    dialog_done(what);
    return what != CTUI_DIALOG_NONE;
  }
  if (m->col < SIDEBAR) {
    CTUI_WIDGET area = ctui_widget_make(0, 0, SIDEBAR, 1000, NULL, NULL, NULL);
    int what = ctui_tabs_mouse(&side, &area, m);
    if (m->action == CTUI_MOUSE_PRESS) {
      in_page = 0;
      return 1;
    }
    return what != CTUI_TABS_NONE;
  }
  CTUI_WIDGET *p = page_widget();
  int changed = m->action == CTUI_MOUSE_PRESS && !in_page;
  if (m->action == CTUI_MOUSE_PRESS) {
    in_page = 1;
  }
  if (p == &form_w) {
    int what = ctui_form_mouse(&form, p, m);
    if (what == CTUI_FORM_CHANGED || what == CTUI_FORM_PRESSED) {
      snprintf(status, sizeof status, "form: '%s' %s",
               form.rows[form.focus].label,
               what == CTUI_FORM_CHANGED ? "changed" : "pressed");
    }
    return changed || what != CTUI_FORM_NONE;
  }
  if (p == &table_w) {
    int what = ctui_table_mouse(&table, p, m);
    if (what == CTUI_TABLE_SORT) {
      sort_files();
    } else if (what == CTUI_TABLE_ACTIVATE) {
      snprintf(status, sizeof status, "table: open %s",
               files[order[table.selected]].name);
    } else if (what == CTUI_TABLE_MENU) {
      snprintf(status, sizeof status, "table: menu for %s",
               files[order[table.selected]].name);
    }
    return changed || what != CTUI_TABLE_NONE;
  }
  return changed || ctui_textview_mouse(&textview, p, m);
}

int main(void) {
  static const char *const stems[] = {"notes", "photo", "report", "song",
                                      "draft", "backup", "src/", "docs/"};
  static const char *const exts[] = {".txt", ".png", ".pdf", ".flac",
                                     ".md", ".tar", "", ""};
  srand(7);
  for (int i = 0; i < FILES; i++) {
    int k = i % 8;
    snprintf(files[i].name, sizeof files[i].name, "%s%s%d%s", stems[k],
             k >= 6 ? "" : "-", i, k >= 6 ? "" : exts[k]);
    if (k >= 6) { /* "src/12" -> "src12/" */
      snprintf(files[i].name, sizeof files[i].name, "%.*s%d/",
               (int)strlen(stems[k]) - 1, stems[k], i);
    }
    files[i].size = rand() % 5000000;
    files[i].day = 1 + rand() % 26;
    order[i] = i;
  }

  CTUI_GFX_MODE gfx_mode = CTUI_GFX_ANSI16;
  if (ctui_init(E_INF | E_WRN | E_ERR, &gfx_mode) != 0) {
    fprintf(stderr, "failed to init ctui\n");
    return 1;
  }
  int rows, cols;
  ctui_get_termsize(&rows, &cols);
  CTUI_SCREEN *screen = ctui_screen_create(rows, cols);

  ctui_textview_set(&textview, text, sizeof text - 1);
  sort_files();

  CTUI_WIDGET side_w = ctui_widget_make(0, 0, 0, 0, &side, ctui_tabs_render,
                                        side_layout);
  CTUI_WIDGET rule_w = ctui_widget_make(0, 0, 0, 0, NULL, rule_render,
                                        rule_layout);
  CTUI_WIDGET page_w = ctui_widget_make(0, 0, 0, 0, NULL, page_render,
                                        page_layout);
  CTUI_WIDGET status_w = ctui_widget_make(0, 0, 0, 0, NULL, status_render,
                                          status_layout);
  CTUI_WIDGET dialog_w = ctui_widget_make(0, 0, 0, 0, &dialog,
                                          ctui_dialog_render, full_layout);
  form_w = ctui_widget_make(0, 0, 0, 0, &form, ctui_form_render, page_layout);
  table_w =
      ctui_widget_make(0, 0, 0, 0, &table, ctui_table_render, page_layout);
  text_w = ctui_widget_make(0, 0, 0, 0, &textview, ctui_textview_render,
                            page_layout);

  CTUI_WIDGET *widgets[] = {&side_w, &rule_w, &page_w, &status_w, &dialog_w};
  CTUI_APP app;
  if (ctui_app_init(&app, widgets, 5, rows, cols) != 0) {
    fprintf(stderr, "failed to init ctui app\n");
    return 1;
  }
  app.quit_on_esc = 0;
  ctui_mouse_enable(CTUI_MOUSE_TRACK_DRAG);
  ctui_event_register("input", CTUI_KEYPRESS_EVENT, &side_w, on_key);
  ctui_event_register("input", CTUI_MOUSE_EVENT, &side_w, on_mouse);

  ctui_app_run(&app, screen, 0);

  ctui_textview_free(&textview);
  ctui_app_free(&app);
  ctui_screen_free(screen);
  ctui_shutdown();
  return 0;
}
