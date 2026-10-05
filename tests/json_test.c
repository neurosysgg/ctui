/* json.c's writer: ctui_json_write() has to read back into an equal
 * tree (ctui-wm's re-exec'd supervisor compares signatures written by
 * it across binaries). And its numbers: JSON's grammar, not
 * strtod's, read into integers without an undefined cast. */
#include "json/json.h"

#include "ctui.h"
#include "ctui_test.h"

#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_round_trip(void) {
  const char *text =
      "{\"widget\": \"column\", // a comment\n"
      " \"children\": [{\"widget\": \"clock\", \"size\": 3, \"x\": -0.25,"
      " \"on\": true, \"off\": false, \"none\": null},"
      " \"tab\\there \\\"quoted\\\" \\u00e9 \\ud83d\\ude00\", [], {}],"
      " \"big\": 12345678901234,}";
  char err[256] = "";
  CTUI_JSON *a = ctui_json_parse(text, err, sizeof err);
  CTUI_TEST_ASSERT(a != NULL, "the test JSON parses");
  CTUI_BUF b = {0};
  ctui_json_write(&b, a);
  CTUI_JSON *back = b.s ? ctui_json_parse(b.s, err, sizeof err) : NULL;
  CTUI_TEST_ASSERT(back != NULL, "what it writes parses");
  CTUI_TEST_ASSERT(ctui_json_equal(a, back), "into an equal tree");
  int ascii = 1;
  for (size_t i = 0; b.s && i < b.len; i++) {
    ascii &= (unsigned char)b.s[i] >= 0x20 && (unsigned char)b.s[i] < 0x7f;
  }
  CTUI_TEST_ASSERT(ascii, "as one line of printable ASCII");
  ctui_json_free(back);
  ctui_json_free(a);
  free(b.s);

  CTUI_BUF n = {0};
  ctui_json_write(&n, NULL);
  CTUI_TEST_ASSERT(n.s && strcmp(n.s, "null") == 0, "NULL writes null");
  free(n.s);
}

static void test_numbers(void) {
  static const char *good[] = {
      "0",     "-0",  "7",    "-12",    "0.5",
      "-0.25", "1e3", "1E+3", "2.5e-3", "12345678901234"};
  static const char *bad[] = {"-",     "01",    "-01",       "1.",   ".5",
                              "+1",    "1e",    "1e+",       "0x1F", "-0x1F",
                              "inf",   "-inf",  "-Infinity", "nan",  "-nan",
                              "1e999", "-1e999"};
  char err[256] = "";
  for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
    CTUI_JSON *v = ctui_json_parse(good[i], err, sizeof err);
    CTUI_TEST_ASSERT(v && v->type == CTUI_JSON_NUMBER &&
                         v->number == strtod(good[i], NULL),
                     "%s is a number", good[i]);
    ctui_json_free(v);
  }
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    CTUI_JSON *v = ctui_json_parse(bad[i], err, sizeof err);
    CTUI_TEST_ASSERT(v == NULL, "%s is refused (%s)", bad[i], err);
    ctui_json_free(v);
  }
  CTUI_JSON *v = ctui_json_parse("[01]", err, sizeof err);
  CTUI_TEST_ASSERT(!v && strstr(err, "invalid number") && strstr(err, "col 2"),
                   "at the number: %s", err);
  ctui_json_free(v);

  v = ctui_json_parse("[1e20, -1e20, 2.9, -2.9, -1, 4294967296, 1e19]", err,
                      sizeof err);
  CTUI_TEST_ASSERT(v && v->count == 7, "the integers parse");
  if (v && v->count == 7) {
    CTUI_TEST_ASSERT(ctui_json_int(&v->items[0], 0) == INT_MAX &&
                         ctui_json_int(&v->items[1], 0) == INT_MIN,
                     "an int is clamped, not cast");
    CTUI_TEST_ASSERT(ctui_json_int(&v->items[2], 0) == 2 &&
                         ctui_json_int(&v->items[3], 0) == -2,
                     "and truncated toward zero, like the cast");
    CTUI_TEST_ASSERT(ctui_json_uint(&v->items[4], 9) == 0 &&
                         ctui_json_uint(&v->items[5], 9) == UINT_MAX,
                     "unsigned: negative is 0, too big is the top");
    CTUI_TEST_ASSERT(ctui_json_llong(&v->items[5], 0) == 4294967296LL &&
                         ctui_json_llong(&v->items[6], 0) == LLONG_MAX &&
                         ctui_json_llong(&v->items[1], 0) == LLONG_MIN,
                     "long long: exact inside, clamped past 2^63");
  }
  CTUI_TEST_ASSERT(ctui_json_int(NULL, 5) == 5 &&
                       ctui_json_uint(ctui_json_get(v, "x"), 6) == 6,
                   "no number: the fallback");
  ctui_json_free(v);

  /* a hand-built non-finite number can't be written as one */
  CTUI_JSON inf = {.type = CTUI_JSON_NUMBER, .number = INFINITY};
  CTUI_BUF b = {0};
  ctui_json_write(&b, &inf);
  CTUI_TEST_ASSERT(b.s && strcmp(b.s, "null") == 0, "inf is written null");
  free(b.s);
}

/* notes/REVIEW.md's parser notes */
static void test_notes(void) {
  char err[256] = "";
  CTUI_JSON *v = ctui_json_parse("\"a\\u0000b\"", err, sizeof err);
  CTUI_TEST_ASSERT(v && strcmp(v->string, "a\xef\xbf\xbd"
                                          "b") == 0,
                   "\\u0000 becomes U+FFFD, not a string cut short");
  ctui_json_free(v);

  CTUI_JSON hand = {.type = CTUI_JSON_STRING}, empty = hand;
  empty.string = "";
  CTUI_BUF b = {0};
  ctui_json_write(&b, &hand);
  CTUI_TEST_ASSERT(b.s && strcmp(b.s, "\"\"") == 0 &&
                       ctui_json_equal(&hand, &empty),
                   "a hand-built NULL string writes and compares as \"\"");
  free(b.s);

  b = (CTUI_BUF){0};
  ctui_buf_add(&b, "abc", 3);
  for (int i = 0; i < 4; i++) {
    ctui_buf_add(&b, b.s, b.len); /* each doubles it, past the cap */
  }
  CTUI_TEST_ASSERT(b.len == 48 && strncmp(b.s + 45, "abc", 3) == 0,
                   "a buffer can be added to itself");
  free(b.s);

  size_t big = 1u << 20;
  char *s = malloc(big + 1);
  memset(s, 'y', big);
  s[big] = '\0';
  b = (CTUI_BUF){0};
  ctui_buf_printf(&b, "<%s>", s);
  CTUI_TEST_ASSERT(b.len == big + 2 && b.s[big + 1] == '>',
                   "a 1 MiB %%s formats (on the heap)");
  free(b.s);
  free(s);

  if (setlocale(LC_NUMERIC, "de_DE.UTF-8") ||
      setlocale(LC_NUMERIC, "fr_FR.UTF-8")) {
    v = ctui_json_parse("0.5", err, sizeof err);
    b = (CTUI_BUF){0};
    ctui_json_write(&b, v);
    CTUI_TEST_ASSERT(v && v->number == 0.5 && strcmp(b.s, "0.5") == 0,
                     "a comma-decimal locale reads and writes 0.5 as JSON");
    free(b.s);
    ctui_json_free(v);
    setlocale(LC_NUMERIC, "C");
  } else {
    printf("  --  no comma-decimal locale installed: skipping that check\n");
  }
}

int main(void) {
  ctui_log_init(E_ALL);
  test_round_trip();
  test_numbers();
  test_notes();
  ctui_log_shutdown();
  return ctui_test_summary();
}
