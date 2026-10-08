/* The drawn controls' groundwork (src/look/, look/drawn.c): looks from
 * config, the primitives' pixels, each control's shape, snapping (two
 * states that look alike are one image) and the PNG cache. Pixels are
 * pinned as role letters: the test look gives every role its own colour,
 * so a pixel reads back as the role it was painted with. */
/* setenv(), mkdtemp() are POSIX, not C11 */
#define _POSIX_C_SOURCE 200809L

#include "look/control.h"
#include "look/drawn.h"
#include "look/filter.h"
#include "look/icon.h"
#include "look/look.h"
#include "look/lookconf.h"
#include "look/paint.h"
#include "look/picto.h"
#include "look/png.h"

#include "ctui.h"
#include "ctui_test.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char LETTERS[] = "fhlsdaAgxtw"; /* CTUI_LOOK_ROLE order */

/* win95's shape with a colour of its own for every role, no panel */
static CTUI_LOOK test_look(void) {
  CTUI_LOOK l;
  ctui_look_builtin("win95", &l);
  for (int r = 0; r < CTUI_LOOK_ROLES; r++) {
    l.color[r][0] = (unsigned char)(10 + 20 * r);
    l.color[r][1] = (unsigned char)(7 * r);
    l.color[r][2] = (unsigned char)(250 - 11 * r);
  }
  l.panel = 0;
  return l;
}

/* the pixel's role letter, '.' if clear, '?' if no role's colour */
static char px(const unsigned char *rgba, int w, const CTUI_LOOK *l, int x,
               int y) {
  const unsigned char *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
  if (!p[3]) {
    return '.';
  }
  for (int r = 0; r < CTUI_LOOK_ROLES; r++) {
    if (memcmp(p, l->color[r], 3) == 0) {
      return LETTERS[r];
    }
  }
  return '?';
}

/* row y from x0, n pixels, as letters (a static buffer) */
static const char *row(const unsigned char *rgba, int w, const CTUI_LOOK *l,
                       int y, int x0, int n) {
  static char s[512];
  for (int i = 0; i < n; i++) {
    s[i] = px(rgba, w, l, x0 + i, y);
  }
  s[n] = 0;
  return s;
}

static int count(const unsigned char *rgba, int w, int h, const CTUI_LOOK *l,
                 char letter) {
  int n = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      n += px(rgba, w, l, x, y) == letter;
    }
  }
  return n;
}

static unsigned char g_buf[64 * 64 * 4];

static const unsigned char *paint(CTUI_LOOK_CONTROL c, const CTUI_LOOK *l,
                                  int w, int h) {
  ctui_look_control_paint(&c, l, g_buf, w, h);
  return g_buf;
}

static void test_looks(void) {
  CTUI_LOOK l;
  for (int i = 0; ctui_look_names[i]; i++) {
    CTUI_TEST_ASSERT(ctui_look_builtin(ctui_look_names[i], &l) == 0,
                     "built-in look %s", ctui_look_names[i]);
  }
  CTUI_TEST_ASSERT(ctui_look_builtin("win3.1", &l) == -1, "no such built-in");
  CTUI_TEST_ASSERT(ctui_look_builtin("win95", &l) == 0 &&
                       l.color[CTUI_LOOK_FACE][0] == 0xc0 &&
                       l.color[CTUI_LOOK_ACCENT][2] == 0x80 && l.bevel == 2,
                   "win95: silver face, navy accent, 2 px bevels");
  int roundtrip = 1;
  for (int r = 0; r < CTUI_LOOK_ROLES; r++) {
    roundtrip &= ctui_look_role(ctui_look_role_name(r)) == r;
  }
  CTUI_TEST_ASSERT(roundtrip && ctui_look_role("chrome") == -1,
                   "role names both ways");

  CTUI_LOOK a = test_look(), b = test_look();
  CTUI_TEST_ASSERT(ctui_look_hash(&a) == ctui_look_hash(&b),
                   "equal looks hash alike");
  b.color[CTUI_LOOK_ACCENT][0] ^= 1;
  CTUI_TEST_ASSERT(ctui_look_hash(&a) != ctui_look_hash(&b),
                   "a colour changes the hash");
  b = a;
  b.chunk = 5;
  CTUI_TEST_ASSERT(ctui_look_hash(&a) != ctui_look_hash(&b),
                   "a shape parameter changes the hash");
}

/* parses text as the "look" value: its result, err in err */
static int parse(const char *text, CTUI_LOOK *out, char *err) {
  char perr[256];
  CTUI_JSON *v = ctui_json_parse(text, perr, sizeof perr);
  err[0] = 0;
  int r = ctui_look_parse(v, out, err, 256);
  ctui_json_free(v);
  return r;
}

static void test_config(void) {
  CTUI_LOOK l, base;
  char err[256];
  ctui_look_builtin("nt-dark", &base);
  CTUI_TEST_ASSERT(parse("\"nt-dark\"", &l, err) == 1 &&
                       ctui_look_hash(&l) == ctui_look_hash(&base),
                   "a name: that built-in");
  CTUI_TEST_ASSERT(ctui_look_parse(NULL, &l, err, sizeof err) == 0 &&
                       parse("false", &l, err) == 0 &&
                       parse("null", &l, err) == 0,
                   "missing, false, null: no look");
  CTUI_TEST_ASSERT(
      parse("{\"base\": \"flat\", \"colors\": {\"accent\": \"#102030\", "
            "\"text\": \"#ffffff\"}, \"bevel\": 1, \"chunk\": 4, \"gap\": 1, "
            "\"outline\": true, \"panel\": true, \"thumb\": 7}",
            &l, err) == 1 &&
          l.color[CTUI_LOOK_ACCENT][0] == 0x10 &&
          l.color[CTUI_LOOK_ACCENT][2] == 0x30 &&
          l.color[CTUI_LOOK_TEXT][1] == 0xff && l.bevel == 1 && l.chunk == 4 &&
          l.gap == 1 && l.outline && l.panel && l.thumb == 7,
      "a base with colours and shape overridden");
  ctui_look_builtin("win95", &base);
  CTUI_TEST_ASSERT(parse("{\"groove\": 1}", &l, err) == 1 && l.groove == 1 &&
                       l.color[CTUI_LOOK_FACE][0] ==
                           base.color[CTUI_LOOK_FACE][0],
                   "no base: win95's");

  static const struct {
    const char *text, *says;
  } bad[] = {
      {"\"win31\"", "no look \"win31\" (win95, nt-dark, flat)"},
      {"3", "a look's name or an object"},
      {"{\"base\": 1}", "\"base\" is a look's name"},
      {"{\"bevel\": 3}", "\"bevel\" is a whole number from 0 to 2"},
      {"{\"bevel\": 1.5}", "from 0 to 2"},
      {"{\"thumb\": -1}", "\"thumb\" is a whole number"},
      {"{\"outline\": 1}", "\"outline\" is true or false"},
      {"{\"colors\": {\"chrome\": \"#000000\"}}", "no colour role \"chrome\""},
      {"{\"colors\": {\"face\": \"silver\"}}", "\"face\" is \"#rrggbb\""},
      {"{\"colors\": []}", "\"colors\" must be an object"},
      {"{\"bevl\": 1}", "unknown setting \"bevl\""},
  };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    int r = parse(bad[i].text, &l, err);
    CTUI_TEST_ASSERT(r == -1 && strstr(err, bad[i].says), "%s: \"%s\" (%s)",
                     bad[i].text, bad[i].says, err);
  }
}

static void test_primitives(void) {
  CTUI_LOOK l = test_look();
  unsigned char buf[8 * 8 * 4];
  CTUI_LOOK_CANVAS c = {buf, 6, 5};
  memset(buf, 0, sizeof buf);
  ctui_look_paint_box(&c, 0, 0, 6, 5, &l, CTUI_LOOK_PAINT_RAISED, 2, 0,
                      l.color[CTUI_LOOK_FACE]);
  static const char *const raised[] = {"hhhhhd", "hlllsd", "hlffsd", "hssssd",
                                       "dddddd"};
  for (int y = 0; y < 5; y++) {
    CTUI_TEST_ASSERT(strcmp(row(buf, 6, &l, y, 0, 6), raised[y]) == 0,
                     "raised, 2 px: row %d %s (%s)", y, raised[y],
                     row(buf, 6, &l, y, 0, 6));
  }

  c = (CTUI_LOOK_CANVAS){buf, 3, 3};
  memset(buf, 0, sizeof buf);
  ctui_look_paint_box(&c, 0, 0, 3, 3, &l, CTUI_LOOK_PAINT_SUNKEN, 1, 0,
                      l.color[CTUI_LOOK_FACE]);
  CTUI_TEST_ASSERT(strcmp(row(buf, 3, &l, 0, 0, 3), "ssh") == 0 &&
                       strcmp(row(buf, 3, &l, 1, 0, 3), "sfh") == 0 &&
                       strcmp(row(buf, 3, &l, 2, 0, 3), "hhh") == 0,
                   "sunken, 1 px");

  c = (CTUI_LOOK_CANVAS){buf, 5, 4};
  memset(buf, 0, sizeof buf);
  ctui_look_paint_box(&c, 0, 0, 5, 4, &l, CTUI_LOOK_PAINT_RAISED, 1, 1, NULL);
  CTUI_TEST_ASSERT(strcmp(row(buf, 5, &l, 0, 0, 5), "ddddd") == 0 &&
                       strcmp(row(buf, 5, &l, 1, 0, 5), "dhhsd") == 0 &&
                       strcmp(row(buf, 5, &l, 2, 0, 5), "dsssd") == 0 &&
                       strcmp(row(buf, 5, &l, 3, 0, 5), "ddddd") == 0,
                   "an outline around a 1 px edge, no fill");

  l.corner = 2;
  memset(buf, 0, sizeof buf);
  ctui_look_paint_box(&c, 0, 0, 5, 4, &l, CTUI_LOOK_PAINT_FLAT, 0, 0,
                      l.color[CTUI_LOOK_FACE]);
  CTUI_TEST_ASSERT(strcmp(row(buf, 5, &l, 0, 0, 5), "..f..") == 0 &&
                       strcmp(row(buf, 5, &l, 1, 0, 5), ".fff.") == 0,
                   "the corner cut leaves the corners alone (%s)",
                   row(buf, 5, &l, 0, 0, 5));
  l.corner = 0;

  c = (CTUI_LOOK_CANVAS){buf, 11, 1};
  l.chunk = 3;
  l.gap = 1;
  memset(buf, 0, sizeof buf);
  ctui_look_paint_fill(&c, 0, 0, 11, 1, 5, &l, l.color[CTUI_LOOK_ACCENT]);
  CTUI_TEST_ASSERT(strcmp(row(buf, 11, &l, 0, 0, 11), "aaa........") == 0,
                   "a chunk shows once the fill reaches its middle");
  ctui_look_paint_fill(&c, 0, 0, 11, 1, 6, &l, l.color[CTUI_LOOK_ACCENT]);
  CTUI_TEST_ASSERT(strcmp(row(buf, 11, &l, 0, 0, 11), "aaa.aaa....") == 0,
                   "chunks with gaps (%s)", row(buf, 11, &l, 0, 0, 11));
  l.chunk = 0;
  memset(buf, 0, sizeof buf);
  ctui_look_paint_fill(&c, 0, 0, 11, 1, 5, &l, l.color[CTUI_LOOK_ACCENT]);
  CTUI_TEST_ASSERT(strcmp(row(buf, 11, &l, 0, 0, 11), "aaaaa......") == 0,
                   "no chunks: one block");

  static const CTUI_LOOK_MASK m = {2, 2, "a..t"};
  c = (CTUI_LOOK_CANVAS){buf, 5, 5};
  memset(buf, 0, sizeof buf);
  ctui_look_paint_mask(&c, 0, 0, 5, 5, &m, &l, 0);
  CTUI_TEST_ASSERT(strcmp(row(buf, 5, &l, 0, 0, 5), "aa...") == 0 &&
                       strcmp(row(buf, 5, &l, 1, 0, 5), "aa...") == 0 &&
                       strcmp(row(buf, 5, &l, 2, 0, 5), "..tt.") == 0 &&
                       strcmp(row(buf, 5, &l, 4, 0, 5), ".....") == 0,
                   "a mask at 2x, nearest neighbour");
  memset(buf, 0, sizeof buf);
  ctui_look_paint_mask(&c, 1, 1, 3, 3, &m, &l, 1);
  CTUI_TEST_ASSERT(strcmp(row(buf, 5, &l, 1, 0, 5), ".x...") == 0 &&
                       strcmp(row(buf, 5, &l, 2, 0, 5), "..x..") == 0,
                   "at 1x centred, off: disabled (%s)",
                   row(buf, 5, &l, 2, 0, 5));
  static const CTUI_LOOK_MASK big = {4, 1, "aaaa"};
  memset(buf, 0, sizeof buf);
  ctui_look_paint_mask(&c, 1, 0, 2, 1, &big, &l, 0);
  CTUI_TEST_ASSERT(strcmp(row(buf, 5, &l, 0, 0, 5), ".aa..") == 0,
                   "too big: clipped to its box");
}

static CTUI_LOOK_CONTROL ctl(CTUI_LOOK_CTL_KIND kind, int value, int max) {
  return (CTUI_LOOK_CONTROL){.kind = kind, .value = value, .max = max};
}

static void test_slider(void) {
  CTUI_LOOK l = test_look();
  CTUI_LOOK_CONTROL c = ctl(CTUI_LOOK_CTL_SLIDER, 50, 100);
  ctui_look_control_snap(&c, &l, 40, 20);
  /* an 11 px thumb (20 / 2, odd) travels 29 px */
  CTUI_TEST_ASSERT(c.value == 15 && c.max == 29, "snapped to the thumb's x");
  CTUI_TEST_ASSERT(
      ctui_look_control_key(&(CTUI_LOOK_CONTROL){CTUI_LOOK_CTL_SLIDER, 51, 100,
                                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                 0, 0},
                            &l, 40, 20) ==
              ctui_look_control_key(&(CTUI_LOOK_CONTROL){CTUI_LOOK_CTL_SLIDER,
                                                         50, 100, 0, 0, 0, 0, 0,
                                                         0, 0, 0, 0, 0, 0, 0},
                                    &l, 40, 20) &&
          ctui_look_control_key(&(CTUI_LOOK_CONTROL){CTUI_LOOK_CTL_SLIDER, 60,
                                                     100, 0, 0, 0, 0, 0, 0, 0,
                                                     0, 0, 0, 0, 0},
                                &l, 40, 20) !=
              ctui_look_control_key(&(CTUI_LOOK_CONTROL){CTUI_LOOK_CTL_SLIDER,
                                                         50, 100, 0, 0, 0, 0, 0,
                                                         0, 0, 0, 0, 0, 0, 0},
                                    &l, 40, 20),
      "50 %% and 51 %% are one image, 60 %% another");

  const unsigned char *p =
      paint(ctl(CTUI_LOOK_CTL_SLIDER, 50, 100), &l, 40, 20);
  /* the groove: 6 px at y 7, from x 5; its inside y 9-10 from x 7 */
  CTUI_TEST_ASSERT(px(p, 40, &l, 5, 7) == 's' && px(p, 40, &l, 7, 9) == 'a' &&
                       px(p, 40, &l, 14, 10) == 'a' &&
                       px(p, 40, &l, 32, 9) == 'g',
                   "the accent up to the thumb, the groove after (%s)",
                   row(p, 40, &l, 10, 0, 40));
  CTUI_TEST_ASSERT(strcmp(row(p, 40, &l, 1, 15, 11), "hhhhhhhhhhd") == 0 &&
                       px(p, 40, &l, 14, 1) == '.' &&
                       px(p, 40, &l, 26, 1) == '.' &&
                       strcmp(row(p, 40, &l, 18, 15, 11), "ddddddddddd") == 0,
                   "the thumb at x 15, raised, 1 px clear above and below");
  p = paint(ctl(CTUI_LOOK_CTL_SLIDER, 0, 100), &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 1) == 'h', "0: the thumb at the left");
  p = paint(ctl(CTUI_LOOK_CTL_SLIDER, 150, 100), &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 39, 10) == 'd' &&
                       px(p, 40, &l, 28, 1) == '.' &&
                       px(p, 40, &l, 29, 1) == 'h',
                   "over max: clamped to the right end");
  CTUI_LOOK_CONTROL off = ctl(CTUI_LOOK_CTL_SLIDER, 50, 100);
  off.flags = CTUI_LOOK_CTL_OFF;
  p = paint(off, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 10, 10) == 'x', "muted: the fill disabled");
  l.panel = 1;
  p = paint(ctl(CTUI_LOOK_CTL_SLIDER, 50, 100), &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 0) == 'f' && px(p, 40, &l, 39, 19) == 'f',
                   "a panel look: the face behind it all");
}

static void test_level_signal(void) {
  CTUI_LOOK l = test_look();
  l.chunk = 0;
  const unsigned char *p = paint(ctl(CTUI_LOOK_CTL_LEVEL, 50, 100), &l, 30, 18);
  /* the field y 3-14 (3 px margins), 2 px edge: the fill from x 2, 26 px */
  CTUI_TEST_ASSERT(px(p, 30, &l, 0, 2) == '.' && px(p, 30, &l, 0, 3) == 's' &&
                       strcmp(row(p, 30, &l, 9, 0, 30),
                              "sdaaaaaaaaaaaaagggggggggggggl"
                              "h") == 0,
                   "a solid level at half (%s)", row(p, 30, &l, 9, 0, 30));
  CTUI_LOOK_CONTROL c = ctl(CTUI_LOOK_CTL_LEVEL, 50, 100);
  ctui_look_control_snap(&c, &l, 30, 18);
  CTUI_TEST_ASSERT(c.value == 13 && c.max == 26, "snapped to the fill's px");
  l.chunk = 4;
  l.gap = 2;
  p = paint(ctl(CTUI_LOOK_CTL_LEVEL, 100, 100), &l, 30, 18);
  CTUI_TEST_ASSERT(
      strcmp(row(p, 30, &l, 9, 0, 30), "sdgaaaaggaaaaggaaaaggaaaaggglh") == 0,
      "chunks, a px clear of the edge (%s)", row(p, 30, &l, 9, 0, 30));

  l = test_look();
  /* 4 bars 3 px wide, 2 apart, over 14 px (3 px margins) */
  p = paint(ctl(CTUI_LOOK_CTL_SIGNAL, 2, 4), &l, 18, 20);
  CTUI_TEST_ASSERT(
      px(p, 18, &l, 1, 15) == 'a' && px(p, 18, &l, 1, 13) == '.' &&
          px(p, 18, &l, 6, 12) == 'a' && px(p, 18, &l, 11, 12) == 'g' &&
          px(p, 18, &l, 16, 4) == 'g' && px(p, 18, &l, 16, 2) == '.',
      "2 of 4 bars lit, rising");
  CTUI_TEST_ASSERT(
      ctui_look_control_key(&(CTUI_LOOK_CONTROL){CTUI_LOOK_CTL_SIGNAL, 9, 4, 0,
                                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                 0},
                            &l, 18, 20) ==
          ctui_look_control_key(&(CTUI_LOOK_CONTROL){CTUI_LOOK_CTL_SIGNAL, 4, 4,
                                                     0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                     0, 0, 0},
                                &l, 18, 20),
      "more than max lit: all of them");
}

static void test_toggles(void) {
  CTUI_LOOK l = test_look();
  const unsigned char *p = paint(ctl(CTUI_LOOK_CTL_CHECK, 0, 0), &l, 18, 20);
  CTUI_TEST_ASSERT(count(p, 18, 20, &l, 't') == 0 &&
                       count(p, 18, 20, &l, 'g') > 0,
                   "unchecked: an empty box");
  CTUI_LOOK_CONTROL c = ctl(CTUI_LOOK_CTL_CHECK, 0, 0);
  c.flags = CTUI_LOOK_CTL_CHECKED;
  p = paint(c, &l, 18, 20);
  int ticks = count(p, 18, 20, &l, 't');
  CTUI_TEST_ASSERT(ticks > 10, "checked: a tick (%d px)", ticks);
  c.flags |= CTUI_LOOK_CTL_OFF;
  p = paint(c, &l, 18, 20);
  CTUI_TEST_ASSERT(count(p, 18, 20, &l, 't') == 0 &&
                       count(p, 18, 20, &l, 'x') == ticks,
                   "off: the tick disabled");
  c = ctl(CTUI_LOOK_CTL_RADIO, 0, 0);
  p = paint(c, &l, 24, 16);
  char was = px(p, 24, &l, 0, 0);
  c.flags = CTUI_LOOK_CTL_FOCUSED;
  p = paint(c, &l, 24, 16);
  CTUI_TEST_ASSERT(was != 't' && px(p, 24, &l, 0, 0) == 't' &&
                       px(p, 24, &l, 1, 0) != 't' &&
                       px(p, 24, &l, 23, 14) == 't',
                   "focused (a page's radio button without a label): a "
                   "dotted rectangle round it in the text's colour");
  c = ctl(CTUI_LOOK_CTL_BUTTON, 0, 0);
  p = paint(c, &l, 36, 20);
  char face = px(p, 36, &l, 18, 4);
  c.flags = CTUI_LOOK_CTL_FOCUSED;
  p = paint(c, &l, 36, 20);
  int in = l.bevel + !!l.outline + 1, dots = 0;
  for (int x = in; x < 36 - in; x++) {
    dots += px(p, 36, &l, x, in) == 't';
  }
  CTUI_TEST_ASSERT(face != 't' && dots > 5 && dots < 36 - 2 * in &&
                       px(p, 36, &l, 0, 0) != 't',
                   "a focused button (an app's): 95's dotted rectangle "
                   "inside its bevel (%d dots)",
                   dots);
  CTUI_TEST_ASSERT(
      ctui_look_control_key(
          &(CTUI_LOOK_CONTROL){.kind = CTUI_LOOK_CTL_BUTTON,
                               .picto = CTUI_LOOK_PICTO_HOME,
                               .flags = CTUI_LOOK_CTL_FOCUSED},
          &l, 36, 20) ==
          ctui_look_control_key(
              &(CTUI_LOOK_CONTROL){.kind = CTUI_LOOK_CTL_BUTTON,
                                   .picto = CTUI_LOOK_PICTO_HOME},
              &l, 36, 20),
      "focused means nothing to a toolbar's button: the same image");

  c = ctl(CTUI_LOOK_CTL_TOGGLE, 0, 0);
  p = paint(c, &l, 36, 20);
  CTUI_TEST_ASSERT(px(p, 36, &l, 0, 4) == 'h' && px(p, 36, &l, 30, 10) == 'g',
                   "off: the thumb left, the groove");
  c.flags = CTUI_LOOK_CTL_CHECKED;
  p = paint(c, &l, 36, 20);
  CTUI_TEST_ASSERT(px(p, 36, &l, 35, 15) == 'd' && px(p, 36, &l, 5, 10) == 'a',
                   "on: the thumb right, the accent");
  CTUI_TEST_ASSERT(
      ctui_look_control_key(&(CTUI_LOOK_CONTROL){CTUI_LOOK_CTL_SLIDER, 5, 10, 0,
                                                 CTUI_LOOK_CTL_CHECKED, 0, 0, 0,
                                                 0, 0, 0, 0, 0, 0, 0},
                            &l, 36, 20) ==
          ctui_look_control_key(&(CTUI_LOOK_CONTROL){CTUI_LOOK_CTL_SLIDER, 5,
                                                     10, 0, 0, 0, 0, 0, 0, 0, 0,
                                                     0, 0, 0, 0},
                                &l, 36, 20),
      "a flag a kind doesn't draw doesn't split its images");
}

static void test_buttons(void) {
  CTUI_LOOK l = test_look();
  CTUI_LOOK_CONTROL c = ctl(CTUI_LOOK_CTL_BUTTON, 0, 0);
  c.glyph = CTUI_LOOK_GLYPH_CLOSE;
  const unsigned char *p = paint(c, &l, 18, 20);
  int marks = count(p, 18, 20, &l, 't');
  CTUI_TEST_ASSERT(px(p, 18, &l, 0, 0) == 'h' && px(p, 18, &l, 17, 19) == 'd' &&
                       marks > 0,
                   "a raised button with its glyph");
  /* the ✕ is mirror-symmetric */
  int sym = 1;
  for (int y = 0; y < 20; y++) {
    for (int x = 0; x < 18; x++) {
      int a = px(p, 18, &l, x, y) == 't';
      int m = px(p, 18, &l, 17 - x, y) == 't';
      sym &= a == m;
    }
  }
  CTUI_TEST_ASSERT(sym, "the close glyph mirrors");
  c.flags = CTUI_LOOK_CTL_PRESSED;
  p = paint(c, &l, 18, 20);
  CTUI_TEST_ASSERT(px(p, 18, &l, 0, 0) == 's' &&
                       count(p, 18, 20, &l, 't') == marks,
                   "pressed: sunken, the same glyph");

  int seen[CTUI_LOOK_GLYPHS][2] = {{0}};
  for (int g = CTUI_LOOK_GLYPH_CLOSE; g < CTUI_LOOK_GLYPHS; g++) {
    CTUI_LOOK_CONTROL chip = ctl(CTUI_LOOK_CTL_CHIP, 0, 0);
    chip.glyph = (CTUI_LOOK_GLYPH)g;
    p = paint(chip, &l, 18, 20);
    seen[g][0] = count(p, 18, 20, &l, 't');
    seen[g][1] = count(p, 18, 20, &l, '.');
    CTUI_TEST_ASSERT(seen[g][0] > 4 && seen[g][0] + seen[g][1] == 18 * 20,
                     "chip glyph %d: marks on nothing (%d px)", g, seen[g][0]);
  }
  CTUI_TEST_ASSERT(
      seen[CTUI_LOOK_GLYPH_PREV][0] == seen[CTUI_LOOK_GLYPH_NEXT][0] &&
          seen[CTUI_LOOK_GLYPH_PLAY][0] != seen[CTUI_LOOK_GLYPH_PAUSE][0],
      "prev/next mirror each other; play isn't pause");
  CTUI_LOOK_CONTROL chip = ctl(CTUI_LOOK_CTL_CHIP, 0, 0);
  chip.glyph = CTUI_LOOK_GLYPH_EJECT;
  chip.flags = CTUI_LOOK_CTL_HOVER;
  p = paint(chip, &l, 18, 20);
  CTUI_TEST_ASSERT(px(p, 18, &l, 0, 0) == 'h' && px(p, 18, &l, 17, 19) == 's' &&
                       count(p, 18, 20, &l, 't') ==
                           seen[CTUI_LOOK_GLYPH_EJECT][0],
                   "a hovered chip raises, its glyph unchanged");
  /* a menu's submenu arrow on its selection: the accent under, the glyph
   * in accent_text */
  const unsigned char *at = l.color[CTUI_LOOK_ACCENT_TEXT],
                      *ac = l.color[CTUI_LOOK_ACCENT];
  CTUI_LOOK_CONTROL arrow = ctl(CTUI_LOOK_CTL_CHIP, 0, 0);
  arrow.glyph = CTUI_LOOK_GLYPH_PLAY;
  arrow.tint = CTUI_LOOK_CTL_TINT | (uint32_t)at[0] << 16 |
               (uint32_t)at[1] << 8 | at[2];
  arrow.under = CTUI_LOOK_CTL_TINT | (uint32_t)ac[0] << 16 |
                (uint32_t)ac[1] << 8 | ac[2];
  p = paint(arrow, &l, 18, 20);
  CTUI_TEST_ASSERT(count(p, 18, 20, &l, 'A') == seen[CTUI_LOOK_GLYPH_PLAY][0] &&
                       count(p, 18, 20, &l, 'a') ==
                           18 * 20 - seen[CTUI_LOOK_GLYPH_PLAY][0],
                   "a tinted chip: its glyph in the tint, on what's under");
  arrow.flags = CTUI_LOOK_CTL_OFF;
  ctui_look_control_snap(&arrow, &l, 18, 20);
  CTUI_TEST_ASSERT(arrow.tint == 0, "off: the tint dropped (greyed)");
  l.bevel = 0;
  p = paint(chip, &l, 18, 20);
  CTUI_TEST_ASSERT(px(p, 18, &l, 0, 0) == 'l', "flat: the light face instead");
}

static void test_scroll(void) {
  CTUI_LOOK l = test_look();
  CTUI_LOOK_CONTROL c = {
      CTUI_LOOK_CTL_SCROLL, 75, 100, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const unsigned char *p = paint(c, &l, 10, 40);
  CTUI_TEST_ASSERT(
      px(p, 10, &l, 0, 0) == 'f' && px(p, 10, &l, 1, 0) == 'h' &&
          px(p, 10, &l, 0, 30) == 'h' && px(p, 10, &l, 9, 39) == 'd' &&
          px(p, 10, &l, 4, 29) == 'h' && px(p, 10, &l, 5, 29) == 'f',
      "vertical: the checker track, the thumb at the end");
  ctui_look_control_snap(&c, &l, 10, 40);
  CTUI_TEST_ASSERT(c.value == 30 && c.span == 10 && c.max == 40,
                   "snapped: the thumb's y and length");
  c = (CTUI_LOOK_CONTROL){
      CTUI_LOOK_CTL_SCROLL, 0, 1000, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  ctui_look_control_snap(&c, &l, 40, 10);
  CTUI_TEST_ASSERT(c.span == 10, "horizontal: a thumb never under square");
  c = (CTUI_LOOK_CONTROL){
      CTUI_LOOK_CTL_SCROLL, 3, 10, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  p = paint(c, &l, 10, 40);
  CTUI_TEST_ASSERT(px(p, 10, &l, 0, 0) == 'h' && px(p, 10, &l, 9, 39) == 'd',
                   "all of it shown: the thumb fills the track");
}

static void test_field(void) {
  CTUI_LOOK l = test_look();
  l.bevel = 2;
  l.outline = 0;
  CTUI_LOOK_CONTROL c = ctl(CTUI_LOOK_CTL_FIELD, 0, 0);
  const unsigned char *p = paint(c, &l, 60, 20);
  CTUI_TEST_ASSERT(px(p, 60, &l, 0, 0) == 's' && px(p, 60, &l, 59, 19) == 'h' &&
                       px(p, 60, &l, 1, 1) == 'd' &&
                       px(p, 60, &l, 30, 10) == 'g' &&
                       px(p, 60, &l, 57, 17) == 'g',
                   "a field: sunken, two px deep, the groove inside to its "
                   "right end");
  c.glyph = CTUI_LOOK_GLYPH_DOWN;
  p = paint(c, &l, 60, 20);
  CTUI_TEST_ASSERT(px(p, 60, &l, 42, 2) == 'h' && px(p, 60, &l, 41, 2) == 'g' &&
                       px(p, 60, &l, 57, 17) == 'd' &&
                       px(p, 60, &l, 30, 10) == 'g' &&
                       count(p, 60, 20, &l, 't') > 4,
                   "a drop-down: a raised button 16 px square at the right "
                   "end inside the edge, its arrow in it (%s)",
                   row(p, 60, &l, 2, 38, 22));
  c.flags = CTUI_LOOK_CTL_OFF;
  p = paint(c, &l, 60, 20);
  CTUI_TEST_ASSERT(px(p, 60, &l, 30, 10) == 'f' &&
                       count(p, 60, 20, &l, 'x') > 4,
                   "disabled: the face inside, the arrow greyed");
  c = ctl(CTUI_LOOK_CTL_CHECK, 0, 0);
  c.under = CTUI_LOOK_CTL_TINT | 0x010203u;
  p = paint(c, &l, 18, 20);
  CTUI_TEST_ASSERT(p[3] == 255 && p[0] == 1 && p[1] == 2 && p[2] == 3 &&
                       px(p, 18, &l, 9, 10) == 'g',
                   "under: what the control leaves clear painted in it");
  CTUI_LOOK_CONTROL d = c;
  d.under = 0x010203u;
  ctui_look_control_snap(&d, &l, 18, 20);
  CTUI_TEST_ASSERT(d.under == 0 && ctui_look_control_key(&c, &l, 18, 20) !=
                                       ctui_look_control_key(&d, &l, 18, 20),
                   "under without the tint bit is nothing; it is in the key");
}

static void test_frame(void) {
  CTUI_LOOK l = test_look();
  l.bevel = 2;
  l.outline = 0;
  CTUI_LOOK_CONTROL c = ctl(CTUI_LOOK_CTL_FRAME, 0, 0);
  c.under = CTUI_LOOK_CTL_TINT | 0x010203u;
  const unsigned char *p = paint(c, &l, 40, 16);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 0) == 's' && px(p, 40, &l, 1, 1) == 'h' &&
                       px(p, 40, &l, 38, 14) == 's' &&
                       px(p, 40, &l, 39, 15) == 'h' &&
                       px(p, 40, &l, 20, 0) == 's' &&
                       px(p, 40, &l, 20, 1) == 'h' && p[(8 * 40 + 20) * 4] == 1,
                   "a frame: 95's etched edge (shadow, highlight a px in), "
                   "the paper inside");
  l.bevel = 0;
  p = paint(c, &l, 40, 16);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 0) == 's' && px(p, 40, &l, 39, 15) == 's' &&
                       p[(1 * 40 + 1) * 4] == 1,
                   "flat: a 1 px line");
  l.outline = 1;
  p = paint(c, &l, 40, 16);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 0) == 'd', "with an outline: dark");
}

/* painting the snapped state gives the same pixels, at every size */
static void test_snap_invariant(void) {
  static const int sizes[][2] = {{9, 20}, {45, 20}, {14, 30}, {3, 3},
                                 {1, 1},  {10, 40}, {64, 9}};
  static unsigned char a[64 * 64 * 4], b[64 * 64 * 4];
  const char *names[] = {"win95", "nt-dark", "flat"};
  int bad = 0, n = 0;
  for (int li = 0; li < 3; li++) {
    CTUI_LOOK l;
    ctui_look_builtin(names[li], &l);
    l.corner = li; /* 0, 1, 2 */
    l.outline = li == 1;
    for (int k = 0; k < CTUI_LOOK_CTL_KINDS; k++) {
      for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        int w = sizes[s][0], h = sizes[s][1];
        for (int v = -5; v <= 120; v += 7) {
          CTUI_LOOK_CONTROL c = {(CTUI_LOOK_CTL_KIND)k,
                                 v,
                                 100,
                                 30,
                                 (unsigned)v & 15u,
                                 (CTUI_LOOK_GLYPH)((v + 7) % CTUI_LOOK_GLYPHS),
                                 (CTUI_LOOK_PICTO)((v + 5) % CTUI_LOOK_PICTOS),
                                 0,
                                 0,
                                 (uint8_t)(v % CTUI_LOOK_BOXES),
                                 (uint8_t)v,
                                 (uint8_t)(v * 3),
                                 (v & 4)   ? CTUI_LOOK_CTL_TINT | 0x506070u
                                 : (v & 2) ? 0x77u
                                           : 0,
                                 0,
                                 0};
          if (k == CTUI_LOOK_CTL_SIGNAL) {
            c.value = v / 20;
            c.max = 5;
          }
          c.tint = (v & 8)    ? CTUI_LOOK_CTL_TINT | 0x3c9f5au
                   : (v & 16) ? 0x42u
                              : 0;
          c.under = (v & 32)   ? CTUI_LOOK_CTL_TINT | 0x102030u
                    : (v & 64) ? 0x55u
                               : 0;
          CTUI_LOOK_CONTROL s2 = c;
          ctui_look_control_snap(&s2, &l, w, h);
          ctui_look_control_paint(&c, &l, a, w, h);
          ctui_look_control_paint(&s2, &l, b, w, h);
          n++;
          if (memcmp(a, b, (size_t)w * (size_t)h * 4) != 0 ||
              ctui_look_control_key(&c, &l, w, h) !=
                  ctui_look_control_key(&s2, &l, w, h)) {
            bad++;
            fprintf(stderr, "look %s kind %d %dx%d v %d: snap differs\n",
                    names[li], k, w, h, v);
          }
        }
      }
    }
  }
  CTUI_TEST_ASSERT(bad == 0, "snap keeps pixels and key (%d of %d differ)", bad,
                   n);
}

static CTUI_LOOK_CONTROL picto(CTUI_LOOK_PICTO p, int value, int max, int off) {
  return (CTUI_LOOK_CONTROL){.kind = CTUI_LOOK_CTL_PICTO,
                             .picto = p,
                             .value = value,
                             .max = max,
                             .flags = off ? CTUI_LOOK_CTL_OFF : 0};
}

/* opaque pixels outside x0..x1 x y0..y1 (inclusive) */
static int outside(const unsigned char *rgba, int w, int h, int x0, int y0,
                   int x1, int y1) {
  int n = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      n += rgba[(y * w + x) * 4 + 3] && (x < x0 || x > x1 || y < y0 || y > y1);
    }
  }
  return n;
}

static int opaque(const unsigned char *rgba, int w, int h) {
  return outside(rgba, w, h, w, h, -1, -1);
}

static void test_pictos(void) {
  CTUI_LOOK l = test_look();
  int all_fit = 1, all_scale = 1;
  for (int p = CTUI_LOOK_PICTO_NONE + 1; p < CTUI_LOOK_PICTOS; p++) {
    CTUI_LOOK_CONTROL c = picto((CTUI_LOOK_PICTO)p, 100, 100, 0);
    ctui_look_control_snap(&c, &l, 18, 20);
    const unsigned char *a = paint(c, &l, 18, 20);
    int n = opaque(a, 18, 20);
    /* 2 cells of DP-1: the 16x16 grid at 1x, centred from (1, 2) */
    if (n < 20 || outside(a, 18, 20, 1, 2, 16, 17)) {
      all_fit = 0;
      fprintf(stderr, "picto %d: %d px, %d outside its grid\n", p, n,
              outside(a, 18, 20, 1, 2, 16, 17));
    }
    const unsigned char *b = paint(c, &l, 36, 40);
    all_scale &= opaque(b, 36, 40) == 4 * n;
  }
  CTUI_TEST_ASSERT(all_fit, "every pictogram draws inside its 16x16 grid, "
                            "centred in 2 cells of 9x20");
  CTUI_TEST_ASSERT(all_scale, "twice the room: each pixel 2x2");
  /* 2 cells of HDMI (14x30): the top bar's pictograms and a browser's
   * toolbar's have 24 px art, centred from (2, 3); the others stay 16 px
   * from (6, 7) */
  int big_fit = 1, bigs = 0;
  for (int p = CTUI_LOOK_PICTO_NONE + 1; p < CTUI_LOOK_PICTOS; p++) {
    CTUI_LOOK_CONTROL c = picto((CTUI_LOOK_PICTO)p, 100, 100, 0);
    ctui_look_control_snap(&c, &l, 28, 30);
    int n18 = opaque(paint(c, &l, 18, 20), 18, 20);
    const unsigned char *a = paint(c, &l, 28, 30);
    int n = opaque(a, 28, 30), small = !outside(a, 28, 30, 6, 7, 21, 22);
    bigs += !small;
    if (outside(a, 28, 30, 2, 3, 25, 26) || (small ? n != n18 : n <= n18)) {
      big_fit = 0;
      fprintf(stderr, "picto %d at 28x30: %d px (%d at 18x20)\n", p, n, n18);
    }
  }
  CTUI_TEST_ASSERT(big_fit && bigs == 27,
                   "2 cells of 14x30: %d pictograms on their 24x24 grid, "
                   "the rest as before",
                   bigs);
  CTUI_LOOK_CONTROL big_bat = picto(CTUI_LOOK_PICTO_BATTERY, 50, 100, 0);
  CTUI_LOOK_CONTROL big_mute = picto(CTUI_LOOK_PICTO_SPEAKER, 80, 100, 1);
  CTUI_LOOK_CONTROL big_mic = picto(CTUI_LOOK_PICTO_MIC, 0, 0, 1);
  int half = count(paint(big_bat, &l, 28, 30), 28, 30, &l, 'a');
  int crossed = count(paint(big_mute, &l, 28, 30), 28, 30, &l, 't');
  int slashed = count(paint(big_mic, &l, 28, 30), 28, 30, &l, 't');
  CTUI_TEST_ASSERT(half == 70 && crossed == 36 && slashed == 44,
                   "... the battery half lit (7 of 15 columns of 10), the "
                   "cross and the slash at that size (%d %d %d)",
                   half, crossed, slashed);
  CTUI_LOOK_CONTROL none = picto(CTUI_LOOK_PICTO_NONE, 0, 0, 0);
  CTUI_TEST_ASSERT(opaque(paint(none, &l, 18, 20), 18, 20) == 0,
                   "no pictogram: nothing");

  /* the speaker: a wave for any level, one more each third */
  static const int levels[][2] = {{0, 0},  {1, 1},  {33, 1},  {34, 2},
                                  {66, 2}, {67, 3}, {100, 3}, {150, 3}};
  int waves_ok = 1;
  for (size_t i = 0; i < sizeof levels / sizeof *levels; i++) {
    int v = levels[i][0], m = 100;
    ctui_look_picto_snap(CTUI_LOOK_PICTO_SPEAKER, 0, &v, &m);
    waves_ok &= v == levels[i][1] && m == 3;
  }
  CTUI_TEST_ASSERT(waves_ok, "speaker: 0, 1-33, 34-66, 67+ -> 0-3 waves");
  CTUI_LOOK_CONTROL sp = picto(CTUI_LOOK_PICTO_SPEAKER, 0, 100, 0);
  int body = count(paint(sp, &l, 18, 20), 18, 20, &l, 't');
  int grew[3];
  for (int k = 1; k <= 3; k++) {
    CTUI_LOOK_CONTROL c = picto(CTUI_LOOK_PICTO_SPEAKER, 33 * k, 99, 0);
    ctui_look_control_snap(&c, &l, 18, 20);
    grew[k - 1] = count(paint(c, &l, 18, 20), 18, 20, &l, 't') - body;
  }
  CTUI_TEST_ASSERT(grew[0] == 4 && grew[1] == 12 && grew[2] == 24,
                   "the waves: 4, 8 and 12 px (%d %d %d)", grew[0], grew[1],
                   grew[2]);
  CTUI_LOOK_CONTROL muted = picto(CTUI_LOOK_PICTO_SPEAKER, 80, 100, 1);
  ctui_look_control_snap(&muted, &l, 18, 20);
  const unsigned char *m = paint(muted, &l, 18, 20);
  CTUI_TEST_ASSERT(muted.value == 0 && count(m, 18, 20, &l, 't') == 20 &&
                       count(m, 18, 20, &l, 'x') > 20 &&
                       count(m, 18, 20, &l, 'h') == 0,
                   "muted: no waves, the body greyed, a cross in ink");
  CTUI_LOOK_CONTROL mic = picto(CTUI_LOOK_PICTO_MIC, 0, 0, 1);
  const unsigned char *mm = paint(mic, &l, 18, 20);
  CTUI_TEST_ASSERT(count(mm, 18, 20, &l, 'x') > 20 &&
                       count(mm, 18, 20, &l, 't') == 28,
                   "a muted mic: greyed, slashed");

  /* the battery's charge: 10 columns of 6 */
  int charge[3];
  const int pct[3] = {0, 50, 100};
  for (int i = 0; i < 3; i++) {
    CTUI_LOOK_CONTROL c = picto(CTUI_LOOK_PICTO_BATTERY, pct[i], 100, 0);
    ctui_look_control_snap(&c, &l, 18, 20);
    charge[i] = count(paint(c, &l, 18, 20), 18, 20, &l, 'a');
  }
  CTUI_TEST_ASSERT(charge[0] == 0 && charge[1] == 30 && charge[2] == 60,
                   "battery: its charge in accent (%d %d %d px)", charge[0],
                   charge[1], charge[2]);
  CTUI_LOOK_CONTROL level = picto(CTUI_LOOK_PICTO_BATTERY, 55, 0, 0);
  ctui_look_control_snap(&level, &l, 18, 20);
  const unsigned char *lv = paint(level, &l, 18, 20);
  CTUI_TEST_ASSERT(count(lv, 18, 20, &l, 'a') == 0 &&
                       count(lv, 18, 20, &l, 'g') == 96,
                   "no number (a level word only): empty inside");

  CTUI_LOOK_CONTROL bt = picto(CTUI_LOOK_PICTO_BLUETOOTH, 0, 0, 0);
  const unsigned char *on = paint(bt, &l, 18, 20);
  int rune = count(on, 18, 20, &l, 'A'), oval = count(on, 18, 20, &l, 'a');
  bt.flags = CTUI_LOOK_CTL_OFF;
  const unsigned char *off = paint(bt, &l, 18, 20);
  CTUI_TEST_ASSERT(rune > 20 && oval > 60 &&
                       count(off, 18, 20, &l, 'x') == rune &&
                       opaque(off, 18, 20) == rune,
                   "bluetooth off: the rune alone, greyed");

  CTUI_LOOK_CONTROL k1 = picto(CTUI_LOOK_PICTO_SPEAKER, 40, 100, 0);
  CTUI_LOOK_CONTROL k2 = picto(CTUI_LOOK_PICTO_SPEAKER, 60, 100, 0);
  CTUI_LOOK_CONTROL k3 = picto(CTUI_LOOK_PICTO_SPEAKER, 70, 100, 0);
  CTUI_LOOK_CONTROL k4 = picto(CTUI_LOOK_PICTO_SPEAKER, 10, 100, 1);
  CTUI_LOOK_CONTROL k5 = picto(CTUI_LOOK_PICTO_SPEAKER, 90, 100, 1);
  CTUI_LOOK_CONTROL k6 = picto(CTUI_LOOK_PICTO_MIC, 40, 100, 0);
  CTUI_LOOK_CONTROL k7 = picto(CTUI_LOOK_PICTO_MIC, 90, 100, 0);
  uint64_t key[7] = {ctui_look_control_key(&k1, &l, 18, 20),
                     ctui_look_control_key(&k2, &l, 18, 20),
                     ctui_look_control_key(&k3, &l, 18, 20),
                     ctui_look_control_key(&k4, &l, 18, 20),
                     ctui_look_control_key(&k5, &l, 18, 20),
                     ctui_look_control_key(&k6, &l, 18, 20),
                     ctui_look_control_key(&k7, &l, 18, 20)};
  CTUI_TEST_ASSERT(key[0] == key[1] && key[1] != key[2] && key[3] == key[4] &&
                       key[3] != key[0] && key[5] == key[6] && key[5] != key[0],
                   "keys: equal for the same waves, any muted level, a mic's "
                   "level; apart for another picture");

  int bad = 0;
  for (int p = 0; p < CTUI_LOOK_PICTOS; p++) {
    for (int v = -10; v <= 130; v += 9) {
      for (int o = 0; o < 2; o++) {
        CTUI_LOOK_CONTROL c = picto((CTUI_LOOK_PICTO)p, v, 100, o), s2 = c;
        ctui_look_control_snap(&s2, &l, 18, 20);
        static unsigned char a[18 * 20 * 4], b[18 * 20 * 4];
        ctui_look_control_paint(&c, &l, a, 18, 20);
        ctui_look_control_paint(&s2, &l, b, 18, 20);
        bad += memcmp(a, b, sizeof a) != 0;
      }
    }
  }
  CTUI_TEST_ASSERT(bad == 0, "pictograms: snap keeps the pixels (%d differ)",
                   bad);
}

/* ctui-web's chrome (PLAN.md "ctui-web as a 90s browser", n1): labelled
 * toolbar buttons, the key, the busy logo, a status bar's panel */
static void test_browser(void) {
  CTUI_LOOK l = test_look();
  l.bevel = 2;
  l.outline = 0;
  /* a toolbar button two rows of 14x30: its pictogram in the top row */
  CTUI_LOOK_CONTROL b = {.kind = CTUI_LOOK_CTL_BUTTON,
                         .picto = CTUI_LOOK_PICTO_BACK,
                         .value = 1,
                         .max = 2};
  const unsigned char *p = paint(b, &l, 63, 60);
  int below = 0, art = 0;
  for (int y = 2; y < 58; y++) {
    for (int x = 2; x < 61; x++) {
      char c = px(p, 63, &l, x, y);
      art += c != 'f';
      below += c != 'f' && y >= 30;
    }
  }
  CTUI_TEST_ASSERT(art > 100 && below == 0 && px(p, 63, &l, 0, 0) == 'h' &&
                       px(p, 63, &l, 62, 59) == 'd',
                   "a toolbar button: raised, its pictogram in the top row, "
                   "the label's row left plain (%d px, %d below)",
                   art, below);
  CTUI_LOOK_CONTROL whole = b;
  whole.max = 0;
  int top = count(paint(b, &l, 63, 60), 63, 60, &l, 'a');
  int all = count(paint(whole, &l, 63, 60), 63, 60, &l, 'a');
  CTUI_TEST_ASSERT(all > top,
                   "without max: the pictogram fills the button "
                   "(bigger: %d px of accent, %d)",
                   all, top);
  CTUI_LOOK_CONTROL stop = b;
  stop.picto = CTUI_LOOK_PICTO_STOP;
  stop.tint = CTUI_LOOK_CTL_TINT | 0xc02020u;
  int red = 0;
  p = paint(stop, &l, 63, 60);
  for (int i = 0; i < 63 * 60; i++) {
    red += p[i * 4] == 0xc0 && p[i * 4 + 1] == 0x20 && p[i * 4 + 2] == 0x20;
  }
  CTUI_TEST_ASSERT(red > 50 && count(p, 63, 60, &l, 'a') == 0,
                   "stop: its sign in the button's tint (%d px)", red);
  stop.flags = CTUI_LOOK_CTL_OFF;
  CTUI_LOOK_CONTROL s2 = stop;
  ctui_look_control_snap(&s2, &l, 63, 60);
  CTUI_TEST_ASSERT(s2.tint == 0 &&
                       count(paint(stop, &l, 63, 60), 63, 60, &l, 'x') > 50,
                   "disabled: greyed, the tint gone");

  /* the key: whole for https, broken (still in colour) for http */
  CTUI_LOOK_CONTROL k = picto(CTUI_LOOK_PICTO_KEY, 0, 0, 0);
  int warm = count(paint(k, &l, 28, 30), 28, 30, &l, 'w');
  k.flags = CTUI_LOOK_CTL_OFF;
  const unsigned char *kb = paint(k, &l, 28, 30);
  CTUI_TEST_ASSERT(warm > 40 && count(kb, 28, 30, &l, 'w') > 30 &&
                       count(kb, 28, 30, &l, 'x') == 0,
                   "the key; broken, in colour");

  /* the busy logo: each frame its own picture, 16 of them, idle apart */
  uint64_t keys[17];
  int distinct = 1;
  for (int f = 0; f <= 16; f++) {
    CTUI_LOOK_CONTROL t = picto(CTUI_LOOK_PICTO_LOGO, f, 16, 0);
    keys[f] = ctui_look_control_key(&t, &l, 56, 54);
    for (int g = 0; g < f; g++) {
      distinct &= keys[g] != keys[f];
    }
  }
  CTUI_LOOK_CONTROL f17 = picto(CTUI_LOOK_PICTO_LOGO, 17, 32, 0);
  CTUI_LOOK_CONTROL idle = picto(CTUI_LOOK_PICTO_LOGO, 0, 16, 0);
  CTUI_LOOK_CONTROL plain = picto(CTUI_LOOK_PICTO_LOGO, 0, 0, 0);
  CTUI_TEST_ASSERT(distinct &&
                       ctui_look_control_key(&f17, &l, 56, 54) == keys[1] &&
                       ctui_look_control_key(&idle, &l, 56, 54) ==
                           ctui_look_control_key(&plain, &l, 56, 54),
                   "the throbber: 16 frames, counting on wraps round, frame "
                   "0 is the logo at rest");
  int lit[2];
  for (int f = 0; f < 2; f++) {
    CTUI_LOOK_CONTROL t = picto(CTUI_LOOK_PICTO_LOGO, f + 1, 16, 0);
    lit[f] = count(paint(t, &l, 48, 48), 48, 48, &l, 'h');
  }
  int rest = count(paint(plain, &l, 48, 48), 48, 48, &l, 'h');
  CTUI_TEST_ASSERT(lit[0] > rest && lit[1] > rest && lit[0] != lit[1],
                   "busy: lines printed up its screen (%d %d, %d at rest)",
                   lit[0], lit[1], rest);

  /* a status bar's panel: 1 px sunken, the face inside */
  CTUI_LOOK_CONTROL pn = ctl(CTUI_LOOK_CTL_PANEL, 0, 0);
  p = paint(pn, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 0) == 's' && px(p, 40, &l, 39, 19) == 'h' &&
                       px(p, 40, &l, 1, 1) == 'f' &&
                       px(p, 40, &l, 20, 10) == 'f',
                   "a panel: a shadow top-left, highlight bottom-right, the "
                   "face inside");

  /* a splitter: the face, three raised bumps in its middle; the shadow
   * while dragged */
  CTUI_LOOK_CONTROL sp = ctl(CTUI_LOOK_CTL_SPLITTER, 0, 0);
  p = paint(sp, &l, 9, 60);
  /* 9 px across: 3 px bumps at x 3..5, 15 px of them from y 22 */
  CTUI_TEST_ASSERT(px(p, 9, &l, 0, 0) == 'f' && px(p, 9, &l, 8, 59) == 'f' &&
                       px(p, 9, &l, 4, 10) == 'f',
                   "a splitter: the face along it");
  CTUI_TEST_ASSERT(px(p, 9, &l, 3, 22) == 'h' && px(p, 9, &l, 5, 24) == 's' &&
                       px(p, 9, &l, 3, 34) == 'h' && px(p, 9, &l, 3, 25) == 'f',
                   "its grip: raised bumps a bump apart down the middle");
  p = paint(sp, &l, 60, 9);
  CTUI_TEST_ASSERT(px(p, 60, &l, 22, 3) == 'h' && px(p, 60, &l, 34, 3) == 'h',
                   "wider than tall: the grip runs across");
  sp.flags = CTUI_LOOK_CTL_PRESSED | CTUI_LOOK_CTL_HOVER;
  p = paint(sp, &l, 9, 60);
  CTUI_TEST_ASSERT(px(p, 9, &l, 0, 0) == 's' && px(p, 9, &l, 3, 22) == 'h',
                   "dragged: the shadow round its grip");
  CTUI_LOOK_CONTROL hover = ctl(CTUI_LOOK_CTL_SPLITTER, 5, 9);
  hover.flags = CTUI_LOOK_CTL_HOVER;
  CTUI_TEST_ASSERT(
      ctui_look_control_key(&hover, &l, 9, 60) ==
          ctui_look_control_key(
              &(CTUI_LOOK_CONTROL){.kind = CTUI_LOOK_CTL_SPLITTER}, &l, 9, 60),
      "only being dragged tells two splitters apart");

  /* a bar's surface: the face, raised along the sides facing the screen
   * (a bottom taskbar's top, a top bar's bottom) */
  CTUI_LOOK_CONTROL bar = ctl(CTUI_LOOK_CTL_BAR, 0, 0);
  p = paint(bar, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 0) == 'f' && px(p, 40, &l, 39, 19) == 'f',
                   "a bar without sides: the face alone");
  bar.sides = 1;
  p = paint(bar, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 5, 0) == 'h' && px(p, 40, &l, 5, 1) == 'l' &&
                       px(p, 40, &l, 5, 2) == 'f' &&
                       px(p, 40, &l, 5, 19) == 'f',
                   "a taskbar: lit along its top, the face below");
  bar.sides = 4;
  p = paint(bar, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 5, 19) == 'd' && px(p, 40, &l, 5, 18) == 's' &&
                       px(p, 40, &l, 5, 0) == 'f',
                   "a top bar: shaded along its bottom");
  CTUI_LOOK flat = l;
  flat.bevel = 0;
  p = paint(bar, &flat, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &flat, 5, 19) == 's' &&
                       px(p, 40, &flat, 5, 18) == 'f',
                   "flat: a line in the shadow");
  CTUI_LOOK_CONTROL tb = ctl(CTUI_LOOK_CTL_BAR, 3, 9);
  tb.sides = 1 | 0x30;
  tb.flags = CTUI_LOOK_CTL_HOVER | CTUI_LOOK_CTL_CHECKED;
  CTUI_LOOK_CONTROL top0 = ctl(CTUI_LOOK_CTL_BAR, 0, 0);
  top0.sides = 1;
  CTUI_TEST_ASSERT(ctui_look_control_key(&tb, &l, 40, 20) ==
                           ctui_look_control_key(&top0, &l, 40, 20) &&
                       ctui_look_control_key(&top0, &l, 40, 20) !=
                           ctui_look_control_key(&bar, &l, 40, 20),
                   "only a bar's sides tell two apart");

  /* a latched button (the active window's on a taskbar): sunken, the
   * highlight checkered over the face inside */
  CTUI_LOOK_CONTROL up = ctl(CTUI_LOOK_CTL_BUTTON, 0, 0);
  CTUI_LOOK_CONTROL down = up;
  down.flags = CTUI_LOOK_CTL_CHECKED;
  p = paint(down, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 0) == 's' && px(p, 40, &l, 39, 19) == 'h' &&
                       px(p, 40, &l, 10, 10) == 'h' &&
                       px(p, 40, &l, 11, 10) == 'f',
                   "latched: sunken, checkered inside (%c %c)",
                   px(p, 40, &l, 10, 10), px(p, 40, &l, 11, 10));
  CTUI_TEST_ASSERT(ctui_look_control_key(&down, &l, 40, 20) !=
                       ctui_look_control_key(&up, &l, 40, 20),
                   "a latched button is an image of its own");
  p = paint(down, &flat, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &flat, 10, 10) == 'l' &&
                       px(p, 40, &flat, 11, 10) == 'l',
                   "flat: latched is the light face");

  /* a taskbar's button kept off the bar's edge: clear above and below */
  CTUI_LOOK_CONTROL off = up;
  off.span = 3;
  p = paint(off, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 10, 2) == '.' && px(p, 40, &l, 10, 3) == 'h' &&
                       px(p, 40, &l, 10, 16) == 'd' &&
                       px(p, 40, &l, 10, 17) == '.',
                   "span: the button between 3 clear px top and bottom");
  CTUI_LOOK_CONTROL far = up, quarter = up;
  far.span = 9;
  quarter.span = 5;
  CTUI_TEST_ASSERT(ctui_look_control_key(&off, &l, 40, 20) !=
                           ctui_look_control_key(&up, &l, 40, 20) &&
                       ctui_look_control_key(&far, &l, 40, 20) ==
                           ctui_look_control_key(&quarter, &l, 40, 20),
                   "a span is its own image, up to a quarter of the height");
}

/* a page's frames a row at a time (CTUI_LOOK_CTL_BOX; n2) */
static CTUI_LOOK_CONTROL box(int style, int sides, int place, int cols) {
  return (CTUI_LOOK_CONTROL){.kind = CTUI_LOOK_CTL_BOX,
                             .span = cols,
                             .box = (uint8_t)style,
                             .sides = (uint8_t)sides,
                             .place = (uint8_t)place,
                             .under = CTUI_LOOK_CTL_TINT | 0x0a0b0cu};
}

static void test_frames(void) {
  CTUI_LOOK l = test_look();
  /* a rule: one row, its top edge through the middle, etched */
  CTUI_LOOK_CONTROL hr =
      box(CTUI_LOOK_BOX_GROOVE, 1,
          CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_BOTTOM | CTUI_LOOK_BOX_THICK, 4);
  const unsigned char *p = paint(hr, &l, 40, 20);
  CTUI_TEST_ASSERT(
      px(p, 40, &l, 20, 9) == 's' && px(p, 40, &l, 20, 10) == 'h' &&
          p[(5 * 40 + 20) * 4] == 0x0a && p[(15 * 40 + 20) * 4] == 0x0a &&
          !strcmp(row(p, 40, &l, 9, 0, 6), "????ss"),
      "a rule: shadow over highlight mid-row from the middle of "
      "its first cell, the paper round it ('%s')",
      row(p, 40, &l, 9, 0, 6));
  /* a group box's top row (an app's form): its edge open over the cells
   * its heading takes, a different image than without */
  CTUI_LOOK_CONTROL grp = box(CTUI_LOOK_BOX_GROOVE, 0xf,
                              CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_FLUSH_X, 8);
  grp.under = 0;
  uint64_t open_key = ctui_look_control_key(&grp, &l, 80, 20);
  grp.gap = 2;
  grp.gap_cols = 3;
  p = paint(grp, &l, 80, 20);
  CTUI_TEST_ASSERT(px(p, 80, &l, 12, 9) == 's' && px(p, 80, &l, 20, 9) == '.' &&
                       px(p, 80, &l, 44, 9) == '.' &&
                       px(p, 80, &l, 52, 9) == 's' &&
                       ctui_look_control_key(&grp, &l, 80, 20) != open_key,
                   "a group box's top edge: open over cells 2-4 (its "
                   "heading), whole around them");
  /* a raised box's middle row: only its sides, through the middle of its
   * first and last cell */
  CTUI_LOOK_CONTROL mid =
      box(CTUI_LOOK_BOX_OUTSET, 0xf, CTUI_LOOK_BOX_THICK, 4);
  p = paint(mid, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 4, 0) == 'h' && px(p, 40, &l, 5, 19) == 'l' &&
                       px(p, 40, &l, 35, 7) == 'd' &&
                       px(p, 40, &l, 34, 7) == 's' &&
                       px(p, 40, &l, 20, 0) == '?',
                   "a raised box's middle row: highlight / light left, dark / "
                   "shadow right, nothing across");
  /* a sunken cell's only row, flush: all four sides at the cells' edge */
  CTUI_LOOK_CONTROL cell = box(CTUI_LOOK_BOX_INSET, 0xf,
                               CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_BOTTOM |
                                   CTUI_LOOK_BOX_FLUSH | CTUI_LOOK_BOX_FLUSH_X,
                               4);
  p = paint(cell, &l, 40, 20);
  CTUI_TEST_ASSERT(
      px(p, 40, &l, 10, 0) == 's' && px(p, 40, &l, 0, 10) == 's' &&
          px(p, 40, &l, 10, 19) == 'h' && px(p, 40, &l, 39, 10) == 'h' &&
          px(p, 40, &l, 39, 0) == 'h' && px(p, 40, &l, 0, 19) == 'h',
      "a sunken cell: shadow top-left, highlight bottom-right "
      "(winning their corners), flush");
  /* only the side columns flush (a collapsed table's cell): its edges
   * through the middle of the bars' columns */
  CTUI_LOOK_CONTROL col = box(CTUI_LOOK_BOX_SOLID, 0xf, CTUI_LOOK_BOX_FLUSH, 4);
  p = paint(col, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 4, 10) == 's' &&
                       px(p, 40, &l, 34, 10) == 's' &&
                       px(p, 40, &l, 0, 10) == '?',
                   "flush rows, columns through the middle");
  /* a field: the groove inside its sunken edge */
  CTUI_LOOK_CONTROL field =
      box(CTUI_LOOK_BOX_FIELD, 0,
          CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_FLUSH | CTUI_LOOK_BOX_FLUSH_X, 4);
  p = paint(field, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 0, 0) == 's' && px(p, 40, &l, 1, 1) == 'd' &&
                       px(p, 40, &l, 20, 10) == 'g' &&
                       px(p, 40, &l, 39, 10) == 'h',
                   "a code block's field: sunken 2 px, the groove inside");
  /* dashed and dotted: gaps; a flat line in its tint */
  CTUI_LOOK_CONTROL dash =
      box(CTUI_LOOK_BOX_DASHED, 1,
          CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_FLUSH | CTUI_LOOK_BOX_FLUSH_X, 4);
  const char *d = row(paint(dash, &l, 40, 20), 40, &l, 0, 0, 12);
  CTUI_TEST_ASSERT(!strcmp(d, "ssss??ssss??"), "dashed: 4 on, 2 off ('%s')", d);
  dash.box = CTUI_LOOK_BOX_DOTTED;
  d = row(paint(dash, &l, 40, 20), 40, &l, 0, 0, 6);
  CTUI_TEST_ASSERT(!strcmp(d, "s?s?s?"), "dotted: every other ('%s')", d);
  dash.box = CTUI_LOOK_BOX_SOLID;
  dash.tint = CTUI_LOOK_CTL_TINT | 0x123456u;
  p = paint(dash, &l, 40, 20);
  CTUI_TEST_ASSERT(p[0] == 0x12 && p[1] == 0x34 && p[2] == 0x56,
                   "a flat line in its tint (the page's dim ink)");
  /* an edge the paper's own colour: the face instead (white on white) */
  CTUI_LOOK_CONTROL white = cell;
  white.under = CTUI_LOOK_CTL_TINT |
                (uint32_t)l.color[CTUI_LOOK_HIGHLIGHT][0] << 16 |
                (uint32_t)l.color[CTUI_LOOK_HIGHLIGHT][1] << 8 |
                l.color[CTUI_LOOK_HIGHLIGHT][2];
  p = paint(white, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 10, 19) == 'f' && px(p, 40, &l, 10, 0) == 's',
                   "a highlight on paper of its colour: the face");
  /* an icon two rows high: each row its half, clear round it */
  CTUI_LOOK_CONTROL top = box(CTUI_LOOK_BOX_WARNING, 0, CTUI_LOOK_BOX_TOP, 4);
  CTUI_LOOK_CONTROL bot =
      box(CTUI_LOOK_BOX_WARNING, 0, CTUI_LOOK_BOX_BOTTOM, 4);
  CTUI_LOOK_CONTROL whole = box(CTUI_LOOK_BOX_WARNING, 0,
                                CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_BOTTOM, 4);
  top.under = bot.under = whole.under = 0;
  int wt = count(paint(top, &l, 32, 32), 32, 32, &l, 'w');
  int wb = count(paint(bot, &l, 32, 32), 32, 32, &l, 'w');
  int ww = count(paint(whole, &l, 32, 64), 32, 64, &l, 'w');
  CTUI_TEST_ASSERT(wt > 0 && wb > 0 && wt + wb == ww && g_buf[3] == 0,
                   "an icon over two rows: halves of the whole (%d + %d = %d)",
                   wt, wb, ww);
  CTUI_LOOK_CONTROL k1 = box(CTUI_LOOK_BOX_INSET, 0xf, CTUI_LOOK_BOX_TOP, 4);
  CTUI_LOOK_CONTROL k2 = box(CTUI_LOOK_BOX_INSET, 0xf, CTUI_LOOK_BOX_BOTTOM, 4);
  CTUI_LOOK_CONTROL k3 = box(CTUI_LOOK_BOX_OUTSET, 0xf, CTUI_LOOK_BOX_TOP, 4);
  CTUI_TEST_ASSERT(ctui_look_control_key(&k1, &l, 40, 20) !=
                           ctui_look_control_key(&k2, &l, 40, 20) &&
                       ctui_look_control_key(&k1, &l, 40, 20) !=
                           ctui_look_control_key(&k3, &l, 40, 20),
                   "keys: apart by place and style");
  /* n4: a background inside the edges, the paper round them */
  CTUI_LOOK_CONTROL panel =
      box(CTUI_LOOK_BOX_SOLID, 0xf, CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_THICK, 4);
  panel.fill = CTUI_LOOK_CTL_TINT | 0x405060u;
  p = paint(panel, &l, 40, 20);
  CTUI_TEST_ASSERT(
      p[(15 * 40 + 20) * 4] == 0x40 && p[(15 * 40 + 20) * 4 + 2] == 0x60 &&
          p[(2 * 40 + 20) * 4] == 0x0a && p[(15 * 40 + 1) * 4] == 0x0a &&
          px(p, 40, &l, 20, 9) == 's',
      "a filled box's top row: its background under the edge, "
      "the paper above and beside it");
  CTUI_LOOK_CONTROL plain = panel;
  plain.fill = 0;
  CTUI_TEST_ASSERT(ctui_look_control_key(&panel, &l, 40, 20) !=
                       ctui_look_control_key(&plain, &l, 40, 20),
                   "... its key apart");
  CTUI_LOOK_CONTROL icon = top;
  icon.fill = CTUI_LOOK_CTL_TINT | 0x405060u;
  ctui_look_control_snap(&icon, &l, 32, 32);
  CTUI_LOOK_CONTROL btn = {.kind = CTUI_LOOK_CTL_BUTTON,
                           .fill = CTUI_LOOK_CTL_TINT | 1};
  ctui_look_control_snap(&btn, &l, 32, 32);
  CTUI_TEST_ASSERT(!icon.fill && !btn.fill, "an icon, a button: no fill");
  CTUI_LOOK_CONTROL lone =
      box(CTUI_LOOK_BOX_SOLID, 0xf,
          CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_BOTTOM | CTUI_LOOK_BOX_FLUSH_TOP |
              CTUI_LOOK_BOX_FLUSH_X,
          4);
  p = paint(lone, &l, 40, 20);
  CTUI_TEST_ASSERT(px(p, 40, &l, 20, 0) == 's' && px(p, 40, &l, 20, 9) == 's' &&
                       px(p, 40, &l, 20, 19) != 's',
                   "one row, the top edge flush alone: at the top, the bottom "
                   "through the middle");
  /* bordered on the left only (a note's bar): the background to the
   * cells' outer side on the right, from the bar on the left */
  CTUI_LOOK_CONTROL bar =
      box(CTUI_LOOK_BOX_SOLID, 1 << 3, CTUI_LOOK_BOX_FLUSH_RIGHT, 4);
  bar.fill = CTUI_LOOK_CTL_TINT | 0x405060u;
  p = paint(bar, &l, 40, 20);
  int right = p[(10 * 40 + 39) * 4] == 0x40, left = p[(10 * 40 + 1) * 4];
  CTUI_LOOK_CONTROL both = bar;
  both.place = 0;
  p = paint(both, &l, 40, 20);
  CTUI_TEST_ASSERT(right && left == 0x0a && p[(10 * 40 + 39) * 4] == 0x0a &&
                       px(p, 40, &l, 4, 10) == 's',
                   "one side bordered: the fill to the other's outer side, "
                   "the paper left of the bar (through its column without)");
}

static int pngs(const char *dir) {
  DIR *d = opendir(dir);
  int n = 0;
  for (struct dirent *e; d && (e = readdir(d));) {
    n += strstr(e->d_name, ".png") != NULL;
  }
  if (d) {
    closedir(d);
  }
  return n;
}

/* the widget column's additions (step 3): radio buttons, a tinted
 * pictogram, a level snapped to its chunks */
static void test_column(void) {
  CTUI_LOOK l = test_look();
  CTUI_LOOK_CONTROL radio = ctl(CTUI_LOOK_CTL_RADIO, 0, 0);
  const unsigned char *p = paint(radio, &l, 18, 20);
  int ring_s = count(p, 18, 20, &l, 's'), ring_h = count(p, 18, 20, &l, 'h');
  CTUI_TEST_ASSERT(count(p, 18, 20, &l, 't') == 0 && ring_s > 8 &&
                       ring_s == ring_h && count(p, 18, 20, &l, 'g') > 30 &&
                       outside(p, 18, 20, 3, 4, 14, 15) == 0,
                   "a radio button: a 12 px ring, shadow top-left and "
                   "highlight bottom-right alike, groove inside, centred");
  radio.flags = CTUI_LOOK_CTL_CHECKED;
  p = paint(radio, &l, 18, 20);
  int dot = count(p, 18, 20, &l, 't');
  CTUI_TEST_ASSERT(dot >= 12 && dot <= 16 && px(p, 18, &l, 8, 9) == 't',
                   "checked: a dot in the middle (%d px)", dot);
  radio.flags |= CTUI_LOOK_CTL_OFF;
  p = paint(radio, &l, 18, 20);
  CTUI_TEST_ASSERT(count(p, 18, 20, &l, 'x') == dot,
                   "disabled: the dot greyed");
  l.bevel = 0;
  p = paint(radio, &l, 18, 20);
  CTUI_TEST_ASSERT(count(p, 18, 20, &l, 's') == 0 &&
                       count(p, 18, 20, &l, 'd') == ring_s + ring_h,
                   "no bevel: one dark ring");
  l = test_look();

  CTUI_LOOK_CONTROL lamp = {.kind = CTUI_LOOK_CTL_PICTO,
                            .picto = CTUI_LOOK_PICTO_LAMP};
  int glass = count(paint(lamp, &l, 18, 20), 18, 20, &l, 'a');
  CTUI_LOOK_CONTROL tinted = lamp;
  tinted.tint = CTUI_LOOK_CTL_TINT | 0x010203u;
  p = paint(tinted, &l, 18, 20);
  CTUI_TEST_ASSERT(glass > 40 && count(p, 18, 20, &l, 'a') == 0 &&
                       count(p, 18, 20, &l, '?') == glass &&
                       p[(10 * 18 + 8) * 4] == 1 &&
                       p[(10 * 18 + 8) * 4 + 2] == 3,
                   "a tint: the lamp's glass in the light's colour");
  CTUI_LOOK_CONTROL other = tinted, off = tinted, plain_off = lamp;
  other.tint = CTUI_LOOK_CTL_TINT | 0x010204u;
  off.flags = plain_off.flags = CTUI_LOOK_CTL_OFF;
  CTUI_LOOK_CONTROL untagged = lamp;
  untagged.tint = 0x010203u; /* no CTUI_LOOK_CTL_TINT: no tint */
  CTUI_TEST_ASSERT(
      ctui_look_control_key(&tinted, &l, 18, 20) !=
              ctui_look_control_key(&other, &l, 18, 20) &&
          ctui_look_control_key(&tinted, &l, 18, 20) !=
              ctui_look_control_key(&lamp, &l, 18, 20) &&
          ctui_look_control_key(&off, &l, 18, 20) ==
              ctui_look_control_key(&plain_off, &l, 18, 20) &&
          ctui_look_control_key(&untagged, &l, 18, 20) ==
              ctui_look_control_key(&lamp, &l, 18, 20),
      "keys: one per colour; off (all greyed) or untagged: the colour "
      "doesn't count");
  p = paint(off, &l, 18, 20);
  CTUI_TEST_ASSERT(
      count(p, 18, 20, &l, '?') == 0 && count(p, 18, 20, &l, 'a') == 0 &&
          count(p, 18, 20, &l, 'x') > 20 && px(p, 18, &l, 8, 10) == '.',
      "a light off: the outline greyed, the glass clear");

  /* chunks 4 wide, 2 apart over 24 px (a px clear of the edge): at 0 6
   * 12 18, each shown from its middle on */
  l.chunk = 4;
  l.gap = 2;
  CTUI_LOOK_CONTROL a = ctl(CTUI_LOOK_CTL_LEVEL, 40, 100); /* 10 px */
  CTUI_LOOK_CONTROL b = ctl(CTUI_LOOK_CTL_LEVEL, 50, 100); /* 12 px */
  CTUI_LOOK_CONTROL c = ctl(CTUI_LOOK_CTL_LEVEL, 60, 100); /* 14 px */
  CTUI_LOOK_CONTROL full = ctl(CTUI_LOOK_CTL_LEVEL, 100, 100);
  ctui_look_control_snap(&b, &l, 30, 18);
  ctui_look_control_snap(&full, &l, 30, 18);
  CTUI_TEST_ASSERT(
      b.value == 10 && full.value == 22 &&
          ctui_look_control_key(&a, &l, 30, 18) ==
              ctui_look_control_key(&b, &l, 30, 18) &&
          ctui_look_control_key(&b, &l, 30, 18) !=
              ctui_look_control_key(&c, &l, 30, 18),
      "a chunked level snaps to its chunks: one image per chunk (%d %d)",
      b.value, full.value);
}

/* the weather pictogram: parts layered by its bits */
static int top_row(const unsigned char *rgba, int w, int h) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (rgba[(y * w + x) * 4 + 3]) {
        return y;
      }
    }
  }
  return -1;
}

static void test_sky(void) {
  CTUI_LOOK l = test_look();
  CTUI_LOOK_CONTROL c = {.kind = CTUI_LOOK_CTL_PICTO,
                         .picto = CTUI_LOOK_PICTO_SKY,
                         .value = CTUI_LOOK_SKY_SUN,
                         .max = 1};
  const unsigned char *p = paint(c, &l, 18, 20);
  int sun = count(p, 18, 20, &l, 'w');
  CTUI_TEST_ASSERT(sun > 30 && count(p, 18, 20, &l, 'h') == 0,
                   "the sun in the warm role (%d px)", sun);
  c.value = CTUI_LOOK_SKY_CLOUD;
  p = paint(c, &l, 18, 20);
  int cloud_top = top_row(p, 18, 20);
  c.value = CTUI_LOOK_SKY_CLOUD | CTUI_LOOK_SKY_RAIN;
  p = paint(c, &l, 18, 20);
  int rain = count(p, 18, 20, &l, 'a');
  CTUI_TEST_ASSERT(top_row(p, 18, 20) == cloud_top - 3 && rain >= 9,
                   "rain: the cloud goes up 3 px, drops under it (%d px)",
                   rain);
  c.value |= CTUI_LOOK_SKY_HEAVY;
  p = paint(c, &l, 18, 20);
  CTUI_TEST_ASSERT(count(p, 18, 20, &l, 'a') > rain, "heavy: more of it");
  c.value = CTUI_LOOK_SKY_SUN | CTUI_LOOK_SKY_PEEK | CTUI_LOOK_SKY_CLOUD;
  p = paint(c, &l, 18, 20);
  int peek = count(p, 18, 20, &l, 'w');
  CTUI_TEST_ASSERT(peek > 0 && peek < sun / 2,
                   "partly: a small sun behind the cloud (%d px)", peek);
  CTUI_LOOK_CONTROL a = c, b = c;
  b.value |= 1 << 14; /* no such part */
  CTUI_TEST_ASSERT(ctui_look_control_key(&a, &l, 18, 20) ==
                           ctui_look_control_key(&b, &l, 18, 20) &&
                       ctui_look_control_key(&a, &l, 18, 20) !=
                           ctui_look_control_key(&c, &l, 36, 40),
                   "keys: unknown bits dropped");
  c.value = CTUI_LOOK_SKY_HEAVY; /* heavy alone: nothing to add to */
  CTUI_TEST_ASSERT(opaque(paint(c, &l, 18, 20), 18, 20) == 0,
                   "heavy without rain: nothing");
}

/* the app icons' period filter (look/filter.c) */
static unsigned char g_src[64 * 64 * 4], g_out[32 * 32 * 4];

static void fill(int w, int h, unsigned r, unsigned g, unsigned b, unsigned a) {
  for (int i = 0; i < w * h; i++) {
    g_src[i * 4] = (unsigned char)r;
    g_src[i * 4 + 1] = (unsigned char)g;
    g_src[i * 4 + 2] = (unsigned char)b;
    g_src[i * 4 + 3] = (unsigned char)a;
  }
}

static int opaque_n(const unsigned char *px, int n) {
  int k = 0;
  for (int i = 0; i < n; i++) {
    k += px[i * 4 + 3] != 0;
  }
  return k;
}

static int colours(const unsigned char *px, int n) {
  uint32_t seen[64];
  int k = 0;
  for (int i = 0; i < n && k < 64; i++) {
    if (!px[i * 4 + 3]) {
      continue;
    }
    uint32_t c = (uint32_t)px[i * 4] << 16 | (uint32_t)px[i * 4 + 1] << 8 |
                 px[i * 4 + 2];
    int dup = 0;
    for (int j = 0; j < k; j++) {
      dup |= seen[j] == c;
    }
    if (!dup) {
      seen[k++] = c;
    }
  }
  return k;
}

static void test_filter(void) {
  CTUI_LOOK l;
  ctui_look_builtin("win95", &l);
  CTUI_TEST_ASSERT(l.filter.on && l.filter.size == 16 &&
                       l.filter.palette == CTUI_LOOK_FILTER_PALETTE_WIN95 &&
                       strcmp(l.name, "win95") == 0,
                   "win95 filters: 16 px, its colours");
  l.filter.dither = CTUI_LOOK_FILTER_DITHER_NONE;
  l.filter.boost = 0;
  fill(64, 64, 250, 5, 5, 255);
  ctui_look_filter_icon(g_src, 64, 64, &l, g_out);
  CTUI_TEST_ASSERT(opaque_n(g_out, 256) == 256 && g_out[0] == 255 &&
                       g_out[1] == 0 && colours(g_out, 256) == 1,
                   "a red square: 16x16 of 95's red");
  fill(64, 32, 0, 0, 128, 255);
  ctui_look_filter_icon(g_src, 64, 32, &l, g_out);
  CTUI_TEST_ASSERT(
      opaque_n(g_out, 256) == 128 && g_out[(3 * 16) * 4 + 3] == 0 &&
          g_out[(4 * 16) * 4 + 3] == 255 && g_out[(12 * 16) * 4 + 3] == 0,
      "wider than tall: 16x8, centred (rows 4-11)");
  fill(64, 64, 0, 0, 128, 100);
  ctui_look_filter_icon(g_src, 64, 64, &l, g_out);
  CTUI_TEST_ASSERT(opaque_n(g_out, 256) == 0, "under the alpha cut: clear");
  l.filter.alpha = 0;
  ctui_look_filter_icon(g_src, 64, 64, &l, g_out);
  CTUI_TEST_ASSERT(g_out[3] == 100 && g_out[2] == 128,
                   "alpha 0: soft edges kept, colours still the palette's");
  l.filter.alpha = 128;
  /* a checker at 1 px: averaged to grey */
  for (int i = 0; i < 64 * 64; i++) {
    unsigned v = ((i % 64) + (i / 64)) % 2 ? 255 : 0;
    memset(g_src + i * 4, (int)v, 3);
    g_src[i * 4 + 3] = 255;
  }
  ctui_look_filter_icon(g_src, 64, 64, &l, g_out);
  CTUI_TEST_ASSERT(g_out[0] == 0x80 || g_out[0] == 0xa0 || g_out[0] == 0xc0,
                   "4 px into 1 by their area: grey (%02x)", g_out[0]);
  fill(64, 64, 0x90, 0x90, 0x92, 255); /* between 95's 0x80 and 0xa0 */
  l.filter.dither = CTUI_LOOK_FILTER_DITHER_ORDERED;
  ctui_look_filter_icon(g_src, 64, 64, &l, g_out);
  unsigned char again[16 * 16 * 4];
  ctui_look_filter_icon(g_src, 64, 64, &l, again);
  CTUI_TEST_ASSERT(colours(g_out, 256) == 2 &&
                       memcmp(g_out, again, sizeof again) == 0,
                   "between two greys, ordered: a pattern of both, the same "
                   "every time");
  l.filter.dither = CTUI_LOOK_FILTER_DITHER_FS;
  for (int i = 0; i < 64 * 64; i++) {
    g_src[i * 4] = (unsigned char)(i % 64 * 4);
    g_src[i * 4 + 1] = (unsigned char)(i / 64 * 4);
    g_src[i * 4 + 2] = 90;
    g_src[i * 4 + 3] = 255;
  }
  ctui_look_filter_icon(g_src, 64, 64, &l, g_out);
  int in_palette = 1;
  for (int i = 0; i < 256; i++) {
    int ok = 0;
    static const uint32_t P[20] = {
        0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080,
        0xc0c0c0, 0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff,
        0x00ffff, 0xffffff, 0xc0dcc0, 0xa6caf0, 0xfffbf0, 0xa0a0a4};
    uint32_t c = (uint32_t)g_out[i * 4] << 16 |
                 (uint32_t)g_out[i * 4 + 1] << 8 | g_out[i * 4 + 2];
    for (int j = 0; j < 20; j++) {
      ok |= P[j] == c;
    }
    in_palette &= ok;
  }
  CTUI_TEST_ASSERT(in_palette && colours(g_out, 256) >= 4,
                   "Floyd-Steinberg on a gradient: only the palette's colours");

  /* outline and shadow: sources as big as the room they get (14 px in
   * 16 with the outline, 15 with the shadow), so nothing is scaled */
  l.filter.dither = CTUI_LOOK_FILTER_DITHER_NONE;
  l.filter.outline = 1;
  memset(g_src, 0, sizeof g_src);
  for (int y = 4; y < 10; y++) {
    for (int x = 4; x < 10; x++) {
      memset(g_src + (y * 14 + x) * 4, 255, 4);
    }
  }
  ctui_look_filter_icon(g_src, 14, 14, &l, g_out);
  int dark = 0;
  for (int i = 0; i < 256; i++) {
    dark += g_out[i * 4 + 3] && !g_out[i * 4] && !g_out[i * 4 + 1] &&
            !g_out[i * 4 + 2];
  }
  int body = opaque_n(g_out, 256) - dark;
  CTUI_TEST_ASSERT(dark == 4 * 6 && body == 36 && g_out[(4 * 16 + 5) * 4 + 3] &&
                       !g_out[(4 * 16 + 5) * 4],
                   "outline: a ring round the mask in dark (%d px round %d)",
                   dark, body);
  l.filter.outline = 0;
  l.filter.shadow = 1;
  memset(g_src, 0, sizeof g_src);
  for (int y = 5; y < 10; y++) {
    for (int x = 5; x < 10; x++) {
      memset(g_src + (y * 15 + x) * 4, 255, 4);
    }
  }
  ctui_look_filter_icon(g_src, 15, 15, &l, g_out);
  int grey = 0;
  for (int i = 0; i < 256; i++) {
    grey +=
        g_out[i * 4 + 3] && g_out[i * 4] == 0x80 && g_out[i * 4 + 2] == 0x80;
  }
  CTUI_TEST_ASSERT(grey == 2 * 5 - 1 && opaque_n(g_out, 256) == 25 + grey &&
                       g_out[(10 * 16 + 10) * 4 + 3],
                   "shadow: down and right, in shadow (%d px)", grey);

  CTUI_LOOK a, b;
  ctui_look_builtin("win95", &a);
  b = a;
  CTUI_TEST_ASSERT(ctui_look_filter_hash(&a) == ctui_look_filter_hash(&b),
                   "hash: the same filter, the same");
  b.color[CTUI_LOOK_ACCENT][0] ^= 1;
  CTUI_TEST_ASSERT(ctui_look_filter_hash(&a) == ctui_look_filter_hash(&b),
                   "... a colour it doesn't use: the same");
  b.filter.palette = CTUI_LOOK_FILTER_PALETTE_ROLES;
  uint64_t roles = ctui_look_filter_hash(&b);
  b.color[CTUI_LOOK_ACCENT][0] ^= 1;
  CTUI_TEST_ASSERT(roles != ctui_look_filter_hash(&a) &&
                       roles != ctui_look_filter_hash(&b),
                   "... another palette, or one of the roles it uses: apart");

  unsigned char tiny[2 * 2 * 4] = {1, 1, 1, 255, 2, 2, 2, 255,
                                   3, 3, 3, 255, 4, 4, 4, 255};
  static unsigned char big[36 * 40 * 4];
  CTUI_TEST_ASSERT(ctui_look_filter_upscale(tiny, 2, 2, big, 5, 3) == 0 &&
                       big[(0 * 5 + 1) * 4] == 1 && big[(0 * 5 + 2) * 4] == 2 &&
                       big[(1 * 5 + 1) * 4] == 3 && big[3] == 0 &&
                       big[(2 * 5 + 1) * 4 + 3] == 0,
                   "upscale at 1x: centred (x 1, y 0 of the odd row)");
  CTUI_TEST_ASSERT(ctui_look_filter_upscale(tiny, 2, 2, big, 5, 5) == 0 &&
                       big[(0 * 5 + 0) * 4 + 3] == 255 &&
                       big[(1 * 5 + 1) * 4] == 1 && big[(2 * 5 + 2) * 4] == 4 &&
                       big[(4 * 5 + 4) * 4 + 3] == 0,
                   "... 2x in 5x5: each pixel 2x2, a clear edge");
  CTUI_TEST_ASSERT(ctui_look_filter_upscale(tiny, 2, 2, big, 1, 5) == -1,
                   "too small a box: -1");
}

static void test_filter_config(void) {
  CTUI_LOOK l;
  char err[256];
  CTUI_TEST_ASSERT(parse("{\"filter\": false}", &l, err) == 1 && !l.filter.on,
                   "filter false: off");
  CTUI_TEST_ASSERT(
      parse("{\"base\": \"flat\"}", &l, err) == 1 && !l.filter.on &&
          parse("{\"base\": \"flat\", \"filter\": true}", &l, err) == 1 &&
          l.filter.on && l.filter.size == 16,
      "flat has none; true gives 95's");
  CTUI_TEST_ASSERT(
      parse("{\"filter\": {\"size\": 32, \"palette\": \"grey\", \"dither\": "
            "\"fs\", \"alpha\": 0, \"boost\": 0, \"outline\": true, "
            "\"shadow\": true}}",
            &l, err) == 1 &&
          l.filter.on && l.filter.size == 32 &&
          l.filter.palette == CTUI_LOOK_FILTER_PALETTE_GREY &&
          l.filter.dither == CTUI_LOOK_FILTER_DITHER_FS &&
          l.filter.alpha == 0 && l.filter.outline && l.filter.shadow,
      "every setting");
  static const char *const bad[] = {
      "{\"filter\": {\"size\": 24}}",   "{\"filter\": {\"palette\": \"ega\"}}",
      "{\"filter\": {\"palette\": 3}}", "{\"filter\": {\"alpha\": 300}}",
      "{\"filter\": {\"sharpen\": 1}}", "{\"filter\": \"yes\"}"};
  int all = 1;
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    all &= parse(bad[i], &l, err) == -1 && strstr(err, "filter");
  }
  CTUI_TEST_ASSERT(all, "a wrong size, palette, number or key: an error");
}

/* the zones' prepare hook (drawn.c): a small icon at the cells' px */

static void test_cache(void) {
  char root[] = "/tmp/look_test.XXXXXX";
  if (!mkdtemp(root)) {
    CTUI_TEST_ASSERT(0, "mkdtemp");
    return;
  }
  char dir[4096];
  snprintf(dir, sizeof dir, "%s/cache", root);
  ctui_drawn_set_dir(dir);
  CTUI_LOOK l = test_look();
  CTUI_LOOK_CONTROL c = ctl(CTUI_LOOK_CTL_SLIDER, 50, 100);

  ctui_drawn_cell(9, 20);
  CTUI_TEST_ASSERT(ctui_drawn_id(&c, &l, 5, 1) == 0 && pngs(dir) == 0,
                   "icons off: no image (text instead)");
  ctui_icon_enable(1);
  unsigned int id = ctui_drawn_id(&c, &l, 5, 1);
  char path[4096];
  ctui_drawn_path(&c, &l, 45, 20, path, sizeof path);
  struct stat st;
  CTUI_TEST_ASSERT(id != 0 && stat(path, &st) == 0 && pngs(dir) == 1,
                   "the first time: painted into %s", path);
  FILE *f = fopen(path, "rb");
  unsigned char head[24] = {0};
  CTUI_TEST_ASSERT(f && fread(head, 1, 24, f) == 24 &&
                       memcmp(head, "\x89PNG", 4) == 0 && head[19] == 45 &&
                       head[23] == 20,
                   "a PNG of the cells' exact size, 45x20");
  if (f) {
    fclose(f);
  }
  c.value = 51;
  CTUI_TEST_ASSERT(ctui_drawn_id(&c, &l, 5, 1) == id && pngs(dir) == 1,
                   "a state that looks the same: the same image");
  c.value = 90;
  unsigned int id2 = ctui_drawn_id(&c, &l, 5, 1);
  CTUI_TEST_ASSERT(id2 && id2 != id && pngs(dir) == 2, "another: a new one");
  c.value = 50;
  CTUI_TEST_ASSERT(ctui_drawn_id(&c, &l, 5, 1) == id && pngs(dir) == 2,
                   "back again: its id, nothing written");
  l.color[CTUI_LOOK_ACCENT][0] ^= 1;
  CTUI_TEST_ASSERT(ctui_drawn_id(&c, &l, 5, 1) != id && pngs(dir) == 3,
                   "a changed look draws anew");
  ctui_drawn_cell(14, 30);
  CTUI_TEST_ASSERT(ctui_drawn_id(&c, &l, 5, 1) != 0 && pngs(dir) == 4,
                   "another cell size: its own pixels");

  ctui_icon_enable(0);
  ctui_icon_reset();
  ctui_drawn_cell(0, 0);
  DIR *d = opendir(dir);
  for (struct dirent *e; d && (e = readdir(d));) {
    if (e->d_name[0] != '.') {
      char p[8192];
      snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
      unlink(p);
    }
  }
  if (d) {
    closedir(d);
  }
  rmdir(dir);
  rmdir(root);
  ctui_drawn_set_dir(NULL);
}

/* a framed box's rows: the place flags, the heading's gap on top only */
static void test_box_rows(void) {
  CTUI_LOOK_CONTROL t = ctui_look_box_row(CTUI_LOOK_BOX_GROOVE, 20, 0, 4, 2, 5);
  CTUI_LOOK_CONTROL m = ctui_look_box_row(CTUI_LOOK_BOX_GROOVE, 20, 1, 4, 2, 5);
  CTUI_LOOK_CONTROL b = ctui_look_box_row(CTUI_LOOK_BOX_GROOVE, 20, 3, 4, 2, 5);
  CTUI_TEST_ASSERT(t.kind == CTUI_LOOK_CTL_BOX && t.span == 20 && t.sides == 0xf &&
                       t.place == (CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_FLUSH_X) && t.gap == 2 &&
                       t.gap_cols == 5,
                   "a group box's top row: open over its heading, flush left / right");
  CTUI_TEST_ASSERT(m.place == CTUI_LOOK_BOX_FLUSH_X && m.gap_cols == 0 &&
                       b.place == (CTUI_LOOK_BOX_BOTTOM | CTUI_LOOK_BOX_FLUSH_X |
                                   CTUI_LOOK_BOX_FLUSH),
                   "a middle row has sides only, the bottom row is flush at the bottom");
  CTUI_LOOK_CONTROL one = ctui_look_box_row(CTUI_LOOK_BOX_GROOVE, 8, 0, 1, 0, 0);
  CTUI_LOOK_CONTROL o = ctui_look_box_row(CTUI_LOOK_BOX_OUTSET, 8, 2, 3, 0, 0);
  CTUI_TEST_ASSERT(one.place == (CTUI_LOOK_BOX_TOP | CTUI_LOOK_BOX_BOTTOM | CTUI_LOOK_BOX_FLUSH_X) &&
                       o.place == (CTUI_LOOK_BOX_BOTTOM | CTUI_LOOK_BOX_THICK),
                   "a one-row box is top and bottom; a dialog's frame is thick, not flush");
}

int main(void) {
  ctui_log_init(0);
  test_looks();
  test_config();
  test_primitives();
  test_slider();
  test_level_signal();
  test_toggles();
  test_buttons();
  test_scroll();
  test_field();
  test_frame();
  test_snap_invariant();
  test_pictos();
  test_browser();
  test_frames();
  test_column();
  test_sky();
  test_cache();
  test_box_rows();
  test_filter();
  test_filter_config();
  return ctui_test_summary();
}
