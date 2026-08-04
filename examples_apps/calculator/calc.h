#ifndef CALC_H
#define CALC_H

/* pure 4-function calculator engine, no parens -- deliberately has no
 * dependency on ctui.h. main.c is the thin translation layer: it maps a
 * CTUI_GRID item's label to a CALC_TOKEN, calls calc_apply(), and copies the
 * resulting CALC_RESULT.text into the CTUI_DISPLAY widget. */

typedef enum {
  CALC_DIGIT,     /* value = the digit char, '0'-'9' */
  CALC_DOT,
  CALC_OP,        /* value = one of '+', '-', '*', '/' */
  CALC_EQUALS,
  CALC_CLEAR,
  CALC_BACKSPACE,
  CALC_NEGATE,
  CALC_PERCENT,
} CALC_TOKEN_TYPE;

typedef struct {
  CALC_TOKEN_TYPE type;
  char value; /* only meaningful for CALC_DIGIT/CALC_OP */
} CALC_TOKEN;

typedef struct {
  double accumulator;
  char pending_op; /* 0 = none */
  int fresh_entry; /* true right after an operator, '=', or clear -- the
                    * next digit starts a new number instead of appending */
  int error;       /* set on divide-by-zero; calc_apply() rejects every
                    * token but CALC_CLEAR until this is cleared */
  char text[32];    /* the current operand/result, as typed/formatted */
} CALC_STATE;

typedef struct {
  const char *text; /* == state->text after the token was applied; borrowed,
                     * valid as long as the CALC_STATE it came from is */
  int error;
  int changed; /* 0 only when the token was rejected outright (error state
               * blocking non-clear input); every accepted token -- even one
               * that was itself a no-op, e.g. a second '.' -- reports 1 */
} CALC_RESULT;

/* fresh state: accumulator 0, no pending op, text "0" */
CALC_STATE calc_make(void);

/* applies one token to state, mutating it in place, and returns a summary
 * for the caller to render */
CALC_RESULT calc_apply(CALC_STATE *state, CALC_TOKEN token);

#endif
