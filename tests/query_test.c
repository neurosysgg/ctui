/* query/query.h: what matches, what sorts how, what's an error and where
 * -- foobar2000's keywords as ctui-mus had them, the short forms, typed
 * values (sizes, lengths, dates, ages), folding beyond ASCII, the cost
 * order */
#define _DEFAULT_SOURCE

#include "query/query.h"

#include "ctui.h"
#include "ctui_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  const char *name, *ext, *artist, *date;
  long long size, length, modified, year;
  int link;
} REC;

enum { F_NAME, F_EXT, F_ARTIST, F_DATE, F_SIZE, F_LENGTH, F_MODIFIED, F_AGE,
       F_YEAR, F_LINK, F_COUNT };

static const CTUI_QUERY_FIELD FIELDS[] = {
    {"name", CTUI_QUERY_TEXT, CTUI_QUERY_BARE, 0, 0},
    {"ext", CTUI_QUERY_TEXT, CTUI_QUERY_EXACT, 0, 0},
    {"artist", CTUI_QUERY_TEXT, CTUI_QUERY_BARE, 5, 0},
    {"date", CTUI_QUERY_TEXT, 0, 5, 0},
    {"size", CTUI_QUERY_SIZE, 0, 1, 0},
    {"length", CTUI_QUERY_DURATION, 0, 5, 0},
    {"modified", CTUI_QUERY_DATE, 0, 1, 0},
    {"age", CTUI_QUERY_AGE, 0, 1, F_MODIFIED},
    {"year", CTUI_QUERY_NUMBER, 0, 5, 0},
    {"link", CTUI_QUERY_FLAG, 0, 0, 0},
};

#define NOW 1767225600LL /* 2026-01-01T00:00:00Z */
#define DAY 86400LL

static const REC RECS[] = {
    {"Speak to Me.flac", "flac", "Pink Floyd", "1973", 30LL << 20, 90,
     NOW - 2 * DAY, 1973, 0},
    {"Breathe.FLAC", "flac", "Pink Floyd", "1973-03-01", 50LL << 20, 163,
     NOW - 10 * DAY, 1973, 0},
    {"Ärger.mp3", "mp3", "Die Ärzte", "1998", 4LL << 20, 185, NOW - 400 * DAY,
     1998, 0},
    {"untagged.wav", "wav", "", "", 300LL << 20, 30, NOW - 3600, 0, 1},
    {"ΣΟΦΙΑ.mp3", "mp3", "Band", "2001", 1500, 240, NOW - 40 * DAY, 2001, 0},
};
#define NRECS (int)(sizeof RECS / sizeof *RECS)

static int g_gets[F_COUNT]; /* each field's getter calls */

static void get(const void *record, int field, CTUI_QUERY_VALUE *v, char *buf,
                size_t cap, void *ud) {
  (void)buf;
  (void)cap;
  (void)ud;
  const REC *r = record;
  g_gets[field]++;
  switch (field) {
  case F_NAME:
    v->s = r->name;
    break;
  case F_EXT:
    v->s = r->ext;
    break;
  case F_ARTIST:
    v->s = r->artist;
    break;
  case F_DATE:
    v->s = r->date;
    break;
  case F_SIZE:
    v->n = r->size;
    v->has = 1;
    break;
  case F_LENGTH:
    v->n = r->length;
    v->has = 1;
    break;
  case F_MODIFIED:
    v->n = r->modified;
    v->has = 1;
    break;
  case F_YEAR:
    v->n = r->year;
    v->has = r->year != 0;
    break;
  case F_LINK:
    v->n = r->link;
    v->has = 1;
    break;
  default:
    break;
  }
}

static const CTUI_QUERY_SCHEMA SCHEMA = {FIELDS, F_COUNT, get, NULL};

static char g_err[CTUI_QUERY_ERR_MAX];

/* which records src matches, as a string of their numbers ("013"); "!"
 * when it doesn't compile */
static const char *matches(const char *src) {
  static char out[NRECS + 1];
  CTUI_QUERY *q;
  if (ctui_query_compile(&SCHEMA, src, NOW, &q, g_err, sizeof g_err) != 0) {
    printf("    (\"%s\": %s)\n", src, g_err);
    return "!";
  }
  int n = 0;
  for (int i = 0; i < NRECS; i++) {
    if (ctui_query_match(q, &RECS[i])) {
      out[n++] = (char)('0' + i);
    }
  }
  out[n] = '\0';
  ctui_query_free(q);
  return out;
}

static int is(const char *src, const char *want) {
  const char *got = matches(src);
  if (strcmp(got, want) != 0) {
    printf("    (\"%s\": %s, not %s)\n", src, got, want);
    return 0;
  }
  return 1;
}

static int fails_with(const char *src, const char *want) {
  CTUI_QUERY *q = NULL;
  int rc = ctui_query_compile(&SCHEMA, src, NOW, &q, g_err, sizeof g_err);
  ctui_query_free(q);
  if (rc == 0 || !strstr(g_err, want)) {
    printf("    (\"%s\": \"%s\")\n", src, g_err);
    return 0;
  }
  return 1;
}

static void test_keywords(void) {
  CTUI_TEST_ASSERT(is("artist HAS pink", "01") && is("artist HAS pink floyd", "01"),
                   "HAS: contains, any case; the text runs to the next keyword");
  CTUI_TEST_ASSERT(is("ext IS FLAC", "01") && is("ext IS fla", ""),
                   "IS: the whole value, any case");
  CTUI_TEST_ASSERT(is("date GREATER 1975", "24") && is("date LESS 1974", "01"),
                   "GREATER / LESS on a text field: leading numbers; an empty "
                   "date matches neither");
  CTUI_TEST_ASSERT(is("artist MISSING", "3") && is("year MISSING", "3") &&
                       is("year PRESENT", "0124"),
                   "MISSING / PRESENT, text and typed");
  CTUI_TEST_ASSERT(is("%artist% HAS band", "4"), "a field as %%name%%");
  CTUI_TEST_ASSERT(is("ALL", "01234") && is("", "01234") && is("  ", "01234"),
                   "ALL, and nothing: everything");
  CTUI_TEST_ASSERT(is("size GREATER 40M", "13") && is("length LESS 1m", "3"),
                   "GREATER / LESS on typed fields: typed values");
}

static void test_words_and_logic(void) {
  CTUI_TEST_ASSERT(is("breathe", "1") && is("FLOYD", "01") && is("untagged", "3"),
                   "bare words: the BARE fields, any case");
  CTUI_TEST_ASSERT(is("pink speak", "0") && is("pink aria", ""), "two words: both");
  CTUI_TEST_ASSERT(is("ext IS mp3 OR ext IS wav", "234"), "OR");
  CTUI_TEST_ASSERT(is("NOT ext IS flac", "234") && is("NOT NOT ext IS flac", "01"),
                   "NOT");
  CTUI_TEST_ASSERT(is("ext IS mp3 OR ext IS flac AND year IS 1973", "0124"),
                   "AND binds tighter than OR");
  CTUI_TEST_ASSERT(is("(ext IS mp3 OR ext IS flac) AND year IS 1998", "2"),
                   "parentheses");
  CTUI_TEST_ASSERT(is("\"to me\"", "0"), "a quoted phrase");
}

static void test_short_forms(void) {
  CTUI_TEST_ASSERT(is("artist:floyd", "01") && is("artist=\"pink floyd\"", "01") &&
                       is("artist=floyd", ""),
                   "field:x is HAS, field=x IS");
  CTUI_TEST_ASSERT(is("ext:fla", "") && is("ext:FLAC", "01"),
                   "an EXACT field's : is IS");
  CTUI_TEST_ASSERT(is("ext:flac,wav", "013"), "a list: any of them");
  CTUI_TEST_ASSERT(is("name:*.mp3", "24") && is("name:b?eathe*", "1") &&
                       is("name:?rger*", "2"),
                   "a glob over the whole value (? one codepoint: Ä)");
  CTUI_TEST_ASSERT(is("-ext:flac", "234") && is("-pink -band", "23") &&
                       is("-(ext:mp3 OR ext:wav)", "01"),
                   "- is NOT");
  CTUI_TEST_ASSERT(is("is:link", "3") && is("-is:link", "0124") &&
                       is("link:no", "0124"),
                   "flags");
  CTUI_TEST_ASSERT(is("artist:floyd SORT BY name", "01") && is("ext:mp3 artist:band", "4"),
                   "short forms next to keywords and each other");
  CTUI_TEST_ASSERT(is("(artist:floyd)", "01") && is("artist:\"die ä\"", "2"),
                   "a value ends at a parenthesis; a quoted one may hold spaces");
}

static void test_typed(void) {
  CTUI_TEST_ASSERT(is("size>40M", "13") && is("size<5M", "24") &&
                       is("size:1M..50M", "012") && is("size<=50M", "0124") &&
                       is("size>=50mib", "13") && is("size:1500", "4") &&
                       is("size<1k", "") && is("size<1.5K", "4"),
                   "sizes: units in 1024s, any case, decimals; ranges include "
                   "their ends");
  CTUI_TEST_ASSERT(is("length>3m", "24") && is("length:2:43", "1") &&
                       is("length:1m30s", "0") && is("length<=0:30", "3") &&
                       is("length:..1m30", "03"),
                   "lengths: 3m, m:ss, 1m30s, a bare number in seconds, open ranges");
  CTUI_TEST_ASSERT(is("year>1990", "24") && is("year:1973", "01") &&
                       is("year:1990..", "24"),
                   "numbers");
  CTUI_TEST_ASSERT(is("modified:2025-12", "013") && is("modified>2025-12-22", "03"),
                   "dates: their whole period (> is after the day)");
  CTUI_TEST_ASSERT(is("modified>=2025-12-22", "013") && is("modified<2025", "2") &&
                       is("modified:2024..2025-11", "24") &&
                       is("modified:2025-12-31T23:00", "3"),
                   "dates: from a day's start, before a year, ranges, to the minute");
  CTUI_TEST_ASSERT(is("age<7d", "03") && is("age>1y", "2") && is("age:1d..2w", "01") &&
                       is("age<=2h", "3") && is("age>=1w AND age<2mo", "14"),
                   "ages: compared with modified, now minus the age");
  CTUI_TEST_ASSERT(is("NOT age<7d", "124") && is("modified PRESENT", "01234") &&
                       is("age PRESENT", "01234"),
                   "an age is its date field for MISSING / PRESENT");
}

static void test_fold(void) {
  CTUI_TEST_ASSERT(is("ärger", "2") && is("ÄRGER", "2") && is("artist:ärzte", "2"),
                   "case folding beyond ASCII (Ä/ä)");
  CTUI_TEST_ASSERT(is("σοφια", "4") && is("name:\"σοφ\"", "4"), "and Greek (Σ/σ)");
  char out[64];
  ctui_query_fold("ǅungla İ Ω ẞ", out, sizeof out);
  CTUI_TEST_ASSERT(!strcmp(out, "ǆungla İ ω ß"),
                   "titlecase, the dotted I (simple folding keeps it), "
                   "capital sharp s (%s)",
                   out);
  ctui_query_fold("ABCDEF", out, 4);
  CTUI_TEST_ASSERT(!strcmp(out, "abc"), "a short buffer: cut, terminated");
  ctui_query_fold("ÄÄ", out, 4);
  CTUI_TEST_ASSERT(!strcmp(out, "ä"), "cut at a codepoint (%s)", out);
  ctui_query_fold("a\xff\xc3", out, sizeof out);
  CTUI_TEST_ASSERT(!strcmp(out, "a\xff\xc3"), "bytes that aren't UTF-8 kept");
  ctui_query_fold("\xc0\x80" "A\xed\xa0\x80\xf4\x90\x80\x80", out, sizeof out);
  CTUI_TEST_ASSERT(!strcmp(out, "\xc0\x80" "a\xed\xa0\x80\xf4\x90\x80\x80"),
                   "an overlong NUL, a surrogate, past U+10FFFF: kept as bytes "
                   "(the fuzzer's: folding twice changed them)");
}

static void test_errors(void) {
  CTUI_TEST_ASSERT(fails_with("artist HAS", "col 8: HAS needs a text after it"),
                   "an operator without a text, at its column");
  CTUI_TEST_ASSERT(fails_with("HAS x", "col 1: HAS needs a field before it"),
                   "an operator without a field");
  CTUI_TEST_ASSERT(fails_with("(ext:flac", "col 1: a parenthesis isn't closed") &&
                       fails_with("ext:flac)", "col 9: a parenthesis closes nothing"),
                   "parentheses that don't pair");
  CTUI_TEST_ASSERT(fails_with("été \"x", "col 5: a quote isn't closed") &&
                       fails_with("name:\"x", "col 6: a quote isn't closed"),
                   "columns count code points");
  CTUI_TEST_ASSERT(fails_with("artst:x", "col 1: no field \"artst\""),
                   "an unknown field in a short form");
  CTUI_TEST_ASSERT(fails_with("artist:", "artist: needs a value"),
                   "a short form without a value");
  CTUI_TEST_ASSERT(fails_with("size>huge", "size takes a size (10M, 1.5G), not \"huge\"") &&
                       fails_with("size>10Q", "not \"10Q\"") &&
                       fails_with("length:3:", "length takes") &&
                       fails_with("year:x", "year takes a number"),
                   "a typed value that isn't one");
  CTUI_TEST_ASSERT(fails_with("modified<7d", "an age is age<7d") &&
                       fails_with("modified:2026-13", "modified takes a date") &&
                       fails_with("modified:2026-1-1", "modified takes a date"),
                   "a date: an age given to it names the age field");
  CTUI_TEST_ASSERT(fails_with("age:7d", "age takes <, > or a range (age<7d)"),
                   "an age on its own is no period");
  CTUI_TEST_ASSERT(fails_with("is:artist", "is: takes a flag: link") &&
                       fails_with("link:maybe", "link takes yes or no"),
                   "flags");
  CTUI_TEST_ASSERT(fails_with("ext:flac,,mp3", "an empty value in a list"),
                   "an empty list item");
  CTUI_TEST_ASSERT(fails_with("a AND", "a search ends too soon") &&
                       fails_with("a OR OR b", "\"OR\" isn't expected here"),
                   "a missing term");
  CTUI_TEST_ASSERT(fails_with("ALL SORT date", "SORT needs BY") &&
                       fails_with("ALL SORT BY", "SORT BY needs a field") &&
                       fails_with("ALL SORT BY artsit", "no such field to sort by"),
                   "SORT BY's mistakes");
  char deep[300];
  memset(deep, '(', 100);
  snprintf(deep + 100, sizeof deep - 100, "a");
  CTUI_TEST_ASSERT(fails_with(deep, "nested too deeply"), "nesting has a limit");
  memset(deep, '-', 100);
  snprintf(deep + 100, sizeof deep - 100, "a");
  CTUI_TEST_ASSERT(fails_with(deep, "nested too deeply"), "so do NOTs");
  char *big = malloc(CTUI_QUERY_SOURCE_MAX + 2);
  memset(big, 'a', CTUI_QUERY_SOURCE_MAX + 1);
  big[CTUI_QUERY_SOURCE_MAX + 1] = '\0';
  CTUI_TEST_ASSERT(fails_with(big, "too long"), "so has the length");
  free(big);
}

/* an AND's terms run cheapest first; the fields a query reads */
static void test_cost_and_fields(void) {
  CTUI_QUERY *q;
  ctui_query_compile(&SCHEMA, "artist:floyd ext:wav", NOW, &q, g_err, sizeof g_err);
  memset(g_gets, 0, sizeof g_gets);
  for (int i = 0; i < NRECS; i++) {
    ctui_query_match(q, &RECS[i]);
  }
  CTUI_TEST_ASSERT(g_gets[F_EXT] == NRECS && g_gets[F_ARTIST] == 1,
                   "the cheap ext first: artist read only where ext matched "
                   "(%d, %d)",
                   g_gets[F_EXT], g_gets[F_ARTIST]);
  ctui_query_free(q);
  ctui_query_compile(&SCHEMA, "pink age<7d OR size>1G SORT BY year, artist", NOW, &q,
                     g_err, sizeof g_err);
  int f[16];
  int n = ctui_query_fields(q, f, 16);
  CTUI_TEST_ASSERT(n == 5 && f[0] == F_NAME && f[1] == F_ARTIST && f[2] == F_MODIFIED &&
                       f[3] == F_SIZE && f[4] == F_YEAR,
                   "fields read: the bare ones, an age's date, then SORT BY's "
                   "(%d)",
                   n);
  CTUI_TEST_ASSERT(ctui_query_fields(q, f, 2) == 5 && f[1] == F_ARTIST,
                   "the count past cap, cap written");
  ctui_query_free(q);
}

static const char *order(const char *src) {
  static char out[NRECS + 1];
  CTUI_QUERY *q;
  int *hits = NULL;
  if (ctui_query_compile(&SCHEMA, src, NOW, &q, g_err, sizeof g_err) != 0) {
    return "!";
  }
  int n = ctui_query_run(q, RECS, sizeof *RECS, NRECS, &hits);
  for (int i = 0; i < n; i++) {
    out[i] = (char)('0' + hits[i]);
  }
  out[n < 0 ? 0 : n] = '\0';
  free(hits);
  ctui_query_free(q);
  return out;
}

static void test_run(void) {
  CTUI_TEST_ASSERT(!strcmp(order("ext:mp3 OR ext:flac"), "0124"),
                   "unsorted: the records' order");
  CTUI_TEST_ASSERT(!strcmp(order("ALL SORT BY size"), "42013") &&
                       !strcmp(order("ALL SORT BY size DESCENDING"), "31024"),
                   "a typed key");
  CTUI_TEST_ASSERT(!strcmp(order("ALL SORT BY year, length DESCENDING"), "31024"),
                   "two keys; a missing value first (%s)",
                   order("ALL SORT BY year, length DESCENDING"));
  CTUI_TEST_ASSERT(!strcmp(order("ALL SORT BY artist, name"), "34210"),
                   "text keys folded: Band, Die Ärzte, Pink Floyd (%s)",
                   order("ALL SORT BY artist, name"));
  CTUI_TEST_ASSERT(!strcmp(order("ALL SORT BY age"), "24103"),
                   "an age sorts by its date: oldest first (%s)",
                   order("ALL SORT BY age"));
  CTUI_TEST_ASSERT(!strcmp(order("ext:ogg"), ""), "none: nothing");
}

int main(void) {
  ctui_log_init(E_ALL);
  setenv("TZ", "UTC", 1);
  tzset();
  test_keywords();
  test_words_and_logic();
  test_short_forms();
  test_typed();
  test_fold();
  test_errors();
  test_cost_and_fields();
  test_run();
  ctui_log_shutdown();
  return ctui_test_summary();
}
