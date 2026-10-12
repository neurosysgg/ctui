#ifndef CTUI_JSON_RECORD_H
#define CTUI_JSON_RECORD_H

#include <stddef.h>

/* JSON objects -- an NDJSON file's lines, say -- read straight into an
 * app's own fixed records: no tree, no allocation per value. The app
 * describes where each key it wants goes (its kind and offset in the
 * record); a line's other keys, nested values included, are stepped over
 * unparsed, and its strings are decoded into an arena whose pointers stay
 * put until it's reset. What a cache of thousands of records needs, where
 * json.h's DOM costs an allocation per value and several times the text
 * in memory. Strict JSON (no comments, no trailing commas: machines write
 * these files); a duplicate key's last value wins. A \u0000 or a lone
 * surrogate becomes U+FFFD, as with json.c; numbers past a long long's or a
 * double's range are clamped (json.c refuses them). */

typedef enum {
  CTUI_JSON_REC_STR,    /* a const char * (into the arena) */
  CTUI_JSON_REC_LLONG,  /* a long long (a fraction truncated, clamped) */
  CTUI_JSON_REC_DOUBLE, /* a double */
  CTUI_JSON_REC_BOOL,   /* an int: 1 true, 0 false */
} CTUI_JSON_REC_KIND;

typedef struct {
  const char *key;
  CTUI_JSON_REC_KIND kind;
  size_t at; /* the value's offset in the record */
  /* an int set to 1 when the key is there with a value of its kind
   * (offsetof), or -1 for none. null, or a value of another kind, leaves
   * the field and it alone */
  long has;
} CTUI_JSON_REC_FIELD;

/* the arena: blocks chained, never moved */
typedef struct CTUI_JSON_ARENA_BLOCK CTUI_JSON_ARENA_BLOCK;
typedef struct {
  CTUI_JSON_ARENA_BLOCK *head; /* the block being filled, the rest after */
} CTUI_JSON_ARENA;

/* forgets every string but keeps the first block for the next ones */
void ctui_json_arena_reset(CTUI_JSON_ARENA *a);
void ctui_json_arena_free(CTUI_JSON_ARENA *a);
/* n bytes of it (NULL: no memory) */
char *ctui_json_arena_alloc(CTUI_JSON_ARENA *a, size_t n);

typedef struct CTUI_JSON_REC CTUI_JSON_REC;

/* fields' keys hashed once (the table is copied, the keys aren't: they
 * outlive it); NULL on no memory */
CTUI_JSON_REC *ctui_json_rec_new(const CTUI_JSON_REC_FIELD *fields, int n);
void ctui_json_rec_free(CTUI_JSON_REC *r);

/* the object in text[0..len) (whitespace around it allowed) into rec,
 * its strings into a: 0, or -1 when it isn't one (or a's out of memory)
 * -- rec may then hold some of it. rec isn't cleared first. */
int ctui_json_rec_read(const CTUI_JSON_REC *r, const char *text, size_t len,
                       void *rec, CTUI_JSON_ARENA *a);

#endif
