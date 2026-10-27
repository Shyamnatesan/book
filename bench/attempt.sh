#!/usr/bin/env bash
# One hillclimb attempt: gate the working tree, then A/B its bench against a
# kept binary. Usage: [BENCHMARK_SRC=dir] bench/attempt.sh <kept_bench_binary> [runs] [filter]
# Leaves the candidate binary at $OUT/candidate_bench.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
kept="$1"; runs="${2:-5}"; filter="${3:-}"
OUT="${OUT:-/tmp/fb-rw}"
build="$OUT/new"
cd "$repo"

# BENCHMARK_SRC: an existing google/benchmark checkout, so the build needs no network.
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release \
  ${BENCHMARK_SRC:+-DFETCHCONTENT_SOURCE_DIR_BENCHMARK="$BENCHMARK_SRC" -DFETCHCONTENT_FULLY_DISCONNECTED=ON} \
  > "$OUT/attempt-cmake.log"
cmake --build "$build" -j"$(nproc)" 2>&1 | grep -iE "warning|error" && exit 1
(cd "$build" && ctest --output-on-failure > "$OUT/attempt-ctest.log") || { cat "$OUT/attempt-ctest.log"; exit 1; }
for t in book_test invariant_test; do
  g++ -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Iinclude "tests/$t.cpp" -o "$OUT/${t}_asan"
  "$OUT/${t}_asan" | tail -1
done
.claude/skills/verify-fastbook-matching/check/run.sh 2>/dev/null | grep -E "PASS|FAIL|INCOMPLETE"
cp "$build/book_bench" "$OUT/candidate_bench"
python3 bench/compare.py "$kept" "$OUT/candidate_bench" --runs "$runs" ${filter:+--filter "$filter"} 2>/dev/null
