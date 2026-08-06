#!/usr/bin/env bash
# Builds every tests/*.c against a --coverage-instrumented core+widgets,
# running each one so its coverage accumulates -- all builds share the same
# -o name (coverage/run), so gcc gives every translation unit the same
# .gcno/.gcda path across builds, and gcov's runtime adds each run's counts
# into the existing .gcda instead of overwriting it. End result: one merged
# view of what the whole test suite (not just one binary) exercises.
#
# gcov over lcov/gcovr: no extra dependency (gcov ships with gcc), and a
# per-file "N% of M lines" summary is all a first read needs -- see
# CLAUDE.md's Testing section for the harness this measures.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COV_DIR="$ROOT/coverage"
CC=${CC:-cc}

CORE_SRC=("$ROOT"/src/*.c "$ROOT"/src/core/*.c "$ROOT"/src/widgets/*.c)

rm -rf "$COV_DIR"
mkdir -p "$COV_DIR"

for t in "$ROOT"/tests/*.c; do
  "$CC" -Wall -Wextra -std=c11 -Isrc -Itools -g --coverage \
    -o "$COV_DIR/run" "${CORE_SRC[@]}" "$t"
  (cd "$COV_DIR" && ./run >/dev/null 2>&1)
done

total_exec=0
total_lines=0
printf "%-20s %9s   lines\n" "file" "coverage"
for f in "$ROOT"/src/core/*.c; do
  base=$(basename "$f" .c)
  gcda="$COV_DIR/run-$base.gcda"
  [ -f "$gcda" ] || continue
  read -r pct n < <(gcov -n "$gcda" 2>/dev/null |
    grep "Lines executed" | head -1 |
    sed -E 's/Lines executed:([0-9.]+)% of ([0-9]+)/\1 \2/')
  exec_lines=$(awk -v p="$pct" -v n="$n" 'BEGIN{printf "%.0f", p/100*n}')
  total_exec=$((total_exec + exec_lines))
  total_lines=$((total_lines + n))
  printf "%-20s %8s%%   %s/%s\n" "$base.c" "$pct" "$exec_lines" "$n"
done

awk -v e="$total_exec" -v n="$total_lines" \
  'BEGIN{printf "\n%-20s %8.1f%%   %d/%d\n", "TOTAL (src/core)", e/n*100, e, n}'

echo
echo "annotated per-line detail: gcov -o coverage -s src/core <file>.c"
