/* must precede every #include -- clock_gettime() is POSIX, not C11 (see
 * term.c's identical comment for why this has to come before the first
 * system header, even a transitive one). */
#define _POSIX_C_SOURCE 200809L

#include "profile.h"

#include "log.h"

#include <stdio.h>
#include <string.h>

#define CTUI_PROFILE_MAX_COUNTERS 32
#define CTUI_PROFILE_NAME_MAX 63

static struct {
  char name[CTUI_PROFILE_NAME_MAX + 1];
  unsigned long long count;
  unsigned long long total_ns;
  unsigned long long min_ns;
  unsigned long long max_ns;
} g_counters[CTUI_PROFILE_MAX_COUNTERS];
static int g_counter_count;

CTUI_PROFILE_SPAN ctui_profile_begin(void) {
  CTUI_PROFILE_SPAN span;
  clock_gettime(CLOCK_MONOTONIC, &span.t0);
  return span;
}

/* linear scan, same "array, not a hash map, at this scale" convention
 * event.c's registry already uses -- a handful of named sections, checked
 * once per section per frame, not a hot inner loop. Returns NULL (logging
 * once) if the table's full rather than growing it: every call site is
 * known at compile time, so a fixed cap that's never actually hit in
 * practice is simpler than a realloc path that only exists for a case
 * that never happens. */
static int find_or_create(const char *name) {
  for (int i = 0; i < g_counter_count; i++) {
    if (strcmp(g_counters[i].name, name) == 0) {
      return i;
    }
  }
  if (g_counter_count >= CTUI_PROFILE_MAX_COUNTERS) {
    ctui_logf(E_WRN,
              "[CTUI:PROFILE] - counter table full (%d), dropping \"%s\"\n",
              CTUI_PROFILE_MAX_COUNTERS, name);
    return -1;
  }
  int i = g_counter_count++;
  snprintf(g_counters[i].name, sizeof(g_counters[i].name), "%s", name);
  g_counters[i].count = 0;
  g_counters[i].total_ns = 0;
  g_counters[i].min_ns = ~0ULL;
  g_counters[i].max_ns = 0;
  return i;
}

void ctui_profile_end(CTUI_PROFILE_SPAN span, const char *name) {
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  unsigned long long ns =
      (unsigned long long)(t1.tv_sec - span.t0.tv_sec) * 1000000000ULL +
      (unsigned long long)(t1.tv_nsec - span.t0.tv_nsec);

  int i = find_or_create(name);
  if (i < 0) {
    return;
  }
  g_counters[i].count++;
  g_counters[i].total_ns += ns;
  if (ns < g_counters[i].min_ns) {
    g_counters[i].min_ns = ns;
  }
  if (ns > g_counters[i].max_ns) {
    g_counters[i].max_ns = ns;
  }
}

void ctui_profile_dump(void) {
  for (int i = 0; i < g_counter_count; i++) {
    double avg_us = g_counters[i].count
                        ? (double)g_counters[i].total_ns / g_counters[i].count / 1000.0
                        : 0.0;
    ctui_logf(E_INF,
              "[CTUI:PROFILE] - %s: n=%llu avg=%.1fus min=%.1fus max=%.1fus\n",
              g_counters[i].name, g_counters[i].count, avg_us,
              g_counters[i].min_ns / 1000.0, g_counters[i].max_ns / 1000.0);
  }
}
