#ifndef CTUI_JSON_H
#define CTUI_JSON_H

#include "buf.h"

#include <stddef.h>

/* A small JSON DOM parser for hand-edited configs (promoted from
 * ctui-wm, its second user ctui-mus). Accepts standard JSON plus two
 * concessions to humans: // line and block comments anywhere whitespace
 * is allowed, and trailing commas in arrays/objects. Strings are decoded
 * to UTF-8 (\uXXXX escapes and surrogate pairs included). Object key
 * order is preserved -- a config's things come out in the order they're
 * written. */

typedef enum {
  CTUI_JSON_NULL,
  CTUI_JSON_BOOL,
  CTUI_JSON_NUMBER,
  CTUI_JSON_STRING,
  CTUI_JSON_ARRAY,
  CTUI_JSON_OBJECT,
} CTUI_JSON_TYPE;

typedef struct CTUI_JSON CTUI_JSON;
struct CTUI_JSON {
  CTUI_JSON_TYPE type;
  int boolean;
  double number;
  char *string;
  /* ARRAY: items[0..count). OBJECT: keys[i] -> items[i], in file order. */
  CTUI_JSON *items;
  char **keys;
  int count;
  /* where the value was in the parsed text: bytes [start, end) (so a
   * value can be replaced in the text without touching the comments and
   * layout around it); 0, 0 for a tree built by hand */
  size_t start, end;
  /* an object member's: where its "key" began (an editor deleting a
   * member from there) */
  size_t key_start;
};

/* parses text into a freshly allocated tree, or returns NULL and writes a
 * "line L, col C: what went wrong" message into err */
CTUI_JSON *ctui_json_parse(const char *text, char *err, size_t err_cap);
void ctui_json_free(CTUI_JSON *v);

/* object member lookup; NULL if v isn't an object or has no such key */
CTUI_JSON *ctui_json_get(const CTUI_JSON *v, const char *key);

/* typed reads with a fallback for "missing or wrong type", so config
 * code can read optional settings in one line */
const char *ctui_json_str(const CTUI_JSON *v, const char *fallback);
double ctui_json_num(const CTUI_JSON *v, double fallback);
int ctui_json_bool(const CTUI_JSON *v, int fallback);
/* a number as an integer: truncated toward zero and clamped to the
 * type's range, where a plain cast of an out-of-range double is
 * undefined ("lines": 1e20 in a config, a peer's pid) */
int ctui_json_int(const CTUI_JSON *v, int fallback);
unsigned ctui_json_uint(const CTUI_JSON *v, unsigned fallback);
long long ctui_json_llong(const CTUI_JSON *v, long long fallback);

/* deep equality (object members compared in order); NULLs are equal to
 * each other only */
int ctui_json_equal(const CTUI_JSON *a, const CTUI_JSON *b);

/* --- writing (into buf.h's growable string) --- */

/* appends s as a JSON string literal, ASCII-only (non-ASCII as \u
 * escapes, like kitten @ itself sends), so no byte of it can be mistaken
 * for framing -- neither kitty's ESC-based one nor a newline-per-message
 * stream */
void ctui_json_quote(CTUI_BUF *b, const char *s);
/* a NULL-terminated list as a JSON array of strings */
void ctui_json_quote_list(CTUI_BUF *b, const char *const *list);
/* appends v as JSON (strings quoted as above; NULL writes null): what
 * ctui_json_parse() reads back into an equal tree */
void ctui_json_write(CTUI_BUF *b, const CTUI_JSON *v);

#endif
