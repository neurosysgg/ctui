#ifndef CTUI_CORE_PROFILE_H
#define CTUI_CORE_PROFILE_H

#include <time.h>

/* minimal wall-clock instrumentation for hot render/compute paths --
 * exactly the "grep the log, don't guess" convention CLAUDE.md's Testing
 * approach already leans on, just with real numbers instead of a
 * stopwatch. Deliberately not a sampling profiler or anything perf/
 * valgrind-shaped: those measure a whole process from outside, this
 * measures the handful of named sections a caller actually wants numbers
 * for, cheaply enough to leave compiled in permanently (one
 * clock_gettime() + a linear scan over a short flat array per span, same
 * "array, not a hash map, at this scale" call event.c's registry already
 * makes).
 *
 * Usage:
 *   CTUI_PROFILE_SPAN sp = ctui_profile_begin();
 *   ... code being measured ...
 *   ctui_profile_end(sp, "widget.section");
 *
 * `name` is looked up by strcmp, not pointer identity, so passing the same
 * literal text from multiple call sites (or translation units) accumulates
 * into one shared counter -- callers don't need to share a #define to get
 * that. ctui_profile_dump() logs a one-line summary per counter. */

typedef struct {
  struct timespec t0;
} CTUI_PROFILE_SPAN;

CTUI_PROFILE_SPAN ctui_profile_begin(void);

/* not thread-safe *across* two threads racing to update the *same* named
 * counter -- every existing call site so far only ever ends a given name
 * from one thread (audiovis's consumer thread owns the FFT counters, the
 * GUI thread owns every render/kitty-transmit counter), so there's never
 * actually a race in practice. If a future caller needs the same name
 * updated from two threads, that needs a real lock added here first --
 * don't just start doing it and rely on plain-int updates being "close
 * enough", since count/total_ns drifting out of sync from each other
 * would make the averages wrong, not just imprecise. */
void ctui_profile_end(CTUI_PROFILE_SPAN span, const char *name);

/* logs every counter recorded so far via ctui_logf(E_INF, ...), one line
 * each: sample count, min/avg/max microseconds. Counters persist across
 * calls (this doesn't reset anything) -- call it periodically (a tick
 * counter modulo, a debug hotkey) to watch trends over a run. */
void ctui_profile_dump(void);

#endif
