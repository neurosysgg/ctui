#include "control.h"

#include <limits.h>

#include "paint.h"

#include <string.h>

static int min2(int a, int b) { return a < b ? a : b; }
static int max2(int a, int b) { return a > b ? a : b; }
static int clamp(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

/* value of max as a share of len px, rounded */
static int share(int value, int max, int len) {
  if (max <= 0 || len <= 0) {
    return 0;
  }
  value = clamp(value, 0, max);
  return (int)(((long long)value * len + max / 2) / max);
}

/* --- geometry, shared by snap and paint --- */

static int slider_thumb(const CTUI_LOOK *l, int w, int h) {
  int tw = l->thumb ? l->thumb : max2(5, (h / 2) | 1);
  return min2(tw, w);
}

/* a bar's room above and below inside a cell-high box */
static int margin(int h) { return h >= 12 ? h / 6 : 0; }

/* a level's fill area: x from *x0, *len px */
static void level_area(const CTUI_LOOK *l, int w, int *x0, int *len) {
  int pad = l->chunk ? 1 : 0;
  *x0 = l->groove + pad;
  *len = max2(0, w - 2 * *x0);
}

static int signal_bars(int max) { return max <= 0 ? 4 : min2(max, 8); }

/* a meter's LEDs: inside its well's edge (*x0 in from each side), *n of
 * them *seg px wide with a px between */
static void meter_leds(const CTUI_LOOK *l, int w, int h, int *x0, int *seg,
                       int *n) {
  *x0 = max2(l->groove, 1) + 1;
  *seg = max2(2, h / 10);
  *n = max2(0, (w - 2 * *x0 + 1) / (*seg + 1));
}

/* a graph's points: one each step px, inside a 1 px edge */
/* a graph's points apart in px: as many as span says across the well,
 * else Task Manager's (a tenth of the height) */
static int graph_step(const CTUI_LOOK_CONTROL *c, int w, int h) {
  return c->span > 0 ? max2(1, (w - 2) / c->span) : max2(2, h / 10);
}

/* a scrollbar's length along its axis and its thumb's smallest length */
static void scroll_axis(int w, int h, int *len, int *least) {
  int vertical = h >= w;
  *len = vertical ? h : w;
  *least = min2(*len, vertical ? w : h);
}

static int scroll_thumb(const CTUI_LOOK_CONTROL *c, int len, int least) {
  if (c->max <= 0 || c->span >= c->max) {
    return len;
  }
  return clamp(share(c->span, c->max, len), least, len);
}

void ctui_look_control_snap(CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l, int w,
                            int h) {
  switch (c->kind) {
  case CTUI_LOOK_CTL_SLIDER: {
    int travel = w - slider_thumb(l, w, h);
    c->value = share(c->value, c->max, travel);
    c->max = travel > 0 ? travel : 1;
    c->span = 0;
    break;
  }
  case CTUI_LOOK_CTL_LEVEL: {
    int x0, len;
    level_area(l, w, &x0, &len);
    c->value = share(c->value, c->max, len);
    c->max = len > 0 ? len : 1;
    c->span = 0;
    if (l->chunk > 0) {
      /* chunks: the end of the last one shown (paint.h's rule), so a
       * progress bar makes a new image per chunk, not per pixel */
      int step = l->chunk + l->gap, shown = 0;
      for (int at = 0; at < len; at += step) {
        int cw = min2(l->chunk, len - at);
        if (c->value < at + (cw + 1) / 2) {
          break;
        }
        shown = at + cw;
      }
      c->value = shown;
    }
    break;
  }
  case CTUI_LOOK_CTL_METER: {
    int x0, seg, n;
    meter_leds(l, h > w ? h : w, h > w ? w : h, &x0, &seg, &n);
    int lit = share(c->value, c->max, n);
    int held = c->span > 0 ? max2(1, share(c->span, c->max, n)) : 0;
    c->value = lit;
    c->span = held > lit ? held : 0;
    c->max = n;
    break;
  }
  case CTUI_LOOK_CTL_SIGNAL:
    c->max = signal_bars(c->max);
    c->value = clamp(c->value, 0, c->max);
    c->span = 0;
    break;
  case CTUI_LOOK_CTL_SCROLL: {
    int len, least;
    scroll_axis(w, h, &len, &least);
    int tl = scroll_thumb(c, len, least);
    int room = c->max - c->span;
    c->value = room > 0 ? share(c->value, room, len - tl) : 0;
    c->span = tl;
    c->max = len;
    break;
  }
  case CTUI_LOOK_CTL_PICTO:
    ctui_look_picto_snap(c->picto, !!(c->flags & CTUI_LOOK_CTL_OFF), &c->value,
                         &c->max);
    c->span = 0;
    break;
  case CTUI_LOOK_CTL_BOX:
    c->value = c->max = 0;
    c->span = clamp(c->span, 1, w > 0 ? w : 1);
    c->box = (uint8_t)(c->box < CTUI_LOOK_BOXES ? c->box : 0);
    c->sides &= 0xf;
    c->place &= 0xff;
    c->fill = c->box < CTUI_LOOK_BOX_INFO && (c->fill & CTUI_LOOK_CTL_TINT)
                  ? c->fill & 0x1ffffffu
                  : 0;
    break;
  case CTUI_LOOK_CTL_BUTTON:
    /* a toolbar button's pictogram: the px of the height it takes */
    c->value = c->picto != CTUI_LOOK_PICTO_NONE && c->max > 0
                   ? share(c->value, c->max, h)
                   : 0;
    c->max = c->value ? h : 0;
    c->span = clamp(c->span, 0, h / 4);
    break;
  case CTUI_LOOK_CTL_SPLITTER:
    /* dragged or not: nothing else shows */
    c->value = c->max = c->span = 0;
    c->flags &= CTUI_LOOK_CTL_PRESSED;
    break;
  case CTUI_LOOK_CTL_BAR:
    /* only its raised sides show */
    c->value = c->max = c->span = 0;
    c->flags = 0;
    c->sides &= 0xf;
    break;
  case CTUI_LOOK_CTL_GRAPH:
    /* the span giving the same step (the most points that step fits):
     * spans spacing the points alike look alike */
    c->value = c->max = 0;
    c->span = c->span > 0 ? max2(1, (w - 2) / graph_step(c, w, h)) : 0;
    break;
  default:
    c->value = c->max = c->span = 0;
    break;
  }
  if (c->kind == CTUI_LOOK_CTL_BUTTON) {
    /* a toolbar button's pictogram may be tinted, the rest as they are */
    if (!c->max || !(c->tint & CTUI_LOOK_CTL_TINT) ||
        (c->flags & CTUI_LOOK_CTL_OFF)) {
      c->tint = 0;
    }
  } else if (c->kind == CTUI_LOOK_CTL_BOX) {
    c->picto = CTUI_LOOK_PICTO_NONE;
    c->tint = c->tint & CTUI_LOOK_CTL_TINT ? c->tint : 0;
  } else if (c->kind == CTUI_LOOK_CTL_CHIP) {
    c->picto = CTUI_LOOK_PICTO_NONE;
    c->tint = (c->tint & CTUI_LOOK_CTL_TINT) && !(c->flags & CTUI_LOOK_CTL_OFF)
                  ? c->tint
                  : 0;
  } else if (c->kind != CTUI_LOOK_CTL_PICTO) {
    c->picto = CTUI_LOOK_PICTO_NONE;
    c->tint = 0;
  } else if (!(c->tint & CTUI_LOOK_CTL_TINT) ||
             (c->flags & CTUI_LOOK_CTL_OFF)) {
    c->tint = 0; /* off paints it all disabled: the colour doesn't show */
  }
  /* what a kind doesn't draw mustn't split its cache entries */
  int button = c->kind == CTUI_LOOK_CTL_BUTTON ||
               c->kind == CTUI_LOOK_CTL_CHIP || c->kind == CTUI_LOOK_CTL_FIELD;
  int check = c->kind == CTUI_LOOK_CTL_CHECK ||
              c->kind == CTUI_LOOK_CTL_TOGGLE ||
              c->kind == CTUI_LOOK_CTL_RADIO || c->kind == CTUI_LOOK_CTL_METER;
  if (c->kind != CTUI_LOOK_CTL_GRAPH) {
    c->samples = c->samples2 = NULL;
    c->count = 0;
  }
  if (!button) {
    c->glyph = CTUI_LOOK_GLYPH_NONE;
  }
  if (c->kind != CTUI_LOOK_CTL_CHECK && c->kind != CTUI_LOOK_CTL_RADIO &&
      (c->kind != CTUI_LOOK_CTL_BUTTON || c->picto != CTUI_LOOK_PICTO_NONE)) {
    c->flags &= ~(unsigned)CTUI_LOOK_CTL_FOCUSED;
  }
  if ((!button && c->kind != CTUI_LOOK_CTL_SPLITTER) ||
      c->kind == CTUI_LOOK_CTL_FIELD) {
    c->flags &= ~(unsigned)CTUI_LOOK_CTL_PRESSED;
  }
  if (!(c->under & CTUI_LOOK_CTL_TINT)) {
    c->under = 0;
  }
  if (!l->panel || c->under || c->kind == CTUI_LOOK_CTL_BOX) {
    c->flags &= ~(unsigned)CTUI_LOOK_CTL_BARE; /* changes nothing then */
  }
  if (c->kind != CTUI_LOOK_CTL_CHIP) {
    c->flags &= ~(unsigned)CTUI_LOOK_CTL_HOVER;
  }
  if (c->kind != CTUI_LOOK_CTL_BOX) {
    c->box = c->place = 0;
    c->sides = c->kind == CTUI_LOOK_CTL_BAR ? c->sides : 0;
    c->fill = 0;
  }
  if (c->kind != CTUI_LOOK_CTL_BOX || !(c->place & CTUI_LOOK_BOX_TOP) ||
      !c->gap_cols) {
    c->gap = c->gap_cols = 0;
  }
  if (!check && c->kind != CTUI_LOOK_CTL_BUTTON) {
    c->flags &= ~(unsigned)CTUI_LOOK_CTL_CHECKED;
  }
}

/* bumped when a control's pixels change for the same state and size: its
 * images outlive a build in drawn.h's cache, named by this key (2: the
 * 24 px pictograms; 3: a disabled check box greyed inside) */
#define ART_VERSION 3u

uint64_t ctui_look_control_key(const CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l,
                               int w, int h) {
  CTUI_LOOK_CONTROL s = *c;
  ctui_look_control_snap(&s, l, w, h);
  const uint32_t f[] = {(uint32_t)s.kind,
                        (uint32_t)s.value,
                        (uint32_t)s.max,
                        (uint32_t)s.span,
                        s.flags,
                        (uint32_t)s.glyph,
                        (uint32_t)w,
                        (uint32_t)h,
                        (uint32_t)s.picto,
                        s.tint,
                        s.under,
                        (uint32_t)s.box | (uint32_t)s.sides << 8 |
                            (uint32_t)s.place << 16,
                        s.fill,
                        (uint32_t)s.gap | (uint32_t)s.gap_cols << 16};
  uint64_t k = ctui_look_hash(l) ^ ART_VERSION;
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    for (int b = 0; b < 4; b++) {
      k = (k ^ ((f[i] >> (8 * b)) & 0xff)) * 0x100000001b3u;
    }
  }
  /* a graph: the samples it shows, as the rows they land on */
  /* the one past the left edge too: its line runs to the edge */
  int room = h - 2, shown = (w - 2) / graph_step(c, w, h) + 2;
  for (int i = max2(0, s.count - shown); i < s.count && room > 0; i++) {
    int y = min2(s.samples[i], 100) * (room - 1) / 100;
    k = (k ^ (uint64_t)(y + 1)) * 0x100000001b3u;
    if (s.samples2) {
      y = min2(s.samples2[i], 100) * (room - 1) / 100;
      k = (k ^ (uint64_t)(y + 0x10001)) * 0x100000001b3u;
    }
  }
  return k;
}

/* --- glyphs: drawn on an n x n grid at (ox, oy), 1-bit --- */

typedef struct {
  CTUI_LOOK_CANVAS *c;
  int ox, oy, n, flip;
  const unsigned char *rgb;
} GRID;

static void dot(const GRID *g, int x, int y) {
  if (x >= 0 && y >= 0 && x < g->n && y < g->n) {
    ctui_look_paint_px(g->c, g->ox + (g->flip ? g->n - 1 - x : x), g->oy + y,
                       g->rgb);
  }
}

static void bar(const GRID *g, int x, int y, int w, int h) {
  for (int j = y; j < y + h; j++) {
    for (int i = x; i < x + w; i++) {
      dot(g, i, j);
    }
  }
}

/* a triangle pointing right with its back at x, rows 0..n-1 */
static void triangle(const GRID *g, int x) {
  for (int y = 0; y < g->n; y++) {
    bar(g, x, y, min2(y, g->n - 1 - y) + 1, 1);
  }
}

/* ⏭ (⏮ mirrored): a triangle pointing at a bar by its tip, centred */
static void skip(GRID *g, int flip) {
  int n = g->n, b = n >= 8 ? 2 : 1, tw = (n + 1) / 2;
  int x = (n - tw - b) / 2;
  g->flip = flip;
  triangle(g, x);
  bar(g, x + tw, 0, b, n);
}

/* ↻: a 1 px ring open at the top right, its arrowhead there (pixel art:
 * a ring this small drawn from its radius comes out a blob), scaled by
 * whole factors into the grid */
static void refresh(const GRID *g) {
  static const char ART[] = "...ttt.t."
                            ".tt...tt."
                            ".t...ttt."
                            "t........"
                            "t.......t"
                            "t.......t"
                            ".t.....t."
                            ".tt...tt."
                            "...ttt...";
  int k = max2(1, g->n / 9), o = (g->n - 9 * k) / 2;
  for (int i = 0; i < 81; i++) {
    if (ART[i] == 't') {
      bar(g, o + i % 9 * k, o + i / 9 * k, k, k);
    }
  }
}

static void glyph(CTUI_LOOK_CANVAS *c, int x, int y, int w, int h, int inset,
                  CTUI_LOOK_GLYPH which, const unsigned char *rgb) {
  int side = min2(w, h);
  int n = side - 2 * max2(inset + 1, side / 4);
  if (n < 3 || which == CTUI_LOOK_GLYPH_NONE) {
    return;
  }
  GRID g = {c, x + (w - n) / 2, y + (h - n) / 2, n, 0, rgb};
  switch (which) {
  case CTUI_LOOK_GLYPH_CLOSE: {
    int t = n >= 8 ? 2 : 1;
    for (int d = 0; d <= n - t; d++) {
      for (int k = 0; k < t; k++) {
        dot(&g, d + k, d + (t - 1) / 2);
        dot(&g, n - 1 - d - k, d + (t - 1) / 2);
      }
    }
    break;
  }
  case CTUI_LOOK_GLYPH_EJECT: {
    int th = (n + 1) / 2, b = max2(1, n / 6), mid = (n - 1) / 2;
    int top = (n - th - 1 - b) / 2;
    for (int r = 0; r < th; r++) {
      bar(&g, mid - r, top + r, 2 * r + 1, 1);
    }
    bar(&g, 0, top + th + 1, n, b);
    break;
  }
  case CTUI_LOOK_GLYPH_PLAY:
    triangle(&g, (n - (n + 1) / 2) / 2);
    break;
  case CTUI_LOOK_GLYPH_BACK:
    g.flip = 1;
    triangle(&g, (n - (n + 1) / 2) / 2);
    break;
  case CTUI_LOOK_GLYPH_DOWN: {
    /* a triangle pointing down, half as tall as wide, centred */
    int half = (n - 1) / 2, th = half + 1, top = (n - th) / 2;
    for (int r = 0; r < th; r++) {
      bar(&g, r, top + r, n - 2 * r - (n % 2 ? 0 : 1), 1);
    }
    break;
  }
  case CTUI_LOOK_GLYPH_PAUSE: {
    int b = max2(1, n / 3), e = n / 6;
    bar(&g, e, 0, b, n);
    bar(&g, n - e - b, 0, b, n);
    break;
  }
  case CTUI_LOOK_GLYPH_STOP:
    bar(&g, n / 6, n / 6, n - 2 * (n / 6), n - 2 * (n / 6));
    break;
  case CTUI_LOOK_GLYPH_PREV:
    skip(&g, 1);
    break;
  case CTUI_LOOK_GLYPH_REFRESH:
    refresh(&g);
    break;
  case CTUI_LOOK_GLYPH_BUSY: {
    /* three dots on the baseline */
    int d = max2(1, n / 5), gap = (n - 3 * d) / 2;
    for (int i = 0; i < 3; i++) {
      bar(&g, i * (d + gap), n - d, d, d);
    }
    break;
  }
  case CTUI_LOOK_GLYPH_NEXT:
    skip(&g, 0);
    break;
  default:
    break;
  }
}

/* a tick in an n x n square at (x, y): two 45-degree strokes, n/3 thick */
static void tick(CTUI_LOOK_CANVAS *c, int x, int y, int n,
                 const unsigned char *rgb) {
  int t = max2(1, (n + 1) / 3), by = n - t, bx = n / 3;
  for (int i = 0; i < n; i++) {
    int top = i <= bx ? by - bx + i : by - (i - bx);
    if (top < 0) {
      continue;
    }
    ctui_look_paint_rect(c, x + i, y + top, 1, t, rgb);
  }
}

/* --- the controls --- */

static const unsigned char *role(const CTUI_LOOK *l, int r) {
  return l->color[r];
}

static const unsigned char *marks(const CTUI_LOOK_CONTROL *c,
                                  const CTUI_LOOK *l, int r) {
  return role(l, (c->flags & CTUI_LOOK_CTL_OFF) ? CTUI_LOOK_DISABLED : r);
}

static void paint_slider(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                         const CTUI_LOOK *l) {
  int w = cv->w, h = cv->h, tw = slider_thumb(l, w, h);
  int pos = share(c->value, c->max, w - tw);
  /* the groove's edge plus room inside it for the accent */
  int gh = min2(h, 2 * l->groove + max2(2, h / 8)), gy = (h - gh) / 2;
  int gx = tw / 2, gw = w - 2 * (tw / 2);
  ctui_look_paint_box(cv, gx, gy, gw, gh, l, CTUI_LOOK_PAINT_SUNKEN, l->groove,
                      0, role(l, CTUI_LOOK_GROOVE));
  int in = l->groove;
  ctui_look_paint_rect(cv, gx + in, gy + in, pos + tw / 2 - gx - in,
                       gh - 2 * in, marks(c, l, CTUI_LOOK_ACCENT));
  int ty = h > 4 ? 1 : 0;
  ctui_look_paint_box(cv, pos, ty, tw, h - 2 * ty, l, CTUI_LOOK_PAINT_RAISED,
                      l->bevel, l->outline, role(l, CTUI_LOOK_FACE));
}

static void paint_level(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                        const CTUI_LOOK *l) {
  int w = cv->w, h = cv->h, m = margin(h), x0, len;
  level_area(l, w, &x0, &len);
  ctui_look_paint_box(cv, 0, m, w, h - 2 * m, l, CTUI_LOOK_PAINT_SUNKEN,
                      l->groove, 0, role(l, CTUI_LOOK_GROOVE));
  ctui_look_paint_fill(cv, x0, m + x0, len, h - 2 * m - 2 * x0,
                       share(c->value, c->max, len), l,
                       marks(c, l, CTUI_LOOK_ACCENT));
}

static void paint_signal(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                         const CTUI_LOOK *l) {
  int w = cv->w, h = cv->h, n = signal_bars(c->max), m = margin(h);
  int lit = clamp(c->value, 0, n);
  int bw = max2(1, (w - l->gap * (n - 1)) / n);
  int x = (w - n * bw - l->gap * (n - 1)) / 2, room = h - 2 * m;
  for (int i = 0; i < n; i++, x += bw + l->gap) {
    int bh = max2(1, room * (i + 1) / n);
    int y = h - m - bh;
    if (i < lit) {
      ctui_look_paint_box(cv, x, y, bw, bh, l, CTUI_LOOK_PAINT_RAISED,
                          min2(l->bevel, 1), l->outline,
                          marks(c, l, CTUI_LOOK_ACCENT));
    } else {
      ctui_look_paint_box(cv, x, y, bw, bh, l, CTUI_LOOK_PAINT_SUNKEN,
                          min2(l->groove, 1), 0, role(l, CTUI_LOOK_GROOVE));
    }
  }
}

/* the meter's and the graph's colours: an instrument's, in every look */
static const unsigned char WELL[3] = {0, 0, 0}, LED_GREEN[3] = {48, 224, 80},
                           LED_AMBER[3] = {240, 192, 48},
                           LED_RED[3] = {240, 64, 64},
                           GRID_GREEN[3] = {0, 96, 48},
                           LINE_GREEN[3] = {0, 255, 64},
                           LINE_RED[3] = {255, 48, 48};

static void dim(const unsigned char in[3], unsigned char out[3]) {
  for (int k = 0; k < 3; k++) {
    out[k] = (unsigned char)(in[k] / 5);
  }
}

/* Task Manager's: rows of LEDs from the bottom, two columns of them, all
 * green */
static void paint_meter_up(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                           const CTUI_LOOK *l) {
  int w = cv->w, h = cv->h, y0, seg, n;
  meter_leds(l, h, w, &y0, &seg, &n);
  ctui_look_paint_box(cv, 0, 0, w, h, l, CTUI_LOOK_PAINT_SUNKEN,
                      max2(l->groove, 1), 0, WELL);
  int lw = (w - 2 * y0 - 1) / 2;
  if (lw < 1) {
    return;
  }
  int x1 = y0 + lw + 1;
  int lit = share(c->value, c->max, n);
  unsigned char off[3];
  dim(LED_GREEN, off);
  for (int i = 0, y = h - y0 - seg; i < n; i++, y -= seg + 1) {
    const unsigned char *rgb = i < lit ? LED_GREEN : off;
    ctui_look_paint_rect(cv, y0, y, lw, seg, rgb);
    ctui_look_paint_rect(cv, x1, y, lw, seg, rgb);
  }
}

static void paint_meter(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                        const CTUI_LOOK *l) {
  if (cv->h > cv->w) {
    paint_meter_up(cv, c, l);
    return;
  }
  int w = cv->w, h = cv->h, m = margin(h), x0, seg, n;
  meter_leds(l, w, h, &x0, &seg, &n);
  ctui_look_paint_box(cv, 0, m, w, h - 2 * m, l, CTUI_LOOK_PAINT_SUNKEN,
                      max2(l->groove, 1), 0, WELL);
  int y = m + x0, lh = h - 2 * m - 2 * x0;
  int clip = (c->flags & CTUI_LOOK_CTL_CHECKED) != 0;
  /* snapped or not: the same LEDs (snap's own rule) */
  int lit = share(c->value, c->max, n);
  int held = c->span > 0 ? max2(1, share(c->span, c->max, n)) : 0;
  for (int i = 0, x = x0; i < n; i++, x += seg + 1) {
    /* its zone by where it ends: 20 of 1 is red from the 20th */
    const unsigned char *z = (i + 1) * 100 > n * 95   ? LED_RED
                             : (i + 1) * 100 > n * 70 ? LED_AMBER
                                                      : LED_GREEN;
    unsigned char off[3];
    dim(z, off);
    int on = i < lit || i + 1 == held || (clip && i == n - 1);
    ctui_look_paint_rect(cv, x, y, seg, lh, on ? z : off);
  }
}

/* the newest sample at the right edge, a point each step px leftwards,
 * each joined to the next by a straight line */
static void graph_line(CTUI_LOOK_CANVAS *cv, const unsigned char *samples,
                       int count, int x0, int y0, int gw, int gh, int step,
                       const unsigned char *rgb) {
  int px = -1, py = -1;
  /* the point past the left edge still gets its line, cut at the edge:
   * a full graph reaches it */
  for (int i = count - 1, x = x0 + gw - 1; i >= 0 && px != INT_MIN;
       i--, x -= step) {
    int y = y0 + gh - 1 - min2(samples[i], 100) * (gh - 1) / 100;
    if (px < 0) {
      ctui_look_paint_rect(cv, x, y, 1, 1, rgb);
    } else {
      int dx = px - x, dy = py - y, n = max2(dx, dy < 0 ? -dy : dy);
      for (int k = 0; k <= n; k++) {
        int lx = x + (dx * k + n / 2) / n;
        if (lx >= x0) {
          ctui_look_paint_rect(cv, lx,
                               y + (dy * k + (dy < 0 ? -n : n) / 2) / n, 1,
                               1, rgb);
        }
      }
    }
    px = x < x0 ? INT_MIN : x; /* past the edge: that was the last */
    py = y;
  }
}

static void paint_graph(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                        const CTUI_LOOK *l) {
  int w = cv->w, h = cv->h;
  ctui_look_paint_box(cv, 0, 0, w, h, l, CTUI_LOOK_PAINT_SUNKEN, 1, 0, WELL);
  int x0 = 1, y0 = 1, gw = w - 2, gh = h - 2;
  if (gw < 2 || gh < 2) {
    return;
  }
  /* the grid's squares, a sixth of its height (Task Manager's 12 px at
   * 72), from the bottom right */
  int sq = max2(4, gh / 3);
  for (int y = y0 + gh - 1; y >= y0; y -= sq) {
    ctui_look_paint_rect(cv, x0, y, gw, 1, GRID_GREEN);
  }
  for (int x = x0 + gw - 1; x >= x0; x -= sq) {
    ctui_look_paint_rect(cv, x, y0, 1, gh, GRID_GREEN);
  }
  graph_line(cv, c->samples, c->count, x0, y0, gw, gh, graph_step(c, w, h),
             LINE_GREEN);
  if (c->samples2) {
    graph_line(cv, c->samples2, c->count, x0, y0, gw, gh, graph_step(c, w, h),
               LINE_RED);
  }
}

int ctui_look_graph_at(const CTUI_LOOK_CONTROL *c, int w, int h, int x) {
  /* graph_line()'s points: the newest at x gw, one each step leftwards */
  int gw = w - 2, step = graph_step(c, w, h), count = c->count;
  if (count <= 0 || gw < 2 || h - 2 < 2 || x < 1 || x > gw) {
    return -1;
  }
  int i = count - 1 - (gw - x + step / 2) / step;
  return i >= 0 ? i : -1;
}

static void edge_run(CTUI_LOOK_CANVAS *cv, int box, int across, int at,
                     int from, int to, const unsigned char *rgb);

/* CTUI_LOOK_CTL_FOCUSED: a dotted rectangle in px from the canvas's edge */
static void focus_ring(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                       const CTUI_LOOK *l, int in) {
  int x1 = cv->w - 1 - in, y1 = cv->h - 1 - in;
  if (!(c->flags & CTUI_LOOK_CTL_FOCUSED) || x1 - in < 2 || y1 - in < 2) {
    return;
  }
  const unsigned char *ink = role(l, CTUI_LOOK_TEXT);
  edge_run(cv, CTUI_LOOK_BOX_DOTTED, 1, in, in, x1, ink);
  edge_run(cv, CTUI_LOOK_BOX_DOTTED, 1, y1, in, x1, ink);
  edge_run(cv, CTUI_LOOK_BOX_DOTTED, 0, in, in, y1, ink);
  edge_run(cv, CTUI_LOOK_BOX_DOTTED, 0, x1, in, y1, ink);
}

static void paint_check(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                        const CTUI_LOOK *l) {
  int side = min2(cv->w, cv->h), s = side - 2 * (side / 6);
  int x = (cv->w - s) / 2, y = (cv->h - s) / 2;
  ctui_look_paint_box(cv, x, y, s, s, l, CTUI_LOOK_PAINT_SUNKEN, l->groove, 0,
                      role(l, (c->flags & CTUI_LOOK_CTL_OFF)
                                  ? CTUI_LOOK_FACE
                                  : CTUI_LOOK_GROOVE));
  int in = l->groove + 1, n = s - 2 * in;
  if ((c->flags & CTUI_LOOK_CTL_CHECKED) && n >= 3) {
    tick(cv, x + in, y + in, n, marks(c, l, CTUI_LOOK_TEXT));
  }
  focus_ring(cv, c, l, 0);
}

/* 95's radio button: a 12 px circle, its outer ring shadow top-left and
 * highlight bottom-right, the inner dark / light, groove inside, a dot
 * when checked -- rings from the distance to the centre, as a role mask
 * (whole-factor scaled like a pictogram). No bevel: one dark ring. */
#define RADIO 12
static void paint_radio(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                        const CTUI_LOOK *l) {
  char px[RADIO * RADIO + 1];
  int on = !!(c->flags & CTUI_LOOK_CTL_CHECKED),
      off = !!(c->flags & CTUI_LOOK_CTL_OFF);
  for (int y = 0; y < RADIO; y++) {
    for (int x = 0; x < RADIO; x++) {
      /* doubled coordinates around the centre (5.5, 5.5) */
      int dx = 2 * x - (RADIO - 1), dy = 2 * y - (RADIO - 1);
      int d2 = dx * dx + dy * dy, lit = dx + dy > 0;
      char ch = '.';
      if (d2 <= 12 * 12 && d2 > 10 * 10) {
        ch = !l->bevel ? 'd' : lit ? 'h' : 's';
      } else if (d2 <= 10 * 10 && d2 > 8 * 8 && l->bevel) {
        ch = lit ? 'l' : 'd';
      } else if (d2 <= 10 * 10) {
        ch = on && d2 <= 4 * 4 ? (off ? 'x' : 't') : 'g';
      }
      px[y * RADIO + x] = ch;
    }
  }
  px[RADIO * RADIO] = '\0';
  CTUI_LOOK_MASK m = {RADIO, RADIO, px};
  ctui_look_paint_mask(cv, 0, 0, cv->w, cv->h, &m, l, 0);
  focus_ring(cv, c, l, 0);
}

static void paint_toggle(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                         const CTUI_LOOK *l) {
  int w = cv->w, h = cv->h, m = margin(h), th = h - 2 * m;
  int on = !!(c->flags & CTUI_LOOK_CTL_CHECKED);
  ctui_look_paint_box(cv, 0, m, w, th, l, CTUI_LOOK_PAINT_SUNKEN, l->groove, 0,
                      on ? marks(c, l, CTUI_LOOK_ACCENT)
                         : role(l, CTUI_LOOK_GROOVE));
  int side = min2(th, w / 2);
  ctui_look_paint_box(cv, on ? w - side : 0, m + (th - side) / 2, side, side, l,
                      CTUI_LOOK_PAINT_RAISED, l->bevel, l->outline,
                      role(l, CTUI_LOOK_FACE));
}

/* a latched button's inside, from inset to x1 / y1: 95's checker of the
 * highlight over the face; a flat look's light face */
static void checker(CTUI_LOOK_CANVAS *cv, int inset, int x1, int y1,
                    const CTUI_LOOK *l) {
  for (int y = inset; y < y1; y++) {
    for (int x = inset; x < x1; x++) {
      if (!l->bevel || ((x + y) & 1) == 0) {
        ctui_look_paint_px(
            cv, x, y,
            role(l, l->bevel ? CTUI_LOOK_HIGHLIGHT : CTUI_LOOK_LIGHT));
      }
    }
  }
}

static void paint_button(CTUI_LOOK_CANVAS *whole, const CTUI_LOOK_CONTROL *c,
                         const CTUI_LOOK *l) {
  /* a button kept off a bar's edge: the rows between its spans */
  int off =
      c->kind == CTUI_LOOK_CTL_BUTTON ? clamp(c->span, 0, whole->h / 4) : 0;
  CTUI_LOOK_CANVAS inside = {whole->px + (size_t)off * (size_t)whole->w * 4,
                             whole->w, whole->h - 2 * off};
  CTUI_LOOK_CANVAS *cv = &inside;
  int pressed = !!(c->flags & CTUI_LOOK_CTL_PRESSED);
  int depth = l->bevel, inset = depth + !!l->outline;
  if (c->kind == CTUI_LOOK_CTL_CHIP) {
    depth = inset = min2(depth, 1); /* the same glyph size in every state */
  }
  if (c->kind == CTUI_LOOK_CTL_BUTTON) {
    int latched = !!(c->flags & CTUI_LOOK_CTL_CHECKED);
    ctui_look_paint_box(cv, 0, 0, cv->w, cv->h, l,
                        pressed || latched ? CTUI_LOOK_PAINT_SUNKEN
                                           : CTUI_LOOK_PAINT_RAISED,
                        depth, l->outline, role(l, CTUI_LOOK_FACE));
    if (latched) {
      checker(cv, inset, cv->w - inset, cv->h - inset, l);
    }
    pressed |= latched;
  } else if (pressed || (c->flags & CTUI_LOOK_CTL_HOVER)) {
    /* a flat look has no edge to raise: the light face says it instead */
    ctui_look_paint_box(cv, 0, 0, cv->w, cv->h, l,
                        pressed ? CTUI_LOOK_PAINT_SUNKEN
                                : CTUI_LOOK_PAINT_RAISED,
                        depth, 0, depth ? NULL : role(l, CTUI_LOOK_LIGHT));
  }
  if (c->kind == CTUI_LOOK_CTL_BUTTON && c->picto != CTUI_LOOK_PICTO_NONE) {
    /* inside the edge, pushed down-right with the button; a toolbar's in
     * its top part, tinted */
    int top = c->max > 0 ? share(c->value, c->max, whole->h) : 0;
    top = top > 0 ? max2(top - off, 1) : 0;
    int ph = top > 0 ? top - inset : cv->h - 2 * inset;
    CTUI_LOOK tinted = *l;
    if (c->tint & CTUI_LOOK_CTL_TINT) {
      unsigned char *a = tinted.color[CTUI_LOOK_ACCENT];
      a[0] = (unsigned char)(c->tint >> 16);
      a[1] = (unsigned char)(c->tint >> 8);
      a[2] = (unsigned char)c->tint;
    }
    ctui_look_picto_paint(cv, inset + pressed, inset + pressed,
                          cv->w - 2 * inset, ph, c->picto, 0, 0,
                          !!(c->flags & CTUI_LOOK_CTL_OFF), &tinted);
    return;
  }
  const unsigned char tint[3] = {(unsigned char)(c->tint >> 16),
                                 (unsigned char)(c->tint >> 8),
                                 (unsigned char)c->tint};
  glyph(cv, pressed, pressed, cv->w, cv->h, inset, c->glyph,
        c->kind == CTUI_LOOK_CTL_CHIP && (c->tint & CTUI_LOOK_CTL_TINT) &&
                !(c->flags & CTUI_LOOK_CTL_OFF)
            ? tint
            : marks(c, l, CTUI_LOOK_TEXT));
  focus_ring(cv, c, l, inset + 1);
}

/* 95's text box: a sunken edge as deep as a button's raised one, the
 * groove inside; a drop-down's button in it at the right, as wide as
 * tall */
static void paint_field(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                        const CTUI_LOOK *l) {
  int depth = max2(l->bevel, l->groove);
  /* a disabled one greyed as 95's: the face inside */
  ctui_look_paint_box(
      cv, 0, 0, cv->w, cv->h, l, CTUI_LOOK_PAINT_SUNKEN, depth, 0,
      role(l,
           (c->flags & CTUI_LOOK_CTL_OFF) ? CTUI_LOOK_FACE : CTUI_LOOK_GROOVE));
  if (c->glyph == CTUI_LOOK_GLYPH_NONE) {
    return;
  }
  int bh = cv->h - 2 * depth, bw = min2(bh, cv->w / 2);
  if (bh < 3 || bw < 3) {
    return;
  }
  int bx = cv->w - depth - bw, inset = l->bevel + !!l->outline;
  ctui_look_paint_box(cv, bx, depth, bw, bh, l, CTUI_LOOK_PAINT_RAISED,
                      l->bevel, l->outline, role(l, CTUI_LOOK_FACE));
  glyph(cv, bx, depth, bw, bh, inset, c->glyph, marks(c, l, CTUI_LOOK_TEXT));
}

/* a 1 px ring around x, y, w, h */
static void ring(CTUI_LOOK_CANVAS *cv, int x, int y, int w, int h,
                 const unsigned char *rgb) {
  ctui_look_paint_rect(cv, x, y, w, 1, rgb);
  ctui_look_paint_rect(cv, x, y + h - 1, w, 1, rgb);
  ctui_look_paint_rect(cv, x, y, 1, h, rgb);
  ctui_look_paint_rect(cv, x + w - 1, y, 1, h, rgb);
}

/* 95's group box edge: a shadow ring with a highlight one a px down and
 * right of it; a flat look's a shadow line (dark with an outline) */
static void paint_frame(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                        const CTUI_LOOK *l) {
  const unsigned char *line =
      marks(c, l, l->outline ? CTUI_LOOK_DARK : CTUI_LOOK_SHADOW);
  if (!l->bevel || cv->w < 3 || cv->h < 3) {
    ring(cv, 0, 0, cv->w, cv->h, line);
    return;
  }
  ring(cv, 1, 1, cv->w - 1, cv->h - 1, role(l, CTUI_LOOK_HIGHLIGHT));
  ring(cv, 0, 0, cv->w - 1, cv->h - 1, line);
}

/* 95's status bar panel: a 1 px sunken edge around the face */
static void paint_panel(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK *l) {
  ctui_look_paint_box(cv, 0, 0, cv->w, cv->h, l, CTUI_LOOK_PAINT_SUNKEN,
                      l->bevel ? 1 : 0, !l->bevel, role(l, CTUI_LOOK_FACE));
}

/* a bar's surface: the face, each raised side as a button's edge on
 * it (lit on top and left, shaded on the bottom and right); a flat
 * look's a line in the shadow */
static void paint_bar(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                      const CTUI_LOOK *l) {
  ctui_look_paint_rect(cv, 0, 0, cv->w, cv->h, role(l, CTUI_LOOK_FACE));
  int depth = l->bevel;
  for (int side = 0; side < 4; side++) {
    if (!(c->sides & (1 << side))) {
      continue;
    }
    int lit = side == 0 || side == 3;
    for (int k = 0; k < (depth ? depth : 1); k++) {
      /* outer ring then inner, as ctui_look_paint_box() raises */
      int r = !depth       ? CTUI_LOOK_SHADOW
              : depth == 1 ? (lit ? CTUI_LOOK_HIGHLIGHT : CTUI_LOOK_SHADOW)
              : lit        ? (k ? CTUI_LOOK_LIGHT : CTUI_LOOK_HIGHLIGHT)
              : k          ? CTUI_LOOK_SHADOW
                           : CTUI_LOOK_DARK;
      if (side == 0 || side == 2) {
        ctui_look_paint_rect(cv, 0, side == 0 ? k : cv->h - 1 - k, cv->w, 1,
                             role(l, r));
      } else {
        ctui_look_paint_rect(cv, side == 3 ? k : cv->w - 1 - k, 0, 1, cv->h,
                             role(l, r));
      }
    }
  }
}

/* a splitter: the face (the shadow while dragged), three bumps across its
 * middle along its length -- raised, or in the shadow without bevels */
static void paint_splitter(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                           const CTUI_LOOK *l) {
  int vertical = cv->h > cv->w;
  int across = vertical ? cv->w : cv->h, len = vertical ? cv->h : cv->w;
  int pressed = !!(c->flags & CTUI_LOOK_CTL_PRESSED);
  ctui_look_paint_rect(cv, 0, 0, cv->w, cv->h,
                       role(l, pressed ? CTUI_LOOK_SHADOW : CTUI_LOOK_FACE));
  int bump = clamp(across / 3, 2, 4), n = 3;
  int span = (2 * n - 1) * bump;
  if (across < bump + 2 || len < span + 2) {
    return;
  }
  int at = (len - span) / 2, off = (across - bump) / 2;
  for (int i = 0; i < n; i++, at += 2 * bump) {
    int x = vertical ? off : at, y = vertical ? at : off;
    if (l->bevel) {
      ctui_look_paint_box(cv, x, y, bump, bump, l, CTUI_LOOK_PAINT_RAISED, 1,
                          0, role(l, CTUI_LOOK_FACE));
    } else {
      ctui_look_paint_rect(cv, x, y, bump, bump,
                           role(l, pressed ? CTUI_LOOK_FACE : CTUI_LOOK_SHADOW));
    }
  }
}

/* --- a frame's row (CTUI_LOOK_CTL_BOX) --- */

/* the colours of a box's edge line k (0 the outermost) on a side: the top
 * or left (lit) one, or the bottom or right; flat: a flat style's ink.
 * NULL: none there */
static const unsigned char *edge_px(const CTUI_LOOK *l, int box, int k, int px,
                                    int lit, const unsigned char *flat) {
  int r;
  switch (box) {
  case CTUI_LOOK_BOX_GROOVE:
  case CTUI_LOOK_BOX_RIDGE: {
    int dark = (k == 0) == lit;
    if (box == CTUI_LOOK_BOX_RIDGE) {
      dark = !dark;
    }
    r = dark ? CTUI_LOOK_SHADOW : CTUI_LOOK_HIGHLIGHT;
    break;
  }
  case CTUI_LOOK_BOX_INSET:
  case CTUI_LOOK_BOX_FIELD:
    r = px < 2 ? (lit ? CTUI_LOOK_SHADOW : CTUI_LOOK_HIGHLIGHT)
        : lit  ? (k ? CTUI_LOOK_DARK : CTUI_LOOK_SHADOW)
               : (k ? CTUI_LOOK_LIGHT : CTUI_LOOK_HIGHLIGHT);
    break;
  case CTUI_LOOK_BOX_OUTSET:
    r = px < 2 ? (lit ? CTUI_LOOK_HIGHLIGHT : CTUI_LOOK_SHADOW)
        : lit  ? (k ? CTUI_LOOK_LIGHT : CTUI_LOOK_HIGHLIGHT)
               : (k ? CTUI_LOOK_SHADOW : CTUI_LOOK_DARK);
    break;
  case CTUI_LOOK_BOX_DOUBLE:
    return k == 1 ? NULL : flat;
  default:
    return flat;
  }
  return l->color[r];
}

/* an edge's colour where it shows on the paper under it: one the paper's
 * own (95's white highlight on a white page) is the face's instead */
static const unsigned char *on_paper(const CTUI_LOOK_CONTROL *c,
                                     const CTUI_LOOK *l,
                                     const unsigned char *rgb) {
  if (!rgb || !(c->under & CTUI_LOOK_CTL_TINT)) {
    return rgb;
  }
  uint32_t v = (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
  return v == (c->under & 0xffffffu) ? l->color[CTUI_LOOK_FACE] : rgb;
}

/* whether the pixel at t along a dashed / dotted line is drawn */
static int dash_on(int box, int t) {
  return box == CTUI_LOOK_BOX_DASHED   ? t % 6 < 4
         : box == CTUI_LOOK_BOX_DOTTED ? t % 2 == 0
                                       : 1;
}

/* a horizontal (across) or vertical run of a box's edge, dashed by box */
static void edge_run(CTUI_LOOK_CANVAS *cv, int box, int across, int at,
                     int from, int to, const unsigned char *rgb) {
  for (int t = from; t <= to; t++) {
    if (dash_on(box, t)) {
      ctui_look_paint_px(cv, across ? t : at, across ? at : t, rgb);
    }
  }
}

static void paint_box_row(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                          const CTUI_LOOK *l) {
  int w = cv->w, h = cv->h, box = c->box < CTUI_LOOK_BOXES ? c->box : 0;
  int top = c->place & CTUI_LOOK_BOX_TOP,
      bottom = c->place & CTUI_LOOK_BOX_BOTTOM;
  if (box >= CTUI_LOOK_BOX_INFO) {
    /* an icon two rows high: this row's half of it */
    static const CTUI_LOOK_PICTO icons[] = {
        CTUI_LOOK_PICTO_INFO, CTUI_LOOK_PICTO_WARNING, CTUI_LOOK_PICTO_ERROR};
    CTUI_LOOK tinted = *l;
    if (c->tint & CTUI_LOOK_CTL_TINT) {
      unsigned char *a = tinted.color[CTUI_LOOK_ACCENT];
      a[0] = (unsigned char)(c->tint >> 16);
      a[1] = (unsigned char)(c->tint >> 8);
      a[2] = (unsigned char)c->tint;
    }
    int tall = top && bottom ? h : 2 * h;
    ctui_look_picto_paint(cv, 0, top ? 0 : h - tall, w, tall,
                          icons[box - CTUI_LOOK_BOX_INFO], 0, 0, 0, &tinted);
    return;
  }
  int px = c->place & CTUI_LOOK_BOX_THICK ? 2 : 1;
  if (box == CTUI_LOOK_BOX_DOUBLE) {
    px = 3;
  } else if (box == CTUI_LOOK_BOX_FIELD) {
    px = 2;
  }
  int cw = w / clamp(c->span, 1, w > 0 ? w : 1); /* as snapped */
  int flush = c->place & CTUI_LOOK_BOX_FLUSH;
  int flush_x = c->place & CTUI_LOOK_BOX_FLUSH_X;
  /* the box's outer edge (inclusive) in this row's px; a side past the
   * row's top or bottom is in another row */
  int fl = flush_x || (c->place & CTUI_LOOK_BOX_FLUSH_LEFT);
  int fr = flush_x || (c->place & CTUI_LOOK_BOX_FLUSH_RIGHT);
  int x0 = fl ? 0 : (cw - px) / 2;
  int x1 = fr ? w - 1 : w - cw + (cw - px) / 2 + px - 1;
  int flush_top = flush || (c->place & CTUI_LOOK_BOX_FLUSH_TOP);
  int y0 = !top ? -px - 1 : flush_top ? 0 : (h - px) / 2;
  int y1 = !bottom ? h + px : flush ? h - 1 : (h - px) / 2 + px - 1;
  if (c->fill & CTUI_LOOK_CTL_TINT) {
    /* the page's background inside the edges (under them too: they're
     * drawn over it) */
    const unsigned char f[3] = {(unsigned char)(c->fill >> 16),
                                (unsigned char)(c->fill >> 8),
                                (unsigned char)c->fill};
    int ya = y0 < 0 ? 0 : y0, yb = y1 >= h ? h - 1 : y1;
    ctui_look_paint_rect(cv, x0, ya, x1 - x0 + 1, yb - ya + 1, f);
  }
  if (box == CTUI_LOOK_BOX_FIELD) {
    ctui_look_paint_rect(cv, x0 + px, y0 + px, x1 - x0 + 1 - 2 * px,
                         y1 - y0 + 1 - 2 * px, l->color[CTUI_LOOK_GROOVE]);
  }
  int st = c->sides & 1 << 0, sr = c->sides & 1 << 1;
  int sb = c->sides & 1 << 2, sl = c->sides & 1 << 3;
  if (box == CTUI_LOOK_BOX_FIELD) {
    st = sr = sb = sl = 1;
  }
  /* 95's order: the lit sides first, the bottom / right ones win the
   * corners they share with them */
  /* a flat style's ink: the tint (the page's), else the shadow */
  unsigned char ink[3] = {(unsigned char)(c->tint >> 16),
                          (unsigned char)(c->tint >> 8),
                          (unsigned char)c->tint};
  const unsigned char *flat =
      c->tint & CTUI_LOOK_CTL_TINT ? ink : l->color[CTUI_LOOK_SHADOW];
  for (int k = 0; k < px; k++) {
    const unsigned char *lit = on_paper(c, l, edge_px(l, box, k, px, 1, flat));
    const unsigned char *dim = on_paper(c, l, edge_px(l, box, k, px, 0, flat));
    int xa = x0 + (sl ? k : 0), xb = x1 - (sr ? k : 0);
    int ya = y0 + (st ? k : 0), yb = y1 - (sb ? k : 0);
    if (st && top && lit && c->gap_cols) {
      /* open over the heading's cells */
      int ga = c->gap * cw, gb = (c->gap + c->gap_cols) * cw;
      edge_run(cv, box, 1, y0 + k, xa, min2(xb, ga - 1), lit);
      edge_run(cv, box, 1, y0 + k, max2(xa, gb), xb, lit);
    } else if (st && top && lit) {
      edge_run(cv, box, 1, y0 + k, xa, xb, lit);
    }
    if (sl && lit) {
      edge_run(cv, box, 0, x0 + k, ya, yb, lit);
    }
    if (sb && bottom && dim) {
      edge_run(cv, box, 1, y1 - k, xa, xb, dim);
    }
    if (sr && dim) {
      edge_run(cv, box, 0, x1 - k, ya, yb, dim);
    }
  }
}

static void paint_scroll(CTUI_LOOK_CANVAS *cv, const CTUI_LOOK_CONTROL *c,
                         const CTUI_LOOK *l) {
  int w = cv->w, h = cv->h, vertical = h >= w, len, least;
  scroll_axis(w, h, &len, &least);
  /* 95's track: a checker of highlight and face */
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int r = !l->bevel       ? CTUI_LOOK_GROOVE
              : ((x ^ y) & 1) ? CTUI_LOOK_HIGHLIGHT
                              : CTUI_LOOK_FACE;
      ctui_look_paint_px(cv, x, y, role(l, r));
    }
  }
  int tl = scroll_thumb(c, len, least), room = c->max - c->span;
  int pos = room > 0 ? share(c->value, room, len - tl) : 0;
  ctui_look_paint_box(cv, vertical ? 0 : pos, vertical ? pos : 0,
                      vertical ? w : tl, vertical ? tl : h, l,
                      CTUI_LOOK_PAINT_RAISED, l->bevel, l->outline,
                      role(l, CTUI_LOOK_FACE));
}

void ctui_look_control_paint(const CTUI_LOOK_CONTROL *c, const CTUI_LOOK *l,
                             unsigned char *rgba, int w, int h) {
  CTUI_LOOK_CANVAS cv = {rgba, w, h};
  memset(rgba, 0, (size_t)w * (size_t)h * 4);
  if (c->under & CTUI_LOOK_CTL_TINT) {
    const unsigned char u[3] = {(unsigned char)(c->under >> 16),
                                (unsigned char)(c->under >> 8),
                                (unsigned char)c->under};
    ctui_look_paint_rect(&cv, 0, 0, w, h, u);
  } else if (l->panel && c->kind != CTUI_LOOK_CTL_BOX &&
             !(c->flags & CTUI_LOOK_CTL_BARE)) {
    /* (a frame's icon is over its box: clear round it) */
    ctui_look_paint_rect(&cv, 0, 0, w, h, role(l, CTUI_LOOK_FACE));
  }
  switch (c->kind) {
  case CTUI_LOOK_CTL_SLIDER:
    paint_slider(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_LEVEL:
    paint_level(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_SIGNAL:
    paint_signal(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_METER:
    paint_meter(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_GRAPH:
    paint_graph(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_CHECK:
    paint_check(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_TOGGLE:
    paint_toggle(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_BUTTON:
  case CTUI_LOOK_CTL_CHIP:
    paint_button(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_SCROLL:
    paint_scroll(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_PICTO: {
    CTUI_LOOK tinted = *l;
    if ((c->tint & CTUI_LOOK_CTL_TINT) && !(c->flags & CTUI_LOOK_CTL_OFF)) {
      unsigned char *a = tinted.color[CTUI_LOOK_ACCENT];
      a[0] = (unsigned char)(c->tint >> 16);
      a[1] = (unsigned char)(c->tint >> 8);
      a[2] = (unsigned char)c->tint;
    }
    ctui_look_picto_paint(&cv, 0, 0, w, h, c->picto, c->value, c->max,
                          !!(c->flags & CTUI_LOOK_CTL_OFF), &tinted);
    break;
  }
  case CTUI_LOOK_CTL_RADIO:
    paint_radio(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_FIELD:
    paint_field(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_FRAME:
    paint_frame(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_PANEL:
    paint_panel(&cv, l);
    break;
  case CTUI_LOOK_CTL_BOX:
    paint_box_row(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_SPLITTER:
    paint_splitter(&cv, c, l);
    break;
  case CTUI_LOOK_CTL_BAR:
    paint_bar(&cv, c, l);
    break;
  default:
    break;
  }
}

CTUI_LOOK_CONTROL ctui_look_box_row(int box, int cols, int r, int rows, int gap,
                                    int gap_cols) {
  int top = r == 0, bottom = r == rows - 1;
  int place = (top ? CTUI_LOOK_BOX_TOP : 0) | (bottom ? CTUI_LOOK_BOX_BOTTOM : 0);
  if (box == CTUI_LOOK_BOX_OUTSET) {
    place |= CTUI_LOOK_BOX_THICK;
  } else {
    place |= CTUI_LOOK_BOX_FLUSH_X | (bottom && !top ? CTUI_LOOK_BOX_FLUSH : 0);
  }
  int open = top && gap_cols > 0;
  return (CTUI_LOOK_CONTROL){.kind = CTUI_LOOK_CTL_BOX,
                             .span = cols,
                             .box = (uint8_t)box,
                             .sides = 0xf,
                             .place = (uint8_t)place,
                             .gap = (uint16_t)(open ? gap : 0),
                             .gap_cols = (uint16_t)(open ? gap_cols : 0)};
}
