/* json/record.h: objects into fixed records -- the kinds, strings' escapes
 * into the arena, other keys (nested ones too) stepped over, what isn't
 * an object refused, the arena's pointers kept over many records, and
 * against json.c's parser on the same lines. */
#include "json/json.h"
#include "json/record.h"

#include "ctui.h"
#include "ctui_test.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *path, *artist;
  long long size, track;
  double gain;
  int cover;
  int has_size, has_track, has_gain, has_cover;
} SONG;

static const CTUI_JSON_REC_FIELD FIELDS[] = {
    {"path", CTUI_JSON_REC_STR, offsetof(SONG, path), -1},
    {"artist", CTUI_JSON_REC_STR, offsetof(SONG, artist), -1},
    {"size", CTUI_JSON_REC_LLONG, offsetof(SONG, size),
     offsetof(SONG, has_size)},
    {"track", CTUI_JSON_REC_LLONG, offsetof(SONG, track),
     offsetof(SONG, has_track)},
    {"gain", CTUI_JSON_REC_DOUBLE, offsetof(SONG, gain),
     offsetof(SONG, has_gain)},
    {"cover", CTUI_JSON_REC_BOOL, offsetof(SONG, cover),
     offsetof(SONG, has_cover)},
};
#define NF ((int)(sizeof FIELDS / sizeof *FIELDS))

static int read_line(const CTUI_JSON_REC *r, const char *s, SONG *out,
                     CTUI_JSON_ARENA *a) {
  memset(out, 0, sizeof *out);
  return ctui_json_rec_read(r, s, strlen(s), out, a);
}

static void test_kinds(CTUI_JSON_REC *r, CTUI_JSON_ARENA *a) {
  SONG s;
  int rc = read_line(r,
                     " {\"path\":\"/m/a.flac\", \"size\": 1234567890123,"
                     "\"skip\":{\"x\":[1,{\"y\":\"}\"},[]],\"z\":\"\\\"\"},"
                     "\"track\":-3,\"gain\":-6.5e-1,\"cover\":true,"
                     "\"artist\":\"Bo\\u00e9 \\ud83d\\ude00 \\\"q\\\" \\\\ "
                     "\\/\\n\"}\n",
                     &s, a);
  CTUI_TEST_ASSERT(rc == 0 && !strcmp(s.path, "/m/a.flac") &&
                       s.size == 1234567890123LL && s.has_size &&
                       s.track == -3 && s.gain == -0.65 && s.cover == 1 &&
                       s.has_cover,
                   "every kind; a nested value stepped over (rc %d)", rc);
  CTUI_TEST_ASSERT(s.artist && !strcmp(s.artist, "Bo\xc3\xa9 \xf0\x9f\x98\x80 "
                                                 "\"q\" \\ /\n"),
                   "a string's escapes, a surrogate pair (%s)", s.artist);

  rc = read_line(r,
                 "{\"size\":\"12\",\"track\":null,\"cover\":0,\"gain\":3,"
                 "\"size\":7.9}",
                 &s, a);
  CTUI_TEST_ASSERT(rc == 0 && s.size == 7 && s.has_size && !s.has_track &&
                       !s.has_cover && s.gain == 3 && s.has_gain,
                   "another kind or null leaves a field; the last of a "
                   "duplicate key wins; a fraction truncated");
  rc = read_line(r,
                 "{\"size\":99999999999999999999999,\"track\":-1e300,"
                 "\"artist\":\"\\ud800x\"}",
                 &s, a);
  CTUI_TEST_ASSERT(rc == 0 && s.size == LLONG_MAX && s.track == LLONG_MIN &&
                       !strcmp(s.artist, "\xef\xbf\xbdx"),
                   "numbers clamped, a lone surrogate replaced (%lld %lld)",
                   s.size, s.track);
  CTUI_TEST_ASSERT(read_line(r, "{}", &s, a) == 0 && !s.path,
                   "an empty object");
}

static void test_refused(CTUI_JSON_REC *r, CTUI_JSON_ARENA *a) {
  static const char *const BAD[] = {
      "",
      "[1]",
      "{\"a\":1,}",
      "{\"a\" 1}",
      "{\"a\":01}",
      "{\"a\":1.}",
      "{\"a\":tru}",
      "{\"path\":\"x\\q\"}",
      "{\"path\":\"\\u12\"}",
      "{\"a\":\"unterminated}",
      "{\"a\":[1,2}",
      "{\"a\":1} x",
      "{\"a\":1}{}",
      "{\"skip\":" /* deeper than the cap */
      "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[",
  };
  int ok = 1;
  SONG s;
  for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
    if (read_line(r, BAD[i], &s, a) != -1) {
      ok = 0;
      printf("  took: %s\n", BAD[i]);
    }
  }
  CTUI_TEST_ASSERT(ok, "what isn't one JSON object is refused");
  /* deeper than 512, closed: refused, no stack blown */
  size_t n = 2000;
  char *deep = malloc(2 * n + 16);
  strcpy(deep, "{\"skip\":");
  memset(deep + 8, '[', n);
  memset(deep + 8 + n, ']', n);
  strcpy(deep + 8 + 2 * n, "}");
  CTUI_TEST_ASSERT(read_line(r, deep, &s, a) == -1, "nesting capped");
  free(deep);
  /* every cut of a good line: refused, never past it (ASan's job) */
  const char *good = "{\"path\":\"\\u00e9\",\"skip\":[{\"a\":\"\\\\\"}],"
                     "\"gain\":1.5e3,\"cover\":false}";
  size_t len = strlen(good);
  ok = 1;
  for (size_t k = 0; k < len; k++) {
    char *cut = malloc(k ? k : 1);
    memcpy(cut, good, k);
    memset(&s, 0, sizeof s);
    ok &= ctui_json_rec_read(r, cut, k, &s, a) == -1;
    free(cut);
  }
  CTUI_TEST_ASSERT(ok, "a line cut anywhere is refused");
}

/* the arena: strings from many records all still there; reset keeps one
 * block, a big string gets its own */
static void test_arena(CTUI_JSON_REC *r) {
  CTUI_JSON_ARENA a = {0};
  enum { N = 20000 };
  const char **paths = malloc(sizeof *paths * N);
  char line[128];
  SONG s;
  int ok = 1;
  for (int i = 0; i < N; i++) {
    snprintf(line, sizeof line, "{\"path\":\"/music/%05d.flac\"}", i);
    ok &= read_line(r, line, &s, &a) == 0;
    paths[i] = s.path;
  }
  for (int i = 0; i < N && ok; i++) {
    snprintf(line, sizeof line, "/music/%05d.flac", i);
    ok = !strcmp(paths[i], line);
  }
  CTUI_TEST_ASSERT(ok, "20000 records' strings kept in place");
  size_t big = 200000;
  char *text = malloc(big + 32);
  strcpy(text, "{\"path\":\"");
  memset(text + 9, 'x', big);
  strcpy(text + 9 + big, "\"}");
  CTUI_TEST_ASSERT(read_line(r, text, &s, &a) == 0 && strlen(s.path) == big &&
                       !strcmp(paths[0], "/music/00000.flac"),
                   "a string bigger than a block, the others untouched");
  ctui_json_arena_reset(&a);
  CTUI_TEST_ASSERT(a.head &&
                       read_line(r, "{\"path\":\"again\"}", &s, &a) == 0 &&
                       !strcmp(s.path, "again"),
                   "reset: one block kept and used again");
  free(text);
  free(paths);
  ctui_json_arena_free(&a);
  CTUI_TEST_ASSERT(!a.head, "freed");
}

/* the same lines through json.c: the same values */
static void test_against_dom(CTUI_JSON_REC *r, CTUI_JSON_ARENA *a) {
  static const char *const LINES[] = {
      "{\"path\":\"a\\tb\",\"size\":0,\"gain\":1e-3,\"track\":-0}",
      "{\"artist\":\"\\u0041\\u00df\\u4e2d\\ud834\\udd1e\",\"size\":-42}",
      "{\"size\":9007199254740992,\"cover\":false}",
  };
  int ok = 1;
  for (size_t i = 0; i < sizeof LINES / sizeof *LINES; i++) {
    SONG s;
    char err[128];
    CTUI_JSON *j = ctui_json_parse(LINES[i], err, sizeof err);
    ok &= j && read_line(r, LINES[i], &s, a) == 0;
    const char *p = ctui_json_str(ctui_json_get(j, "path"), NULL);
    const char *ar = ctui_json_str(ctui_json_get(j, "artist"), NULL);
    ok &= (!p && !s.path) || (p && s.path && !strcmp(p, s.path));
    ok &= (!ar && !s.artist) || (ar && s.artist && !strcmp(ar, s.artist));
    if (ctui_json_get(j, "size")) {
      ok &= s.size == ctui_json_llong(ctui_json_get(j, "size"), 1);
    }
    if (ctui_json_get(j, "gain")) {
      ok &= s.gain == ctui_json_num(ctui_json_get(j, "gain"), 1);
    }
    ctui_json_free(j);
  }
  CTUI_TEST_ASSERT(ok, "json.c reads the same values");
  /* past 2^53, where json.c's doubles round: a time in nanoseconds */
  SONG s;
  CTUI_TEST_ASSERT(read_line(r, "{\"size\":1790544701123456789}", &s, a) == 0 &&
                       s.size == 1790544701123456789LL,
                   "integers read exactly, not through a double");
}

int main(void) {
  ctui_log_init(E_ALL);
  CTUI_JSON_REC *r = ctui_json_rec_new(FIELDS, NF);
  CTUI_TEST_ASSERT(r != NULL, "the table hashed");
  if (!r) {
    return ctui_test_summary();
  }
  CTUI_JSON_ARENA a = {0};
  test_kinds(r, &a);
  test_refused(r, &a);
  test_arena(r);
  test_against_dom(r, &a);
  ctui_json_arena_free(&a);
  ctui_json_rec_free(r);
  CTUI_JSON_REC *none = ctui_json_rec_new(NULL, 0);
  SONG s;
  CTUI_TEST_ASSERT(none && read_line(none, "{\"path\":\"x\"}", &s, &a) == 0 &&
                       !s.path,
                   "no fields: every key stepped over");
  ctui_json_arena_free(&a);
  ctui_json_rec_free(none);
  return ctui_test_summary();
}
