#ifndef CTUI_QUERY_H
#define CTUI_QUERY_H

#include <stddef.h>

/* A search over records the app describes: foobar2000's search syntax (a
 * subset) plus short forms for typing, over a table of fields the app
 * gives with a getter for their values. Compiled once, matched per record;
 * headless, no allocation while matching, and a compiled query is
 * read-only (match and run it from any thread).
 *
 *   artist HAS pink floyd   the field contains the text (any case, UTF-8);
 *                           the text runs to the next keyword, or quote it
 *   genre IS "Rock"         the field is the text (any case)
 *   date GREATER 1999       a text field: numbers as numbers ("1999-03" is
 *   date LESS 2000          1999; a value without one matches neither),
 *                           else text compared bytewise
 *   album MISSING           no value; PRESENT: one
 *   ALL                     every record
 *   pink floyd              bare words: each in one of the fields marked
 *                           CTUI_QUERY_BARE
 *   a AND b, a OR b, NOT a, ( ... ), a b (an AND)
 *   ... SORT BY date DESCENDING, track
 *
 * The short forms, a field's name right before the operator:
 *
 *   artist:boards           HAS (IS for a CTUI_QUERY_EXACT field); with a
 *                           * or ? in it, a glob over the whole value
 *   artist="Boards"         IS (or the glob)
 *   ext:flac,mp3            any of them
 *   title:"two words"       a quoted value
 *   size>10M  size<=1G      typed values: sizes (K M G T, 1024s), lengths
 *   size:1M..50M  year:..1999   (90, 5m, 1h30m, 3:30), dates (2026,
 *                           2026-01, 2026-01-31, 2026-01-31T10:30, local
 *                           time): a date stands for its whole period, so
 *                           modified>2026-01 is from February on and
 *                           modified:2026 is within the year; ranges
 *                           include both ends
 *   age<7d                  an age, compared with the date field it's the
 *                           age of: age<7d is modified>(now - 7 days)
 *   is:link                 a flag field is set
 *   -word  -artist:x        NOT
 *
 * Keywords only in capitals (a "has" in a search is a word); a field as its
 * name or %name%. An error names its column (code points, from 1). */

typedef enum {
  CTUI_QUERY_TEXT,
  CTUI_QUERY_NUMBER,   /* an integer */
  CTUI_QUERY_SIZE,     /* bytes */
  CTUI_QUERY_DURATION, /* seconds */
  CTUI_QUERY_DATE,     /* seconds since the epoch */
  CTUI_QUERY_AGE,      /* how long before now its `of` field's date is */
  CTUI_QUERY_FLAG,     /* set (n != 0) or not */
} CTUI_QUERY_KIND;

#define CTUI_QUERY_BARE 1  /* bare words look in it */
#define CTUI_QUERY_EXACT 2 /* field:x is IS, not HAS */

typedef struct {
  const char *name;
  CTUI_QUERY_KIND kind;
  int flags; /* CTUI_QUERY_BARE | CTUI_QUERY_EXACT */
  /* how dear its value is to get: an AND's terms run cheapest first (a
   * name before a file's tags) */
  int cost;
  int of; /* CTUI_QUERY_AGE: the index of its CTUI_QUERY_DATE field */
} CTUI_QUERY_FIELD;

typedef struct {
  /* TEXT: the value, NULL or "" for none (MISSING); the getter points it
   * at its own string or at buf */
  const char *s;
  long long n; /* the other kinds' value */
  int has;     /* the other kinds: 0 = none (MISSING) */
} CTUI_QUERY_VALUE;

#define CTUI_QUERY_VALUE_MAX 4096 /* the buf a getter may write into */

typedef struct {
  const CTUI_QUERY_FIELD *fields;
  int nfields;
  /* record's value of field (an index into fields) into *v (zeroed
   * before the call); text may be written into buf (cap bytes) */
  void (*get)(const void *record, int field, CTUI_QUERY_VALUE *v, char *buf,
              size_t cap, void *ud);
  void *ud;
} CTUI_QUERY_SCHEMA;

typedef struct CTUI_QUERY CTUI_QUERY;

#define CTUI_QUERY_SOURCE_MAX 4096
#define CTUI_QUERY_ERR_MAX 160

/* src -> *out (ctui_query_free() it), over schema (kept by pointer: it
 * outlives the query). now: what ages count from (seconds since the
 * epoch). 0, or -1 with "col 9: HAS needs a text after it" in err (*out
 * NULL). An empty src is ALL. */
int ctui_query_compile(const CTUI_QUERY_SCHEMA *schema, const char *src,
                       long long now, CTUI_QUERY **out, char *err,
                       size_t err_cap);

/* 1 if record matches */
int ctui_query_match(const CTUI_QUERY *q, const void *record);

/* 1 if it has a SORT BY */
int ctui_query_sorted(const CTUI_QUERY *q);

/* the matching ones of n records (record i at records + i * stride): their
 * indexes, malloc'd into *out (NULL for none), in SORT BY's order, else
 * theirs; ties keep their order. The count, or -1 on no memory. */
int ctui_query_run(const CTUI_QUERY *q, const void *records, size_t stride,
                   int n, int **out);

/* the fields the query reads (indexes, each once, in the order they come;
 * a bare word's are every CTUI_QUERY_BARE field, an age's its date field;
 * SORT BY's last): how many, at most cap written. What a search reads per
 * record and what columns it shows. */
int ctui_query_fields(const CTUI_QUERY *q, int *out, int cap);

void ctui_query_free(CTUI_QUERY *q);

/* s folded (Unicode's simple case folding) into out (cap bytes, always
 * terminated; cut at a codepoint): its length */
size_t ctui_query_fold(const char *s, char *out, size_t cap);

#endif
