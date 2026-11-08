#define _POSIX_C_SOURCE 200809L

/* node/ui.h's widgets: found as they're drawn (core/widget.h's hook),
 * matched to the app's names at each wait, their state read into
 * ui/w/NAME/ */

#include "ui_internal.h"

#include "../widgets/dialog.h"
#include "../widgets/entry.h"
#include "../widgets/form.h"
#include "../widgets/list.h"
#include "../widgets/popmenu.h"
#include "../widgets/table.h"
#include "../widgets/tabs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLAG ctui_ui_t_flag
#define TEXT ctui_ui_t_text

static const CTUI_NODE_TYPE COUNT = {CTUI_NODE_INT,    NULL, 1, 0, 0,
                                     CTUI_NODE_LINEAR, NULL, 0, 0};
static const CTUI_NODE_TYPE ROW_RW = {
    CTUI_NODE_INT,     NULL, 1, 0, -1, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};
static const CTUI_NODE_TYPE TEXT_RW = {
    CTUI_NODE_STRING,  NULL, 1, 0, 0, CTUI_NODE_LINEAR, NULL, 0,
    CTUI_NODE_WRITABLE};

/* draws kept a frame; more are left out (logged once) */
#define DRAWN_MAX 1024
/* unnamed kinds counted a frame */
#define KINDS_MAX 32

/* the hook: obj drawn in the frame being drawn */
void ctui_ui_on_drawn(const char *kind, void *obj, int x, int y, int w, int h,
                      void *arg) {
  CTUI_UI *ui = arg;
  if (ui->drawn_frame != ui->app->frames) {
    ui->drawn_frame = ui->app->frames;
    ui->ndrawn = 0;
  }
  for (int i = 0; i < ui->ndrawn; i++) {
    DRAWN *d = &ui->drawn[i];
    if (d->obj == obj && strcmp(d->kind, kind) == 0) {
      *d = (DRAWN){kind, obj, x, y, w, h}; /* drawn again: the last place */
      return;
    }
  }
  if (ui->ndrawn == ui->drawn_cap) {
    if (ui->drawn_cap == DRAWN_MAX) {
      return;
    }
    int cap = ui->drawn_cap ? ui->drawn_cap * 2 : 32;
    DRAWN *a = realloc(ui->drawn, (size_t)cap * sizeof *a);
    if (!a) {
      return;
    }
    ui->drawn = a;
    ui->drawn_cap = cap;
  }
  ui->drawn[ui->ndrawn++] = (DRAWN){kind, obj, x, y, w, h};
}

static W_KIND kind_of(const char *kind) {
  static const struct {
    const char *s;
    W_KIND k;
  } KINDS[] = {{"entry", W_ENTRY},    {"table", W_TABLE},   {"list", W_LIST},
               {"tabs", W_TABS},      {"dialog", W_DIALOG}, {"form", W_FORM},
               {"popmenu", W_POPMENU}};
  for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) {
    if (strcmp(kind, KINDS[i].s) == 0) {
      return KINDS[i].k;
    }
  }
  return W_OTHER;
}

static UIW *find_name(CTUI_UI *ui, const char *name, size_t len) {
  for (int i = 0; i < ui->nws; i++) {
    if (strlen(ui->ws[i]->name) == len &&
        memcmp(ui->ws[i]->name, name, len) == 0) {
      return ui->ws[i];
    }
  }
  return NULL;
}

int ctui_ui_widget_cell(CTUI_UI *ui, const char *path, size_t len, int dc,
                        int dr, int *x, int *y) {
  if (len < 3 || memcmp(path, "w/", 2) != 0) {
    return ENOENT;
  }
  const UIW *u = find_name(ui, path + 2, len - 2);
  if (!u) {
    return ENOENT;
  }
  if (!u->shown || u->w <= 0 || u->h <= 0) {
    return ENXIO;
  }
  if ((dc >= 0 && dc >= u->w) || (dr >= 0 && dr >= u->h)) {
    return ERANGE;
  }
  *x = u->x + (dc >= 0 ? dc : u->w / 2);
  *y = u->y + (dr >= 0 ? dr : u->h / 2);
  return 0;
}

/* a widget's row cursor moved from outside: kept in view where the widget
 * doesn't do that itself */
static void list_select(CTUI_LIST *l, int row, int h) {
  l->selected = row;
  if (row < l->scroll_offset) {
    l->scroll_offset = row;
  } else if (h > 0 && row >= l->scroll_offset + h) {
    l->scroll_offset = row - h + 1;
  }
}

static int frame_soon(CTUI_WIDGET *self, CTUI_EVENT *ev) {
  (void)ev;
  CTUI_UI *ui = self->widget_data;
  ctui_timer_cancel(ui->frame_soon);
  ui->frame_soon = NULL;
  return 1; /* drawn after this tick */
}

static int widget_rows(const UIW *u) {
  switch (u->kind) {
  case W_TABLE:
    return ((const CTUI_TABLE *)u->obj)->count;
  case W_LIST:
    return ((const CTUI_LIST *)u->obj)->count;
  case W_TABS:
    return ((const CTUI_TABS *)u->obj)->count;
  default:
    return 0;
  }
}

static int widget_write(CTUI_NODE *n, const CTUI_NODE_VALUE *v, CTUI_BUF *reply,
                        void *user) {
  (void)reply;
  UIW *u = user;
  CTUI_UI *ui = u->ui;
  if (n == u->text) {
    CTUI_ENTRY *e = u->obj;
    if (v->len >= e->cap) {
      return E2BIG;
    }
    ctui_entry_set(e, v->s);
  } else {
    if (v->i < -1 || v->i >= widget_rows(u) ||
        (v->i < 0 && u->kind == W_TABS)) {
      return ERANGE;
    }
    int row = (int)v->i;
    if (u->kind == W_TABLE) {
      ctui_table_select(u->obj, row);
    } else if (u->kind == W_LIST) {
      list_select(u->obj, row, u->h);
    } else {
      ctui_tabs_select(u->obj, row);
    }
  }
  int err = u->changed(u->obj, u->user);
  if (err) {
    return err;
  }
  if (!ui->frame_soon) {
    ui->frame_soon = ctui_timer_register(1, &ui->self, frame_soon);
  }
  ctui_logf(E_INF, "[CTUI:UI] - w/%s written @ tick %d\n", u->name,
            ctui_tick_advance());
  return ctui_ui_hold(ui, n);
}

static const CTUI_NODE_OPS WIDGET_OPS = {widget_write, NULL};

/* s into a STRING node, cut to what it holds (at a character) */
static void set_text(CTUI_NODE *n, const char *s) {
  static char cut[CTUI_NODE_TEXT_MAX + 1];
  size_t len = s ? strlen(s) : 0;
  if (len > CTUI_NODE_TEXT_MAX) {
    len = CTUI_NODE_TEXT_MAX;
    while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80) {
      len--;
    }
    memcpy(cut, s, len);
    cut[len] = '\0';
    s = cut;
  }
  if (ctui_node_string_ok(ctui_node_type(n), s ? s : "", len)) {
    ctui_node_set_str(n, s ? s : "");
  }
}

void ctui_ui_slug(const char *l, char *out, size_t cap) {
  if (cap == 0) {
    return;
  }
  size_t o = 0;
  for (; l && *l && o + 1 < cap && o < 40; l++) {
    unsigned char c = (unsigned char)*l;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[o++] = (char)c;
    } else if (c >= 'A' && c <= 'Z') {
      out[o++] = (char)(c - 'A' + 'a');
    } else if (o > 0 && out[o - 1] != '_') {
      out[o++] = '_';
    }
  }
  while (o > 0 && out[o - 1] == '_') {
    o--;
  }
  out[o] = '\0';
}

static int is_field(const CTUI_FORM_ROW *r) {
  return r->kind != CTUI_FORM_HEADING && r->kind != CTUI_FORM_BUTTON;
}

static CTUI_NODE *wnode(UIW *u, const char *leaf, const CTUI_NODE_TYPE *ro,
                        const CTUI_NODE_TYPE *rw) {
  int writable = rw && (u->flags & CTUI_UI_RDWR);
  return ctui_node_publishf(u->ui->ns, writable ? rw : ro,
                            writable ? &WIDGET_OPS : NULL, u, "ui/w/%s/%s",
                            u->name, leaf);
}

/* form row's field name: its label's slug ("row3" without one), _2, _3,
 * ... when an earlier field's is the same */
static void field_name(const CTUI_FORM *f, int row, char *out, size_t cap) {
  ctui_ui_slug(f->rows[row].label, out, cap);
  if (!*out) {
    snprintf(out, cap, "row%d", row);
    return;
  }
  int same = 1;
  for (int r = 0; r < row; r++) {
    char other[48];
    if (is_field(&f->rows[r])) {
      ctui_ui_slug(f->rows[r].label, other, sizeof other);
      same += strcmp(other, out) == 0;
    }
  }
  size_t o = strlen(out);
  if (same > 1 && o + 4 < cap) {
    snprintf(out + o, cap - o, "_%d", same);
  }
}

static void fields_free(UIW *u) {
  for (int i = 0; i < u->nfields; i++) {
    if (u->fields[i]) {
      ctui_node_remove(u->fields[i]);
    }
    free(u->field_names[i]);
  }
  free(u->fields);
  free(u->field_names);
  u->fields = NULL;
  u->field_names = NULL;
  u->nfields = 0;
}

/* a form's fields/NAME: republished when its rows changed */
static void sync_fields(UIW *u, const CTUI_FORM *f) {
  int n = 0, same = 1;
  for (int r = 0; r < f->count; r++) {
    if (!is_field(&f->rows[r])) {
      continue;
    }
    char name[48];
    field_name(f, r, name, sizeof name);
    same = same && n < u->nfields && strcmp(u->field_names[n], name) == 0;
    n++;
  }
  if (!same || n != u->nfields) {
    fields_free(u);
    u->fields = calloc((size_t)(n ? n : 1), sizeof *u->fields);
    u->field_names = calloc((size_t)(n ? n : 1), sizeof *u->field_names);
    if (!u->fields || !u->field_names) {
      free(u->fields);
      free(u->field_names);
      u->fields = NULL;
      u->field_names = NULL;
      return;
    }
    for (int r = 0; r < f->count; r++) {
      if (is_field(&f->rows[r])) {
        char name[48], leaf[64];
        field_name(f, r, name, sizeof name);
        snprintf(leaf, sizeof leaf, "fields/%s", name);
        u->field_names[u->nfields] = strdup(name);
        u->fields[u->nfields] = wnode(u, leaf, &TEXT, NULL);
        if (!u->field_names[u->nfields]) {
          if (u->fields[u->nfields]) {
            ctui_node_remove(u->fields[u->nfields]);
          }
          break;
        }
        u->nfields++;
      }
    }
  }
  int i = 0;
  for (int r = 0; r < f->count && i < u->nfields; r++) {
    const CTUI_FORM_ROW *row = &f->rows[r];
    if (!is_field(row)) {
      continue;
    }
    char num[24];
    const char *v = num;
    switch (row->kind) {
    case CTUI_FORM_ENTRY:
      v = row->entry ? row->entry->buf : "";
      break;
    case CTUI_FORM_CHOICE:
      if (row->options && row->value >= 0 && row->value < row->option_count) {
        v = row->options[row->value];
      } else {
        snprintf(num, sizeof num, "%d", row->value); /* no such option */
      }
      break;
    default:
      snprintf(num, sizeof num, "%d", row->value);
      break;
    }
    if (u->fields[i]) {
      set_text(u->fields[i], v);
    }
    i++;
  }
}

/* what the widget holds into its nodes (the kind known: drawn once) */
static void sync_one(UIW *u) {
  char buf[64];
  ctui_node_set_bool(u->shown_n, u->shown);
  snprintf(buf, sizeof buf, "%d %d %d %d", u->x, u->y, u->w, u->h);
  ctui_node_set_str(u->rect, buf);
  switch (u->kind) {
  case W_ENTRY: {
    const CTUI_ENTRY *e = u->obj;
    if (ctui_node_string_ok(ctui_node_type(u->text), e->buf, strlen(e->buf))) {
      ctui_node_set_str(u->text, e->buf);
    }
    ctui_node_set_int(u->cursor, (int64_t)e->cursor);
    break;
  }
  case W_TABLE: {
    const CTUI_TABLE *t = u->obj;
    int sel = t->count > 0 ? t->selected : -1;
    ctui_node_set_int(u->rows, t->count);
    ctui_node_set_int(u->selected, sel);
    ctui_node_set_int(u->top, t->scroll);
    char scratch[512];
    const char *txt = sel >= 0 && t->cell
                          ? t->cell(t->ctx, sel, 0, scratch, sizeof scratch)
                          : "";
    if (txt && ctui_node_string_ok(ctui_node_type(u->selected_text), txt,
                                   strlen(txt))) {
      ctui_node_set_str(u->selected_text, txt);
    }
    break;
  }
  case W_LIST: {
    const CTUI_LIST *l = u->obj;
    int sel = l->count > 0 ? l->selected : -1;
    ctui_node_set_int(u->rows, l->count);
    ctui_node_set_int(u->selected, sel);
    ctui_node_set_int(u->top, l->scroll_offset);
    const char *txt =
        sel >= 0 && l->items[sel].label ? l->items[sel].label : "";
    if (ctui_node_string_ok(ctui_node_type(u->selected_text), txt,
                            strlen(txt))) {
      ctui_node_set_str(u->selected_text, txt);
    }
    break;
  }
  case W_TABS: {
    const CTUI_TABS *t = u->obj;
    ctui_node_set_int(u->rows, t->count);
    ctui_node_set_int(u->selected, t->selected);
    const char *lab = t->selected >= 0 && t->selected < t->count &&
                              t->items[t->selected].label
                          ? t->items[t->selected].label
                          : "";
    if (ctui_node_string_ok(ctui_node_type(u->label), lab, strlen(lab))) {
      ctui_node_set_str(u->label, lab);
    }
    break;
  }
  case W_DIALOG: {
    const CTUI_DIALOG *d = u->obj;
    set_text(u->title, d->title);
    set_text(u->text, d->text);
    char b[512] = "";
    size_t o = 0;
    for (int i = 0; i < d->button_count && i < CTUI_DIALOG_MAX_BUTTONS; i++) {
      int k = snprintf(b + o, sizeof b - o, "%s%s", i ? "|" : "",
                       d->buttons[i] ? d->buttons[i] : "");
      if (k < 0 || (size_t)k >= sizeof b - o) {
        break;
      }
      o += (size_t)k;
    }
    set_text(u->buttons, b);
    set_text(u->focus,
             d->focus >= 0 && d->focus < d->button_count && d->buttons[d->focus]
                 ? d->buttons[d->focus]
                 : "");
    ctui_node_set_int(u->progress, d->progress);
    set_text(u->entry, d->entry ? d->entry->buf : "");
    break;
  }
  case W_POPMENU: {
    /* the level the keys go to: the deepest open */
    const CTUI_POPMENU *m = u->obj;
    ctui_node_set_int(u->depth, m->depth);
    char b[1024] = "", lab[128];
    const char *focus = "";
    int letter;
    if (m->depth > 0 && m->depth <= CTUI_POPMENU_DEPTH) {
      const CTUI_POPMENU_LEVEL *l = &m->level[m->depth - 1];
      size_t o = 0;
      for (int i = 0; i < l->count; i++) {
        if (l->items[i].flags & CTUI_POPMENU_SEP) {
          continue;
        }
        ctui_popmenu_label(l->items[i].label ? l->items[i].label : "", lab,
                           sizeof lab, &letter);
        int k = snprintf(b + o, sizeof b - o, "%s%s", o ? "|" : "", lab);
        if (k < 0 || (size_t)k >= sizeof b - o) {
          break;
        }
        o += (size_t)k;
      }
      if (l->sel >= 0 && l->sel < l->count) {
        ctui_popmenu_label(l->items[l->sel].label ? l->items[l->sel].label : "",
                           lab, sizeof lab, &letter);
        focus = lab;
      }
    }
    set_text(u->items, b);
    set_text(u->focus, focus);
    break;
  }
  case W_FORM: {
    const CTUI_FORM *f = u->obj;
    char name[48] = "";
    if (f->focus >= 0 && f->focus < f->count) {
      const CTUI_FORM_ROW *row = &f->rows[f->focus];
      if (row->kind == CTUI_FORM_BUTTON) {
        snprintf(name, sizeof name, "%s", row->label ? row->label : "");
      } else {
        field_name(f, f->focus, name, sizeof name);
      }
    }
    set_text(u->focus, name);
    sync_fields(u, f);
    break;
  }
  default:
    break;
  }
}

/* the leaves of what it was first drawn as */
static void publish_kind(UIW *u, const char *kind) {
  u->kind = kind_of(kind);
  u->kind_s = strdup(kind);
  u->rect = wnode(u, "rect", &TEXT, NULL);
  if (u->kind == W_ENTRY) {
    u->text = wnode(u, "text", &TEXT, &TEXT_RW);
    u->cursor = wnode(u, "cursor", &COUNT, NULL);
  } else if (u->kind == W_DIALOG) {
    u->title = wnode(u, "title", &TEXT, NULL);
    u->text = wnode(u, "text", &TEXT, NULL);
    u->buttons = wnode(u, "buttons", &TEXT, NULL);
    u->focus = wnode(u, "focus", &TEXT, NULL);
    u->progress = wnode(u, "progress", &COUNT, NULL);
    u->entry = wnode(u, "entry", &TEXT, NULL);
  } else if (u->kind == W_FORM) {
    u->focus = wnode(u, "focus", &TEXT, NULL);
  } else if (u->kind == W_POPMENU) {
    u->items = wnode(u, "items", &TEXT, NULL);
    u->focus = wnode(u, "focus", &TEXT, NULL);
    u->depth = wnode(u, "depth", &COUNT, NULL);
  } else if (u->kind != W_OTHER) {
    u->rows = wnode(u, "rows", &COUNT, NULL);
    u->selected = wnode(u, "selected", &COUNT, &ROW_RW);
    if (u->kind == W_TABS) {
      u->label = wnode(u, "label", &TEXT, NULL);
    } else {
      u->selected_text = wnode(u, "selected_text", &TEXT, NULL);
      u->top = wnode(u, "top", &COUNT, NULL);
    }
  }
  if (u->named) {
    ctui_logf(E_INF, "[CTUI:UI] - w/%s first drawn (a %s) @ tick %d\n", u->name,
              kind, ctui_tick_advance());
  }
}

/* u's nodes removed and u freed */
static void uiw_free(UIW *u) {
  fields_free(u);
  CTUI_NODE *ns[] = {u->shown_n,  u->rect,     u->text,          u->cursor,
                     u->rows,     u->selected, u->selected_text, u->top,
                     u->label,    u->title,    u->buttons,       u->focus,
                     u->progress, u->entry,    u->items,         u->depth};
  for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
    if (ns[i]) {
      ctui_node_remove(ns[i]);
    }
  }
  free(u->name);
  free(u->kind_s);
  free(u);
}

static UIW *uiw_new(CTUI_UI *ui, const char *name) {
  UIW **a = realloc(ui->ws, (size_t)(ui->nws + 1) * sizeof *a);
  if (!a) {
    return NULL;
  }
  ui->ws = a;
  UIW *u = calloc(1, sizeof *u);
  if (!u || !(u->name = strdup(name))) {
    free(u);
    return NULL;
  }
  u->ui = ui;
  ui->ws[ui->nws++] = u;
  return u;
}

/* a node path holds NAME's longest leaf, and NAME's last part isn't a
 * number (KIND/N is the unnamed ones') */
static int name_ok(const char *name) {
  char path[CTUI_NODE_PATH_MAX + 1];
  if ((size_t)snprintf(path, sizeof path, "ui/w/%s/selected_text", name) >=
          sizeof path ||
      !ctui_node_path_ok(path)) {
    return 0;
  }
  const char *last = strrchr(name, '/');
  last = last ? last + 1 : name;
  return last[strspn(last, "0123456789")] != '\0';
}

/* entry e is a part of a form or dialog drawn among the n draws (its
 * nodes say it: fields/NAME, entry) */
static int inside(const DRAWN *drawn, int n, const void *e) {
  for (int i = 0; i < n; i++) {
    if (strcmp(drawn[i].kind, "dialog") == 0 &&
        ((const CTUI_DIALOG *)drawn[i].obj)->entry == e) {
      return 1;
    }
    if (strcmp(drawn[i].kind, "form") == 0) {
      const CTUI_FORM *f = drawn[i].obj;
      for (int r = 0; r < f->count; r++) {
        if (f->rows[r].kind == CTUI_FORM_ENTRY && f->rows[r].entry == e) {
          return 1;
        }
      }
    }
  }
  return 0;
}

/* the last frame's draws matched to the widgets: the named ones shown or
 * not, the unnamed ones as KIND/N in the order drawn */
static void match_drawn(CTUI_UI *ui) {
  /* the draws are the last frame's, or it drew none of them */
  int n = ui->drawn_frame + 1 == ui->app->frames ? ui->ndrawn : 0;
  for (int i = 0; i < ui->nws; i++) {
    ui->ws[i]->shown = ui->ws[i]->seen = 0;
  }
  const char *kinds[KINDS_MAX];
  int counts[KINDS_MAX], nkinds = 0;
  for (int i = 0; i < n; i++) {
    const DRAWN *d = &ui->drawn[i];
    UIW *u = NULL;
    for (int k = 0; k < ui->nws && !u; k++) {
      if (ui->ws[k]->named && ui->ws[k]->obj == d->obj) {
        u = ui->ws[k];
      }
    }
    if (!u && strcmp(d->kind, "entry") == 0 && inside(ui->drawn, n, d->obj)) {
      continue;
    }
    if (!u) {
      int k = 0;
      while (k < nkinds && strcmp(kinds[k], d->kind) != 0) {
        k++;
      }
      if (k == nkinds) {
        if (nkinds == KINDS_MAX) {
          continue;
        }
        kinds[nkinds] = d->kind;
        counts[nkinds++] = 0;
      }
      char name[CTUI_NODE_PATH_MAX];
      snprintf(name, sizeof name, "%s/%d", d->kind, counts[k]++);
      u = find_name(ui, name, strlen(name));
      if (!u) {
        char path[CTUI_NODE_PATH_MAX + 1];
        if ((size_t)snprintf(path, sizeof path, "ui/w/%s/selected_text",
                             name) >= sizeof path ||
            !ctui_node_path_ok(path) || !(u = uiw_new(ui, name))) {
          continue;
        }
        u->shown_n = wnode(u, "shown", &FLAG, NULL);
        publish_kind(u, d->kind);
      }
      u->obj = d->obj;
    } else if (u->kind == W_NONE) {
      publish_kind(u, d->kind);
    }
    u->shown = u->seen = 1;
    u->x = d->x, u->y = d->y, u->w = d->w, u->h = d->h;
  }
  /* an unnamed one not drawn is gone (its struct may be too) */
  for (int i = 0; i < ui->nws;) {
    UIW *u = ui->ws[i];
    if (!u->named && !u->seen) {
      uiw_free(u);
      ui->ws[i] = ui->ws[--ui->nws];
    } else {
      i++;
    }
  }
}

void ctui_ui_widgets_sync(CTUI_UI *ui) {
  int fresh = ui->app->frames != ui->synced_frame;
  if (fresh) {
    ui->synced_frame = ui->app->frames;
    match_drawn(ui);
  }
  /* a widget's struct is read only right after a frame drew it: what
   * its draw read is there then (a closed picker's rows may be freed,
   * its table's count not reset); one not drawn keeps its last values */
  for (int i = 0; fresh && i < ui->nws; i++) {
    UIW *u = ui->ws[i];
    if (u->shown) {
      sync_one(u);
    } else {
      ctui_node_set_bool(u->shown_n, 0);
    }
  }
}

int ctui_ui_name(CTUI_UI *ui, void *obj, const char *name, unsigned flags,
                 CTUI_UI_CHANGED changed, void *user) {
  if (!ui) {
    return 0;
  }
  if ((flags & CTUI_UI_RDWR) && !changed) {
    ctui_logf(E_WRN, "[CTUI:UI] - w/%s: writable without changed() refused\n",
              name);
    return -1;
  }
  if (!name_ok(name)) {
    ctui_logf(E_WRN, "[CTUI:UI] - w/%s: a bad name refused\n", name);
    return -1;
  }
  for (int i = 0; i < ui->nws; i++) {
    const UIW *o = ui->ws[i];
    if (o->named && (strcmp(o->name, name) == 0 || o->obj == obj)) {
      ctui_logf(E_WRN, "[CTUI:UI] - w/%s: %s named already\n", name,
                o->obj == obj ? "its widget" : "the name");
      return -1;
    }
  }
  UIW *u = uiw_new(ui, name);
  if (!u) {
    return -1;
  }
  u->named = 1;
  u->obj = obj;
  u->flags = flags;
  u->changed = changed;
  u->user = user;
  ctui_nodes_begin(ui->ns);
  u->shown_n = wnode(u, "shown", &FLAG, NULL);
  ctui_nodes_commit(ui->ns);
  ctui_logf(E_INF, "[CTUI:UI] - w/%s named%s @ tick %d\n", name,
            flags & CTUI_UI_RDWR ? " (writable)" : "", ctui_tick_advance());
  return 0;
}

void ctui_ui_forget(CTUI_UI *ui, const char *name) {
  if (!ui) {
    return;
  }
  for (int i = 0; i < ui->nws; i++) {
    UIW *u = ui->ws[i];
    if (u->named && strcmp(u->name, name) == 0) {
      ctui_nodes_begin(ui->ns);
      uiw_free(u);
      ctui_nodes_commit(ui->ns);
      ui->ws[i] = ui->ws[--ui->nws];
      return;
    }
  }
}

void ctui_ui_widgets_free(CTUI_UI *ui) {
  if (ui->frame_soon) {
    ctui_timer_cancel(ui->frame_soon);
  }
  for (int i = 0; i < ui->nws; i++) {
    UIW *u = ui->ws[i];
    for (int k = 0; k < u->nfields; k++) {
      free(u->field_names[k]);
    }
    free(u->fields);
    free(u->field_names);
    free(u->name);
    free(u->kind_s);
    free(u);
  }
  free(ui->ws);
  free(ui->drawn);
}
