#include "vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ST_GROUND, ST_ESC, ST_ESC_INTER, ST_CSI, ST_STR, ST_STR_ESC };

enum { COL_BASIC, COL_256, COL_RGB };

/* a colour as SGR set it, fg and bg apart (a CTUI_CELL has one mode for
 * both: pen_to_cell() folds them) */
typedef struct {
  unsigned char kind, v, r, g, b;
} VT_COLOR;

#define VT_PARAMS 16
#define VT_TEXT 256
#define VT_STR 64

struct CTUI_VT {
  int rows, cols;
  CTUI_CELL *cells;
  int r, c;
  /* the last column was just written: the next glyph goes to the next
   * row first (a terminal's deferred wrap) */
  int wrap;
  VT_COLOR fg, bg;
  unsigned char attr;

  int state;
  int params[VT_PARAMS], nparams;
  char priv, inter;

  /* printable bytes not placed yet: a cluster is only known whole once
   * something not joining it follows */
  char text[VT_TEXT];
  size_t ntext;

  /* a string's (APC, OSC, ...) kind and first bytes */
  char str_kind;
  char str[VT_STR];
  size_t nstr;

  int shm;
  char *reply;
  size_t nreply, reply_cap;

  char *row_buf;
};

/* xterm's 16 colours, then the 6x6x6 cube and the greys */
static void color_rgb(const VT_COLOR *col, int fg, unsigned char out[3]) {
  static const unsigned char base[16][3] = {
      {0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
      {0, 0, 238},     {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
      {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
      {92, 92, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
  if (col->kind == COL_RGB) {
    out[0] = col->r, out[1] = col->g, out[2] = col->b;
    return;
  }
  int i = col->v;
  if (col->kind == COL_BASIC) {
    i = col->v == CTUI_COLOR_DEFAULT ? (fg ? 7 : 0) : col->v - 1;
  }
  if (i < 16) {
    memcpy(out, base[i], 3);
  } else if (i < 232) {
    static const unsigned char steps[6] = {0, 95, 135, 175, 215, 255};
    i -= 16;
    out[0] = steps[i / 36], out[1] = steps[i / 6 % 6], out[2] = steps[i % 6];
  } else {
    out[0] = out[1] = out[2] = (unsigned char)(8 + (i - 232) * 10);
  }
}

/* the pen as one cell's colours: the four shapes ctui's renderer sends
 * map back to the cell it drew from; any other mix (never from ctui)
 * becomes 256 colours, or RGB once either side is RGB */
static void pen_to_cell(const CTUI_VT *vt, CTUI_CELL *cell) {
  const VT_COLOR *fg = &vt->fg, *bg = &vt->bg;
  cell->attr = vt->attr;
  cell->fg_r = cell->fg_g = cell->fg_b = 0;
  cell->bg_r = cell->bg_g = cell->bg_b = 0;
  cell->fg = cell->bg = 0;
  if (fg->kind == COL_BASIC && bg->kind == COL_BASIC) {
    cell->color_mode = CTUI_COLOR_MODE_BASIC;
    cell->fg = fg->v, cell->bg = bg->v;
  } else if (fg->kind == COL_RGB && bg->kind == COL_BASIC) {
    cell->color_mode = CTUI_COLOR_MODE_RGB_FG;
    cell->fg_r = fg->r, cell->fg_g = fg->g, cell->fg_b = fg->b;
    cell->bg = bg->v;
  } else if (fg->kind != COL_RGB && bg->kind != COL_RGB) {
    cell->color_mode = CTUI_COLOR_MODE_256;
    cell->fg = fg->kind == COL_256           ? fg->v
               : fg->v == CTUI_COLOR_DEFAULT ? 7
                                             : (unsigned char)(fg->v - 1);
    cell->bg = bg->kind == COL_256           ? bg->v
               : bg->v == CTUI_COLOR_DEFAULT ? 0
                                             : (unsigned char)(bg->v - 1);
  } else {
    unsigned char f[3], b[3];
    color_rgb(fg, 1, f);
    color_rgb(bg, 0, b);
    cell->color_mode = CTUI_COLOR_MODE_RGB;
    cell->fg_r = f[0], cell->fg_g = f[1], cell->fg_b = f[2];
    cell->bg_r = b[0], cell->bg_g = b[1], cell->bg_b = b[2];
  }
}

/* erased cells take the pen's colours (kitty's background colour erase),
 * no attributes */
static void blank(const CTUI_VT *vt, CTUI_CELL *cell) {
  pen_to_cell(vt, cell);
  cell->attr = 0;
  cell->ch = ' ';
  cell->kitty_row = 0;
}

static CTUI_CELL *row_at(const CTUI_VT *vt, int r) {
  return vt->cells + (size_t)r * (size_t)vt->cols;
}

static void erase(CTUI_VT *vt, int r, int c0, int c1) {
  CTUI_CELL *row = row_at(vt, r);
  /* half a wide glyph erased: its other half goes too */
  if (c0 > 0 && row[c0].ch == CTUI_CELL_CONT) {
    row[c0 - 1].ch = ' ';
  }
  if (c1 < vt->cols && row[c1].ch == CTUI_CELL_CONT) {
    row[c1].ch = ' ';
  }
  for (int c = c0; c < c1; c++) {
    blank(vt, &row[c]);
  }
}

static void erase_rows(CTUI_VT *vt, int r0, int r1) {
  for (int r = r0; r < r1; r++) {
    erase(vt, r, 0, vt->cols);
  }
}

static void linefeed(CTUI_VT *vt) {
  if (vt->r < vt->rows - 1) {
    vt->r++;
    return;
  }
  memmove(vt->cells, row_at(vt, 1),
          sizeof(CTUI_CELL) * (size_t)(vt->rows - 1) * (size_t)vt->cols);
  erase(vt, vt->rows - 1, 0, vt->cols);
}

static void move_to(CTUI_VT *vt, int r, int c) {
  vt->r = r < 0 ? 0 : r >= vt->rows ? vt->rows - 1 : r;
  vt->c = c < 0 ? 0 : c >= vt->cols ? vt->cols - 1 : c;
  vt->wrap = 0;
}

static void put(CTUI_VT *vt, uint32_t ch, int w) {
  if (w == 0) {
    return; /* nothing before it in this run to join */
  }
  if (vt->wrap) {
    vt->c = 0;
    vt->wrap = 0;
    linefeed(vt);
  }
  CTUI_CELL *row = row_at(vt, vt->r);
  if (w == 2 && vt->c == vt->cols - 1) {
    /* a wide glyph never straddles the edge: the last cell stays blank */
    ctui_cell_set_ch(row, vt->cols, vt->c, vt->cols, ' ');
    pen_to_cell(vt, &row[vt->c]);
    vt->c = 0;
    linefeed(vt);
    row = row_at(vt, vt->r);
  }
  int used = ctui_cell_set_ch(row, vt->cols, vt->c, vt->cols, ch);
  for (int i = 0; i < used; i++) {
    pen_to_cell(vt, &row[vt->c + i]);
    row[vt->c + i].kitty_row = 0;
  }
  vt->c += used;
  if (vt->c >= vt->cols) {
    vt->c = vt->cols - 1;
    vt->wrap = 1;
  }
}

/* bytes at the end of s that start a UTF-8 character not complete yet */
static size_t utf8_cut(const char *s, size_t n) {
  for (size_t back = 1; back <= 3 && back <= n; back++) {
    unsigned char b = (unsigned char)s[n - back];
    if ((b & 0xC0) == 0x80) {
      continue;
    }
    size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    return need > back ? back : 0;
  }
  return 0;
}

/* places the pending text; keep_last leaves its last cluster (and a cut
 * UTF-8 character) pending, as more of it may follow */
static void flush_text(CTUI_VT *vt, int keep_last) {
  size_t end = vt->ntext;
  if (keep_last) {
    end -= utf8_cut(vt->text, end);
  }
  size_t i = 0;
  while (i < end) {
    uint32_t ch;
    int w;
    size_t len = ctui_utf8_cluster(vt->text + i, end - i, &ch, &w);
    if (len == 0) {
      break;
    }
    if (keep_last && i + len == end) {
      break;
    }
    put(vt, ch, w);
    i += len;
  }
  memmove(vt->text, vt->text + i, vt->ntext - i);
  vt->ntext -= i;
}

static void reply(CTUI_VT *vt, const char *s, size_t n) {
  if (vt->nreply + n > vt->reply_cap) {
    size_t cap = (vt->nreply + n) * 2;
    char *p = realloc(vt->reply, cap);
    if (p == NULL) {
      return;
    }
    vt->reply = p;
    vt->reply_cap = cap;
  }
  memcpy(vt->reply + vt->nreply, s, n);
  vt->nreply += n;
}

/* kitty's graphics query for the shm transport: "Ga=q,...,t=s,...;" */
static void string_done(CTUI_VT *vt) {
  if (!vt->shm || vt->str_kind != '_' || vt->nstr < 2 || vt->str[0] != 'G') {
    return;
  }
  char head[VT_STR + 2] = ",";
  size_t n = 0;
  while (n < vt->nstr - 1 && vt->str[1 + n] != ';') {
    n++;
  }
  memcpy(head + 1, vt->str + 1, n);
  head[1 + n] = ',';
  head[2 + n] = '\0';
  char *id = strstr(head, ",i=");
  if (!strstr(head, ",a=q,") || !strstr(head, ",t=s,") || id == NULL) {
    return;
  }
  id += 3;
  char out[48];
  size_t len = 0;
  len += (size_t)snprintf(out, sizeof out, "\x1b_Gi=");
  while (*id >= '0' && *id <= '9' && len < 24) {
    out[len++] = *id++;
  }
  memcpy(out + len, ";OK\x1b\\", 6);
  reply(vt, out, len + 5);
}

static int param(const CTUI_VT *vt, int i, int def) {
  return i < vt->nparams && vt->params[i] > 0 ? vt->params[i] : def;
}

static void sgr_color(const CTUI_VT *vt, int *i, VT_COLOR *col) {
  int k = *i;
  if (k + 2 < vt->nparams && vt->params[k + 1] == 5) {
    *col = (VT_COLOR){.kind = COL_256, .v = (unsigned char)vt->params[k + 2]};
    *i = k + 2;
  } else if (k + 4 < vt->nparams && vt->params[k + 1] == 2) {
    *col = (VT_COLOR){.kind = COL_RGB,
                      .r = (unsigned char)vt->params[k + 2],
                      .g = (unsigned char)vt->params[k + 3],
                      .b = (unsigned char)vt->params[k + 4]};
    *i = k + 4;
  } else {
    *i = vt->nparams;
  }
}

static void sgr(CTUI_VT *vt) {
  static const VT_COLOR def = {COL_BASIC, CTUI_COLOR_DEFAULT, 0, 0, 0};
  if (vt->nparams == 0) {
    vt->fg = vt->bg = def;
    vt->attr = 0;
    return;
  }
  for (int i = 0; i < vt->nparams; i++) {
    int p = vt->params[i];
    if (p == 0) {
      vt->fg = vt->bg = def;
      vt->attr = 0;
    } else if (p == 1) {
      vt->attr |= CTUI_ATTR_BOLD;
    } else if (p == 2) {
      vt->attr |= CTUI_ATTR_DIM;
    } else if (p == 3) {
      vt->attr |= CTUI_ATTR_ITALIC;
    } else if (p == 4) {
      vt->attr |= CTUI_ATTR_UNDERLINE;
    } else if (p == 9) {
      vt->attr |= CTUI_ATTR_STRIKE;
    } else if (p == 22) {
      vt->attr &= (unsigned char)~(CTUI_ATTR_BOLD | CTUI_ATTR_DIM);
    } else if (p == 23) {
      vt->attr &= (unsigned char)~CTUI_ATTR_ITALIC;
    } else if (p == 24) {
      vt->attr &= (unsigned char)~CTUI_ATTR_UNDERLINE;
    } else if (p == 29) {
      vt->attr &= (unsigned char)~CTUI_ATTR_STRIKE;
    } else if (p >= 30 && p <= 37) {
      vt->fg = (VT_COLOR){COL_BASIC, (unsigned char)(p - 29), 0, 0, 0};
    } else if (p >= 40 && p <= 47) {
      vt->bg = (VT_COLOR){COL_BASIC, (unsigned char)(p - 39), 0, 0, 0};
    } else if (p >= 90 && p <= 97) {
      vt->fg = (VT_COLOR){COL_256, (unsigned char)(p - 82), 0, 0, 0};
    } else if (p >= 100 && p <= 107) {
      vt->bg = (VT_COLOR){COL_256, (unsigned char)(p - 92), 0, 0, 0};
    } else if (p == 39) {
      vt->fg = def;
    } else if (p == 49) {
      vt->bg = def;
    } else if (p == 38) {
      sgr_color(vt, &i, &vt->fg);
    } else if (p == 48) {
      sgr_color(vt, &i, &vt->bg);
    }
  }
}

static void csi(CTUI_VT *vt, char final) {
  if (vt->priv == '?') {
    if ((final == 'h' || final == 'l') &&
        (param(vt, 0, 0) == 1049 || param(vt, 0, 0) == 1047 ||
         param(vt, 0, 0) == 47)) {
      /* one buffer: the screen switched to is blank either way (a fresh
       * pty's main screen never had anything on it) */
      erase_rows(vt, 0, vt->rows);
      if (param(vt, 0, 0) == 1049) {
        move_to(vt, 0, 0);
      }
    }
    return;
  }
  if (vt->priv || vt->inter) {
    return;
  }
  switch (final) {
  case 'H':
  case 'f':
    move_to(vt, param(vt, 0, 1) - 1, param(vt, 1, 1) - 1);
    break;
  case 'A':
    move_to(vt, vt->r - param(vt, 0, 1), vt->c);
    break;
  case 'B':
    move_to(vt, vt->r + param(vt, 0, 1), vt->c);
    break;
  case 'C':
    move_to(vt, vt->r, vt->c + param(vt, 0, 1));
    break;
  case 'D':
    move_to(vt, vt->r, vt->c - param(vt, 0, 1));
    break;
  case 'G':
    move_to(vt, vt->r, param(vt, 0, 1) - 1);
    break;
  case 'd':
    move_to(vt, param(vt, 0, 1) - 1, vt->c);
    break;
  case 'J': {
    int how = vt->nparams ? vt->params[0] : 0;
    if (how == 0) {
      erase(vt, vt->r, vt->c, vt->cols);
      erase_rows(vt, vt->r + 1, vt->rows);
    } else if (how == 1) {
      erase_rows(vt, 0, vt->r);
      erase(vt, vt->r, 0, vt->c + 1);
    } else if (how == 2 || how == 3) {
      erase_rows(vt, 0, vt->rows);
    }
    break;
  }
  case 'K': {
    int how = vt->nparams ? vt->params[0] : 0;
    if (how == 0) {
      erase(vt, vt->r, vt->c, vt->cols);
    } else if (how == 1) {
      erase(vt, vt->r, 0, vt->c + 1);
    } else if (how == 2) {
      erase(vt, vt->r, 0, vt->cols);
    }
    break;
  }
  case 'm':
    sgr(vt);
    break;
  default:
    break;
  }
}

static void control(CTUI_VT *vt, unsigned char b) {
  switch (b) {
  case '\r':
    vt->c = 0;
    vt->wrap = 0;
    break;
  case '\n':
  case '\v':
  case '\f':
    linefeed(vt);
    vt->c = 0;
    vt->wrap = 0;
    break;
  case '\b':
    move_to(vt, vt->r, vt->c - 1);
    break;
  case '\t':
    move_to(vt, vt->r, (vt->c / 8 + 1) * 8);
    break;
  default:
    break;
  }
}

static void byte(CTUI_VT *vt, unsigned char b) {
  switch (vt->state) {
  case ST_GROUND:
    if (b == 0x1b) {
      flush_text(vt, 0);
      vt->state = ST_ESC;
    } else if (b < 0x20 || b == 0x7f) {
      flush_text(vt, 0);
      control(vt, b);
    } else {
      if (vt->ntext == VT_TEXT) {
        flush_text(vt, 1);
        if (vt->ntext == VT_TEXT) {
          flush_text(vt, 0); /* one cluster this long: cut it */
        }
      }
      vt->text[vt->ntext++] = (char)b;
    }
    return;
  case ST_ESC:
    if (b == '[') {
      vt->state = ST_CSI;
      vt->nparams = 0;
      vt->priv = vt->inter = 0;
    } else if (b == ']' || b == 'P' || b == '_' || b == '^' || b == 'X') {
      vt->state = ST_STR;
      vt->str_kind = (char)b;
      vt->nstr = 0;
    } else if (b >= 0x20 && b <= 0x2f) {
      vt->state = ST_ESC_INTER;
    } else if (b == 0x1b) {
      vt->state = ST_ESC;
    } else {
      vt->state = ST_GROUND;
    }
    return;
  case ST_ESC_INTER:
    if (b < 0x20 || b > 0x2f) {
      vt->state = ST_GROUND;
    }
    return;
  case ST_CSI:
    if (b >= '0' && b <= '9') {
      if (vt->nparams == 0) {
        vt->nparams = 1;
        vt->params[0] = 0;
      }
      int *p = &vt->params[vt->nparams - 1];
      if (*p < 100000) {
        *p = *p * 10 + (b - '0');
      }
    } else if (b == ';' || b == ':') {
      /* sub-parameters (38:2::r:g:b) read as parameters: ctui sends ';' */
      if (vt->nparams == 0) {
        vt->nparams = 1;
        vt->params[0] = 0;
      }
      if (vt->nparams < VT_PARAMS) {
        vt->params[vt->nparams++] = 0;
      }
    } else if (b >= 0x3c && b <= 0x3f) {
      if (!vt->priv) {
        vt->priv = (char)b;
      }
    } else if (b >= 0x20 && b <= 0x2f) {
      vt->inter = (char)b;
    } else if (b >= 0x40 && b <= 0x7e) {
      vt->state = ST_GROUND;
      csi(vt, (char)b);
    } else if (b == 0x1b) {
      vt->state = ST_ESC;
    } else if (b < 0x20) {
      control(vt, b);
    }
    return;
  case ST_STR:
    if (b == 0x07) {
      vt->state = ST_GROUND;
      string_done(vt);
    } else if (b == 0x1b) {
      vt->state = ST_STR_ESC;
    } else if (vt->nstr < VT_STR) {
      vt->str[vt->nstr++] = (char)b;
    }
    return;
  case ST_STR_ESC:
    string_done(vt);
    vt->state = ST_GROUND;
    if (b != '\\') {
      /* ESC ended the string and starts a sequence of its own */
      vt->state = ST_ESC;
      byte(vt, b);
    }
    return;
  default:
    vt->state = ST_GROUND;
    return;
  }
}

void ctui_vt_feed(CTUI_VT *vt, const char *buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    byte(vt, (unsigned char)buf[i]);
  }
  if (vt->state == ST_GROUND) {
    flush_text(vt, 1);
  }
}

CTUI_VT *ctui_vt_new(int rows, int cols) {
  if (rows < 1 || cols < 1) {
    return NULL;
  }
  CTUI_VT *vt = calloc(1, sizeof *vt);
  if (vt == NULL) {
    return NULL;
  }
  vt->cells = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));
  if (vt->cells == NULL) {
    free(vt);
    return NULL;
  }
  vt->rows = rows;
  vt->cols = cols;
  erase_rows(vt, 0, rows);
  return vt;
}

void ctui_vt_free(CTUI_VT *vt) {
  if (vt == NULL) {
    return;
  }
  free(vt->cells);
  free(vt->reply);
  free(vt->row_buf);
  free(vt);
}

void ctui_vt_resize(CTUI_VT *vt, int rows, int cols) {
  if (rows < 1 || cols < 1 || (rows == vt->rows && cols == vt->cols)) {
    return;
  }
  flush_text(vt, 0);
  CTUI_CELL *cells = calloc((size_t)rows * (size_t)cols, sizeof(CTUI_CELL));
  if (cells == NULL) {
    return;
  }
  CTUI_CELL fill;
  blank(vt, &fill);
  fill.color_mode = CTUI_COLOR_MODE_BASIC;
  fill.fg = fill.bg = CTUI_COLOR_DEFAULT;
  for (int r = 0; r < rows; r++) {
    CTUI_CELL *to = cells + (size_t)r * (size_t)cols;
    for (int c = 0; c < cols; c++) {
      to[c] = r < vt->rows && c < vt->cols ? row_at(vt, r)[c] : fill;
    }
    if (cols < vt->cols && r < vt->rows &&
        row_at(vt, r)[cols].ch == CTUI_CELL_CONT) {
      to[cols - 1].ch = ' '; /* its right half was cut off */
    }
  }
  free(vt->cells);
  free(vt->row_buf);
  vt->row_buf = NULL;
  vt->cells = cells;
  vt->rows = rows;
  vt->cols = cols;
  move_to(vt, vt->r, vt->c);
}

int ctui_vt_rows(const CTUI_VT *vt) { return vt->rows; }
int ctui_vt_cols(const CTUI_VT *vt) { return vt->cols; }

const CTUI_CELL *ctui_vt_cell(CTUI_VT *vt, int row, int col) {
  if (row < 0 || row >= vt->rows || col < 0 || col >= vt->cols) {
    return NULL;
  }
  if (vt->state == ST_GROUND) {
    flush_text(vt, 0);
  }
  return &row_at(vt, row)[col];
}

#define ROW_MAX(cols) ((size_t)(cols) * CTUI_CLUSTER_MAX * 4 + 1)

/* row r's text into out (ROW_MAX bytes), trailing blanks dropped; returns
 * its length */
static size_t row_text(const CTUI_VT *vt, int r, char *out) {
  const CTUI_CELL *row = row_at(vt, r);
  size_t len = 0, keep = 0;
  for (int c = 0; c < vt->cols; c++) {
    if (row[c].ch == CTUI_CELL_CONT) {
      continue;
    }
    len += (size_t)ctui_cell_encode(row[c].ch, out + len);
    if (row[c].ch != ' ') {
      keep = len;
    }
  }
  out[keep] = '\0';
  return keep;
}

char *ctui_vt_text(CTUI_VT *vt) {
  if (vt->state == ST_GROUND) {
    flush_text(vt, 0);
  }
  char *out = malloc(ROW_MAX(vt->cols) * (size_t)vt->rows);
  if (out == NULL) {
    return NULL;
  }
  size_t len = 0;
  for (int r = 0; r < vt->rows; r++) {
    len += row_text(vt, r, out + len);
    out[len++] = '\n';
  }
  out[len - 1] = '\0';
  return out;
}

static int color_name(char *out, int mode_rgb, int mode_256, unsigned char v,
                      unsigned char r, unsigned char g, unsigned char b) {
  if (mode_rgb) {
    return sprintf(out, "#%02x%02x%02x", r, g, b);
  }
  if (mode_256) {
    return sprintf(out, "@%d", v);
  }
  return sprintf(out, "%c", v <= CTUI_COLOR_WHITE ? "dkrgybmcw"[v] : '?');
}

/* a cell's STYLE (ctui_vt_styles()) into out (32 bytes) */
static void style_of(const CTUI_CELL *cell, char *out) {
  int blank = cell->ch == ' ';
  int m = cell->color_mode;
  int n = blank ? sprintf(out, "d")
                : color_name(out,
                             m == CTUI_COLOR_MODE_RGB ||
                                 m == CTUI_COLOR_MODE_RGB_FG,
                             m == CTUI_COLOR_MODE_256, cell->fg, cell->fg_r,
                             cell->fg_g, cell->fg_b);
  out[n++] = '/';
  n += color_name(out + n, m == CTUI_COLOR_MODE_RGB, m == CTUI_COLOR_MODE_256,
                  cell->bg, cell->bg_r, cell->bg_g, cell->bg_b);
  unsigned char attr = cell->attr;
  if (blank) {
    attr &= CTUI_ATTR_UNDERLINE | CTUI_ATTR_STRIKE;
  }
  if (attr) {
    out[n++] = '+';
    for (int i = 0; i < 5; i++) {
      if (attr & (1 << i)) {
        out[n++] = "BDIUS"[i];
      }
    }
  }
  out[n] = '\0';
}

char *ctui_vt_styles(CTUI_VT *vt) {
  if (vt->state == ST_GROUND) {
    flush_text(vt, 0);
  }
  /* a row at worst: a change every cell, "COL #rrggbb/#rrggbb+BDIUS " */
  size_t row_max = 16 + (size_t)vt->cols * 32;
  char *out = malloc(row_max * (size_t)vt->rows + 1);
  if (out == NULL) {
    return NULL;
  }
  size_t len = 0;
  for (int r = 0; r < vt->rows; r++) {
    const CTUI_CELL *row = row_at(vt, r);
    char last[32] = "d/d", cur[32];
    size_t start = len;
    len += (size_t)sprintf(out + len, "%d:", r);
    int any = 0;
    for (int c = 0; c < vt->cols; c++) {
      if (row[c].ch == CTUI_CELL_CONT) {
        continue;
      }
      style_of(&row[c], cur);
      if (strcmp(cur, last) != 0) {
        len += (size_t)sprintf(out + len, " %d %s", c, cur);
        memcpy(last, cur, sizeof last);
        any = 1;
      }
    }
    if (any) {
      out[len++] = '\n';
    } else {
      len = start;
    }
  }
  out[len] = '\0';
  return out;
}

int ctui_vt_find(CTUI_VT *vt, const char *needle) {
  if (vt->state == ST_GROUND) {
    flush_text(vt, 0);
  }
  if (vt->row_buf == NULL) {
    vt->row_buf = malloc(ROW_MAX(vt->cols));
    if (vt->row_buf == NULL) {
      return -1;
    }
  }
  for (int r = 0; r < vt->rows; r++) {
    row_text(vt, r, vt->row_buf);
    if (strstr(vt->row_buf, needle)) {
      return r;
    }
  }
  return -1;
}

void ctui_vt_answer_kitty_shm(CTUI_VT *vt, int on) { vt->shm = on; }

const char *ctui_vt_replies(CTUI_VT *vt, size_t *n) {
  *n = vt->nreply;
  if (vt->nreply == 0) {
    return NULL;
  }
  vt->nreply = 0;
  return vt->reply;
}
