#include "calc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double apply_op(double lhs, char op, double rhs, int *ok) {
  *ok = 1;
  switch (op) {
  case '+':
    return lhs + rhs;
  case '-':
    return lhs - rhs;
  case '*':
    return lhs * rhs;
  case '/':
    if (rhs == 0.0) {
      *ok = 0;
      return 0.0;
    }
    return lhs / rhs;
  }
  *ok = 0;
  return rhs;
}

static void format(double v, char *out, size_t cap) {
  snprintf(out, cap, "%.10g", v);
}

static void do_clear(CALC_STATE *s) {
  *s = (CALC_STATE){.fresh_entry = 1, .text = "0"};
}

static void do_digit(CALC_STATE *s, char digit) {
  if (s->fresh_entry || strcmp(s->text, "0") == 0) {
    s->text[0] = digit;
    s->text[1] = '\0';
    s->fresh_entry = 0;
    return;
  }
  size_t len = strlen(s->text);
  if (len + 1 >= sizeof(s->text)) {
    return; /* full, ignore */
  }
  s->text[len] = digit;
  s->text[len + 1] = '\0';
}

static void do_dot(CALC_STATE *s) {
  if (s->fresh_entry) {
    snprintf(s->text, sizeof(s->text), "0.");
    s->fresh_entry = 0;
    return;
  }
  if (strchr(s->text, '.')) {
    return;
  }
  size_t len = strlen(s->text);
  if (len + 1 >= sizeof(s->text)) {
    return;
  }
  s->text[len] = '.';
  s->text[len + 1] = '\0';
}

static void do_backspace(CALC_STATE *s) {
  if (s->fresh_entry) {
    return;
  }
  size_t len = strlen(s->text);
  if (len <= 1) {
    snprintf(s->text, sizeof(s->text), "0");
    s->fresh_entry = 1;
    return;
  }
  s->text[len - 1] = '\0';
}

static void do_negate(CALC_STATE *s) {
  double v = -atof(s->text);
  if (v == 0.0) {
    v = 0.0; /* avoid a "-0" readout */
  }
  format(v, s->text, sizeof(s->text));
}

static void do_percent(CALC_STATE *s) {
  format(atof(s->text) / 100.0, s->text, sizeof(s->text));
}

static void do_operator(CALC_STATE *s, char op) {
  double entry = atof(s->text);
  if (s->pending_op && !s->fresh_entry) {
    int ok;
    double result = apply_op(s->accumulator, s->pending_op, entry, &ok);
    if (!ok) {
      snprintf(s->text, sizeof(s->text), "Error");
      s->error = 1;
      return;
    }
    s->accumulator = result;
    format(result, s->text, sizeof(s->text));
  } else {
    s->accumulator = entry;
  }
  s->pending_op = op;
  s->fresh_entry = 1;
}

static void do_equals(CALC_STATE *s) {
  if (!s->pending_op) {
    return;
  }
  double entry = atof(s->text);
  int ok;
  double result = apply_op(s->accumulator, s->pending_op, entry, &ok);
  s->pending_op = 0;
  s->fresh_entry = 1;
  if (!ok) {
    snprintf(s->text, sizeof(s->text), "Error");
    s->error = 1;
    return;
  }
  s->accumulator = result;
  format(result, s->text, sizeof(s->text));
}

CALC_STATE calc_make(void) { return (CALC_STATE){.fresh_entry = 1, .text = "0"}; }

CALC_RESULT calc_apply(CALC_STATE *state, CALC_TOKEN token) {
  if (state->error && token.type != CALC_CLEAR) {
    return (CALC_RESULT){.text = state->text, .error = 1, .changed = 0};
  }

  switch (token.type) {
  case CALC_CLEAR:
    do_clear(state);
    break;
  case CALC_BACKSPACE:
    do_backspace(state);
    break;
  case CALC_NEGATE:
    do_negate(state);
    break;
  case CALC_PERCENT:
    do_percent(state);
    break;
  case CALC_DOT:
    do_dot(state);
    break;
  case CALC_EQUALS:
    do_equals(state);
    break;
  case CALC_OP:
    do_operator(state, token.value);
    break;
  case CALC_DIGIT:
    do_digit(state, token.value);
    break;
  }

  return (CALC_RESULT){.text = state->text, .error = state->error, .changed = 1};
}
